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

/* .noinit: not in the image, not cleared at startup, and placed in internal
 * SRAM by IDF's sections.ld -- which is what survives the reset a panic causes.
 * .noinit is what makes a crash record survivable: IDF's panic path resets through
 * RTC_SW_CPU_RST, which leaves internal SRAM alone. */
static struct panic_record rec __attribute__((section(".noinit"), aligned(4)));

/* ------------------------------------------------------------------ *
 * Breadcrumbs
 *
 * IDF's panic entry is not a registration point: esp_panic_handler() is a strong
 * symbol in esp_system/panic.c, in an object the link pulls anyway for the watchdog
 * helpers it also defines. Taking it over needs an object of the executable plus
 * -Wl,--allow-multiple-definition, which idf-app/panic_cdc.c does; both crashes in
 * the session that produced this file happened before that existed and left nothing
 * but "Panic handler entered multiple times".
 *
 * Breadcrumbs complement rather than duplicate that: they are written while
 * everything is healthy, so they record a fault the panic entry cannot see -- a
 * watchdog reset, a bus hang, or a handler that re-enters before reaching it.
 *
 * Breadcrumbs invert that: they are written while everything is healthy, at
 * each step that could be the last one, and read on the next boot. A .noinit
 * store needs no working console, no lock, no stack and no panic handler, so
 * it survives precisely the failure that destroys the register dump.
 * ------------------------------------------------------------------ */

/* 40 slots. At 16 the periodic attach samples push out the sequence that starts the
 * attach -- usbEn(1), usbIn, usbFifo, usbConn, usbPost -- which is precisely the part
 * that says whether the transport was brought up and then dropped, or never came up.
 * Each slot is 16 bytes, so this costs 384 bytes of .noinit. */
#define BC_SLOTS 40
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

/* One retained trail entry, counted back from the newest: 0 is the last thing that
 * happened before the reset. This exists because the trail is the only record that
 * survives a hardware exception on this target -- those never reach Rockbox's
 * panic(), they go to IDF's handler, which prints to UART0 and resets, and the
 * .noinit ring is left alone by that reset. A caller with no console (the screen)
 * needs it entry by entry rather than as a dump. Returns 0 when there is nothing at
 * that index -- no trail, or fewer entries than asked for. */
int xtensa_breadcrumb_back(int back, char *buf, int bufsize)
{
    uint32_t n;
    const struct breadcrumb *e;

    if (buf == NULL || bufsize <= 0 || back < 0)
        return 0;
    if (bc_magic != BC_MAGIC || bc_next == 0)
        return 0;

    n = bc_next < BC_SLOTS ? bc_next : BC_SLOTS;
    if ((uint32_t)back >= n)
        return 0;

    /* bc_next only increases and the ring is written by name, so the newest entry
     * is the absolute index bc_next - 1 however many times it has wrapped. */
    e = &bc[(bc_next - 1 - (uint32_t)back) % BC_SLOTS];

    return snprintf(buf, bufsize, "-%d %s %08lx %08lx", back, e->tag,
                    (unsigned long)e->a, (unsigned long)e->b);
}

/* Stamped at every boot. If it shows up in the next boot's trail, .noinit
 * survived the reset and the whole mechanism is proven end to end; if it
 * doesn't, internal DRAM is cleared by panic_restart() and no panic-time
 * record of any kind can work here. */
void xtensa_paniclog_stamp_boot(unsigned long seq)
{
    /* A boundary in the RTC ring. It survives resets, so without a marker there a reader
     * cannot tell the entries the dead boot wrote from the ones this boot wrote. */
    xtensa_breadcrumb_rtc("rBoot", (unsigned long)seq, 0);
    xtensa_breadcrumb("boot", seq, 0);
}

/* The ring is circular and bc_next only ever increases, so slot j holds the entry whose
 * absolute index is j mod BC_SLOTS. Walking absolute indices upward therefore yields
 * chronological order whether or not the ring has wrapped. */
/* Reset reason of the reset that produced this boot, stamped by app_main before
 * anything can overwrite it. The breadcrumb ring is only forty slots and a busy USB
 * attach fills them in seconds, so an rstR entry is gone before anyone can read it;
 * this is not. Read back by xtensa_paniclog_reset_reason(). */
static uint32_t xt_reset_reason;

