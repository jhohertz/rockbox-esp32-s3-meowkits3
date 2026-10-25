/*
 * The Rockbox thread layer's ESP-IDF half.
 *
 * This is its own translation unit on purpose. The arch thread implementation,
 * asm/xtensa/thread.c, is #included into kernel/thread.c rather than compiled
 * separately, so by the time it runs, kernel/thread.c has pulled in the whole
 * Rockbox kernel preamble -- and FreeRTOS.h then fails inside this TU, at the
 * idf_additions.h it implicitly includes at its end, with QueueHandle_t
 * undefined even though idf_additions.h includes freertos/queue.h four lines
 * before it is used. The same includes in a standalone file built with the same
 * flags compile clean, so the fault is in that preamble, not in the flags and
 * not in the order of our own include lines.
 *
 * Rather than untangle the kernel's include graph for this, the IDF calls live
 * here, behind four plain functions. asm/xtensa/thread.c never sees a FreeRTOS
 * type. That is also the better shape regardless of the header problem: the
 * kernel glue and the OS binding are different things.
 */

#include <freertos/FreeRTOS.h>
/* queue.h before task.h and semphr.h: both reach idf_additions.h, whose
 * prototypes name QueueHandle_t. */
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "config.h"
#include "system.h"
#include "thread.h"
#include "panic.h"

#include "asm/xtensa/thread.h"

/* One TCB per thread slot. xTaskCreateStatic wants the caller to own both the
 * stack and the task structure; Rockbox supplies the stack and this supplies
 * the structure, so a thread costs exactly the stack Rockbox already sized. */
/* Not in PSRAM, though internal DRAM is the scarce region here and there is 8
 * MB of PSRAM idle: IDF checks the caller-supplied TCB buffer with
 * xPortCheckValidTCBMem(), which requires internal RAM, and asserts. Measured,
 * not inferred -- putting this in .ext_ram.bss gives
 *   assert failed: xTaskCreateStaticPinnedToCore (xPortCheckValidTCBMem)
 * on the first create_thread, before anything runs.
 *
 * The stacks deliberately stay in internal DRAM too, for a different reason: a
 * thread switched onto a PSRAM stack faults, which is why memory-xtensa-syms.S
 * keeps them in .bss. */
static StaticTask_t tcb_pool[MAXTHREADS];


static void rb_task_body(void *arg)
{
    struct rb_thread_ref *ref = (struct rb_thread_ref *)arg;
    struct regs *ctx = ref->ctx;
    void (*fn)(void) = (void (*)(void))(uintptr_t)ctx->start;

    /* Same rule as the handoff: hold the gate only when the kernel has actually
     * selected this thread, so a stray first give cannot start a thread under
     * someone else's identity. */
    for (;;)
    {
        xSemaphoreTake((SemaphoreHandle_t)ctx->gate, portMAX_DELAY);
        if (rb_thread_running_context() == (void *)ctx)
            break;
    }

    /* We now hold the CPU. Tell the kernel before the thread does anything
     * that might exit, block or panic. */
    rb_thread_set_running(ref->thread);

    fn();

    thread_exit();

    /* thread_exit() hands the core away and does not come back. If it ever
       does, retire the task rather than falling into FreeRTOS's task epilogue
       with a Rockbox slot still marked live. */
    vTaskDelete((TaskHandle_t)ctx->task);
}

