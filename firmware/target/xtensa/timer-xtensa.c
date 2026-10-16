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

#include <soc/extmem_struct.h>
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
 * CONFIG_FREERTOS_HZ=100 and CONFIG_FREERTOS_USE_TICK_HOOK=y and the clock is
 * taken from the tick the kernel is already taking. No second timer, no group
 * to contend with, and the clock cannot drift from the scheduler that decides
 * which thread runs.
 *
 * The hook runs in the tick ISR, so the rule that governed the alarm still
 * applies: advance the clock and nothing else. The registered tick functions
 * are ordinary C -- button polling does I2C, which ends in a semaphore wait,
 * and a Rockbox semaphore wait switches threads, which an exception frame on
 * a core with no REGB option cannot do. apps/main.c's tick thread runs them.
 */
void tick_start(unsigned int interval_in_ms)
{
    /* HZ is what every tick consumer in Rockbox already assumes; all this
     * function can legitimately do is confirm the kernel agrees with it. */
    if (interval_in_ms != 1000u / HZ)
        panicf("tick_start: HZ %d does not agree with a %u ms tick", HZ,
               interval_in_ms);

    ESP_LOGI(TAG, "tick: %u ms (%d Hz) from the FreeRTOS tick hook",
             interval_in_ms, HZ);
}

/* SMP FreeRTOS ticks on both cores; Rockbox runs on one, so counting on both
 * would double the clock. */
extern unsigned int thread_bookkeeping_check(void);
static unsigned int bk_bad;