void xtensa_paniclog_set_reset(unsigned reason)
{
    xt_reset_reason = reason;
}

/* ---------------------------------------------------------------------------
 * Teardown trail in RTC slow memory
 *
 * The .noinit ring above lives in internal SRAM, and a task-watchdog reset
 * clears it: after the unplug crash the ring came back holding only the new
 * boot's entries, so the last steps of the boot that hung are gone. The reset
 * reason survives because it lives in the RTC power domain, and that is the
 * property this ring needs.
 *
 * RTC *fast* memory is not available for it. IDF's own heap is placed there --
 * heap_init reports "At 600FE000 len 00001FE8 (7 KiB): RTCRAM" -- and the first
 * attempt at this used 0x600FFC00 for the ring and 0x600FFE00 for the panic
 * record, which is inside that heap. That build did not boot, and the RTC
 * overlap was the reason, not the access itself.
 *
 * RTC slow (SOC_RTC_DATA_LOW 0x50000000, HIGH 0x50002000) is the other bank.
 * Only the first 0x20 bytes are claimed by the image's RTC_DATA segment, so
 * 0x50001000 is free, and this ring is deliberately small and only used for the
 * steps that can end in a hang: the USB teardown.
 */
#define XT_RTC_SLOW_BASE   0x50001000u
#define XT_RTC_SLOW_MAGIC  0x43525455u   /* "CRTU": bumped from "CRTT", whose slots were the
                             * old 16-byte char-tag layout. Anything under the old magic is stale and is
                             * ignored rather than printed as nonsense. */
#define XT_RTC_SLOTS       24

/* RTC slow memory on this chip only accepts 32-bit accesses: the first version of this
 * stored the tag one byte at a time and those stores did nothing, while the word-wise
 * counter advanced happily -- which is why the ring read back as nothing but stale entries.
 * Every field here is a full word. Tags are truncated to four characters. */
struct rtc_slot
{
    uint32_t tag;   /* up to four characters, little endian */
    uint32_t a;
    uint32_t b;
};

struct rtc_store
{
    uint32_t magic;
    uint32_t next;
    struct rtc_slot slot[XT_RTC_SLOTS];
};

/* The panic record, also in RTC slow. The SRAM copy is cleared by the watchdog reset that
 * follows a hung panic path, which is exactly when its text is wanted: "no panic record" in
 * a post-mortem therefore proves nothing about whether panicf() ran. Written a word at a
 * time, like the ring above, because byte stores to this bank do not land. */
#define XT_PANIC_RTC_BASE  0x50001200u
#define XT_PANIC_RTC_MAGIC 0x43505254u   /* "CRPT" */

struct panic_rtc
{
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t word[24];   /* 96 characters, four per word */
};

static volatile struct rtc_store *rtc_store =
    (volatile struct rtc_store *)XT_RTC_SLOW_BASE;

void xtensa_breadcrumb_rtc(const char *tag, unsigned long a, unsigned long b)
{
    struct rtc_store *s = (struct rtc_store *)rtc_store;
    uint32_t i;
    uint32_t code = 0;
    int n;

    if (s->magic != XT_RTC_SLOW_MAGIC)
    {
        /* First power-on: the bank is whatever the reset left it. */
        s->magic = XT_RTC_SLOW_MAGIC;
        s->next  = 0;
    }

    for (n = 0; n < 4 && tag[n]; n++)
        code |= (uint32_t)(unsigned char)tag[n] << (8 * n);

    i = s->next % XT_RTC_SLOTS;
    s->next++;
    s->slot[i].tag = code;
    s->slot[i].a = (uint32_t)a;
    s->slot[i].b = (uint32_t)b;
}

/* Copy an rtc slot out before formatting it, so the dump never reads the
 * volatile bank directly. */
static int rtc_slot_line(char *buf, int bufsize, const struct rtc_slot *slot)
{
    char tag[5];
    uint32_t code = slot->tag;
    int i;

    for (i = 0; i < 4; i++)
        tag[i] = (char)((code >> (8 * i)) & 0xff);
    tag[4] = '\0';

    return snprintf(buf, bufsize, "%s a=%08lx b=%08lx\n",
                    tag, (unsigned long)slot->a, (unsigned long)slot->b);
}

/* The head line carries the RTC count as well as the reset reason: after a
 * watchdog reset the SRAM ring is gone, and "rtc 0" is the difference between a
 * hang that never reached the USB teardown and one that did. */
