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
#include "tick.h"

#define PANICLOG_MAGIC 0x434e504cu   /* "LNPL" */

struct panic_record
{
    uint32_t magic;
    uint32_t seq;
    char     msg[96];
};

/* RTC fast memory, not .noinit. Measured on the board: after a crash-and-reboot the
 * .noinit ring came back holding only the *new* boot's entries, so internal DRAM is
 * cleared by whatever reset IDF's panic path performs. esp_restart is a chip reset, but
 * not one that leaves DRAM powered, so a .noinit record cannot survive the exact failure
 * it exists to record.
 *
 * RTC fast memory is on the RTC power domain and survives every reset short of removing
 * power -- the same property that made the USB pad mux override persist across reboots.
 * It is 8 KB at 0x600FE000, uncached, and unclaimed here: sdkconfig sets
 * CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP (a dependency check only, not
 * ..._USE_RTC_FAST_MEM_AS_HEAP) and this port never enters deep sleep, so neither the
 * heap nor the wake stub wants it. Everything still sits at the top end.
 */
#define XT_RTC_FAST_BASE   0x600FE000u
#define XT_RTC_FAST_SIZE   8192u

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
 * each step that could be the last one, and read on the next boot. The store needs no
 * working console, no lock, no stack and no panic handler, and it lives in memory that
 * outlives the failure that destroys the register dump.
 * ------------------------------------------------------------------ */

/* 40 slots. At 16 the periodic attach samples push out the sequence that starts the
 * attach -- usbEn(1), usbIn, usbFifo, usbConn, usbPost -- which is precisely the part
 * that says whether the transport was brought up and then dropped, or never came up.
 * Each slot is 16 bytes. */
#define BC_SLOTS 40
#define BC_MAGIC 0x43415242u   /* "BRAC" -- validates the counters, which
                                * RTC fast memory does not zero */

struct breadcrumb
{
    uint32_t a;
    uint32_t b;
    char     tag[24];
};

/* Header plus ring in one block at the top of RTC fast memory: 8 bytes of header and
 * 640 of ring is 648, the panic record is 104, so this claims 752 of the 8192 available
 * and leaves the low end -- where IDF's own bookkeeping would live -- untouched. */
struct bc_store
{
    uint32_t magic;
    uint32_t next;
    struct breadcrumb slots[BC_SLOTS];
};

#define XT_RTC_STORE \
    ((volatile struct bc_store *)(XT_RTC_FAST_BASE + XT_RTC_FAST_SIZE - 1024u))
#define XT_RTC_PANIC \
    ((volatile struct panic_record *)(XT_RTC_FAST_BASE + XT_RTC_FAST_SIZE - 512u))

void xtensa_breadcrumb(const char *tag, unsigned long a, unsigned long b)
{
    volatile struct bc_store *s = XT_RTC_STORE;
    volatile struct breadcrumb *e;

    /* The region holds whatever the last boot left, including garbage on the first ever
     * power-on, so the ring index is only trusted once the magic has been written. */
    if (s->magic != BC_MAGIC)
    {
        s->magic = BC_MAGIC;
        s->next  = 0;
        for (int i = 0; i < BC_SLOTS; i++)
        {
            s->slots[i].a = 0;
            s->slots[i].b = 0;
            s->slots[i].tag[0] = '\0';
        }
    }

    e = &s->slots[s->next % BC_SLOTS];
    e->a = (uint32_t)a;
    e->b = (uint32_t)b;
    for (int i = 0; i < (int)sizeof e->tag - 1 && tag[i]; i++)
        e->tag[i] = tag[i];
    e->tag[sizeof e->tag - 1] = '\0';

    s->next++;
}

/* Stamped at every boot. If it shows up in the next boot's trail, .noinit
 * survived the reset and the whole mechanism is proven end to end; if it
 * doesn't, internal DRAM is cleared by panic_restart() and no panic-time
 * record of any kind can work here. */
void xtensa_paniclog_stamp_boot(unsigned long seq)
{
    xtensa_breadcrumb("boot", seq, 0);
}

/* The ring is circular and the counter only ever increases, so slot j holds the entry
 * whose absolute index is j mod BC_SLOTS. Walking absolute indices upward therefore
 * yields chronological order whether or not the ring has wrapped. */
int xtensa_breadcrumb_window(char *buf, int bufsize, int start, int count)
{
    volatile struct bc_store *s = XT_RTC_STORE;
    int n, i, len = 0;
    char tag[sizeof s->slots[0].tag + 1];

    if (buf == NULL || bufsize <= 0 || s->magic != BC_MAGIC)
        return 0;

    n = (s->next < BC_SLOTS) ? (int)s->next : BC_SLOTS;
    if (start >= n)
        return 0;
    if (start + count > n)
        count = n - start;

    for (i = start; i < start + count; i++)
    {
        volatile struct breadcrumb *e = &s->slots[i % BC_SLOTS];

        /* The tag lives in volatile memory, so copy it out before snprintf wants a
         * plain char*. */
        for (int k = 0; k < (int)sizeof tag - 1; k++)
        {
            tag[k] = e->tag[k];
            if (!tag[k])
                break;
        }
        tag[sizeof tag - 1] = '\0';

        int used = snprintf(buf + len, bufsize - len,
                            "%s %08lx %08lx\r\n",
                            tag, (unsigned long)e->a, (unsigned long)e->b);

        if (used <= 0 || used >= bufsize - len)
            break; /* truncated rather than overflowing the caller */
        len += used;
    }
    return len;
}