void vApplicationTickHook(void)
{
    /* Rockbox's clock has to follow the kernel's, not the core's, and it has to
     * do so on EVERY tick, unconditionally, before anything else in this hook
     * can bail out.
     *
     * In SMP FreeRTOS the tick hook runs on whichever core won the tick
     * increment for that period -- measured on this board, always core 1, even
     * though core 0's tick ISR itself is running fine (IDF's per-core served
     * counter advances on both). Gating the bump on "am I core 0" therefore left
     * current_tick at zero forever: the tick was healthy, no thread ever timed
     * out, and every Rockbox thread blocked and stayed blocked. That is what
     * made a live, ticking core look idle.
     *
     * xTaskGetTickCount() is a single global counter advanced exactly once per
     * tick regardless of which core delivers it, so mirroring it is correct
     * whichever core gets the hook.
     *
     * The two clocks do not run at the same rate, and they must not be forced to
     * agree. Rockbox's HZ is 100 and is not a per-target knob (kernel/include/
     * tick.h), but ESP-IDF defaults FreeRTOS to 1000 Hz and a great many IDF
     * components -- the wireless stack especially -- are written against that
     * granularity. Pinning configTICK_RATE_HZ to 100 to suit Rockbox would bend
     * the operating system around one application and would come back the moment
     * Bluetooth is enabled. So the OS keeps its native rate and this seam divides
     * down to Rockbox's.
     *
     * The divisor is derived from the two macros rather than written as a literal
     * 10, and the #error makes a non-integral ratio a build failure instead of a
     * clock that quietly runs at the wrong speed. */
#if (configTICK_RATE_HZ % HZ) != 0
#error "configTICK_RATE_HZ must be an integer multiple of Rockbox HZ"
#endif
    current_tick = (long)(xTaskGetTickCount() / (configTICK_RATE_HZ / HZ));
    /* TEMPORARY: police core 0's running-thread pointer (see
       thread_bookkeeping_check in thread-common.c). */
    if (!bk_bad)
    {
        extern void scan_trace(int tag, unsigned long a, unsigned long b,
                               unsigned long c);
        static unsigned int bk_n;
        unsigned int bad = thread_bookkeeping_check();

        bk_n++;
        if (bad)
        {
            bk_bad = bad;
            scan_trace(92, bad, (unsigned long)current_tick, bk_n);
        }
    }

#ifdef ROCKBOX_SCAN_TRACE
    {
        static unsigned hook_n;
        hook_n++;

        extern void scan_trace(int tag, unsigned long a, unsigned long b,
                               unsigned long c);
        if ((hook_n & 7u) != 0)
            return;

        unsigned int ie;
        asm volatile ("rsr %0, intenable" : "=r"(ie));
        scan_trace(60, current_tick, esp_cpu_get_core_id(), ie);

        /* The OS tick is a SYSTIMER alarm in periodic mode off the shared
         * unit-1 counter: alarm 0 -> core 0, alarm 1 -> core 1. Sample the
         * source from whichever core is still ticking, so the question
         * "did the counter stop, did the compare stop re-arming, or did the
         * interrupt stop being delivered" is answered by the hardware
         * rather than by inference. */
        if ((hook_n & 63u) == 0)
        {
            /* TEMPORARY: the panic reporter for the current crash is the
               EXTMEM MMU_ENTRY_FAULT status bit, which carries no address. The
               table was verified intact at storage-init time, so re-verify it
               from the other core while the system is running: if it is still
               clean when the fault happens, the faulting address is not in the
               PSRAM window at all. */
            const volatile unsigned int* mmu =
                (const volatile unsigned int*)0x600C5000;
            for (unsigned int i = 16; i < 144; i++)
            {
                unsigned int v = mmu[i];
                if ((v & 0x4000u) || !(v & 0x8000u))
                {
                    scan_trace(94, i, v, current_tick);
                    break;
                }
            }
        }

        {
            /* TEMPORARY: pin the fault in time. The status register latches
               whatever the cache MMU rejected, and the bits are cleared by
               writing 1 back. Sampling it here and clearing it answers two
               questions at once: when relative to boot the first fault
               happened, and whether it is a one-off or keeps re-latching -- a
               recurring fault points at an ongoing operation, a single latch
               points at one bad access whose consequences then unfold. */
            volatile extmem_dev_t* ext =
                (volatile extmem_dev_t*)0x600C4000;    /* DR_REG_EXTMEM_BASE */
            uint32_t ilg = ext->cache_ilg_int_st.val;
            uint32_t c0 = ext->core0_acs_cache_int_st.val;
            uint32_t c1 = ext->core1_acs_cache_int_st.val;

            if (ilg | c0 | c1)
            {
                /* The panic reporter only ever names the illegal-access cause.
                   Sampling the two per-core access-error registers alongside it
                   says which core the hardware blamed, which the report leaves
                   blank ("Core  panic'ed") because it reads them after the
                   reset path has already been entered. */
                scan_trace(96, ilg, c0, current_tick);
                scan_trace(99, c1, hook_n, current_tick);
                ext->cache_ilg_int_st.val = ilg;        /* W1C */
                ext->core0_acs_cache_int_st.val = c0;   /* W1C */
                ext->core1_acs_cache_int_st.val = c1;   /* W1C */
            }
        }

        {
            /* TEMPORARY: the panic path (esp_system/port/soc/esp32s3/
               cache_err_int.c) reports the MMU entry fault cause but never
               reads the two registers that do carry information. The hardware
               latches the faulting virtual address and, alongside it, the
               content of the MMU entry that was consulted plus a code for the
               operation that tripped on it (1 = cpu miss, 2 = preload miss,
               3 = writeback, 6 = external DMA tx, 7 = external DMA rx, with
               bit 3 naming which icache). Reading them from the core that is
               still alive turns "MMU entry fault" into an address. */
            /* IDF 5.x no longer defines the EXTMEM object, so reach the block
               by base address and let the struct type supply the offsets. */
            const volatile uint32_t* extmem =
                (const volatile uint32_t*)0x600C4000;   /* DR_REG_EXTMEM_BASE */
            uint32_t c = extmem[offsetof(extmem_dev_t,
                                         cache_mmu_fault_content) / 4];
            uint32_t v = extmem[offsetof(extmem_dev_t,
                                         cache_mmu_fault_vaddr) / 4];

            if (v || c)
                scan_trace(95, v, c, current_tick);
        }

        if ((hook_n & 63u) == 1)
        {
            static volatile unsigned *const sys =
                (volatile unsigned *)0x60023000;
            scan_trace(61, sys[0x4c / 4],       /* unit1 counter, low half */
                       sys[0x00 / 4],           /* conf: bit24=alarm0, bit23=alarm1 */
                       sys[0x34 / 4]);          /* target0 conf: period + mode */
            scan_trace(62, sys[0x38 / 4],       /* target1 conf (core 1) */
                       sys[0x64 / 4],           /* int_ena  */
                       sys[0x70 / 4]);          /* int_st   */

            /* IDF's SysTickIsrHandler runs, then only calls the tick handler
             * if counter/period has moved past its own per-core count. So a
             * dead tick can mean "the ISR never ran" or "the ISR ran and
             * decided there was nothing to do". Those two look identical from
             * the outside and are fixed differently, so read the bookkeeping
             * directly. Address of this file-static from rockbox.map. */
            {
                static volatile unsigned *const handled =
                    (volatile unsigned *)0x3fc9b52c;
                static unsigned last_h0, stalled, rearmed;
                scan_trace(63, handled[0],      /* core 0's served count */
                           handled[1],          /* core 1's served count */
                           sys[0x4c / 4] / 160000); /* periods elapsed */
                /* If core 0's alarm is armed in the peripheral, its interrupt
                 * is enabled in INTENABLE, and the line never asserts, then
                 * the peripheral's target-0 output is what died. Re-arm it
                 * from here and see whether core 0 starts being served again:
                 * that separates "the alarm state machine stalled" from
                 * "the CPU-side line is dead", and is also the candidate fix. */
                if (handled[0] == last_h0)
                {
                    stalled++;
                    if (stalled == 4)
                    {
                        sys[0x6c / 4] = 1;           /* int_clr: alarm 0 */
                        sys[0x34 / 4] = 0xc0027100;  /* target0: unit1, period, 160000 */
                        sys[0x50 / 4] = 1;           /* comp_load[0]: apply */
                        sys[0x00 / 4] |= (1u << 24); /* target0_work_en */
                        sys[0x64 / 4] |= 1;          /* int_ena alarm 0 */
                        rearmed = 1;
                    }
                }
                else
                {
                    stalled = 0;
                    last_h0 = handled[0];
                }
                scan_trace(66, rearmed, stalled, handled[0]);
                /* The registers the hardware actually compares against. In
                 * period mode these advance on their own; target_val reads 0
                 * and tells you nothing. If alarm 0's real compare is frozen
                 * while alarm 1's runs, the peripheral's state machine for
                 * core 0 is the thing that stopped. */
                scan_trace(67, sys[0x74 / 4], sys[0x78 / 4], sys[0x7c / 4]);
                {
                    extern volatile unsigned rb_late_hits;
                    extern volatile unsigned rb_late_err;
                    extern volatile unsigned rb_probe_ran;
                    scan_trace(64, rb_late_hits,        /* core 0 gptimer ISRs */
                               rb_late_err,            /* 0 == armed cleanly */
                               rb_probe_ran);
                }
            }
        }
    }
#endif
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
