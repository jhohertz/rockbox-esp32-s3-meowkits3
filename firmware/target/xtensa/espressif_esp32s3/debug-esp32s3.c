/***************************************************************************
 * ESP32-S3 debug menu.
 *
 * There is no I/O port register block to dump, so dbg_ports() has nothing to
 * show. dbg_hw_info() is the bring-up report card: the display is the only
 * console this board has, so every subsystem that can be checked without
 * hardware-in-the-loop gets a line.
 *
 * The order is the order a failure is most likely to appear in: the core and
 * its cache first, then the external MMU, then the I2C devices, which are the
 * slowest thing on the bus and the easiest thing to get wrong, then storage
 * and USB. The rows that reported Rockbox's own startup - PSRAM bring-up,
 * IROM arming, the internal SRAM budget - went with the bare-metal startup;
 * under ESP-IDF those subsystems belong to IDF and report themselves.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "paniclog.h"
#include "kernel.h"
#include "list.h"
#include "xtensa.h"
#include "cache-xtensa.h"
#include "i2c.h"
#include "i2c-target.h"
#include "pca9557.h"
#include "es8311.h"
#include "es7210.h"
#include "axp173.h"
#include "i2s-esp32s3.h"
#include "tick.h"
#include "core_alloc.h"
#include <stdio.h>
#include <string.h>

/* The arena rows below read buflib's block map directly, which means taking the
 * mempool backend's internal layout. buflib_mempool.h refuses to be included
 * except through buflib.h, and this is only meaningful for that backend, so the
 * whole instrument is gated on both. core_ctx is the single context core_alloc()
 * uses; core_alloc.h deliberately does not expose it. Going through
 * buflib_get_num_blocks()/core_print_block_at() would require BUFLIB_DEBUG_PRINT,
 * which also makes core_alloc.c allocate a 112-byte test block at startup -- and
 * enabling that flag silenced codec playback on this hardware. Walking the
 * context directly needs no flag and no extra allocation, and the block header
 * carries the ops pointer, which is what names an owner. */
#include "buflib.h"
#if CONFIG_BUFLIB_BACKEND == BUFLIB_BACKEND_MEMPOOL
#include "buflib_mempool.h"
extern struct buflib_context core_ctx;
#define ARENA_MAP_AVAILABLE 1
#endif


#include "esp_pm.h"      /* esp_pm_config_t, esp_pm_get_configuration() */
#include "esp_timer.h"  /* esp_timer_get_time() -- wall clock, runs in sleep */
#include "esp_cpu.h"     /* esp_cpu_get_cycle_count() -- core clock, does not */

static inline uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

/* rsr.prid: [3:0] core revision, [11:8] core variant, [15:12] architecture
 * revision. rsr.revision does not exist on this core config. */
static unsigned int core_prid(void)
{
    unsigned int v;

    asm volatile ("rsr.prid %0" : "=r"(v));
    return v;
}

/*
 * The external cache MMU is a 512-entry table of 32-bit values at 0x600C5000;
 * bit 14 is the "invalid" flag. IDF arms the entries for flash and PSRAM at
 * startup, so this counts what the platform actually mapped - which is what
 * decides whether the audiobuffer and the frame buffers are reachable.
 */
static unsigned int mmu_mapped_pages(void)
{
    volatile const uint32_t *table =
        (volatile const uint32_t *)0x600C5000;
    unsigned int n = 0;

    for (int i = 0; i < 512; i++)
        if (!(table[i] & (1u << 14)))
            n++;

    return n;
}

/*
 * Every device on the bus, in the order the target brings them up. A read of
 * one register is enough to know whether the chip is there: the command-list
 * controller reports a NACK as a failed transaction rather than hanging.
 */
#ifdef ARENA_MAP_AVAILABLE
/* Aggregate the arena by owner. A block's BUFLIB_IDX_OPS word is the address of
 * the struct buflib_callbacks it was allocated with, so grouping blocks by that
 * address groups them by the code that holds them -- including owners whose
 * callbacks are static, or (like dircache) are a field inside a larger struct
 * and therefore not a named symbol at all. NULL is the group for core_alloc(),
 * which registers no callbacks. Resolve an address against the build's symbol
 * table with nm and the link map; for a struct field, subtract the symbol's
 * base to get the offset.
 *
 * buflib stores each block's length in its first word, in units of
 * union buflib_data, including BUFLIB_NUM_FIELDS of per-block metadata. Free
 * space is tagged only where it precedes alloc_end; the pool's tail beyond
 * alloc_end is untagged and simply absent from the walk, so held +
 * core_available() is the pool -- do not expect the walk to show free bytes. */
