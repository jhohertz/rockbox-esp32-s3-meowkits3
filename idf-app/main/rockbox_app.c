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

#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

extern void scan_trace_early_dump(void);

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

/* Diagnostic: allocate an interrupt on core 0 *after* the FreeRTOS tick on
 * that core has gone quiet, and count its callbacks. If a freshly allocated
 * source fires, core 0 still takes interrupts and the systimer's binding
 * specifically is what broke. If this stays at zero too, core 0's interrupt
 * delivery as a whole is dead -- two very different bugs. */
volatile unsigned rb_late_hits;
volatile unsigned rb_late_err;
volatile unsigned rb_probe_ran;

static bool IRAM_ATTR rb_late_cb(gptimer_handle_t timer,
                                 const gptimer_alarm_event_data_t *edata,
                                 void *user_ctx)
{
    (void)timer; (void)edata; (void)user_ctx;
    rb_late_hits++;
    return false;
}

static void rb_late_probe(void *arg)
{
    gptimer_handle_t tm;
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    gptimer_event_callbacks_t cbs = { .on_alarm = rb_late_cb };
    gptimer_alarm_config_t ac = {
        .alarm_count = 100000,        /* 100 ms */
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };

    (void)arg;
    rb_probe_ran = 1;

    /* Everything that matters happens before the first blocking call. This
     * task is pinned to a core that, once its tick dies, is never preempted
     * again -- so anything behind a vTaskDelay there simply never runs. That
     * is what made the first version of this probe worthless. */
    rb_late_err = gptimer_new_timer(&cfg, &tm);
    if (rb_late_err == ESP_OK)
        rb_late_err = gptimer_register_event_callbacks(tm, &cbs, NULL);
    if (rb_late_err == ESP_OK)
        rb_late_err = gptimer_enable(tm);
    if (rb_late_err == ESP_OK)
        rb_late_err = gptimer_set_alarm_action(tm, &ac);
    if (rb_late_err == ESP_OK)
        rb_late_err = gptimer_start(tm);

    /* Then get out of the way. The alarm callback is an ISR; it does not need
     * this task to run again, so Rockbox keeps the core and behaves normally
     * while the counter tells us whether core 0 is being served. */
    vTaskDelay(portMAX_DELAY);
}

void app_main(void)
{
    /* Print what the previous boot was doing when it died, before this
     * one has a chance to overwrite the ring. */
    scan_trace_early_dump();
    /* The task control block is ours too. pvPortMalloc would put it in the
     * internal heap, which works, but a static buffer means the one thing
     * Rockbox is guaranteed to have is its own task: if the heap is exhausted
     * at startup the failure is a missing symbol at link time rather than a
     * silent no-show. */
    static StaticTask_t rockbox_tcb;

    xTaskCreatePinnedToCore(rb_late_probe, "lateprobe", 4096, NULL, 2, NULL, 0);

    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        rockbox_task, "rockbox", ROCKBOX_STACK_WORDS, NULL,
        ROCKBOX_TASK_PRIORITY, (StackType_t *)rockbox_stack, &rockbox_tcb, 0);

    if (handle == NULL)
        ESP_LOGE(TAG, "could not create the rockbox task (%d KB stack)",
                 ROCKBOX_STACK_BYTES / 1024);

}
