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

/* Opt this file into logf output. logf.h compiles logf() away in any file that
 * does not ask for it, and the crash report below is the only message this file
 * emits -- there is no per-tick flooding here, which is the failure mode the
 * comment in button_read_device() documents. logf() is used rather than a
 * hand-rolled pump because Rockbox already routes it to the USB CDC console
 * (firmware/logf.c flushes its ring through usb_serial_send()) under
 * USB_ENABLE_SERIAL, and it flushes once per message, so a payload is bounded by
 * the call site instead of by a loop. */
#define LOGF_ENABLE
#include "logf.h"

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

/* Which image is running this boot, stamped by idf-app/main/rockbox_app.c from
 * esp_app_get_description()->version before Rockbox's init starts.
 *
 * It exists because a flash cannot be confirmed by observation: the post-flash
 * reset pulse travels over D+/D-, and once the previously running image has
 * handed those pads to the DWC core the chip never sees the pulse. Anything read
 * immediately after a flash therefore belongs to the old image, and the only way
 * to tell the two apart is to have the running build say so. The panic handler
 * stores the build id of the image that faulted, so a report can show both. */
static char rb_build_now[32] __attribute__((section(".noinit"), aligned(4)));

void xtensa_build_id_stamp(const char *id)
{
    int i;

    if (id == NULL)
        return;

    /* 32 characters: `git describe` on this tree gives 31 ("baremetal-pre-idf-323-
       g122e992e") and the first version of this buffer was 24, which printed
       "baremetal-pre-idf-323-g" -- the hash is the part that identifies the
       build, so a truncated id defeats the whole point of stamping it. */
    for (i = 0; i < 31 && id[i] != '\0'; i++)
        rb_build_now[i] = id[i];
    rb_build_now[i] = '\0';
}

const char *xtensa_build_id_running(void)
{
    /* .noinit is uninitialised on the first ever boot, so an unstamped buffer
       reads as noise rather than as an empty string. A stamp is printable and
       starts with a digit or a letter; anything else means it never ran. */
    char c = rb_build_now[0];

    if (c < '0' || (c > '9' && c < 'A') || (c > 'Z' && c < 'a') || c > 'z')
        return NULL;

    rb_build_now[31] = '\0';
    return rb_build_now;
}

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

/* The exception frame, raw. The first four recorder crashes all reported
 * pc=40374340, which addr2line resolves to _UserExceptionVector itself
 * (xtensa_vectors.S:646) -- the vector's address, not a faulting instruction.
 * EPC1 holds the vector only when a second exception overwrote it, so the frame
 * we were handed is possibly an XtDbFrame rather than an XtExcFrame, and every
 * register we decoded from it was read at the wrong offset.
 *
 * Rather than guess the layout at fault time, keep the words and decode them on
 * the host, where both layouts are available. info->exception and
 * pseudo_excause say which of the two it is.
 *
 * The third RTC-slow region; the teardown ring ends at 0x50001128 and the panic
 * record at 0x5000126c. IDF's own RTC-slow objects (.rtc.force_slow, three in
 * sleep_modes.c) are placed at the start of the bank, and RTC fast is what this
 * build uses as heap, so 0x50001400 is unclaimed. Word stores only, as above. */
#define XT_FRAME_RTC_BASE   0x50001400u
#define XT_FRAME_RTC_MAGIC  0x47473252u   /* "R2GR": bumped from "FRAG", whose struct had no
                             * register slots. Anything under the old magic is stale and reads
                             * as nothing rather than as shifted words. */
#define XT_FRAME_WORDS      32