#define ARENA_OWNERS 12

struct arena_owner
{
    unsigned long ops;
    long held_kb;
    int blocks;
};

struct arena_stats
{
    long held_kb;
    long pool_kb;
    int blocks;
    int owners;
    struct arena_owner owner[ARENA_OWNERS];
};

static void arena_stats(struct arena_stats *st)
{
    union buflib_data *b = core_ctx.buf_start;
    memset(st, 0, sizeof(*st));
    /* buf_start..last_handle is the whole pool, in bytes */
    st->pool_kb = ((char *)core_ctx.last_handle - (char *)core_ctx.buf_start)
                  / 1024;

    while (b < core_ctx.alloc_end)
    {
        long val = b[BUFLIB_IDX_LEN].val;
        long units = val < 0 ? -val : val;

        if (units <= 0)
            break;                    /* a zero-length block means damage */

        st->blocks++;
        if (val > 0)
        {
            long foot = val * (long)sizeof(union buflib_data);
            unsigned long ops = (unsigned long)(uintptr_t)b[BUFLIB_IDX_OPS].ops;
            long kb = foot / 1024;
            int slot = -1;

            st->held_kb += kb;
            for (int i = 0; i < st->owners; i++)
            {
                if (st->owner[i].ops == ops)
                {
                    slot = i;
                    break;
                }
            }
            if (slot < 0 && st->owners < ARENA_OWNERS)
            {
                slot = st->owners++;
                st->owner[slot].ops = ops;
            }
            if (slot >= 0)
            {
                st->owner[slot].held_kb += kb;
                st->owner[slot].blocks++;
            }
        }

        b += units;
    }

    for (int i = 0; i < st->owners; i++)
        for (int j = i + 1; j < st->owners; j++)
        {
            if (st->owner[j].held_kb > st->owner[i].held_kb)
            {
                struct arena_owner t = st->owner[i];
                st->owner[i] = st->owner[j];
                st->owner[j] = t;
            }
        }
}
#endif /* ARENA_MAP_AVAILABLE */


struct i2c_device {
    unsigned char addr;
    unsigned char reg;
    const char *name;
};

static const struct i2c_device i2c_devices[] =
{
    { PCA9557_ADDR, 0x01, "PCA9557 expander" },
    { ES8311_I2C_ADDR, 0x00, "ES8311 DAC" },
    { ES7210_I2C_ADDR, 0x00, "ES7210 ADC" },
    { AXP173_ADDR, 0x12, "AXP173 PMU" },
    { 0x51, 0x02, "PCF8563 RTC" },
    { FT6x06_ADDR, 0x02, "FT6336 touch" },
};

static bool i2c_answers(const struct i2c_device *dev)
{
    unsigned char val;

    return i2c_readmem(dev->addr, dev->reg, &val, 1) == 1;
}

/* Total core cycles since the PM row started sampling, and how many times the
 * sampling ran. Sampled from the tick thread rather than differenced between
 * screen reads: the cycle counter is 32 bits and wraps well inside the interval
 * this row is meant to cover. See the INFO_PM case below. */
static uint64_t cycle_total;
static unsigned int cycle_samples;

static void pm_cycle_tick(void)
{
    static uint32_t last;
    uint32_t now = esp_cpu_get_cycle_count();

    /* One tick between samples, so this modular difference is exact -- at
     * the fastest clock on this chip a tick is 2.4M cycles, three orders of
     * magnitude below the wrap point. */
    cycle_total += (uint32_t)(now - last);
    last = now;
    cycle_samples++;
}