/* The retained panic message, RTC copy preferred. RTC slow survives the reset that
 * clears internal DRAM, so it is the record that can name a crash the SRAM trail
 * cannot. Returns 0 when nothing was retained. */
int xtensa_paniclog_text(char *buf, int bufsize)
{
    volatile struct panic_rtc *pr = (volatile struct panic_rtc *)XT_PANIC_RTC_BASE;
    int len = 0;

    if (buf == NULL || bufsize <= 0)
        return 0;
    buf[0] = '\0';

    if (pr->magic == XT_PANIC_RTC_MAGIC)
    {
        uint32_t n = pr->len > 96 ? 96 : pr->len;
        uint32_t i;

        /* The bank stores four characters per word. Read them out before
         * formatting so snprintf never touches the volatile region. */
        for (i = 0; i < 24 && len < bufsize - 1; i++)
        {
            uint32_t w = pr->word[i];
            int j;

            for (j = 0; j < 4; j++)
            {
                uint32_t k = i * 4 + (uint32_t)j;
                char c;

                if (k >= n)
                    break;
                c = (char)((w >> (8 * j)) & 0xff);
                if (c == '\0')
                    break;
                buf[len++] = c;
            }
        }
        buf[len] = '\0';
        return len;
    }

    if (rec.magic == PANICLOG_MAGIC)
        return snprintf(buf, bufsize, "%s", rec.msg);

    return 0;
}

/* The reset that produced this boot. The breadcrumb ring is only forty slots and a
 * busy USB attach fills them in seconds, so an rstR entry is gone before anyone can
 * read it; this is not. */
unsigned long xtensa_paniclog_reset_reason(void)
{
    return xt_reset_reason;
}

/* How many entries the RTC ring holds. Zero on a clean power-on, and the difference
 * between "the dead boot never reached the USB teardown" and "it reached it and
 * hung" once a panic message is on the glass. */
unsigned long xtensa_paniclog_rtc_count(void)
{
    return rtc_store->magic == XT_RTC_SLOW_MAGIC ? rtc_store->next : 0;
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

    /* Copy it to RTC slow as words: 96 characters, four per word, zero padded. This is
     * the only record that outlives the reset this panic is about to cause. */
    {
        volatile struct panic_rtc *pr = (volatile struct panic_rtc *)XT_PANIC_RTC_BASE;
        int i;

        pr->magic = XT_PANIC_RTC_MAGIC;
        pr->seq   = rec.seq;
        pr->len   = (uint32_t)strlen(rec.msg);
        for (i = 0; i < 24; i++)
        {
            uint32_t w = 0;
            int j;

            for (j = 0; j < 4; j++)
            {
                char c = rec.msg[i * 4 + j];

                if (c)
                    w |= (uint32_t)(unsigned char)c << (8 * j);
            }
            pr->word[i] = w;
        }
    }

    /* Lock-free, and safe with interrupts masked. */
    esp_rom_printf("\r\n*PANIC* %s\r\n", rec.msg);
}

void xtensa_paniclog_dump(void)
{
    /* The trail is printed independently of the panic record. Both crashes this
     * was built for were hardware exceptions, and those never reach
     * xtensa_panic_log() -- which is what panicked twice before printing
     * anything. Returning early on an absent record therefore hid the one thing
     * that could survive such a crash. */
    if (rec.magic == PANICLOG_MAGIC)
        printf("prev panic #%lu: %s\n", (unsigned long)rec.seq, rec.msg);

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

            printf("  %s a=%08lx b=%08lx\n", e->tag,
                   (unsigned long)e->a, (unsigned long)e->b);
        }

    /* The RTC-slow teardown trail. Printed after the SRAM ring because it is
     * shorter and is the one that survives a watchdog reset. */
    {
        struct rtc_store *s = (struct rtc_store *)rtc_store;
        char line[48];

        if (s->magic == XT_RTC_SLOW_MAGIC && s->next != 0)
        {
            uint32_t cnt = s->next < XT_RTC_SLOTS ? s->next : XT_RTC_SLOTS;
            uint32_t begin = s->next - cnt;
            uint32_t k;

            printf("rtc teardown trail (%lu):\n", (unsigned long)cnt);
            for (k = 0; k < cnt; k++)
            {
                int len = rtc_slot_line(line, sizeof line,
                                        &s->slot[(begin + k) % XT_RTC_SLOTS]);
                if (len > 0)
                    printf("%s", line);
            }
        }
    }
    }

    /* Consume the record, so a stale one is not replayed forever: a fault that looks
     * like it keeps happening is the kind of lead that costs a day. The dump above has
     * already printed it by now.
     */
    rec.magic = 0;

    /* Do NOT clear bc_next. The trail is ring-buffered and written by name, so
     * leaving it in place means the next crash overwrites the oldest entries
     * rather than starting from a blank slate -- and clearing it here would also
     * erase the boot stamp that proves the trail survived the reset at all. */
}