struct frame_rtc
{
    uint32_t magic;
    uint32_t seq;
    uint32_t exception;   /* IDF panic_exception_t */
    uint32_t pseudo;      /* info->pseudo_excause */
    uint32_t count;
    uint32_t word[XT_FRAME_WORDS];
    /* Read from the registers rather than the frame, and only when PS.INTLEVEL
     * says they are legal. On a double exception the ISA shifts the original
     * EPC1 down to EPC2, so epc2 is the faulting instruction and depc the address
     * that doubled -- neither appears in any frame the handler can see. */
    uint32_t have_regs;   /* magic above, set once the slots are written */
    uint32_t ps;
    uint32_t epc1;
    uint32_t epc2;
    uint32_t epc3;
    uint32_t epc4;
    uint32_t depc;
    uint32_t cache_vaddr; /* the access the cache refused */
    uint32_t cache_size;  /* and how wide it was */
    uint32_t regs_magic;
};

#define XT_REGS_RTC_MAGIC 0x47524732u   /* "2GR" */

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

/* Keep the raw exception frame across the reset.
 *
 * Called from the panic handler, before anything is sent to the console: the
 * CDC transport cannot be relied on at fault time because the reset closes the
 * host's file descriptor and whatever was queued dies with the endpoint. RTC
 * slow is the only thing that survives, so the frame goes there and is read
 * back at the next boot, where the reader is reliably open.
 *
 * The words are copied verbatim rather than decoded. Which layout they are in is
 * the question being asked, and deciding it here would be guessing. */
void xtensa_paniclog_frame(const unsigned long *words, int count,
                           int exception, int pseudo)
{
    volatile struct frame_rtc *fr =
        (volatile struct frame_rtc *)XT_FRAME_RTC_BASE;
    int i;

    if (fr->magic != XT_FRAME_RTC_MAGIC)
    {
        fr->magic = XT_FRAME_RTC_MAGIC;
        fr->seq   = 0;
    }

    fr->seq      = fr->seq + 1;
    fr->exception = (uint32_t)exception;
    fr->pseudo    = (uint32_t)pseudo;

    if (count > XT_FRAME_WORDS)
        count = XT_FRAME_WORDS;
    fr->count = (uint32_t)count;

    for (i = 0; i < count; i++)
        fr->word[i] = (uint32_t)words[i];
}

void xtensa_paniclog_frame_info(unsigned long *exception, unsigned long *pseudo,
                                unsigned long *count)
{
    volatile struct frame_rtc *fr =
        (volatile struct frame_rtc *)XT_FRAME_RTC_BASE;

    if (fr->magic != XT_FRAME_RTC_MAGIC)
    {
        if (exception) *exception = 0;
        if (pseudo)    *pseudo = 0;
        if (count)     *count = 0;
        return;
    }

    if (exception) *exception = fr->exception;
    if (pseudo)    *pseudo = fr->pseudo;
    if (count)     *count = fr->count;
}

/* Four words per line, line 0 being the frame's first word. Returns 0 when there
 * is nothing retained or the line is past the stored count. */
int xtensa_paniclog_frame_words(char *buf, int bufsize, int line)
{
    volatile struct frame_rtc *fr =
        (volatile struct frame_rtc *)XT_FRAME_RTC_BASE;
    uint32_t base;
    uint32_t n;
    int len;
    int i;

    if (buf == NULL || bufsize <= 0 || line < 0)
        return 0;
    if (fr->magic != XT_FRAME_RTC_MAGIC || fr->count == 0)
        return 0;

    base = (uint32_t)line * 4u;
    if (base >= fr->count)
        return 0;

    n = fr->count - base;
    if (n > 4)
        n = 4;

    len = snprintf(buf, bufsize, "w%lu:", (unsigned long)base);
    for (i = 0; (uint32_t)i < n && len < bufsize - 12; i++)
        len += snprintf(buf + len, bufsize - len, " %08lx",
                        (unsigned long)fr->word[base + (uint32_t)i]);
    return len;
}

/* Store ps, epc1..epc4, depc and the cache-error detail. regs[] is the fixed
 * order documented in idf-app/panic_cdc.c: ps, epc1, epc2, epc3, epc4, depc,
 * cache_vaddr, cache_size. Words that could not be read legally are zero and
 * the line says so, rather than presenting a zero as if it were a real value. */
