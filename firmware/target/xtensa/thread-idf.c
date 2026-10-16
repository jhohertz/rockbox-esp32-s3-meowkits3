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

/* TEMPORARY bring-up: the OS half of "why is this thread not running".
 *
 * Rockbox marks a thread blocked only when it goes through block_thread(),
 * which sets thread->blocker. A Rockbox thread that calls into IDF and waits on
 * an IDF semaphore, queue or event is Blocked as far as FreeRTOS is concerned
 * and completely unblocked as far as Rockbox is concerned -- so the Rockbox-side
 * readout prints the two identically, and they need opposite fixes. This keeps
 * just enough of a registry, indexed by the same slot the TCB pool uses, to ask
 * FreeRTOS the same question about the same thread.
 *
 * The handles are recorded here rather than recovered from tcb_pool because a
 * slot is reused after vTaskDelete(), and eTaskGetState() on a deleted TCB is
 * not a thing that can be done safely. */
static TaskHandle_t os_task[MAXTHREADS];
static TaskHandle_t os_task0;

/* eTaskGetState() returns an enum with no name table, and the whole point is
 * readability under a serial console at the moment things are going wrong. */
static const char *os_state_name(eTaskState st)
{
    switch (st)
    {
    case eReady:     return "Ready";
    case eRunning:   return "Running";
    case eBlocked:   return "BLOCKED";
    case eSuspended: return "Suspended";
    case eDeleted:   return "Deleted";
    default:         return "?";
    }
}

/* Render every Rockbox-owned task as name=OSstate.
 *
 * A thread reading `run` on the Rockbox side and `BLOCKED` here is sitting
 * inside a driver, not stranded awaiting its gate -- which is the distinction
 * that has been unresolvable so far. */
int rb_thread_os_states(char *out, int sz)
{
    int pos = 0;
    int i;

    if (sz <= 0)
        return 0;
    out[0] = '\0';

    /* Names are read from the live handle rather than cached alongside it.
     * Slots are reused after vTaskDelete(), and a cached const char * survives
     * that, so the first version of this printed one thread name twice and
     * omitted another -- precisely the kind of artefact that reads like a
     * finding. Only the handle is stored, and only while it is valid. */
    /* Slot index and handle printed as well: if one name appears twice it
     * matters enormously whether that is two different slots (two live tasks
     * with the same name, i.e. a leaked task whose Rockbox thread is gone) or
     * the same slot printed twice (this file being wrong). Do not guess which. */
    if (os_task0 != NULL)
        pos += snprintf(out + pos, sz - pos, "t0:%s=%s/%p ",
                        pcTaskGetName(os_task0),
                        os_state_name(eTaskGetState(os_task0)),
                        (void *)os_task0);

    for (i = 0; i < MAXTHREADS && pos < sz - 48; i++)
    {
        if (os_task[i] == NULL)
            continue;
        pos += snprintf(out + pos, sz - pos, "s%d:%s=%s/%p ", i,
                        pcTaskGetName(os_task[i]),
                        os_state_name(eTaskGetState(os_task[i])),
                        (void *)os_task[i]);
    }
    return pos;
}

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
        /* The handle in the registry is now a dangling pointer; drop it before
         * anything can ask its state. */
        if (slot < MAXTHREADS)
            os_task[slot] = NULL;
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

    if (slot < MAXTHREADS)
        os_task[slot] = (TaskHandle_t)ctx->task;
}

/* Thread 0 is not created here: it is the task app_main started Rockbox on. It
   still needs a gate, because switching away from it has to block it like any
   other thread. Adopt the task we are already on. */
void rb_thread_adopt_self(struct regs *ctx)
{
    /* Record the adopted task too: thread 0 is a real Rockbox thread and its OS
     * state matters as much as any other's. */
    os_task0 = xTaskGetCurrentTaskHandle();
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
    {
        xSemaphoreTake((SemaphoreHandle_t)self->gate, portMAX_DELAY);

        /* Resumed. Whoever gave that gate meant us to be running, and the
         * kernel's own notion is set by whichever core did the giving -- which
         * is not necessarily the core we land on. Re-assert it. */
        rb_thread_set_running(self->thread);
    }
}