/* Proven on hardware the first time it was used: a crash during WAV resume
 * ended the trail at "placed", and the next boot printed it. Two things that
 * result settled, both of which the register dump could not give us at the time
 * because IDF's panic entry had not yet been taken over and the handler re-entered
 * before printing anything:
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
 * and no txstop. sink_stop() stamped "stop" and then called stop_tx(), which
 * stamped "txstop", so the fault was inside sink_stop() between those two -- a
 * window whose only substantial content was a 40-argument diagnostic dump that
 * had no business being in the teardown path. Removing it from that call site
 * ended both repro modes. Both crashes in this file's history were the probe,
 * not the audio path, not the codec, and not a malformed WAV: the recorder's
 * headers are correct (0x0001, 2ch, 44100, 16-bit, byte_rate 176400, block_align
 * 4, data_chunk == file - 44, riff_size == file - 8).
 *
 * The lesson worth keeping: an instrument placed in the path it observes can
 * become the fault being observed. This one survived three crashes before the
 * trail was wide enough to name it.
 */

/* Send everything that survived the reset over the USB CDC console.
 *
 * The hardware-info screen shows six trail rows because that is what fits on a
 * 320x240 panel; a host can have the whole ring, the teardown trail and the panic
 * record without anyone navigating a debug menu.
 *
 * Nothing calls this right now. It was wired to a one-shot task at boot and that
 * combination put the board into a reboot loop; the caller is removed, the
 * function is kept, and the CDC report goes back in only once the fault is
 * identified. Note that this is the only caller of usb_serial_send() anywhere in
 * the tree -- Rockbox has no serial console output on any target -- so a fault
 * here is the first IN transfer the CDC endpoint has ever attempted, not a race
 * introduced by a second producer.
 *
 * usb_serial_send() is used rather than printf: it fills the stack's transmit ring
 * and returns, and drops the data if the CDC function is not the one currently
 * configured, so a device plugged into a host that asked for MSC instead of a
 * serial port simply stays quiet.
 */
void xtensa_paniclog_cdc_dump(void)
{
    /* Declared rather than included: firmware/usbstack is not on the include path
     * for the firmware library. The signature matches usbstack/usb_serial.h. */
    extern void usb_serial_send(const unsigned char *data, int length);
    char line[64];
    int len;
    int back;

    len = snprintf(line, sizeof line,
                   "\r\nrb crash report: reset %lu, trail %lu, rtc %lu\r\n",
                   xtensa_paniclog_reset_reason(),
                   (unsigned long)(bc_magic == BC_MAGIC ? bc_next : 0),
                   xtensa_paniclog_rtc_count());
    if (len > 0)
        usb_serial_send((const unsigned char *)line, len);

    len = xtensa_paniclog_text(line, sizeof line);
    if (len > 0)
    {
        usb_serial_send((const unsigned char *)"  panic: ", strlen("  panic: "));
        usb_serial_send((const unsigned char *)line, len);
        usb_serial_send((const unsigned char *)"\r\n", 2);
    }

    /* Newest first, so this matches what panic_cdc.c writes at fault time and what
     * the debug page shows. The three readouts of the same ring should agree. */
    usb_serial_send((const unsigned char *)"  trail, newest first:\r\n",
                    strlen("  trail, newest first:\r\n"));
    for (back = 0; back < BC_SLOTS; back++)
    {
        len = xtensa_breadcrumb_back(back, line, sizeof line);
        if (len == 0)
            break;
        usb_serial_send((const unsigned char *)"  ", 2);
        usb_serial_send((const unsigned char *)line, len);
        usb_serial_send((const unsigned char *)"\r\n", 2);
    }
}
