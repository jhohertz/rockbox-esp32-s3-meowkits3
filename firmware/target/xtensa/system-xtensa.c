/***************************************************************************
 * ESP32-S3 system level.
 *
 * Under ESP-IDF there is very little for this file to do, and that is the
 * point. IDF's startup has already brought up the cache and the flash MMU,
 * armed the PSRAM mapping, chosen the core clock, gated the peripherals it
 * needs, installed its own interrupt vector table and started FreeRTOS. Every
 * one of those was, on the bare-metal branch, something Rockbox did by hand -
 * and each one is a thing two owners cannot share.
 *
 * The previous version of this file did three of them, and all three are now
 * removed deliberately rather than left as harmless leftovers:
 *
 *   - `wsr vecbase` with Rockbox's own table, plus zeroing intenable and
 *     clearing every pending interrupt. That does not "take over the vectors"
 *     from a guest's position; it uninstalls FreeRTOS's tick, the GDMA
 *     completions the audio path depends on, and the gptimer alarm that drives
 *     the Rockbox tick itself.
 *   - `cpu_clock_up()` and `set_cpu_frequency()`, which wrote CPUPERIOD_SEL,
 *     SYSCLK_CONF and the LDO slave count. IDF owns the clock tree, and the
 *     analog bias (dbias) that has to move with it is something only IDF can
 *     do - it comes from the ROM's PVT calibration through REGI2C. Writing the
 *     LDO slaves by hand is the sort of change that browns out the SoC rather
 *     than failing visibly.
 *   - peripheral clock gating written into SYSTEM_PERIPHS_CLK_EN by address.
 *     IDF's drivers enable their own clocks; a blanket write from here can
 *     only duplicate or undo that.
 *
 * Rockbox's adjustable-CPU-FREQ machinery (cpu_boost) is switched off in the
 * target config for the same reason: it exists to call set_cpu_frequency().
 * If power management is wanted later it should go through IDF's esp_pm DFS,
 * not around it.
 ****************************************************************************/

#include "config.h"

#include <stdlib.h>     /* abort(): esp_cache.h used to drag it in, cache_hal.h does not */

#include "system.h"

#include "sdkconfig.h"

/* cpu_frequency is declared in system.h and defined by firmware/kernel/system.c
 * for every target; this file only has to put the right number in it. */

/* CPU_FREQ lives in the target config and CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ in
 * the IDF project, and only the second one is real. Drift between them shows
 * up as every udelay() being wrong by a fixed factor, which is a miserable
 * thing to chase, so it is a compile error instead. */
_Static_assert(CPU_FREQ ==
               (long)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000L,
               "CPU_FREQ in meowkits3.h disagrees with CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ");

void system_init(void)
{
    /* udelay() in system-target.h counts core cycles, so it needs the frequency
     * the core is actually on. IDF picked it in sdkconfig; reading it back from
     * the hardware would mean re-deriving IDF's clock tree, which is what this
     * file no longer does. If IDF's dynamic frequency scaling is ever enabled,
     * this has to become esp_clk_cpu_freq() and udelay() has to stop caching
     * it. */
    cpu_frequency = CPU_FREQ;

    /* Nothing else. Anything added here should be checked against the list at
     * the top of the file: if IDF already owns it, it does not belong here. */
}

void system_reboot(void)
{
    extern void esp_restart(void);

    esp_restart();
}

/*
 * Reached from the panic paths after the message has been printed. IDF's abort
 * is the honest answer: it runs the registered panic handler, which prints a
 * backtrace and either halts or resets depending on CONFIG_ESP_SYSTEM_PANIC_*.
 * The bare-metal version spun on `waiti 0` forever, which under IDF just waits
 * for the RTC watchdog to reset the chip without saying why.
 */
void system_exception_wait(void)
{
    abort();

    while (1)
        ;
}
