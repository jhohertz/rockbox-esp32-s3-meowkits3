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
 *   - `cpu_clock_up()`, which wrote CPUPERIOD_SEL, SYSCLK_CONF and the LDO slave
 *     count. IDF owns the clock tree, and the analog bias (dbias) that has to move
 *     with it comes from the ROM's PVT calibration through REGI2C, which Rockbox
 *     cannot reproduce. Writing the LDO slaves by hand is the sort of change that
 *     browns out the SoC rather than failing visibly. `set_cpu_frequency()` does
 *     exist -- see below -- but it asks IDF for the frequency instead of setting it.
 *   - peripheral clock gating written into SYSTEM_PERIPHS_CLK_EN by address.
 *     IDF's drivers enable their own clocks; a blanket write from here can
 *     only duplicate or undo that.
 *
 * Rockbox's adjustable-CPU-FREQ machinery is switched on in the target config
 * (HAVE_ADJUSTABLE_CPU_FREQ). The core side already does all the bookkeeping --
 * firmware/system.c keeps the boost counter and calls set_cpu_frequency() with
 * CPUFREQ_MAX on the first boost, CPUFREQ_NORMAL when it drops, and CPUFREQ_DEFAULT
 * when cpu_idle_mode() has marked the machine idle -- so this file only has to turn
 * one of those three numbers into a clock change, and it does that through IDF.
 ****************************************************************************/

#include "config.h"
#include <stdio.h>

#include "system.h"
#include "memory-xtensa.h"   /* XT_LOADADDRESS_ADDR -- needs config.h first */
#include "button-target.h"   /* button_hw_any_pressed() -- the panic hold */
#include "lcd.h"             /* the panic screen is already drawn; this adds the hint */
#include "font.h"            /* FONT_SYSFIXED, FONT_HEIGHT */

#include "sdkconfig.h"
#include "esp_pm.h"

/* cpu_frequency is declared in system.h and defined by firmware/kernel/system.c
 * for every target; this file puts the right number in it, and keeps it right when
 * the number changes -- see set_cpu_frequency(). */

/* CPU_FREQ lives in the target config and CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ in
 * the IDF project, and only the second one is real. Drift between them shows
 * up as every udelay() being wrong by a fixed factor, which is a miserable
 * thing to chase, so it is a compile error instead. */
_Static_assert(CPU_FREQ ==
               (long)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000L,
               "CPU_FREQ in meowkits3.h disagrees with CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ");

void system_init(void)
{
    /* app_main() already replayed the previous boot's trail before anything here
     * could overwrite it, and the trail is deliberately *not* consumed (see
     * paniclog.c), so calling the dump again here printed the same breadcrumb
     * list twice on every boot. The boot stamp is still recorded here because it
     * belongs to Rockbox's own init, not to IDF's entry point. */
    xtensa_paniclog_stamp_boot(0);

    /* The codec and plugin buffers are pinned to a fixed address so that this
     * side agrees with the CODEC_ORIGIN / PLUGIN_ORIGIN that plugin.lds gives
     * the images -- apps/codecs.c rejects anything else. Because they are
     * absolute symbols rather than .space, no section reserves the range, so
     * the only thing standing between a growing .ext_ram.bss and silent
     * corruption of a loaded codec is this check. Loud beats corrupt. */
    {
        extern char rb_ext_ram_end[];
        extern char _ext_ram_bss_end[];
        extern unsigned char codecbuf[];

        /* rb_ext_ram_end alone was the blind spot. It is a label in
         * memory-xtensa-syms.S sitting right after audiobufend, so it marks the
         * end of what THAT file puts in PSRAM -- not the end of the section. IDF
         * places its own .ext_ram.bss content around and after it, and the
         * linker's real section end is _ext_ram_bss_end. Measured on this build:
         * rb_ext_ram_end 0x3c55d2c0 against _ext_ram_bss_end 0x3c579d3c, so
         * 117,372 bytes of the section were invisible to the one check whose
         * entire job is to watch that section. It would have reported a clean
         * layout while .ext_ram.bss was already growing toward the pinned codec
         * image.
         *
         * Take the larger of the two. Both symbols stay meaningful: one is our
         * contribution, the other the whole section, and the guard cares about
         * whichever reaches further. */
        unsigned long ours = (unsigned long)rb_ext_ram_end;
        unsigned long sect = (unsigned long)_ext_ram_bss_end;
        unsigned long end = sect > ours ? sect : ours;

        if (end > (unsigned long)codecbuf)
            printf("MEMORY: .ext_ram.bss ends %p, past pinned codecbuf %p\n",
                   (void *)end, (void *)codecbuf);
    }

    /* The other end of the same problem, and the check that was missing.
     *
     * The external window is MEMORYSIZE MB starting at 0x3C000000, and the flash
     * MMU has exactly that many 64 KB entries pointing at PSRAM -- entries
     * 0..(MEMORYSIZE*16 - 1), ending at 0x3C000000 + MEMORYSIZE MB. There is no
     * entry beyond that to map: all 128 physical pages are already claimed.
     *
     * XT_DROMORIG carries a +0x20 so that flash-mapped segments satisfy the
     * cache's page-congruence rule (paddr % 64KB == vaddr % 64KB), but codecbuf
     * and pluginbuf are derived from it and inherit that offset -- which puts the
     * top of the arena 32 bytes PAST the last mapped entry.
     *
     * Not academic: the last 32 bytes of the plugin buffer are served by MMU
     * entry 128, which the boot map reports as INVALID, and
     * fill_random_playlist_indexes() in apps/tagtree.c writes the whole buffer
     * -- for (i = 0; i < arr_sz; i++) -- making it the one thing in the tree that
     * ever touches them. Observed as A9 = 0x3c80001f in a Core 0
     * IllegalInstruction crash entered by "all tracks" from the database. */
    {
        unsigned long top = XT_LOADADDRESS_ADDR;
        unsigned long win = 0x3C000000UL + (MEMORYSIZE * 0x100000);

        if (top > win)
            printf("MEMORY: arena top %p is %lu bytes past the mapped window "
                   "%p (MMU entry %lu, want <= %lu)\n", (void *)top, top - win,
                   (void *)win, (top & 0x1FFFFFFu) >> 16,
                   (unsigned long)(MEMORYSIZE * 16) - 1);
    }

    /* udelay() in system-target.h counts core cycles, so cpu_frequency has to be
     * the frequency the core is actually on. Rockbox owns that choice through
     * set_cpu_frequency() below and nothing else moves the clock, so assigning it
     * here is enough -- but it is assigned through the same function, so the value
     * and the hardware cannot disagree. If IDF's dynamic frequency scaling is ever
     * enabled (max != min, or light sleep), this has to become esp_clk_cpu_freq()
     * and udelay() has to stop caching it. */
    set_cpu_frequency(CPU_FREQ);

    /* Nothing else. Anything added here should be checked against the list at
     * the top of the file: if IDF already owns it, it does not belong here. */
}

