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
#include <stdio.h>

#include <stdlib.h>     /* abort(): esp_cache.h used to drag it in, cache_hal.h does not */

#include "system.h"
#include "memory-xtensa.h"   /* XT_LOADADDRESS_ADDR -- needs config.h first */

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
    /* Report anything the previous boot left behind, before this boot can
     * overwrite it. Both the panic record and the breadcrumb trail live in
     * .noinit internal SRAM, which survives the reset a panic causes, and the
     * dump consumes them so a stale one is not replayed as a live fault. */
    xtensa_paniclog_dump();
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
