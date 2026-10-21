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
#include "esp_system.h"   /* esp_reset_reason */

extern void scan_trace_early_dump(void);
extern void xtensa_paniclog_dump(void);
extern void xtensa_breadcrumb(const char *tag, unsigned long a, unsigned long b);
extern int xtensa_breadcrumb_window(char *buf, int bufsize, int start, int count);
extern void xtensa_paniclog_set_reset(unsigned reason);

/* Rockbox's own top level. Declared rather than included: apps/main.c has no
 * header, and pulling its headers in here would drag the whole apps tree into
 * the one file whose job is to stay small. It never returns. */
extern int main(void);

/* Implemented in firmware/target/xtensa/thread-idf.c. Rockbox's thread 0 must be
 * bound to this task specifically; binding it to "whichever task is current at
 * the first context switch" previously attached thread 0 to the scroll thread's
 * task, so main and scroll shared one gate. */
extern void rb_thread_set_thread0_task(void *handle);

/* Rockbox's tick seam divides the kernel tick count down to Rockbox's HZ. It has
 * to know the rate the kernel was really configured with, and reading
 * configTICK_RATE_HZ from the Rockbox side does not give that: those objects are
 * built by the Rockbox makefiles against IDF's generated headers, which are not
 * reliably regenerated when sdkconfig changes. Measured: with the kernel at
 * 1000 Hz the macro still read 100 on the Rockbox side, the division folded to
 * 1, and current_tick ran ten times fast -- which surfaced as a screen timeout
 * that fired almost immediately and as every Rockbox timeout expiring early
 * enough to keep the codec from ever starting a transfer.
 *
 * This file is compiled by the IDF build, so the value baked in here cannot
 * disagree with the kernel that actually gets linked. */
const unsigned int rb_kernel_tick_hz = configTICK_RATE_HZ;

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

/* TEMPORARY diagnostic: take the level-4 cache-error path out of the picture.
 *
 * IDF's esp_cache_err_int_init() routes three sources to INUM 25
 * (ETS_MEMACCESS_ERR_INUM): the illegal-access source plus one access-error
 * source per core. Level 4 is not serviceable from C -- xtensa_vectors.S jumps
 * straight into the panic handler -- so the report carries a cause string
 * decoded from the cache status register and a backtrace of whatever the core
 * happened to be doing when the interrupt arrived. Measured here, both cores
 * were in waiti and the fault address registers stayed zero for this cause.
 *
 * Clearing INTENABLE bit 25 removes that path but leaves the *sources* enabled,
 * so the status bits still latch and the core-1 poller still sees them. If the
 * fault is a CPU access, the core takes its own MemmoryManagement exception
 * (EXCCAUSE 7) instead, which the exception handler reports with a real EPC1
 * and EXCVADDR. If nothing is reported and the system keeps running, the
 * interrupt was the only consequence of the fault at all. */
static void mask_cache_error_int(void)
{
    uint32_t ie;

    asm volatile ("rsr %0, intenable" : "=r"(ie));
    ie &= ~(1u << 25);                  /* ETS_MEMACCESS_ERR_INUM */
    asm volatile ("wsr %0, intenable" :: "r"(ie));
}

static void rockbox_task(void *arg)
{
    (void)arg;

    /* Name this task as Rockbox's thread 0 while we are certainly running on it,
     * before any thread exists to switch out. */
    rb_thread_set_thread0_task((void *)xTaskGetCurrentTaskHandle());

    fill_stack_canary();
    mask_cache_error_int();

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
    /* IDF's console is USB-Serial-JTAG and the pads move to the OTG core about 865 ms
     * after boot -- right where app_main starts. newlib printf buffers, so anything
     * printed here was flushed after the transport was gone and never reached the host
     * (measured: the capture ends at the esp_psram line, 865 ms, and none of app_main's
     * own output arrives). Unbuffered stdout makes the early dumps land.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Print what the previous boot was doing when it died, before this
     * one has a chance to overwrite the ring. */
    scan_trace_early_dump();
    xtensa_paniclog_dump();
    /* The RTC-slow ring survives the reset that clears internal DRAM, and it is where the
     * audio counters are recorded (usbAud: isochronous frames received and the endpoint's
     * remaining transfer count; audR/audA: OUT-event storm and sink state). Console output
     * over USB-Serial-JTAG stops the moment the app hands the pads to the OTG core, so this
     * has to be printed here, first, rather than over the CDC transport later -- and reading
     * it over CDC requires writing to the console, which is what wedges the device.
     */
    {
        extern int xtensa_breadcrumb_rtc_window(char *buf, int bufsize, int start, int count);
        char rtc[256];
        int start;
        int any = 0;
        for (start = 0; start < 32; start += 8)
        {
            int n = xtensa_breadcrumb_rtc_window(rtc, sizeof rtc, start, 8);
            if (n <= 0)
                break;
            if (!any)
            {
                printf("rtc breadcrumbs:\n");
                any = 1;
            }
            printf("%s\n", rtc);
        }
        if (!any)
            printf("rtc breadcrumbs: none\n");
    }
    /* No ring dump here: xtensa_paniclog_dump() above already prints the retained
     * breadcrumbs ("panic breadcrumbs (40):") as the first thing app_main does, and did so
     * before this was added. A second dump doubled the output on the console. */
    /* The reset reason lives in the RTC domain, so it survives the reset that clears
     * internal DRAM -- which the breadcrumb trail does not. After a crash on this board
     * the trail came back holding only the new boot's entries, so the trail cannot say
     * what killed the previous boot; the reset reason can. It also goes into the trail,
     * so it is readable over the CDC transport once the device has re-enumerated,
     * without needing the boot console.
     *
     * Telling ESP_RST_PANIC from ESP_RST_TASK_WDT or ESP_RST_BROWNOUT is the first
     * question about the unplug crash: a hang that ends in a watchdog reset is a
     * different bug from one that ends in a panic, and they point at different code. */
    {
        esp_reset_reason_t why = esp_reset_reason();

        printf("reset reason: %d (%s)\n", (int)why,
               why == ESP_RST_POWERON ? "power-on"
               : why == ESP_RST_SW ? "software esp_restart"
               : why == ESP_RST_PANIC ? "panic"
               : why == ESP_RST_INT_WDT ? "interrupt watchdog"
               : why == ESP_RST_TASK_WDT ? "task watchdog"
               : why == ESP_RST_WDT ? "watchdog"
               : why == ESP_RST_BROWNOUT ? "brownout"
               : why == ESP_RST_USB ? "usb peripheral"
               : why == ESP_RST_UNKNOWN ? "unknown" : "other");
        xtensa_breadcrumb("rstR", (unsigned long)why, 0);
        xtensa_paniclog_set_reset((unsigned)why);
    }
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
