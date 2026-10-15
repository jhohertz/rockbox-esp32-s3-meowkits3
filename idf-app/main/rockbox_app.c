/*
 * rockbox_app.c -- the seam between ESP-IDF's startup and Rockbox.
 *
 * IDF's entry chain is call_start_cpu0 -> esp_startup_start_app -> app_main.
 * IDF does not define main() at all, so Rockbox's apps/main.c main() does not
 * collide with anything; it simply is not called unless app_main() calls it.
 *
 * Rockbox is not run on app_main's own task. That task is sized by
 * CONFIG_ESP_MAIN_TASK_STACK_SIZE (a few KB, enough to start the scheduler) and
 * IDF expects it to finish. Rockbox's main() is the top of its own world: it
 * initialises the UI, the filesystem and the theme engine, and then runs
 * Rockbox's cooperative scheduler for the life of the product. So it gets a
 * task of its own, with a stack sized for that.
 *
 * The task is pinned to core 0. Rockbox's kernel is single-core in practice --
 * it has a CURRENT_CORE and one core_entry -- and its threads are switched
 * inside this one FreeRTOS task by firmware/asm/xtensa/thread.c, not by
 * FreeRTOS. Letting the RTOS migrate the task between cores would leave
 * Rockbox looking at a core_entry that belongs to the core it left.
 *
 * FreeRTOS may still preempt this task (its own timer and ipc tasks do), which
 * is fine: FreeRTOS saves and restores the whole context, and Rockbox's
 * context switch only ever runs from thread context, never from an ISR.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* Rockbox's own top level. Declared rather than included: apps/main.c has no
 * header, and pulling its headers in here would drag the whole apps tree into
 * the one file whose job is to stay small. It never returns. */
extern int main(void);

/* Thread 0's stack, defined in firmware/target/xtensa/memory-xtensa.c. It is
 * created here rather than left to FreeRTOS because Rockbox's kernel reads
 * stackbegin/stackend as linker symbols and uses them as thread 0's stack
 * bounds -- for stack usage reporting, and to place its first thread. If the
 * task's stack were malloc'd by xTaskCreate, those symbols would describe a
 * region nothing was actually running on. */
extern unsigned char rockbox_stack[];

/* Rockbox's thread 0 stack, in bytes. memory-xtensa.h defines the array; this
 * side only needs the size to agree, and xTaskCreateStatic fails loudly (it
 * asserts) if the buffer is smaller than the word count asked for. */
#define ROCKBOX_STACK_BYTES 0x8000
#define ROCKBOX_STACK_WORDS (ROCKBOX_STACK_BYTES / sizeof(StackType_t))

/* Above idle, below IDF's ipc task. Equal to the FreeRTOS timer daemon, which
 * is time-sliced against us rather than blocking us. */
#define ROCKBOX_TASK_PRIORITY  (tskIDLE_PRIORITY + 1)

static const char TAG[] = "rockbox";

/* Rockbox's scheduler checks a canary word at the bottom of every thread
 * stack, and for thread 0 the kernel does not write it: on every native target
 * crt0 fills the main stack with DEADBEEF before C starts (see
 * firmware/target/mips/ingenic_x1000/crt0.S, which does exactly this). There is
 * no crt0 on this path -- IDF owns startup -- so the check finds whatever was
 * left there and switch_thread() reports a stack overflow on a thread that has
 * not used a byte of its stack. The word it actually found was 0xa5a5a5a5:
 * FreeRTOS's prvInitialiseNewTask() memsets the whole stack buffer with
 * tskSTACK_FILL_BYTE (FreeRTOS-Kernel/tasks.c:1044). That settles the order as
 * well -- the canary has to be written after the task is created, or FreeRTOS
 * overwrites it. Writing it at the bottom of a buffer the task is already
 * running in is safe: the task's own frame sits at the top of it. */
static void fill_stack_canary(void)
{
    extern unsigned char stackbegin[];
    uint32_t *w = (uint32_t *)stackbegin;

    w[0] = 0xdeadbeefu;
}

static void rockbox_task(void *arg)
{
    (void)arg;

    fill_stack_canary();

    ESP_LOGI(TAG, "entering rockbox main()");
    main();

    /* main() is declared noreturn in Rockbox; reaching here means it returned
     * after a shutdown that did not power off. Say so instead of letting the
     * task exit quietly, which would leave the scheduler with nothing to run
     * and the screen frozen on whatever was last drawn. */
    ESP_LOGE(TAG, "rockbox main() returned");
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void)
{
    /* The task control block is ours too. pvPortMalloc would put it in the
     * internal heap, which works, but a static buffer means the one thing
     * Rockbox is guaranteed to have is its own task: if the heap is exhausted
     * at startup the failure is a missing symbol at link time rather than a
     * silent no-show. */
    static StaticTask_t rockbox_tcb;

    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        rockbox_task, "rockbox", ROCKBOX_STACK_WORDS, NULL,
        ROCKBOX_TASK_PRIORITY, (StackType_t *)rockbox_stack, &rockbox_tcb, 0);

    if (handle == NULL)
        ESP_LOGE(TAG, "could not create the rockbox task (%d KB stack)",
                 ROCKBOX_STACK_BYTES / 1024);
}
