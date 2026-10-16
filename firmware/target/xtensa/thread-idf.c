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

/* The body every Rockbox thread runs on: wait for the scheduler to hand over
 * the gate, run the thread's function, then exit through Rockbox's own
 * thread_exit() so the slot is torn down by the code that knows how. */
/* rb_thread_ref is declared in asm/xtensa/thread.h: it carries what the task
   needs by value, so this file never has to know the kernel's thread_entry
   layout. thread-internal.h is not on the target include path, and reaching for
   it would couple the OS binding to kernel internals. */

static void rb_task_body(void *arg)
{
    struct rb_thread_ref *ref = (struct rb_thread_ref *)arg;
    struct regs *ctx = ref->ctx;
    void (*fn)(void) = (void (*)(void))(uintptr_t)ctx->start;

    xSemaphoreTake((SemaphoreHandle_t)ctx->gate, portMAX_DELAY);

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
    printf("task_create: %s slot=%u stack=%p size=%u\n",
           ref->name ? ref->name : "?", slot, ref->stack, ref->stack_size);

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
       xTaskCreateStatic is only a macro for xTaskCreateStaticPinnedToCore with
       tskNO_AFFINITY, so unaffinitied tasks let SMP FreeRTOS run two Rockbox
       threads at the same moment on two cores -- measured as CPU 0 running
       "tick" while CPU 1 ran "scroll", with core 1's idle task starved into the
       task watchdog. Rockbox's kernel is single-core (NUM_CORES is 1 here), the
       gate handoff assumes the outgoing and incoming thread are on the same
       core, and switch_thread() holds no cross-core lock. Pinning also frees
       core 1's idle task, which is what the watchdog was actually complaining
       about. */
    printf("task_create: gate ok, creating task\n");
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

    printf("task_create: task=%p\n", ctx->task);

    /* So a switch to this thread can find its own gate from the task handle. */
    vTaskSetThreadLocalStoragePointer((TaskHandle_t)ctx->task, 0, ctx);
    printf("task_create: done\n");
}

/* Thread 0 is not created here: it is the task app_main started Rockbox on. It
   still needs a gate, because switching away from it has to block it like any
   other thread. Adopt the task we are already on. */
void rb_thread_adopt_self(struct regs *ctx)
{
    if (ctx->gate != NULL)
        return;

    ctx->gate = (void *)xSemaphoreCreateBinary();
    ctx->task = (void *)xTaskGetCurrentTaskHandle();

    if (ctx->gate == NULL)
        panicf("thread gate: no memory for thread 0");

    vTaskSetThreadLocalStoragePointer(NULL, 0, ctx);
}

void rb_thread_handoff(struct regs *in)
{
    struct regs *self = (struct regs *)pvTaskGetThreadLocalStoragePointer(NULL, 0);

    /* Give first, then block. Taking our own gate first would let the incoming
       task run only to block again on its. */
    xSemaphoreGive((SemaphoreHandle_t)in->gate);

    if (self != NULL && self != in)
        xSemaphoreTake((SemaphoreHandle_t)self->gate, portMAX_DELAY);
}