void xtensa_paniclog_frame_regs(const unsigned long *regs, int count)
{
    volatile struct frame_rtc *fr =
        (volatile struct frame_rtc *)XT_FRAME_RTC_BASE;

    if (regs == NULL || count < 1)
        return;
    if (fr->magic != XT_FRAME_RTC_MAGIC)
    {
        /* Nothing has been retained yet; the register slots travel with the
         * frame, so without one there is nothing to correlate them against. */
        fr->magic = XT_FRAME_RTC_MAGIC;
        fr->seq = 0;
        fr->count = 0;
    }

    fr->ps = (uint32_t)regs[0];
    if (count > 1) fr->epc1 = (uint32_t)regs[1];
    if (count > 2) fr->epc2 = (uint32_t)regs[2];
    if (count > 3) fr->epc3 = (uint32_t)regs[3];
    if (count > 4) fr->epc4 = (uint32_t)regs[4];
    if (count > 5) fr->depc = (uint32_t)regs[5];
    if (count > 6) fr->cache_vaddr = (uint32_t)regs[6];
    if (count > 7) fr->cache_size = (uint32_t)regs[7];
    fr->regs_magic = XT_REGS_RTC_MAGIC;
}

/* One line with the register slots. Returns 0 when nothing was stored. Slots that
 * could not be read at the exception level are printed as ----- so an absent
 * value cannot be mistaken for a legitimate address of 0. */
int xtensa_paniclog_frame_regs_text(char *buf, int bufsize)
{
    volatile struct frame_rtc *fr =
        (volatile struct frame_rtc *)XT_FRAME_RTC_BASE;

    if (buf == NULL || bufsize <= 0)
        return 0;
    if (fr->magic != XT_FRAME_RTC_MAGIC || fr->regs_magic != XT_REGS_RTC_MAGIC)
        return 0;

    return snprintf(buf, bufsize,
                    "ps %08lx sp %08lx epc1 %08lx epc2 %08lx epc3 %08lx epc4 %08lx depc %08lx",
                    (unsigned long)fr->ps,
                    /* Word 4 of the frame is XtExcFrame.a1, the stack pointer of
                       the interrupted context. It belongs on this line because
                       the fault address is usually a value inside it, and
                       comparing the two is what distinguishes a hijacked return
                       address from a stray pointer. */
                    (unsigned long)fr->word[4],
                    (unsigned long)fr->epc1, (unsigned long)fr->epc2,
                    (unsigned long)fr->epc3, (unsigned long)fr->epc4,
                    (unsigned long)fr->depc);
}

int xtensa_paniclog_frame_cache(char *buf, int bufsize)
{
    volatile struct frame_rtc *fr =
        (volatile struct frame_rtc *)XT_FRAME_RTC_BASE;

    if (buf == NULL || bufsize <= 0)
        return 0;
    if (fr->magic != XT_FRAME_RTC_MAGIC || fr->regs_magic != XT_REGS_RTC_MAGIC)
        return 0;
    /* Zero means the latch reported nothing, not an access at address 0. */
    if (fr->cache_vaddr == 0)
        return 0;

    return snprintf(buf, bufsize, "cache vaddr %08lx size %lu",
                    (unsigned long)fr->cache_vaddr,
                    (unsigned long)fr->cache_size);
}