/*
 * Rockbox's one clock entry point. firmware/system.c calls this with CPUFREQ_MAX,
 * CPUFREQ_NORMAL or CPUFREQ_DEFAULT, picked by the boost counter and cpu_idle_mode();
 * the ladder itself is in system-target.h.
 *
 * Applied through esp_pm_configure() with max == min, which is deliberate. It asks
 * IDF to put the core on that frequency and leaves dynamic frequency scaling inert,
 * so: dbias and the rest of the clock tree move the way IDF moves them; Rockbox's
 * notion of "the frequency" is the whole truth rather than a floor under a scaler;
 * and the PM locks IDF's own drivers take -- the I2S driver acquires one around
 * every transfer, at i2s_common.c:1235 -- cannot move the clock out from under
 * udelay(). Light sleep is left off here for the same reason: it needs locks to
 * hold, and that is its own step.
 *
 * The three points are all BBPLL taps, so APB and AHB (which is GDMA's clock) stay
 * at 80 MHz and I2S, I2C and the timer groups stay on XTAL; only the core moves.
 * CPUFREQ_MAX is what CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ boots at, asserted above.
 */
void set_cpu_frequency(long frequency)
{
    int mhz = (int)(frequency / 1000000L);
    esp_pm_config_t cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };

    /* On failure leave cpu_frequency alone: udelay() counts cycles against it, so
     * a value that matches the hardware is worth more than a value Rockbox asked
     * for. esp_pm_configure() rejects values it cannot express as a rtc clock
     * config, and 240/160/80 all can be. */
    if (esp_pm_configure(&cfg) != ESP_OK)
        return;

    cpu_frequency = frequency;
}

void system_reboot(void)
{
    extern void esp_restart(void);

    esp_restart();
}

/*
 * Reached from firmware/panic.c after the message has been drawn on the glass and
 * written to retained memory. It used to call abort(), which runs IDF's panic
 * handler: that prints a backtrace to a console this board only has while the USB
 * pads are still its -- the pads pass to the OTG core about a second after boot --
 * and CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT then resets within milliseconds. The
 * result was a panic message nobody could read: on the screen for long enough to
 * flash, and gone.
 *
 * Holding the screen is the target's job, and it has to be polled rather than
 * waited on: interrupts are masked on this path, so the tick never runs and
 * button_get_with_timeout() would never return. The GPIO keys are inputs on the
 * IO MUX with pull-ups, so a plain level read works with nothing scheduled.
 *
 * Bounded, because a device hanging with a dead button while the screen already
 * says it died is worse than a reboot you missed. IDF arms the RTC watchdog only
 * inside its own handler, which no longer runs here, so this loop is the only
 * timer -- nothing will reset us out from under it.
 */
#define PANIC_HOLD_STEP_MS  100
#define PANIC_HOLD_MAX_MS   20000

void system_exception_wait(void)
{
    int waited = 0;
    int row = LCD_HEIGHT / SYSFONT_HEIGHT - 1;

    if (row > 1)
    {
        lcd_setfont(FONT_SYSFIXED);
        lcd_puts(1, row, (const unsigned char *)"key = restart");
        lcd_update();
    }

    while (waited < PANIC_HOLD_MAX_MS)
    {
        if (button_hw_any_pressed())
            break;
        udelay(PANIC_HOLD_STEP_MS * 1000);
        waited += PANIC_HOLD_STEP_MS;
    }

    extern void esp_restart(void);

    esp_restart();
}