void xtensa_panic_log(const char *msg)
{
    volatile struct panic_record *rec = XT_RTC_PANIC;

    if (rec->magic != PANICLOG_MAGIC)
    {
        rec->magic = PANICLOG_MAGIC;
        rec->seq   = 0;
    }

    rec->seq++;
    for (int i = 0; i < (int)sizeof rec->msg - 1 && msg[i]; i++)
        rec->msg[i] = msg[i];
    rec->msg[sizeof rec->msg - 1] = '\0';

    /* Lock-free, and safe with interrupts masked. */
    esp_rom_printf("\r\n*PANIC* %s\r\n", rec->msg);
}

/* Called early in the next boot, before anything can panic of its own accord. */
/* Bring-up switch. Remove with the attach instrumentation once enumeration works.
 * Deliberately a define in this file rather than in the model header: nothing else
 * needs to know about it, and it must not silently survive into a shipping build. */
#define XT_BRINGUP_TRAIL_REPEAT 1

#ifdef XT_BRINGUP_TRAIL_REPEAT
/* Bring-up only: re-print the trail from the tick task thread every ~10 s.
 *
 * The dump in app_main() lands about a second after boot, which is before the
 * host has finished enumerating USB-Serial-JTAG and created /dev/ttyACM0 -- so
 * the trail existed, was never consumed (bc_next is deliberately left alone),
 * and was still uncatchable: there was no reader and no node to read from. A
 * repeating print means any listener at any time sees the same history, and it
 * costs nothing to remove once the attach is proven.
 */
static bool xt_repeat_started;

static void xt_trail_repeat(void)
{
    static unsigned ticks;

    if (++ticks < 10 * HZ)
        return;
    ticks = 0;

    xtensa_paniclog_dump();
}

static void xtensa_paniclog_repeat_start(void)
{
    tick_add_task(xt_trail_repeat);
}
#endif

void xtensa_paniclog_dump(void)
{
    /* The trail is printed independently of the panic record. Both crashes this
     * was built for were hardware exceptions, and those never reach
     * xtensa_panic_log() -- which is what panicked twice before printing
     * anything. Returning early on an absent record therefore hid the one thing
     * that could survive such a crash. */
    volatile struct panic_record *pr = XT_RTC_PANIC;

    if (pr->magic == PANICLOG_MAGIC)
        printf("prev panic #%lu: %s\n", (unsigned long)pr->seq, pr->msg);

    /* The trail that led to it. Printed oldest first so the last line is the
     * last thing that happened before the reboot. */
    volatile struct bc_store *s = XT_RTC_STORE;

    if (s->magic == BC_MAGIC && s->next != 0)
    {
        uint32_t n = s->next < BC_SLOTS ? s->next : BC_SLOTS;
        uint32_t start = s->next - n;
        uint32_t i;

        printf("panic breadcrumbs (%lu):\n", (unsigned long)n);
        for (i = 0; i < n; i++)
        {
            volatile struct breadcrumb *e = &s->slots[(start + i) % BC_SLOTS];
            char tag[sizeof e->tag + 1];

            for (int k = 0; k < (int)sizeof tag - 1; k++)
            {
                tag[k] = e->tag[k];
                if (!tag[k])
                    break;
            }
            tag[sizeof tag - 1] = '\0';

            printf("  %s a=%08lx b=%08lx\n", e->tag,
                   (unsigned long)e->a, (unsigned long)e->b);
        }
    }

    /* Consume the record, so a stale one is not replayed forever: a fault that looks
     * like it keeps happening is the kind of lead that costs a day.
     *
     * Except while the trail is being repeated. A panic during a USB attach has no
     * console to print to -- the pads are with the OTG core -- so it reboots quietly,
     * and the next boot prints this record about a second after starting, which is
     * before the host has enumerated USB-Serial-JTAG and created the serial node. The
     * print happens, nobody can read it, and consuming the record destroys the only
     * copy of the reason. Keeping it here means the repeating dump shows it on a
     * console that exists. Remove along with XT_BRINGUP_TRAIL_REPEAT.
     */
#ifdef XT_BRINGUP_TRAIL_REPEAT
    /* keep the record for the repeating dump */
#else
    pr->magic = 0;
#endif

    /* Do NOT clear the ring counter. The trail is ring-buffered and written by name, so
     * leaving it in place means the next crash overwrites the oldest entries rather than
     * starting from a blank slate -- and clearing it would also erase the boot stamp that
     * proves the trail survived the reset at all. */

#ifdef XT_BRINGUP_TRAIL_REPEAT
    if (!xt_repeat_started)
    {
        xt_repeat_started = true;
        xtensa_paniclog_repeat_start();
    }
#endif
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
