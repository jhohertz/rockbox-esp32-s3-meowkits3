/*
 * paniclog.c -- get Rockbox's panic text somewhere readable after the fact.
 *
 * panicf() formats the message into panic_buf, draws it on the LCD, and then
 * calls abort(). IDF prints "abort() was called at PC ..." followed by a
 * backtrace, but never the message -- so the only copy of something like
 * "abandon ship! dircache" is on the glass. That is no use to anyone who is not
 * standing in front of the device, and it made this session's thread bug take
 * far longer than it should have: the panel was correctly reporting the fault
 * while the console reported only that *a* fault happened.
 *
 * Two routes out, because the two ways this goes wrong need different ones:
 *
 *   - esp_rom_printf() immediately. The panic path masks interrupts before
 *     formatting, and IDF's printf takes a lock that may already be held, so
 *     printf here deadlocks or aborts inside the abort handler. The ROM routine
 *     takes no lock and works with interrupts masked.
 *
 *   - a .noinit record, dumped by the next boot. The console is USB-Serial-JTAG,
 *     which resets with the chip, so text emitted in the last milliseconds
 *     before a reboot can be lost before the host reads it. Internal SRAM
 *     survives the reset that a panic causes, so the record does.
 *
 * The record is one deep and prints once. Panics are rare enough that a ring
 * would be speculative, and printing the same panic on every subsequent boot
 * would be worse than not printing it at all.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_rom_sys.h"

#define PANICLOG_MAGIC 0x434e504cu   /* "LNPL" */

struct panic_record
{
    uint32_t magic;
    uint32_t seq;
    char     msg[96];
};

/* .noinit: not in the image, not cleared at startup, and placed in internal
 * SRAM by IDF's sections.ld -- which is what survives the reset a panic causes.
 * Same mechanism scan_trace.c uses, for the same reason. */
static struct panic_record rec __attribute__((section(".noinit"), aligned(4)));

/* ------------------------------------------------------------------ *
 * Breadcrumbs
 *
 * IDF 5.5 offers no hook into its panic path (no panic_handler_register, and
 * the only weak symbols in panic.c are the reset-reason hints), and on this
 * device the handler re-enters and reboots before printing anything -- both
 * crashes in the session that produced this left nothing but
 * "Panic handler entered multiple times". A record written AT panic time
 * therefore cannot be relied on.
 *
 * Breadcrumbs invert that: they are written while everything is healthy, at
 * each step that could be the last one, and read on the next boot. A .noinit
 * store needs no working console, no lock, no stack and no panic handler, so
 * it survives precisely the failure that destroys the register dump.
 * ------------------------------------------------------------------ */

#define BC_SLOTS 16
#define BC_MAGIC 0x43415242u   /* "BRAC" -- validates the counters, which
                                * .noinit does not zero */

struct breadcrumb
{
    uint32_t a;
    uint32_t b;
    char     tag[24];
};

static struct breadcrumb bc[BC_SLOTS] __attribute__((section(".noinit"), aligned(4)));
static uint32_t bc_magic __attribute__((section(".noinit")));
static uint32_t bc_next  __attribute__((section(".noinit")));

void xtensa_breadcrumb(const char *tag, unsigned long a, unsigned long b)
{
    struct breadcrumb *e;

    /* .noinit is uninitialised at first ever boot, so the ring index is only
     * trustworthy once something has written it once. */
    if (bc_magic != BC_MAGIC)
    {
        bc_magic = BC_MAGIC;
        bc_next  = 0;
        memset(bc, 0, sizeof bc);
    }

    e = &bc[bc_next % BC_SLOTS];
    e->a = (uint32_t)a;
    e->b = (uint32_t)b;
    memcpy(e->tag, tag, sizeof e->tag - 1);
    e->tag[sizeof e->tag - 1] = '\0';

    bc_next++;
}

/* Stamped at every boot. If it shows up in the next boot's trail, .noinit
 * survived the reset and the whole mechanism is proven end to end; if it
 * doesn't, internal DRAM is cleared by panic_restart() and no panic-time
 * record of any kind can work here. */