enum
{
    INFO_CPU,
    INFO_CORE,
    INFO_CACHE,
    INFO_MMU,
    /* What a plugin's malloc() actually has to work with.
     * firmware/buflib_malloc.c routes plugin allocation to core_alloc(), not to
     * the plugin buffer, so the 524,288-byte pluginbuf says nothing about
     * whether a plugin can allocate -- its .data and .bss come from there, and
     * its heap comes from here. Two numbers are printed because they answer
     * different questions: core_available() is everything still free,
     * core_allocatable() is the largest single block that can be had, and the
     * difference between them is fragmentation. The second is what decides
     * whether something like doom can run, since its screens are allocated one
     * at a time at LCD_WIDTH*LCD_HEIGHT each (76,800 here, four of them at a
     * screen wipe).
     *
     * Read this before playback starts, not during. audio_reset_buffer()
     * (apps/playback.c:1083) takes the arena through core_alloc_maximum() as
     * soon as a track is buffered, so both numbers legitimately read 0 while
     * playing -- the pool is parked in the audiobuffer, not lost, and that
     * block is shrinkable, which is how anything else gets space back. It
     * stays parked after the player stops as well, because audio_stop() does
     * not release it; only the hard-stop path (apps/playback.c:4005) or the
     * shrink callback giving up does. A reading that is lower than the boot
     * value is therefore the player holding its buffer, and a reading that
     * is lower still after a plugin ran is that plugin's own allocations
     * still outstanding. */
    INFO_ARENA,
/* These rows exist only when the block walk they call is compiled in.
 * Keeping them in the enum without their cases would be a crash: the
 * switch's default branch handles the per-I2C-device rows, and these
 * entries sit *below* INFO_I2C_BASE, so an unmatched item indexes
 * i2c_devices with a negative number. */
#ifdef ARENA_MAP_AVAILABLE
    /* Where the arena's bytes actually are. core_allocatable() says how much is
     * left; it cannot say who holds the rest, and on this target roughly 1.6 MB
     * of the 4.25 MB arena is gone from the moment the player starts, which is
     * too much to be the directory cache alone. The mempool backend lays the
     * arena out as blocks whose first word is a signed length in units of
     * union buflib_data (positive = allocated, negative = free), and
     * core_print_block_at() exposes exactly that, so the map can be read without
     * reaching into buflib internals. Costs one O(n) walk per block; n is tens,
     * and it runs when the row is drawn, not continuously. */
    INFO_ARENA_MAP,
    /* The three largest allocated blocks. Sizes identify owners well enough to
     * act on: the directory cache reserves DIRCACHE_LIMIT, the playlist fileref
     * table is max_files_in_playlist * sizeof(entry), and playback's file buffer
     * is the block audio_reset_buffer() parks. A block that matches none of those
     * is the unknown one, and its size is what narrows the search. */
    INFO_ARENA_TOP,
    /* One row per large block, largest first: its data size, the address of the
     * buflib_callbacks it was allocated with, and its pin count. The callbacks
     * address is the identifier -- it resolves through the build's symbol table
     * to the exact struct, and therefore to the file that owns the allocation. */
    INFO_ARENA_TOP1,
    INFO_ARENA_TOP2,
    INFO_ARENA_TOP3,
#endif /* arena rows */
    INFO_I2C_BASE,          /* one entry per device in i2c_devices */
    INFO_USB = INFO_I2C_BASE + ARRAYLEN(i2c_devices),
    INFO_PANIC,
    INFO_RESET,
    /* Cumulative transmit-path housekeeping: sessions started, descriptor
     * errors, recoveries, and slots armed with silence. Together they say
     * whether the ring is being starved and recovered. Counted always rather
     * than behind a probe, because the moment worth catching is shorter than
     * any sampling interval. */
    INFO_TXSTAT,
    /* The same idea for the capture path, and it exists because the two ways
     * a recorder can go silent look identical from the UI: the reader can be
     * asking and getting nothing (no clocks reaching the ADC, so timeouts),
     * or asking and being refused (a bad configuration, so an error code).
     * The rate is printed because it is the one number that says whether the
     * receiver is running at what the caller asked for -- pitch_detector
     * requests 8 kHz, the file recorder requests whatever Settings says, and
     * playback requests the file's rate. Reading it is free; the counters
     * are incremented per read. */
    INFO_RXSTAT,
    /* What the codec itself reports. The transmit counters can say the
     * transport is healthy while nothing audible comes out, because the
     * chip's state is only knowable by asking it over I2C. Costs 14 polled
     * reads, so it is drawn when the screen is opened rather than sampled
     * continuously. */
    INFO_CODEC,
#ifdef HAVE_RECORDING
/* The ADC's own gain, bias and clock state, read back off the chip. The
 * captured level is the only consumer-visible evidence, and it cannot say
 * whether a setting was written wrongly, written and ignored, or applied to
 * an input that is not the mic -- the registers can. Costs six polled reads
 * when the screen opens. */
    INFO_ADC,
#endif
    /* The AXP173 coulomb counters. They are enabled and cleared at boot by
     * axp173_init(), so a freshly booted board legitimately reads 0/0 -- which
     * is also what a failed I2C read produces, and what a clear that dropped
     * the enable bit produces. Hence the control byte on the same row: it is
     * the only thing on screen that separates "nothing has happened" from
     * "counting is off".
     *
     * The mAh figures come from the counter's own arithmetic -- one LSB per 0.5
     * mA per ADC sample, so 0.02 mA.s at 25 Hz and 180,000 LSB per mAh -- not
     * from the datasheet, which says nothing about the counter's resolution.
     * The raw counts are printed beside them so the scale can be confirmed
     * rather than trusted: read the row, note the interval, and divide the
     * integrated mA-hours (44 mA is dark idle) by the delta. The reference
     * driver's variant of this formula multiplies by 65536 and is wrong by four
     * orders of magnitude, which is exactly what made this row read
     * implausibly.
     *
     * Reading it costs one 8-byte burst read over I2C, drawn when the screen
     * opens.
     */
    INFO_CC,
    /* What the charger was actually told to do. REG 33H carries the enable,
     * the target voltage and the current setting in three separate fields,
     * and a whole-register write that omits the target silently lowers the
     * charge ceiling from 4.2 V to 4.1 V -- which is how a board that had
     * been charging overnight came to read 83 % and measure ~4.0 V.
     * "charging" here is REG 01H bit 6, which reads 0 both when idle and
     * when the cell is finished (this chip has no charge-done bit), and
     * "overtemp" is bit 7 of the same register, which is what suspends
     * charging on this chip's own temperature. */
    INFO_CHG,
    /* The TS pin. Whether a thermistor is wired on this board is unverified;
     * the pin's drive current and mode live in REG 84H alongside the ADC
     * sample rate, and the chip's charge/discharge temperature thresholds
     * act on this input, so a floating reading is worth seeing rather than
     * arguing about. Prints the raw ADC code. */
    INFO_TS,
    /* Whether power management is configured, and what the core actually did.
     * esp_pm_dump_locks() is the obvious tool and is unusable here: it takes a
     * newlib FILE*, and Rockbox shadows stdio.h with its own libc. So this
     * measures the effect instead: esp_cpu_get_cycle_count() counts core clocks
     * and stops while the core is powered down in light sleep
     * (CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP); esp_timer_get_time() is wall
     * time and keeps running.
     *
     * The first version of this row divided one by the other and printed an
     * average MHz. It read zero on every visit -- not "the core slept forever"
     * but the numerator never moving, because a local declaration of the
     * accumulator shadowed the file-scope one that pm_cycle_tick() increments.
     * A derived ratio that can silently be zero is worse than no number, so the
     * row now prints its inputs and nothing else: the sample count, the cycles
     * accumulated since the previous read, and the wall microseconds since it.
     * Those three also separate the ways this can fail, which a single ratio
     * could not:
     *
     *   samples advancing, cycles zero -> the cycle counter itself reads zero,
     *                                     i.e. a sleep does not put it back
     *   samples stuck at zero           -> the tick task is not running
     *
     * The first read arms the sampling and reports no interval; the next
     * covers the time since, which is the dark-idle window worth measuring:
     * open the screen, let it go dark for a few minutes, come back and read
     * it again. */
    INFO_PM,
    /* The retained crash record, in pieces. A hardware exception goes to IDF's
     * handler, which breaks in panic_abort and resets: the pc it reports is
     * that break, so pc and epc1 are both useless as a cause. epc2 holds the
     * address the fault doubled from, and depc the address that doubled --
     * those two are what can be run through addr2line. They are rows here
     * rather than one console line because a 320-pixel row clips the tail,
     * and the tail is where epc2 sits. */
    INFO_REGS_PS,
    INFO_REGS_EPC12,
    INFO_REGS_EPC34,
    INFO_REGS_DEPC,
    INFO_REGS_WIN,
    INFO_REGS_CACHE,
    /* The bytes the core was actually handed at the faulting address. A fault
     * whose address decodes to a legal opcode in the image is a failed fetch,
     * and these two words are the proof; compare them against the image's bytes
     * at that address. */
    INFO_REGS_FAULT,
    /* The exception frame, raw. IDF hands the handler an XtExcFrame whose word
     * order is exit, pc, ps, a0, a1(sp), a2..a15, sar, exccause, excvaddr,
     * lbeg, lend -- so these six rows carry the fault's own pc and PS, the
     * return-address chain (a0, window-encoded: decode with
     * (v & 0x3fffffff) | 0x40000000), and the cause plus refused address. The
     * CSR rows above cannot show any of that: on an abort they are all zero,
     * and the only pc in them is panic_abort's break. Printed raw rather than
     * interpreted, because the handler cannot tell whether it was given an
     * XtExcFrame or an XtDbFrame -- the host decode is where that is settled.
     * */
    INFO_REGS_FRM0,
    INFO_REGS_FRM1,
    INFO_REGS_FRM2,
    INFO_REGS_FRM3,
    INFO_REGS_FRM4,
    INFO_REGS_FRM5,
    /* The crash trail, newest first. A hardware exception goes to IDF's
     * handler, prints to UART0 and resets -- it never reaches Rockbox's
     * panic(), so there is no message to show and this ring is the only
     * surviving record. Six rows is enough to see which subsystem was last;
     * the ring holds forty. */
    INFO_TRAIL_BASE,
    INFO_TRAIL_COUNT = 6,
    INFO_COUNT = INFO_TRAIL_BASE + INFO_TRAIL_COUNT
};

