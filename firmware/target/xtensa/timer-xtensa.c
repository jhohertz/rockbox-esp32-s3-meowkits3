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

#include <esp_cpu.h>
#include "config.h"
#include "system.h"
#include "tick.h"
#include "timer.h"

/* FreeRTOS.h has to precede task.h, and both come after the Rockbox headers so
 * that config.h is what decides the target's types. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* The Rockbox clock comes from FreeRTOS's own tick, not from a timer of our
 * own.
 *
 * It used to have its own gptimer, and that was the last piece of IDF
 * avoidance in this path. Measured, it was not solvable as written: the OS
 * tick is itself a gptimer alarm, and a second gptimer allocated with no group
 * preference shares that group's single interrupt line. The signature was
 * Rockbox's current_tick stopping at 46 while INTENABLE was unchanged
 * (0x1300132e) and the counter was still counting -- i.e. the interrupt was
 * armed but never delivered -- with FreeRTOS's own tick count going quiet at
 * the same moment. Two consumers of one interrupt line, going silent together.
 *
 * Rockbox's HZ is 100 and IDF's tick rate is configurable, so sdkconfig sets
 * CONFIG_FREERTOS_HZ and CONFIG_FREERTOS_USE_TICK_HOOK=y and the clock is taken from the
 * tick the kernel is already taking -- no second timer, no group to contend with, and the
 * clock cannot drift from the scheduler that decides which thread runs. The kernel rate is
 * not Rockbox's: it stays at IDF's granularity and the seam below divides it down, so the
 * only requirement is that it be an exact multiple of HZ.
 *
 * The hook runs in the tick ISR, so the rule that governed the alarm still
 * applies: advance the clock and nothing else. The registered tick functions
 * are ordinary C -- button polling does I2C, which ends in a semaphore wait,
 * and a Rockbox semaphore wait switches threads, which an exception frame on
 * a core with no REGB option cannot do. apps/main.c's tick thread runs them.
 */
/* Authoritative kernel tick rate. Defined in idf-app/main/rockbox_app.c, which
 * is compiled by the IDF build, so its configTICK_RATE_HZ is the rate the kernel
 * was actually configured with -- unlike this translation unit, whose view of
 * that macro comes from IDF headers the Rockbox makefiles do not guarantee to
 * regenerate. */
extern const unsigned int rb_kernel_tick_hz;

void tick_start(unsigned int interval_in_ms)
{
    /* The seam has to divide an exact number of times or Rockbox's clock drifts
     * by a whole factor and nothing else in the tree can explain it. */
    if (rb_kernel_tick_hz < HZ || (rb_kernel_tick_hz % HZ) != 0)
        panicf("tick_start: kernel %u Hz is not a multiple of Rockbox HZ %d",
               rb_kernel_tick_hz, HZ);

    /* HZ is what every tick consumer in Rockbox already assumes; all this
     * function can legitimately do is confirm the kernel agrees with it. */
    if (interval_in_ms != 1000u / HZ)
        panicf("tick_start: HZ %d does not agree with a %u ms tick", HZ,
               interval_in_ms);

    ESP_LOGI(TAG, "tick: kernel %u Hz -> rockbox %d Hz (divide by %u, %u ms)",
             rb_kernel_tick_hz, HZ, rb_kernel_tick_hz / HZ, interval_in_ms);
}

/* The kernel tick is the only clock this target has; Rockbox mirrors it rather
 * than arming its own timer. */

void vApplicationTickHook(void)
{
    /* Rockbox's clock has to follow the kernel's, and it has to do so on EVERY
     * tick, unconditionally, before anything else in this hook can bail out.
     * The symptom of gating it on anything was a tick that looked healthy while
     * current_tick stayed at zero: no thread ever timed out, and every Rockbox
     * thread blocked and stayed blocked. That is what made a live, ticking core
     * look idle.
     *
     * xTaskGetTickCount() is a single global counter advanced exactly once per
     * tick, so mirroring it is correct whichever context delivers the hook. When
     * this project ran on the SMP kernel the hook arrived on core 1 rather than
     * core 0 even though core 0's tick ISR was fine, which is what first made
     * the unconditional mirror necessary; the kernel is not SMP now, but the
     * requirement does not depend on that.
     *
     * The two clocks do not run at the same rate, and they must not be forced to
     * agree. Rockbox's HZ is 100 and is not a per-target knob (kernel/include/
     * tick.h), and this build sets configTICK_RATE_HZ to 1000 -- note that IDF's
     * own default is 100 (components/freertos/Kconfig:37), so 1000 is a choice
     * made here rather than something inherited, and the claim that IDF
     * components need it is being tested separately. So the OS keeps its rate
     * and this seam divides down to Rockbox's.
     *
     * The divisor must come from the kernel that is actually running, NOT from
     * configTICK_RATE_HZ as seen from this translation unit. Rockbox's objects
     * are compiled by the Rockbox makefiles against IDF's generated headers, and
     * those are not reliably regenerated when sdkconfig changes: with the rate
     * raised to 1000, the macro here still read 100, the division folded to 1,
     * and current_tick ran ten times fast. A compile-time #error cannot catch
     * that because 100 % 100 == 0 is perfectly valid -- the disagreement is
     * between two build systems, not within one.
     *
     * rb_kernel_tick_hz is defined in idf-app/main/rockbox_app.c, which the IDF
     * build compiles, so its configTICK_RATE_HZ is by construction the rate the
     * kernel was actually configured with. tick_start() checks the seam and says
     * what it found, because this cost a long detour. */
    current_tick = (long)(xTaskGetTickCount() / (rb_kernel_tick_hz / HZ));
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