/* ---------------------------------------------------------------------------
 * The frame's caller chain, in RTC slow.
 *
 * The frame dump says what the CPU was doing; it cannot say who put it there.
 * IDF prints exactly that in its panic handler, but printing needs a transport
 * and this context has none, so the walk is taken here and stored instead.
 *
 * It is IDF's walk, not a reconstruction of one. esp_backtrace_get_next_frame
 * (IRAM, 0x40375808 in this image) reads frame(i-1)'s base-save area from the
 * four words below SP: the return PC at sp-16 and the caller's SP at sp-12.
 * Those words exist for every exception because xtensa_vectors.S defines
 * XT_DEBUG_BACKTRACE unconditionally -- checked in the linked image, which
 * carries `s32e a0, a5, -16` and `s32e a1, a5, -12` at 0x40374000 -- and the
 * seeding is IDF's too, {pc = f->pc, sp = f->a1, next_pc = f->a0} from
 * panic_arch.c:337.
 *
 * The care taken here is about what happens when the chain is wrong, which is
 * the case we are looking for. Reading through a bad SP faults, and a fault
 * inside the panic handler is a third exception with no handler left: the chip
 * stops, and with it the store. So each SP is validated with IDF's
 * esp_stack_ptr_is_sane *before* the step that reads below it, and every frame
 * is committed before the next one is attempted. A walk that dies part-way
 * leaves the part that succeeded, and `count` says how far it got.
 *
 * PCs are stored after esp_cpu_process_stack_pc, which folds the windowed ABI's
 * top-two-bit window increment back into the address space and steps off the
 * return address. That is why 0x8002e4bc in the frame is a real ROM address
 * (0x4002e4b9) and not the garbage it looks like.
 *
 * 0x50001600 is past the frame record (which ends at 0x500014bc) and inside the
 * 8 KB RTC-slow bank, which runs to 0x50002000. Word stores only, as everywhere
 * else in this bank.
 * --------------------------------------------------------------------------- */
#define XT_BT_RTC_BASE   0x50001600u
#define XT_BT_RTC_MAGIC  0x43544252u   /* "RBTC" */
#define XT_BT_FRAMES     16
#define XT_BT_ID_WORDS   8             /* 32 characters of build id, including NUL */

struct bt_rtc
{
    uint32_t magic;
    uint32_t count;
    uint32_t pc[XT_BT_FRAMES];
    uint32_t sp[XT_BT_FRAMES];
    /* Which image this crash belongs to. The boot report otherwise prints the
       version of the image that is *asking*, and after a flash those are two
       different builds -- a distinction that has already been confused once,
       because a post-flash reset cannot reach this board while Rockbox owns the
       USB pads. */
    uint32_t crash_id[XT_BT_ID_WORDS];
    uint32_t bt_magic;
    /* The loaded image's API handover as it stood at the fault: address of the
       image's codec pointer, the pointer, and the enc_pcmbuf_read slot. */
    uint32_t api[3];
    uint32_t api_magic;
};

void xtensa_paniclog_backtrace(const unsigned long *pcs, const unsigned long *sps,
                               int n, const char *build)
{
    volatile struct bt_rtc *bt = (volatile struct bt_rtc *)XT_BT_RTC_BASE;
    int i;

    if (pcs == NULL || sps == NULL || n < 1)
        return;
    if (n > XT_BT_FRAMES)
        n = XT_BT_FRAMES;

    if (bt->magic != XT_BT_RTC_MAGIC)
        bt->count = 0;
    bt->magic = XT_BT_RTC_MAGIC;

    if (build != NULL)
    {
        char id[XT_BT_ID_WORDS * 4];
        int j;

        memset(id, 0, sizeof id);
        for (j = 0; j < (int)sizeof id - 1 && build[j]; j++)
            id[j] = build[j];
        for (j = 0; j < XT_BT_ID_WORDS; j++)
            bt->crash_id[j] = *(uint32_t *)&id[j * 4];
    }

    for (i = 0; i < n; i++)
    {
        bt->pc[i] = (uint32_t)pcs[i];
        bt->sp[i] = (uint32_t)sps[i];
        /* Commit per frame: if the next read faults, this one is already kept. */
        bt->count = (uint32_t)(i + 1);
    }
    bt->bt_magic = XT_BT_RTC_MAGIC;
}

/* Five PCs per line, oldest first, so line 0 holds the faulting frame. The
 * values are the raw slots: entry 0 is the frame's real PC, and every entry
 * after it is a windowed-ABI return address, which carries the window increment
 * in bits 10-11 and points one instruction past the call. So 0x8002e4bc is
 * 0x4002e4b9 in ROM once the increment is folded out and the instruction
 * stepped back -- exactly what esp_cpu_process_stack_pc does, and deliberately
 * NOT applied here. A slot that resolves to a literal pool rather than to a
 * function is evidence that the chain has gone insensible, and the unprocessed
 * value is what makes that visible. */
