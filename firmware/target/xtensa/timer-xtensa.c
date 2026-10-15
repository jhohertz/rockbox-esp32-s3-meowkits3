/***************************************************************************
 * ESP32-S3 kernel tick, on ESP-IDF's general purpose timer.
 *
 * The bare-metal version armed the core timer (CCOUNT/CCOMPARE0) and installed
 * its own interrupt slot. Neither is available to a guest of FreeRTOS: IDF owns
 * VECBASE and the interrupt allocator, and the core timer's compare interrupt
 * is what the RTOS tick itself runs on. So the Rockbox tick is now one more
 * gptimer alarm, and IDF's interrupt plumbing delivers it.
 *
 * What the callback may and may not do follows from how Rockbox schedules:
 * threads switch at blocking primitives (queue_post, mutex_wait, sleep,
 * yield), never from the tick, so switch_thread() is not reachable from here.
 * That matters on this core: the context switch in firmware/asm/xtensa/thread.c
 * lands in the new thread by returning through the saved a0, which only works
 * from thread context. A switch attempted inside an IDF interrupt frame would
 * return through the wrong exception frame.
 *
 * call_tick_tasks() advances current_tick and runs the registered tasks
 * (button polling, backlight timeout, powermgmt), so those tasks must not
 * block. That is the same contract every other native target has.
 *
 * HZ comes from the target config; tick_start() is called by init_tick() with
 * 1000/HZ milliseconds.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "tick.h"
#include "timer.h"
#include "panic.h"

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

/* 1 MHz is independent of both the CPU frequency and the APB divider, so a
 * cpu_frequency change cannot move the tick. 32 bits at 1 MHz is ~71 minutes,
 * far more than the longest interval we ask for. */
#define TICK_TIMER_HZ       1000000u

static const char TAG[] = "rbtick";

static gptimer_handle_t tick_timer;

static bool IRAM_ATTR tick_alarm(gptimer_handle_t timer,
                                 const gptimer_alarm_event_data_t *edata,
                                 void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    /* Only the clock. The registered tick functions are deliberately not run
     * here, and the reason is the same one that made the bare-metal branch run
     * them from a thread.
     *
     * They are ordinary C: button polling does I2C, and I2C on this target ends
     * in a semaphore wait. A Rockbox semaphore wait switches threads through
     * store_context()/load_context(), and a context switch cannot happen inside
     * an exception frame on this core -- the esp32s3 core config has no REGB
     * option, so the register window cannot be rotated from a handler. Doing it
     * anyway faults inside __xtensa_libgcc_window_spill as a double exception,
     * with PS showing the handler's interrupt level and a return address that is
     * really a data address. apps/main.c's tick thread runs them instead.
     */
    current_tick++;

    /* Nothing here wakes a FreeRTOS task, so there is nothing to reschedule
     * over. Returning false keeps IDF from ending the ISR early. */
    return false;
}

void tick_start(unsigned int interval_in_ms)
{
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TICK_TIMER_HZ,
    };

    if (gptimer_new_timer(&timer_config, &tick_timer) != ESP_OK)
        panicf("tick_start: no general purpose timer available");

    gptimer_event_callbacks_t cbs = {
        .on_alarm = tick_alarm,
    };

    /* Registering callbacks is what puts the timer in interrupt mode; IDF
     * requires this before gptimer_enable(). */
    if (gptimer_register_event_callbacks(tick_timer, &cbs, NULL) != ESP_OK)
        panicf("tick_start: could not claim the timer interrupt");

    if (gptimer_enable(tick_timer) != ESP_OK)
        panicf("tick_start: could not enable the tick timer");

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = (uint64_t)interval_in_ms * (TICK_TIMER_HZ / 1000u),
        .flags.auto_reload_on_alarm = true,
    };

    if (gptimer_set_alarm_action(tick_timer, &alarm_config) != ESP_OK)
        panicf("tick_start: could not arm the tick alarm");

    if (gptimer_start(tick_timer) != ESP_OK)
        panicf("tick_start: could not start the tick timer");

    ESP_LOGI(TAG, "tick: %u ms (%d Hz) on a gptimer alarm",
             interval_in_ms, HZ);
}

/*---------------------------------------------------------------------------
 * The rbcodec timer: firmware/timer.c's target half.
 *
 * This is a different thing from the kernel tick, and it wants a different
 * thing from the hardware. The tick is a fixed 1/HZ metronome that drives
 * buttons, backlight and power management. This one is registered by the DSP
 * and the codec (dsp_tick, pcm pacing, tdspeed) with a period in TIMER_FREQ
 * cycles, can be re-perioded at runtime, and is stopped when the user timer is
 * unregistered. It therefore needs its own timer rather than a second alarm on
 * the tick: the tick's period must not move.
 *
 * TIMER_FREQ on this target is the 1 MHz timer clock (firmware/include/
 * xtensa.h), which is exactly the resolution the gptimer is configured for, so
 * a Rockbox "cycle" is one tick of the counter and no scaling appears here.
 * The static assertion is what keeps that true if either side changes.
 *---------------------------------------------------------------------------
 */

static gptimer_handle_t codec_timer;
static bool codec_timer_running;

static bool IRAM_ATTR codec_alarm(gptimer_handle_t timer,
                                  const gptimer_alarm_event_data_t *edata,
                                  void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;

    /* pfn_timer is the callback firmware/timer.c was given. Every other native
     * target calls it from the timer interrupt, and the DSP code it reaches is
     * written for that: it must not block, and it must not expect to be able
     * to switch Rockbox threads (same reason as the tick above). */
    if (pfn_timer != NULL)
        pfn_timer();

    return false;
}

bool timer_set(long cycles, bool start)
{
    /* `start` says this is the initial registration rather than a re-period.
     * It makes no difference here: setting the alarm action on a running
     * gptimer re-arms it, and on a stopped one it just programs it, which is
     * what timer_start() then starts. */
    (void)start;

    if (cycles <= 0)
        return false;

    if (codec_timer == NULL)
    {
        gptimer_config_t timer_config = {
            .clk_src = GPTIMER_CLK_SRC_DEFAULT,
            .direction = GPTIMER_COUNT_UP,
            .resolution_hz = TIMER_FREQ,
        };

        if (gptimer_new_timer(&timer_config, &codec_timer) != ESP_OK)
            return false;       /* no timer left; caller falls back */

        gptimer_event_callbacks_t cbs = {
            .on_alarm = codec_alarm,
        };

        if (gptimer_register_event_callbacks(codec_timer, &cbs, NULL) != ESP_OK)
            return false;

        if (gptimer_enable(codec_timer) != ESP_OK)
            return false;
    }

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = (uint64_t)cycles,
        .flags.auto_reload_on_alarm = true,
    };

    return gptimer_set_alarm_action(codec_timer, &alarm_config) == ESP_OK;
}

bool timer_start(void)
{
    if (codec_timer == NULL || codec_timer_running)
        return codec_timer != NULL;

    if (gptimer_start(codec_timer) != ESP_OK)
        return false;

    codec_timer_running = true;
    return true;
}

void timer_stop(void)
{
    if (codec_timer != NULL && codec_timer_running)
    {
        gptimer_stop(codec_timer);
        codec_timer_running = false;
    }
}