void xtensa_paniclog_stamp_boot(unsigned long seq)
{
    xtensa_breadcrumb("boot", seq, 0);
}

void xtensa_panic_log(const char *msg)
{
    if (rec.magic != PANICLOG_MAGIC)
    {
        rec.magic = PANICLOG_MAGIC;
        rec.seq   = 0;
    }

    rec.seq++;
    memcpy(rec.msg, msg, sizeof rec.msg - 1);
    rec.msg[sizeof rec.msg - 1] = '\0';

    /* Lock-free, and safe with interrupts masked. */
    esp_rom_printf("\r\n*PANIC* %s\r\n", rec.msg);
}

/* Called early in the next boot, before anything can panic of its own accord. */
void xtensa_paniclog_dump(void)
{
    /* The trail is printed independently of the panic record. Both crashes this
     * was built for were hardware exceptions, and those never reach
     * xtensa_panic_log() -- which is what panicked twice before printing
     * anything. Returning early on an absent record therefore hid the one thing
     * that could survive such a crash. */
    if (rec.magic == PANICLOG_MAGIC)
        printf("prev panic #%u: %s\n", rec.seq, rec.msg);

    /* The trail that led to it. Printed oldest first so the last line is the
     * last thing that happened before the reboot. */
    if (bc_magic == BC_MAGIC && bc_next != 0)
    {
        uint32_t n = bc_next < BC_SLOTS ? bc_next : BC_SLOTS;
        uint32_t start = bc_next - n;
        uint32_t i;

        printf("panic breadcrumbs (%lu):\n", (unsigned long)n);
        for (i = 0; i < n; i++)
        {
            struct breadcrumb *e = &bc[(start + i) % BC_SLOTS];

            printf("  %s a=%08x b=%08x\n", e->tag, e->a, e->b);
        }
    }

    /* Consume both. A stale record replayed on every boot looks like a fault
     * that keeps happening, which is precisely the kind of lead that costs a
     * day -- and with it a stale trail, which would be worse because the
     * breadcrumbs read as a sequence and a stale one points at the wrong file. */
    rec.magic = 0;

    /* Do NOT clear bc_next. The trail is ring-buffered and written by name, so
     * leaving it in place means the next crash overwrites the oldest entries
     * rather than starting from a blank slate -- and clearing it here would also
     * erase the boot stamp that proves the trail survived the reset at all. */
}

/* Proven on hardware the first time it was used: a crash during WAV resume
 * ended the trail at "placed", and the next boot printed it. Two things that
 * result settled, both of which the register dump could not give us because IDF
 * 5.5 has no panic hook and the handler re-enters before printing anything:
 *
 *   - .noinit internal SRAM does survive panic_restart() (RTC_SW_CPU_RST), so a
 *     write-while-healthy trail is a usable crash record on this target;
 *   - the fault is at or after codec image placement, not inside the loader --
 *     both segments were placed and the load returned.
 */

/* What the trail found, first try after widening it to cover backlight, volume
 * and transport stop: a crash reproduced by rapid file cycling and by cancelling
 * at the end of a playlist ended with
 *
 *   seg, seg, placed, blon x12, stop
 *
 * and no txstop. sink_stop() stamps "stop" and then calls stop_tx(), which stamps
 * "txstop", so the fault was inside sink_stop() between those two -- a window
 * whose only substantial content was i2s_audio_probe(), a temporary 40-argument
 * diagnostic that had no business being in the teardown path. Removing it from
 * that call site ended both repro modes. Both crashes in this file's history were
 * the probe, not the audio path, not the codec, and not a malformed WAV: the
 * recorder's headers are correct (0x0001, 2ch, 44100, 16-bit, byte_rate 176400,
 * block_align 4, data_chunk == file - 44, riff_size == file - 8).
 *
 * The lesson worth keeping: an instrument placed in the path it observes can
 * become the fault being observed. This one survived three crashes before the
 * trail was wide enough to name it.
 */