int xtensa_paniclog_backtrace_text(char *buf, int bufsize, int line)
{
    volatile struct bt_rtc *bt = (volatile struct bt_rtc *)XT_BT_RTC_BASE;
    uint32_t base;
    uint32_t n;
    int len;
    int i;

    if (buf == NULL || bufsize <= 0 || line < 0)
        return 0;
    if (bt->magic != XT_BT_RTC_MAGIC || bt->bt_magic != XT_BT_RTC_MAGIC ||
        bt->count == 0)
        return 0;

    base = (uint32_t)line * 5u;
    if (base >= bt->count)
        return 0;

    len = snprintf(buf, bufsize, "bt%d:", line);

    n = bt->count - base;
    if (n > 5)
        n = 5;

    for (i = 0; (uint32_t)i < n && len < bufsize - 12; i++)
        len += snprintf(buf + len, bufsize - len, " %08lx",
                        (unsigned long)bt->pc[base + (uint32_t)i]);
    return len;
}

/* One line with the build id of the crash, as stored by the handler. Returns 0 when absent. */
int xtensa_paniclog_crash_build(char *buf, int bufsize)
{
    volatile struct bt_rtc *bt = (volatile struct bt_rtc *)XT_BT_RTC_BASE;
    char id[XT_BT_ID_WORDS * 4];
    int j;

    if (buf == NULL || bufsize <= 0)
        return 0;
    if (bt->magic != XT_BT_RTC_MAGIC || bt->bt_magic != XT_BT_RTC_MAGIC)
        return 0;

    for (j = 0; j < XT_BT_ID_WORDS; j++)
        *(uint32_t *)&id[j * 4] = bt->crash_id[j];
    id[sizeof id - 1] = '\0';

    if (id[0] == '\0')
        return 0;

    return snprintf(buf, bufsize, "crashed %s", id);
}

/* Store the image's API handover as the handler found it. Written after the
 * backtrace, so a fault during this read still leaves the frames. */
void xtensa_paniclog_api_handover(const unsigned long *words, int n)
{
    volatile struct bt_rtc *bt = (volatile struct bt_rtc *)XT_BT_RTC_BASE;
    int i;

    if (words == NULL || n < 1)
        return;
    if (n > 3)
        n = 3;

    for (i = 0; i < n; i++)
        bt->api[i] = (uint32_t)words[i];
    bt->api_magic = XT_BT_RTC_MAGIC;
}

/* slot = address of the image's codec pointer, ptr = what that pointer holds
 * (the firmware's ci), enc_read = the word at ci+136 the image called. With a
 * healthy handover ptr is the firmware ci and enc_read is a code address; the
 * fault says otherwise, so which of the three differs from the boot-time trace
 * is the answer. */