void rb_thread_task_create(struct rb_thread_ref *ref, unsigned int slot)
{
    struct regs *ctx = ref->ctx;

    extern int printf(const char *, ...);

    /* Slot reuse: Rockbox recycles thread slots, and the previous occupant's
       task is blocked on its gate forever -- nothing will give it again.
       Deleting it from here is legal because we are not that task. */
    if (ctx->task != NULL)
    {
        vTaskDelete((TaskHandle_t)ctx->task);
        vSemaphoreDelete((SemaphoreHandle_t)ctx->gate);
        ctx->task = NULL;
        ctx->gate = NULL;
    }

    ctx->gate = (void *)xSemaphoreCreateBinary();
    if (ctx->gate == NULL)
        panicf("thread gate: no memory for %s", ref->name);

    /* configSTACK_DEPTH_TYPE counts words; Rockbox's stack_size is bytes. */
    /* Pinned to core 0, and that is load-bearing rather than a tuning choice.
       Rockbox's kernel is single-core here (NUM_CORES is 1), the gate handoff
       assumes the outgoing and incoming thread are on the same core, and
       switch_thread() holds no cross-core lock. xTaskCreateStatic is only a
       macro for xTaskCreateStaticPinnedToCore with tskNO_AFFINITY, so an
       unaffinitied task is what would let two Rockbox threads run at the same
       moment on two cores -- which happened when this project was built on the
       SMP kernel, as CPU 0 running "tick" while CPU 1 ran "scroll".

       That kernel is not in use now: idf-app/sdkconfig leaves
       CONFIG_FREERTOS_SMP unset and the build links
       components/freertos/FreeRTOS-Kernel, so core 1 runs no tasks. The pin is
       kept anyway, because Rockbox's single-core assumption is what makes the
       gate design correct, and because SMP would also remove power management
       -- see the note on that coupling in MEMORY.md. */
    ctx->task = (void *)xTaskCreateStaticPinnedToCore(
        rb_task_body, ref->name ? ref->name : "rb",
        (configSTACK_DEPTH_TYPE)(ref->stack_size / sizeof(StackType_t)),
        ref,
        /* Every Rockbox thread at one priority, on purpose. Rockbox's own
           priorities are enforced by its scheduler; here the only thing that
           decides who runs is which gate is held, and a FreeRTOS priority
           difference would let a ready task preempt a Rockbox thread that has
           not yielded -- breaking the cooperative semantics the whole design
           rests on. Above the idle task, so the idle task cannot starve us. */
        (UBaseType_t)(tskIDLE_PRIORITY + 1),
        (StackType_t *)ref->stack,
        &tcb_pool[slot < MAXTHREADS ? slot : MAXTHREADS - 1],
        0 /* core 0: the only core Rockbox's scheduler owns */);

    if (ctx->task == NULL)
        panicf("task create failed for %s", ref->name);

    /* The caller rewrites Rockbox's stack canary after this returns:
       xTaskCreateStatic memsets the whole buffer with tskSTACK_FILL_BYTE
       (0xa5), which is the same word switch_thread() tests against DEADBEEF.
       The constant lives in thread-internal.h, off this file's include path on
       purpose, so the write belongs on the kernel side of the seam. */


    /* So a switch to this thread can find its own gate from the task handle. */
    vTaskSetThreadLocalStoragePointer((TaskHandle_t)ctx->task, 0, ctx);
}

/* Thread 0 is not created here: it is the task rockbox_app.c started Rockbox's
   main() on. It still needs a gate, because switching away from it has to block
   it like any other thread.

   It must NOT be bound to xTaskGetCurrentTaskHandle(). store_context() calls
   this on every switch-out, so binding to "whatever task is running right now"
   means thread 0 gets attached to the first task that happens to switch out --
   which is exactly what happened: thread 0 ended up sharing the scroll thread's
   task and gate, so a handoff to main resumed scroll as main. The task is now
   recorded once by rockbox_task() before main() exists, and adoption uses that
   handle or refuses to proceed. */
static TaskHandle_t os_thread0_handle;

void rb_thread_set_thread0_task(void *handle)
{
    os_thread0_handle = (TaskHandle_t)handle;
}

void rb_thread_adopt_self(struct regs *ctx)
{
    if (ctx->gate != NULL)
        return;

    if (os_thread0_handle == NULL)
        panicf("thread0 task never recorded -- rockbox_task() must call "
               "rb_thread_set_thread0_task() before main()");

    ctx->gate = (void *)xSemaphoreCreateBinary();
    ctx->task = (void *)os_thread0_handle;

    if (ctx->gate == NULL)
        panicf("thread gate: no memory for thread 0");

    /* Set the TLS on the task that owns this context, not on the current one:
       they are only the same task by correctness, not by coincidence. */
    vTaskSetThreadLocalStoragePointer(os_thread0_handle, 0, ctx);
}

void rb_thread_handoff(struct regs *in)
{
    struct regs *self = (struct regs *)pvTaskGetThreadLocalStoragePointer(NULL, 0);

    /* Give first, then block. Taking our own gate first would let the incoming
       task run only to block again on its. */
    xSemaphoreGive((SemaphoreHandle_t)in->gate);

    if (self != NULL && self != in)
    {
        /* The gate is an exclusive token meaning "the kernel has selected you".
         * It can be given without that being true -- a give from a path that did
         * not go through switch_thread(), or a second give while the holder
         * still holds it -- and a task that proceeds anyway then runs Rockbox
         * code under another thread's identity. Measured on this board: the
         * kernel's running-thread and the executing task disagreed 759 times in
         * one boot, and the result was a thread queueing as a writer against a
         * lock it already held, i.e. waiting on itself forever.
         *
         * So re-check on every wake and go back to sleep unless the kernel has
         * actually picked us. Running late is recoverable; running as the wrong
         * thread is not. */
        for (;;)
        {
            xSemaphoreTake((SemaphoreHandle_t)self->gate, portMAX_DELAY);
            if (rb_thread_running_context() == (void *)self)
                break;
        }

        /* Resumed, and selected. Whoever gave that gate meant us to be running,
         * and the kernel's own notion is set by whichever core did the giving --
         * which is not necessarily the core we land on. Re-assert it. */
        rb_thread_set_running(self->thread);
    }
}