static const char *info_get_name(int item, void *data, char *buf, size_t bufsz)
{
    (void)data;

    if (item >= INFO_TRAIL_BASE)
    {
        int k = item - INFO_TRAIL_BASE;   /* 0 = the last thing that ran */

        if (dbg_breadcrumb_back(k, buf, bufsz) == 0)
            snprintf(buf, bufsz, "%s",
                     k == 0 ? "trail: nothing retained" : "");
        return buf;
    }


    switch (item) {
    case INFO_CPU:
        snprintf(buf, bufsz, "CPU frequency: %ld Hz", cpu_frequency);
        break;
    case INFO_CORE: {
        unsigned int prid = core_prid();
        snprintf(buf, bufsz, "Xtensa core: variant %u revision %u",
                 (prid >> 8) & 0xf, prid & 0xf);
        break;
    }
    case INFO_CACHE:
        snprintf(buf, bufsz, "D-cache line size: %u bytes",
                 cache_dcache_line_size());
        break;
    case INFO_MMU:
        snprintf(buf, bufsz, "External cache MMU: %u of 512 pages mapped",
                 mmu_mapped_pages());
        break;
    case INFO_ARENA:
        snprintf(buf, bufsz, "arena: %u free, %u allocatable",
                 (unsigned)core_available(), (unsigned)core_allocatable());
        break;
#ifdef ARENA_MAP_AVAILABLE
    case INFO_ARENA_MAP: {
        struct arena_stats st;
        arena_stats(&st);
        snprintf(buf, bufsz, "arena: %ld KB held of %ld KB, %d blocks",
                 st.held_kb, st.pool_kb, st.blocks);
        break;
    }
    case INFO_ARENA_TOP: {
        struct arena_stats st;
        arena_stats(&st);
        long named = 0;
        for (int i = 3; i < st.owners; i++)
            named += st.owner[i].held_kb;
        snprintf(buf, bufsz, "%d owners, %ld KB free, %ld KB in the rest",
                 st.owners, (long)core_available() / 1024, named);
        break;
    }
    /* One row per owner, largest first: the callbacks address, the total that
     * owner holds across all its blocks, and how many blocks that is. */
    case INFO_ARENA_TOP1: {
        struct arena_stats st;
        arena_stats(&st);
        if (st.owners > 0)
            snprintf(buf, bufsz, "owner %08lx: %ld KB in %d blk",
                     st.owner[0].ops, st.owner[0].held_kb, st.owner[0].blocks);
        break;
    }
    case INFO_ARENA_TOP2: {
        struct arena_stats st;
        arena_stats(&st);
        if (st.owners > 1)
            snprintf(buf, bufsz, "owner %08lx: %ld KB in %d blk",
                     st.owner[1].ops, st.owner[1].held_kb, st.owner[1].blocks);
        break;
    }
    case INFO_ARENA_TOP3: {
        struct arena_stats st;
        arena_stats(&st);
        if (st.owners > 2)
            snprintf(buf, bufsz, "owner %08lx: %ld KB in %d blk",
                     st.owner[2].ops, st.owner[2].held_kb, st.owner[2].blocks);
        break;
    }
#endif
    case INFO_USB:
        /* The DWC is clock-gated until usb_enable() runs, and a gated
         * peripheral reads as zero, so the gate has to be reported separately
         * from the core. GSNPSID is Synopsys' signature; 0x31304040 is the
         * DWC2 family. */
        if (!(rd(XT_PERIP_CLK_EN0) & XT_USB_CLK_EN)) {
            snprintf(buf, bufsz, "DWC2 @ 0x%08x: clock gated", XT_USB_DWC_BASE);
        } else {
            snprintf(buf, bufsz, "DWC2 @ 0x%08x: core id 0x%08x",
                     XT_USB_DWC_BASE, (unsigned)rd(OTGBASE + 0x40));
        }
        break;
    case INFO_PANIC: {
        /* The panic screen is readable for twenty seconds and then the
         * device resets, so this is the copy you get if you missed it. It is
         * the RTC-slow record, which survives the reset that clears the SRAM
         * one. */
        char msg[48];
        int n = paniclog_text(msg, sizeof msg);

        snprintf(buf, bufsz, "Prev panic: %s", n > 0 ? msg : "none");
        break;
    }
    case INFO_RESET:
        /* A reset reason of 1 (power-on) with an rtc count above zero means
         * the previous boot wrote teardown steps and then lost the chip: a
         * hang, not a clean shutdown. rtc 0 with a panic message means it
         * died before the USB teardown ran at all. */
        snprintf(buf, bufsz, "Reset %lu, rtc trail %lu",
                 paniclog_reset_reason(), paniclog_rtc_count());
        break;
    case INFO_REGS_PS:
        if (paniclog_frame_regs_part(buf, bufsz, 0) == 0)
            snprintf(buf, bufsz, "%s", "regs: none retained");
        break;
    case INFO_REGS_EPC12:
        if (paniclog_frame_regs_part(buf, bufsz, 1) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_REGS_EPC34:
        if (paniclog_frame_regs_part(buf, bufsz, 2) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_REGS_DEPC:
        if (paniclog_frame_regs_part(buf, bufsz, 3) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_REGS_WIN:
        /* The window state at fault. A fault whose PC equals the interrupted
         * stack pointer is what a window spill or underflow produces when the
         * ancestor chain is wrong, and a5 is the register the handler indexes
         * the base save area through -- a value there outside the thread's
         * stack is the signature this target has hit repeatedly. */
        if (paniclog_frame_window_text(buf, bufsz) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_REGS_CACHE:
        /* Only present when the cache-error latch reported something; a zero
         * vaddr means no refused access, not an access at address 0. */
        if (paniclog_frame_cache(buf, bufsz) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_REGS_FAULT:
        if (paniclog_frame_fault_text(buf, bufsz) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_REGS_FRM0:
    case INFO_REGS_FRM1:
    case INFO_REGS_FRM2:
    case INFO_REGS_FRM3:
    case INFO_REGS_FRM4:
    case INFO_REGS_FRM5:
        /* Absent rows are meaningful: no frame record means the crash predates
         * this recorder, or the boot was clean rather than a reboot. */
        if (paniclog_frame_words(buf, bufsz, item - INFO_REGS_FRM0) == 0)
            snprintf(buf, bufsz, "%s", "");
        break;
    case INFO_TXSTAT: {
        unsigned int sessions, err, recover, silence;

        i2s_tx_stats(&sessions, &err, &recover, &silence);
        snprintf(buf, bufsz, "TX sess %u err %u rec %u sil %u",
                 sessions, err, recover, silence);
        break;
    }
    case INFO_RXSTAT: {
        struct pcm_rx_stats s;

        pcm_rec_dma_stats(&s);
        snprintf(buf, bufsz, "RX %luHz en%d bus%d rd%d to%d e%d %uB",
                 s.rate, s.enabled, s.bus, s.reads, s.timeouts,
                 s.last_err, s.bytes);
        break;
    }
    case INFO_CODEC:
        es8311_stat(buf, bufsz);
        break;
#ifdef HAVE_RECORDING
    case INFO_ADC:
        es7210_stat(buf, bufsz);
        break;
#endif
    case INFO_CC: {
        uint32_t charge, discharge;
        int ctrl;

        axp173_coulomb_read(&charge, &discharge);
        /* The control byte is printed because the counters alone cannot
         * distinguish "nothing has happened yet" from "counting is switched
         * off": a clear written as a bare whole-register value drops the
         * enable bit, and both cases read 0. */
        ctrl = axp173_coulomb_control();
        snprintf(buf, bufsz, "CC ctrl %02X dis %lu=%dmAh chg %lu=%dmAh",
                 ctrl < 0 ? 0 : ctrl & 0xff,
                 (unsigned long)discharge, axp173_coulomb_discharge_mAh(),
                 (unsigned long)charge, axp173_coulomb_charge_mAh());
        break;
    }
    case INFO_CHG: {
        int tgt = axp173_charge_target_mV();
        int set = axp173_charge_current_setting_mA();

        snprintf(buf, bufsz, "CHG tgt %dmV set %dmA chg %s ot %s",
                 tgt < 0 ? 0 : tgt, set < 0 ? 0 : set,
                 axp173_charging() ? "yes" : "no",
                 axp173_over_temp() ? "yes" : "no");
        break;
    }
    case INFO_TS:
        snprintf(buf, bufsz, "TS adc %d", axp173_ts_temperature());
        break;
    case INFO_PM: {
        esp_pm_config_t cfg = { 0 };
        uint64_t us = esp_timer_get_time();
        unsigned int n_samp = 0;
        uint32_t d_cyc = 0, d_us = 0;

        /* The accumulator is named here only as a reader of the file-scope
         * variable: declaring it in this list would create a local that
         * shadows it, and every reading would then be zero -- which is
         * exactly the bug this row had. Both deltas print as 32-bit values;
         * three minutes at this chip's slowest clock is 1.4e8 cycles and
         * 1.8e8 microseconds, so the range covers any interval worth
         * measuring here, and the row stays clear of a 64-bit conversion in
         * a printf other targets build with narrower formats. */
        static uint64_t base_us, base_cycles;
        static unsigned int base_samples;
        static bool sampling;

        if (sampling)
        {
            d_cyc = (uint32_t)(cycle_total - base_cycles);
            d_us = (uint32_t)(us - base_us);
            n_samp = cycle_samples - base_samples;
        }

        base_us = us;
        base_cycles = cycle_total;
        base_samples = cycle_samples;

        if (!sampling)
        {
            /* tick_add_task() returns 0 on success (tick.c:59). */
            if (tick_add_task(pm_cycle_tick) != 0)
            {
                snprintf(buf, bufsz, "PM: tick_add_task failed, cannot sample");
                break;
            }
            sampling = true;
        }

        if (esp_pm_get_configuration(&cfg) == ESP_OK)
            snprintf(buf, bufsz, "PM %u/%u MHz sleep %s, %u samples "
                     "%lu cyc / %lu us",
                     cfg.min_freq_mhz, cfg.max_freq_mhz,
                     cfg.light_sleep_enable ? "on" : "off", n_samp,
                     (unsigned long)d_cyc, (unsigned long)d_us);
        else
            snprintf(buf, bufsz, "PM: esp_pm_get_configuration failed");
        break;
    }
    default: {
        int i = item - INFO_I2C_BASE;

        /* Items below INFO_I2C_BASE have no case here only if a row was added
         * to the enum without its case -- which is out of range in the other
         * direction, and reading i2c_devices at a negative index faults. */
        if (i < 0 || i >= (int)ARRAYLEN(i2c_devices)) {
            buf[0] = '\0';
            break;
        }

        snprintf(buf, bufsz, "%s @ 0x%02x: %s",
                 i2c_devices[i].name, i2c_devices[i].addr,
                 i2c_answers(&i2c_devices[i]) ? "answers" : "no answer");
        break;
    }
    }

    return buf;
}

bool dbg_hw_info(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, MODEL_NAME " hardware info", INFO_COUNT, NULL);
    info.get_name = info_get_name;

    return simplelist_show_list(&info);
}

bool dbg_ports(void)
{
    return false;
}