int xtensa_paniclog_api_handover_text(char *buf, int bufsize)
{
    volatile struct bt_rtc *bt = (volatile struct bt_rtc *)XT_BT_RTC_BASE;

    if (buf == NULL || bufsize <= 0)
        return 0;
    if (bt->magic != XT_BT_RTC_MAGIC || bt->api_magic != XT_BT_RTC_MAGIC)
        return 0;

    return snprintf(buf, bufsize, "api slot %08lx ptr %08lx enc_read %08lx",
                    (unsigned long)bt->api[0], (unsigned long)bt->api[1],
                    (unsigned long)bt->api[2]);
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

/* Send the surviving crash record over the CDC console, through logf().
 *
 * The hardware-info screen used to be the only readout, and this build does not
 * expose that menu, so a host has to be able to fetch it. Each logf() call is one
 * message: it appends to logfbuffer and flushes through usb_serial_send(), which
 * queues at most TRANSIT_BUFFER_SIZE (32) bytes per transfer and lets the driver's
 * completion handler pick up the rest. Four short messages therefore total well
 * under 200 bytes, inside the 512-byte ring, and nothing loops on the endpoint.
 *
 * The previous version of this function pushed all forty trail entries in a loop,
 * roughly 1.4 KB, with no guarantee that anyone had the port open. The ring cannot
 * advance while a transfer is outstanding, so it hammered an endpoint that was
 * still holding the first chunk and the board reboot-looped. That is why the depth
 * here is three and the payload is bounded: the trail's full ring stays on the
 * screen and in SRAM, where it costs nothing.
 *
 * Call this from thread context. It is called once, from the button tick, which is
 * tick_task_thread -- ordinary thread context, the same place every other logf()
 * caller runs, so there is no second producer racing the filesystem's messages.
 */
void xtensa_crash_report_logf(void)
{
    char line[96];
    int len;
    int back;
    unsigned long exc, pseudo, count;

    len = xtensa_paniclog_text(line, sizeof line);
    if (len > 0)
        logf("rb panic: %s", line);
    else
        logf("rb panic: none, reset %lu, rtc %lu",
             xtensa_paniclog_reset_reason(), xtensa_paniclog_rtc_count());

    logf("rb reset %lu rtc %lu trail %lu",
         xtensa_paniclog_reset_reason(), xtensa_paniclog_rtc_count(),
         (unsigned long)(bc_magic == BC_MAGIC ? bc_next : 0));

    /* The register slots first, because they are the only thing that can name
     * the faulting instruction: on a double exception the original EPC1 was
     * shifted to EPC2 and the frame the handler sees belongs to the debug
     * context. The frame words follow for context. Both together are roughly 430
     * bytes, inside the 512-byte ring that overflowed when this reported forty
     * trail entries; the trail itself stays on the debug page and in SRAM. */
    len = xtensa_paniclog_frame_regs_text(line, sizeof line);
    if (len > 0)
        logf("rb %s", line);

    len = xtensa_paniclog_frame_cache(line, sizeof line);
    if (len > 0)
        logf("rb %s", line);

    (void)exc;
    (void)pseudo;
    xtensa_paniclog_frame_info(&exc, &pseudo, &count);

    /* The caller chain: five raw PCs per line, three lines, so fifteen of the
     * sixteen kept. Six was not enough -- the exception handler runs on the
     * faulting thread's own stack, so its frames occupy the first few slots and
     * the interrupted thread's frames only appear above them.
     *
     * The whole thing is bounded on purpose: usb_serial_send() has a 512-byte
     * ring drained by transfers the host reads, and a payload that overruns it
     * does not just lose characters -- the ring cannot advance while a transfer
     * is outstanding, which is how an earlier version of this report put the
     * board into a reboot loop. Panic text, reset line, register line with the
     * interrupted SP, three backtrace lines and the build line come to roughly
     * 400 bytes; the cache line only appears when the latch reported something. */
    for (back = 0; back < 3; back++)
    {
        len = xtensa_paniclog_backtrace_text(line, sizeof line, back);
        if (len == 0)
            break;
        logf("rb %s", line);
    }

    /* The handover as the fault found it. Compare with the codec-load trace's
       line, which printed the same slot healthy: this is the same three words
       read at the moment of the fault. */
    len = xtensa_paniclog_api_handover_text(line, sizeof line);
    if (len > 0)
        logf("rb %s", line);

    /* Which image is reporting, and which one crashed. They differ whenever a
     * flash happened without the chip restarting -- the reset pulse cannot reach
     * a board whose pads the running image gave to the DWC core -- and a backtrace
     * resolved against the wrong build is worse than no backtrace at all. */
    {
        const char *now = xtensa_build_id_running();
        int clen = xtensa_paniclog_crash_build(line, sizeof line);

        if (clen > 0 && now != NULL && strcmp(line + 8, now) == 0)
            logf("rb build %s (same)", now);
        else if (clen > 0)
            logf("rb build now %s, %s", now ? now : "?", line);
        else if (now != NULL)
            logf("rb build now %s", now);
    }
}
