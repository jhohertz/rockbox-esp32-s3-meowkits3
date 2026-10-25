/***************************************************************************
 * ESP32-S3 I2S1 transmitter + GDMA polled sink.
 *
 * Two things shape this driver:
 *
 * 1. DMA completion is signalled by the GDMA OUT_DONE interrupt. OUT_DONE fires when
 *    the last data of a descriptor has actually been transmitted to the peripheral,
 *    which is the point at which the buffer is safe to overwrite. OUT_EOF fires
 *    earlier, when the data has only been read from memory and is still sitting in the
 *    FIFO, so using EOF would let the CPU overwrite audio that is still being played.
 *
 *    The mixer's completion callback is called from the handler, which is the same
 *    arrangement every other Rockbox audio target uses. sink_lock/sink_unlock mask the
 *    interrupt so the callback is never entered while the mixer is busy elsewhere.
 *
 * 2. GDMA's descriptor address register is 20 bits wide, so descriptors have to
 *    sit inside the 0x3FC00000 window. That is where .bss is placed, so the
 *    descriptor ring and the DMA buffers are plain .bss.
 *
 *    Internal SRAM is served by the L1 D-cache, and GDMA does not go through it.
 *    Everything the DMA reads has to be written back first, and anything the DMA
 *    writes has to be invalidated before the CPU reads it back. ESP-IDF does the
 *    same thing on every descriptor it hands to a DMA engine.
 ****************************************************************************/
#include "config.h"

/* One line per transmit session, on the CDC console when a host is present. This
 * file asks for logf deliberately: the session counters below are the difference
 * between "the ring was starved and recovered" and "the FIFO was left misaligned",
 * and they are only useful if they can be read after the fact. */
#define LOGF_ENABLE
#include "logf.h"

#ifdef XT_AUDIO_TRACE
/* TEMPORARY PROBE -- off. The blocks it guards stay in i2s-xtensa.c, es8311.c/.h
 * and usb_audio.c for a later run. */
#include <stdio.h>
#endif

#include "system.h"
#include "string.h"

#include "xtensa.h"
#include "i2s-xtensa.h"
#include "gpio-xtensa.h"
#include "es8311.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "audiohw.h"
#include "kernel.h"
#include "tick.h"
#include "pcm_sampr.h"
#include "pcm_mixer.h"
#include "panic.h"

#include <esp_intr_alloc.h>
#include <soc/interrupts.h>

/* ESP-IDF's HAL layer: the register-level programming of the I2S block, computed
 * by IDF rather than by us. This is the layer the I2S driver itself calls, so it
 * covers the clock tree, the fractional modulator, the TDM/standard slot layout
 * and the WS width -- the four things this file used to hand-roll and got wrong
 * one at a time. It deliberately does NOT own GDMA, which is why the sink
 * contract and the descriptor ring below can stay exactly as they are. */
#include <hal/i2s_hal.h>
#include <hal/i2s_types.h>
#include <hal/i2s_ll.h>

/* The TX design is settled, and there is no switch here any more.
 *
 * IDF's HAL owns the transmitter's clock, slot format and channel enable; IDF's
 * esp_driver_i2s owns the receiver; Rockbox owns exactly one thing -- the TX GDMA
 * descriptor ring. The reason is structural rather than stylistic: GDMA exposes a
 * single completion slot (gdma.c:900, tx_chan->cbs.on_trans_eof) and esp_driver_i2s
 * fills it with the callback that refills dma.msg_queue, so taking that slot means
 * i2s_channel_write() blocks forever, and leaving it means no per-frame completion
 * exists. IDF 5.5 offers no alternative (no register_tx_callback, no i2s_done_cb_t).
 *
 * The attempt to take the slot over -- Stage 1b -- measured ~50% all-zero mixer
 * buffers against ~1% on this path with every TX-side metric identical, and its
 * scaffolding is deleted rather than left switched off. TX-IDF-MIGRATION.md keeps
 * the proof, the measurements, and the one variant that was never tried: ride
 * i2s_channel_write() without touching GDMA and accept IDF's buffer granularity. */


#include <esp_rom_gpio.h>
#include <esp_clk_tree.h>
#include <esp_private/esp_clk.h>
#include <esp_cpu.h>

/* The transmitter port. ESP32-S3 has no soc/i2s_periph.h and therefore no
 * I2S_NUM_1 enumerator; the port index is simply the register block, I2S1 at
 * 0x6002D000. */
#define XT_I2S_TX_PORT 1
/* The receiver lives on I2S0 (0x6000F000): the signal map gives each port a
 * single data signal, so the ADC's data line cannot share I2S1's. */
#define XT_I2S_RX_PORT 0

#ifdef HAVE_RECORDING
#include "es7210.h"
#endif

/* MCLK, SLOT_BITS, SLOT_NUM, I2S_BCLK_DIV, the pin map and the descriptor layout
 * all come from i2s-xtensa.h. */

/* XTAL is the clock source. APLL would be more accurate but needs the clock
 * tree, which Rockbox does not own. */
/* The I2S source clock, matching CLKM_CONF_CLK_SEL_PLL160M. This used to be
 * XTAL_HZ 40 MHz with clk_sel 0; the divider below divides THIS number, so the
 * source selection and the dividend have to move together or MCLK is wrong by
 * a factor of four. */
#define I2S_CLK_SRC_HZ 160000000u
#define MAX_FRACT 512
#define MAX_INTEG 256

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/* Ring of two descriptors, one per half of the ping-pong pair. */
/* GDMA takes the descriptor address from a 20-bit register based at
 * 0x3FC00000, so a descriptor in the PSRAM window loses its high bits. Plain
 * .bss is already the answer under ESP-IDF: it is placed in the internal DRAM
 * segment. The .srambss attribute named a section of Rockbox's own linker
 * script, which this path does not use - and a section name nothing declares
 * is not a request. Same for the buffers below.
 *
 * volatile: the DMA writes the descriptor back after it transmits the buffer, so the
 * owner bit changes through a store the compiler cannot see. The handler reads it to
 * decide whether a half is free, and without the qualifier that test can be hoisted.
 *
 * cur is volatile: refill() calls write_desc() from inside the ISR and sink_play()
 * calls it from thread context without taking the lock. Without the qualifier the
 * compiler may cache it across those boundaries. An earlier note here claimed the
 * interrupt mask made it safe, but sink_play takes no lock at all. */
/* aligned(32), not aligned(4): the struct type asks for a cache line each, but a
 * variable attribute overrides a type attribute, and leaving this at 4 put the
 * array on a 16-byte boundary so the two descriptors still straddled shared
 * lines. GDMA itself only requires 4-byte alignment. */
/* Four slots, and every one of them is always owned by the DMA.
 *
 * Measured, not assumed: with a two-slot ring the engine retires slot 0 and
 * fetches slot 1 within about a microsecond, while the refill that would arm it
 * runs in the out_eof interrupt and has to report the consumption, let the mixer
 * build the next frame, memcpy it and commit the line -- an order of magnitude
 * slower than the fetch it is racing. out_check_owner then makes the engine take
 * that as a descriptor error, and the error handler reset cur to 0, which put the
 * driver and the engine permanently out of phase: write_desc armed slot 0 on both
 * of its calls (hist=0,0) and slot 1 was never armed at all.
 *
 * Ring geometry, measured rather than estimated.
 *
 * Four slots, each armed with one mixer frame: ~21ms of runway at 48 kHz, and the
 * same four frames at any rate because the arm length follows the mixer's scaling.
 * This is the geometry the audio was judged best on, and it independently matches
 * the board's reference firmware (dma_buf_count = 4).
 *
 * Two facts decide depth:
 *
 * 1. OUT_DONE on this GDMA means "transferred to the peripheral", NOT "played by
 *    the DAC". The I2S FIFO holds ~256 bytes = 1.45ms, so it bridges nothing. A
 *    two-slot ring receives the DONE for slot 0 while the engine is already
 *    heading back to it (measured: fifo f0/e1); four gives the engine three more
 *    descriptors -- 17ms -- before the slot just rearmed is needed again.
 *
 * 2. Depth is NOT the lever for DSCR_ERR, and err is not a harm metric. It counts
 *    wraps of the ring, and a wrap costs one error per slot, so seven slots
 *    reported 257 where four reported 19 for comparable audible harm. That is how
 *    a depth experiment came out looking like deeper rings were worse. The actual
 *    cause was in the completion handler: a locked completion deferred the whole
 *    refill, leaving the retired slot CPU-owned in a ring the engine keeps
 *    walking. Arming unconditionally (see gdma_irq) removes that, so a long
 *    sink_lock no longer starves the ring and runway is not the constraint.
 *
 * A slot with no data yet is armed with silence and never left unowned: the
 * engine must always be permitted to take the next descriptor. */
#define XT_TX_RING 4

static volatile struct gdma_desc desc[XT_TX_RING] __attribute__((aligned(32)));
/* GDMA requires a 4-byte aligned buffer address */
/* pcm_mixer.c scales its frame by a rate multiplier: MIX_FRAME_SAMPLES * 4 bytes
 * up to 48 kHz, twice that above 48 kHz up to 96, four times above 96. The slot
 * has to be capacity for the largest frame the target's caps can ask for, which
 * is why HW_SAMPR_MAX feeds the same expression the mixer uses.
 *
 * The slot used to be MIX_FRAME_SAMPLES * 4 only, and fill() copied pend_size into
 * it with no bound. At 96 kHz every handover wrote 2048 bytes into a 1024-byte
 * slot -- 1 KB past it into the next slot, and past the array on the last one, into
 * whatever follows in PSRAM .bss -- while arm() transferred 1024 of the 2048 and
 * the mixer was told the whole frame had been played. Every other 256-frame chunk
 * vanished at the correct tempo, on top of the corruption. That is what "96 kHz
 * doesn't work well" was.
 *
 * Capacity is not arm length: what gets transferred is tx_frame_len, below. The
 * slot was also twice its frame size once, for no reason -- the upper half was
 * never written and never transferred, and that dead 4KB overflowed dram0_0_seg,
 * a region tight enough that two int diagnostic counters overflowed it by exactly
 * 8 bytes. */
#define XT_MIX_MULT(rate) ((rate) > SAMPR_96 ? 4 : ((rate) > SAMPR_48 ? 2 : 1))
#define XT_TX_SLOT (MIX_FRAME_SAMPLES * 4 * XT_MIX_MULT(HW_SAMPR_MAX))

/* The ring's *buffer* lives in PSRAM; the descriptors above stay in internal
 * DRAM. That split is what the hardware actually allows, and it is easy to get
 * backwards:
 *
 *  - esp32s3 declares SOC_PSRAM_DMA_CAPABLE and SOC_AHB_GDMA_SUPPORT_PSRAM, so
 *    GDMA can read a PSRAM buffer.
 *  - GDMA's 20-bit window (base 0x3FC00000) applies to the *descriptor address
 *    register* GDMA_TX_LINK, and to the first descriptor's address. Inside a
 *    descriptor the buffer pointer is a full 32-bit word the engine reads from
 *    memory, so PSRAM addresses are legal there.
 *  - ESP-IDF's own restriction list for this chip says transaction descriptors
 *    cannot be in PSRAM -- which is desc[], kept internal.
 *
 * Two things have to be true for a PSRAM target, and are:
 *
 *  - out_ext_mem_bk_size in conf1 is never programmed, so it holds its reset
 *    value 0 = 16-byte blocks. The PSRAM controller refuses bursts larger than
 *    64 bytes (GDMA_LL_MAX_BURST_SIZE_PSRAM), so 16 is inside the limit. If
 *    data burst is ever enabled here, this becomes a bug.
 *  - the buffer is write-back cached through the Ext_RAM window like every other
 *    address in it, so the commit_dcache_range() calls in write_desc() and
 *    fill() are what make the engine see the mixer's frames. They were defensive
 *    in DRAM; in PSRAM they are load-bearing.
 *
 * The cost is documented rather than guessed: "the bandwidth that DMA accesses
 * external RAM is very limited, especially when the core is trying to access the
 * external RAM at the same time", and this target reads PSRAM constantly (codec
 * images, dircache, tagcache). 4096 bytes leaves internal DRAM, which is the
 * point -- that region had 1,020 bytes free.
 */
static unsigned char dma_buf[XT_TX_RING][XT_TX_SLOT]
    PSRAM_BSS_ATTR __attribute__((aligned(4)));

/* cur is the slot that is free to write. When DONE fires, the slot that just
 * finished transmitting is the one retire points at, so the mixer's next frame
 * goes into cur. Both advance modulo XT_TX_RING -- with a plain xor the upper
 * slots of the ring would never be armed at all. */
/* One buffer handed down by the core and not yet placed in a descriptor. The
 * core pushes through sink_play(); the completion interrupt pulls. */
static volatile const void *pend_addr;
static volatile size_t pend_size;
static volatile int pend_valid;
/* Guards a hardware interrupt mask, which the mixer holds while the completion
 * interrupt is running, so it must not be cached in a register across the
 * read-modify-write of GDMA_TX_INT_ENA. */
static volatile int locked;
static bool playing;
/* Descriptors armed and awaiting transmission. The ISR decrements it as each one
 * retires; refill() only asks the core for a buffer when the ring has run dry,
 * because in the normal case reporting the completion causes ops.play to hand us
 * the next one before we ever need to ask. */
static volatile int queued;
/* The slot that has just been retired and may be rearmed. The engine retires
 * slots in the order we arm them, so one modulo counter tracks it. */
static volatile int done_slot;
/* Rockbox's own convention for a completion arriving inside the play lock
 * (as3525's play_callback_pending): record it, replay it on unlock. */
static volatile int cb_pending;
/* Arm length for the rate in effect: exactly one mixer frame, from the same
 * multiplier pcm_mixer.c uses, and set by i2s_set_frequency(). Never learned from
 * a handover -- a value taken from the first short frame would stop padding at
 * precisely the moment padding is needed. The silence path uses it too, so an idle
 * ring advances the clock at the same pace as a fed one. */
static volatile unsigned long tx_frame_len = MIX_FRAME_SAMPLES * 4;
/* DSCR_ERR entries with no intervening DONE. Two in a row means the engine is
 * hitting a slot the CPU owns, and restarting onto it again is a storm -- the
 * interrupt-watchdog panic measured at first play with a free-running ring. */
static volatile int dscr_streak;

/* Cumulative housekeeping, always counted rather than behind a probe. The failure
 * mode being chased lasts milliseconds, so a sampler cannot catch it; these are
 * read after the fact, from the debug screen or from the session line below.
 *   sessions  transmit sessions started
 *   err       OUT_DSCR_ERR entries (the engine reached a slot the CPU owns)
 *   recover   tx_recover() calls, each of which repeats one frame and then silence
 *   silence   slots armed with silence instead of audio
 */
static volatile unsigned int stat_sessions;
static volatile unsigned int stat_err;
static volatile unsigned int stat_recover;
static volatile unsigned int stat_silence;
/* start_tx() can run inside the GDMA handler -- refill() reaches the core's
 * completion callback, which reaches ops.play, which is sink_play, which calls
 * start_tx() for the first frame. logf() sends over CDC and needs the console to
 * drain, which is fatal with interrupts off (see the note in sink_play), so the
 * line is flagged here and printed by the tick task instead. */
static volatile bool stat_log_pending;

/* IDF interrupt handle for the GDMA OUT channel, allocated once in gdma_init().
 * Keeping the handle is what makes the allocation idempotent: i2s_init() can be
 * reached again and a second esp_intr_alloc() on the same source would fail. */
static intr_handle_t tx_intr;

/* gdma_irq() is defined further down with the rest of the transmit path, but the
 * allocation that installs it happens earlier, so it has to be named first. The
 * wrapper exists because esp_intr_alloc() invokes handlers with a void *argument.
 * Referencing gdma_irq() here is also the only thing that keeps it out of
 * --gc-sections, which is what silently deleted it before. */
void ICODE_ATTR gdma_irq(void);

static void gdma_tx_isr(void *arg)
{
    (void)arg;
    /* Nothing may print here. A printf in this handler takes the newlib stdout
       lock, lock_acquire_generic() sees it claimed from interrupt context and
       aborts the process -- measured backtrace: gdma_tx_isr -> printf ->
       _vfprintf_r -> __retarget_lock_acquire_recursive -> lock_acquire_generic
       -> abort. */
    gdma_irq();
}


/* IDF's HAL context for the transmitter. The HAL is a thin wrapper over the
 * register block, so the context is just the peripheral base address. */
static i2s_hal_context_t tx_hal;

#ifdef HAVE_RECORDING
/* The rate i2s_set_frequency() last programmed; the recording session's IDF
 * channel has to be created at this rate. Declared here because i2s_set_frequency
 * is where it is written, which precedes the receive section of this file. */
static unsigned long rx_rate;
#endif





/* Best rational approximation of freq_error / exp_freq with denominator < max.
 * Ported from ESP-IDF's hal_utils_calc_clk_div_frac_accurate. */


/* i2s_ll_tx_set_mclk's mapping from (integer, num/den) to the four registers. */

/* Flush the whole transmit data path: the GDMA channel and the I2S FIFO.
 *
 * Nothing else in this file resets either one, and both hold data across a
 * transition. link.stop halts the walk and i2s_ll_tx_stop() halts the port, but
 * the bytes already in the I2S FIFO and in the channel's internal FIFO stay where
 * they are, so the next session starts mid-frame. On 16-bit stereo a one-byte
 * offset shifts every subsequent sample, which sounds like distorted audio rather
 * than missing audio -- and it corrects itself whenever the FIFO happens to drain
 * and refill on a frame boundary, which is why the crunch comes and goes while
 * every register reads the same.
 *
 * A rate change is the other moment: the clock moves under whatever the FIFO still
 * holds, so leftover samples from the old rate are played at the new one.
 *
 * Order: stop, reset, re-program, then arm. out_rst clears the channel's state
 * machine and its FIFO; the configuration is written again afterwards rather than
 * read back, which costs nothing and makes the flush independent of whatever the
 * reset touches. The caller must not need the walk running -- this stops it. */
static void tx_flush(void)
{
    unsigned int conf;

    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* outlink.stop */
    i2s_ll_tx_stop(tx_hal.dev);

    /* I2S side: reset the transmitter and its FIFO. Both bits are self-clearing.
       tx_reset also clears the idle and hung latches, so a stale tx_hung cannot be
       reported against a session that has not run yet. */
    wr(I2S_TX_CONF, rd(I2S_TX_CONF) | TX_CONF_RESET | TX_CONF_FIFO_RESET);
    wr(I2S_TX_CONF, rd(I2S_TX_CONF) & ~(TX_CONF_RESET | TX_CONF_FIFO_RESET));

    /* GDMA side: reset the OUT channel, then restore its configuration. */
    conf = rd(GDMA_TX_CONF0);
    wr(GDMA_TX_CONF0, conf | GDMA_OUT_RST);
    wr(GDMA_TX_CONF0, (conf & ~GDMA_OUT_RST) | (1u << 4) | (1u << 2));
    wr(GDMA_TX_CONF1, (1u << 12));                      /* out_check_owner */
    wr(GDMA_TX_PERI_SEL, GDMA_TRIG_I2S1);

    wr(GDMA_TX_INT_CLR, GDMA_INT_DONE | GDMA_INT_DSCR_ERR | GDMA_INT_TOTAL_EOF);
}

/* Program the I2S port only: clock generator, slot, and the mixer frame length that
 * follows from the rate. Deliberately touches neither codec.
 *
 * Split out of i2s_set_frequency() because sink_init() needs the clock generator
 * running *before* audiohw_preinit(): the ES8311 is a slave and takes MCLK, BCLK and
 * WS from pads 15/14/13, and es8311_init() resets the chip without ever writing
 * CLK01..07 -- so anything the codec had been told about its clocks is wiped by that
 * reset. Programming the port first is what lets the chip acquire its interface from
 * something real at power-up.
 */
static void i2s_program_clock(unsigned long rate)
{
    /* Clock and slot configuration, computed by IDF's HAL.
     *
     * i2s_hal_set_tx_clock() picks the source, programs the integral and
     * fractional modulator and the BCLK divider, and pulses tx_update; it
     * replaces calc_div, set_raw_div and program_div entirely. The slot call sets
     * the sample bits, the WS width (which this file had left at zero, giving a
     * one-BCLK frame) and the TDM channel mask, and selects standard mode via the
     * TDM path.
     */
    i2s_hal_clock_info_t clk = {
        .sclk     = I2S_CLK_SRC_HZ,
        .mclk     = (uint32_t)(rate * MCLK_MULTIPLE),
        .bclk     = (uint32_t)(rate * SLOT_BITS * SLOT_NUM),
        .mclk_div = MCLK_MULTIPLE,
        .bclk_div = I2S_BCLK_DIV,
    };
    hal_utils_clk_div_t mclk_div;

    i2s_hal_set_tx_clock(&tx_hal, &clk, I2S_CLK_SRC_DEFAULT, &mclk_div);

    i2s_hal_slot_config_t slot = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
        .slot_mode      = I2S_SLOT_MODE_STEREO,
        .std = {
            .slot_mask     = I2S_STD_SLOT_BOTH,
            .ws_width      = SLOT_BITS,
            .ws_pol        = false,
            .bit_shift     = true,
            .left_align    = true,
            .big_endian    = false,
            .bit_order_lsb = false,
        },
    };

    /* Master role: this side generates BCLK and WS and feeds MCLK to both codecs. */
    i2s_hal_std_set_tx_slot(&tx_hal, false, &slot);
    i2s_hal_std_enable_tx_channel(&tx_hal);

    /* Above 48 kHz the mixer hands twice as many bytes per frame, so one slot has
     * to carry twice as much. This is the only place the rate is known, and pcm.c
     * calls it before any buffer is handed down. */
    tx_frame_len = MIX_FRAME_SAMPLES * 4 * XT_MIX_MULT(rate);

#ifdef HAVE_RECORDING
    /* The rate the recording session's IDF channel must be created at. Recorded
       here rather than in the codec section so it is set even when only the port is
       being programmed. */
    rx_rate = rate;
#endif
}

void i2s_set_frequency(unsigned long rate)
{
    i2s_program_clock(rate);

    /* The codec's clock registers belong after any chip reset, which is why they
       are not in i2s_program_clock(). */
    es8311_set_clock(rate, rate * MCLK_MULTIPLE);

    /* The ADC shares the transmitter's MCLK, so it has to be reprogrammed
     * whenever the rate changes.
     *
     * The receiver is no longer configured here. It is a full ESP-IDF I2S channel
     * created per recording session in pcm-xtensa.c, which sets its own clock and
     * slot. All this file still owes the capture path is the sample rate the
     * session must match, recorded below, and the shared clock pads. */
#ifdef HAVE_RECORDING
    es7210_set_frequency(rate);
#endif

    /* The clock has just moved under whatever the FIFO still holds. Flush so the
       next session starts on a frame boundary rather than mid-sample. */
    tx_flush();
}

static void refill(void);      /* defined with the transmit path below */
static void fill(int i, const void *src, unsigned long len);
static void i2s_stat_tick(void);  /* prints the session line in tick context */

#ifdef XT_AUDIO_TRACE
static void audio_trace_tick(void);
#endif

static void arm(int i, unsigned long len)
{
    desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
    /* The descriptor link, and the record of what was tried, because two variants were
     * conflated for a long time:
     *
     *  - next = &desc[i] (SELF-linked) is genuinely bad: the engine follows one slot
     *    forever, giving one out_dscr_err per frame (err tracked irq exactly, +1) and a
     *    noticeable lag on pause while it replayed the last buffer. Measured.
     *
     *  - next = 0 terminates the list, the encoding of imx233's `dma.next = NULL`, so the
     *    engine stops and the driver restarts deliberately. It cannot be combined with
     *    gdma_init()'s auto-writeback -- see the note there -- and it is NOT what this line
     *    does. An earlier revision of this comment claimed it was.
     *
     *  - next = &desc[(i+1) % XT_TX_RING], which is what this line sets: a forward ring,
     *    the same chain gdma_init() builds, kept live so the engine walks the slots and the
     *    owner bit gates each one. Tried as a change when clicks at frame boundaries were
     *    being chased; it produced no improvement, so that experiment was inconclusive
     *    rather than a refutation of the ring. */
    desc[i].next   = (uint32_t)(unsigned long)&desc[(i + 1) % XT_TX_RING];
    /* len 0 keeps the slot owned by the DMA with nothing to move: the engine
     * takes it, transfers nothing, and moves on. That is what makes an empty
     * slot safe rather than fatal. */
    desc[i].dw0 = XT_TX_SLOT | (len << 12) | DESC_SUC_EOF | DESC_OWNER;
}

/* Place one frame into slot i and hand it to the DMA. len 0 leaves the slot
 * owned with nothing to move, which is what keeps the ring fully owned between
 * hand-offs. */


/* Arm a slot with silence. auto-writeback means the engine walks the ring
 * whether or not we have data, so an empty slot still has to be owned and filled;
 * that is the invariant this mode buys and the price it charges. */
static void fill_silence(int i)
{
    /* Must match what fill() arms, for the same reason: a short slot completes early
     * and hands refill() a consumed-buffer event the DAC has not earned yet. */
    unsigned long len = tx_frame_len;

    stat_silence++;
    memset(dma_buf[i], 0, len);
    discard_dcache_range((const void *)&desc[i], sizeof(desc[i]));
    arm(i, len);
    commit_dcache_range(dma_buf[i], len);
    commit_dcache_range((const void *)&desc[i], sizeof(desc[i]));
}

/* Take the walk down, make every slot owned and filled, and start exactly once.
 *
 * This is the storm guard. With a free-running ring a single unowned descriptor
 * is self-sustaining: the engine reaches it, raises out_dscr_err, and the handler
 * answers with link.start -- onto the same unowned descriptor. Measured as an
 * interrupt-watchdog panic on CPU0 in ISR context at first play. Re-arming the
 * WHOLE ring before restarting is what makes the second error impossible rather
 * than unlikely, because there is then no slot the CPU owns anywhere in the
 * cycle. */
static void tx_recover(void)
{
    stat_recover++;

    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */

    for (int i = 0; i < XT_TX_RING; i++)
    {
        if (pend_valid)
        {
            fill(i, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
        }
        else
            fill_silence(i);
    }

    done_slot = 0;
    dscr_streak = 0;

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);

    /* Clear only the error bit. This runs inside the handler, whose snapshot of
     * INT_ST was taken before the ring was re-armed, so clearing DONE here could
     * discard a completion that fired in the meantime and lose a mixer pace. */
    wr(GDMA_TX_INT_CLR, GDMA_INT_DSCR_ERR);

    /* Re-arming the ring is right whether or not we are playing; starting the
     * walk is not. See the note in the DSCR_ERR branch. */
    if (playing)
        wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start */
}

static void fill(int i, const void *src, unsigned long len)
{
    unsigned long arm_len;

    /* Hard bound. With XT_TX_SLOT derived from HW_SAMPR_MAX it cannot bite while
     * the assert below holds; if the two ever diverge, the loss is audible rather
     * than a write past the ring. */
    if (len > XT_TX_SLOT)
        len = XT_TX_SLOT;

    memcpy(dma_buf[i], (const void *)src, len);

    /* Always hand the engine a whole frame.
     *
     * len is pend_size, which comes straight from mixer_pcm_callback(): *size =
     * next_size, and next_size is smaller than mix_frame_size whenever a channel
     * stopped part-way through a frame -- which is exactly what a USB underflow
     * does, over and over. The GDMA takes the slot as a transfer of len bytes, so a
     * short frame completes EARLY: the next out_done fires before the I2S has
     * drained a frame, refill() dutifully reports a consumed buffer, and the mixer
     * produces another one. Consumption then outruns the DAC clock, self-amplifying,
     * while the transmitter underruns the difference -- and an underrunning I2S TX
     * replays stale bytes rather than silence. That is a draining ring with a healthy
     * wire, correct tempo, and a rasp on top: all three of the things the USB Audio
     * page could not explain.
     *
     * Zero is the degenerate case of the same bug and completes instantly, so it is
     * covered by the same clamp: this makes fill_silence()'s invariant -- every slot
     * is owned AND full -- hold on the data path too, not just the no-buffer path.
     *
     * The reference is tx_frame_len, the mixer frame size for the rate in effect,
     * deliberately not a value learned from what the mixer handed us: a target that
     * ratchets down to the first short frame would stop padding at exactly the moment
     * it was needed -- which is how this paragraph's first draft was wrong. Sizing
     * the slot from HW_SAMPR_MAX is what lets this hold at 96 kHz as well as 48.
     */
    arm_len = tx_frame_len;
    if (len > arm_len)
        arm_len = len;    /* cannot happen while tx_frame_len tracks the mixer */

    if (len < arm_len)
        memset((unsigned char *)dma_buf[i] + len, 0, arm_len - len);

    /* Take the line from memory before modifying it, then push it back. The
     * engine wrote the owner bit clear through the bus, and our cached copy of
     * this line still has whatever we put there. */
    discard_dcache_range((const void *)&desc[i], sizeof(desc[i]));
    arm(i, arm_len);
    commit_dcache_range(dma_buf[i], arm_len);
    commit_dcache_range((const void *)&desc[i], sizeof(desc[i]));

    /* Arming a descriptor is not enough on its own, and this is the step that
     * was missing.
     *
     * outlink.park is not an error condition -- it is the engine doing what it is
     * designed to do when it reaches a descriptor the CPU still owns: it stops
     * walking the list and waits. But it does not re-examine the list on its own,
     * so a descriptor armed afterwards is invisible unless the walk is restarted.
     * That is what gdma_ll_tx_restart() is for ("restart a new outlink right
     * after the last descriptor") and what IDF's gdma_append() does on every
     * hand-off.
     *
     * Without it the very first underrun latches the transmitter off forever,
     * which is why park read 1 in every capture taken since the ring rewrite.
     *
     * Bits from gdma_reg.h: outlink.stop BIT(20), start BIT(21), restart BIT(22),
     * park BIT(23); start and restart are R/W/SC. */
    unsigned int link = rd(GDMA_TX_LINK);

    if (link & (1u << 23))
        wr(GDMA_TX_LINK, link | (1u << 22) | (1u << 21));   /* restart | start */
}


static void gdma_init(void)
{
    /* GDMA channel 0 -> I2S1 */
    wr(GDMA_TX_PERI_SEL, GDMA_TRIG_I2S1);

    /* conf0: outdscr_burst_en | out_auto_wrback -- a free-running ring.
     *
     * The two GDMA settings are a pair, not a choice, and getting them
     * mismatched is what produced the original crackle. The failure mode that
     * was actually measured: auto-writeback ON together with arm() writing
     * next = 0, on the misunderstanding that "zero terminates the link list".
     * This GDMA has no such convention -- with auto-writeback on the engine
     * follows next unconditionally, so it fetched its next descriptor from
     * address 0x00000000: garbage, owner clear, length zero. err tracked irq
     * exactly 1:1 (360 per 379 interrupts), one descriptor fault per audio
     * frame at 172/s, each one stalling the transport until the handler
     * restarted it. That stall was the crackle.
     *
     * Auto-writeback OFF was the previous design: the engine halted after each
     * descriptor and the DONE handler re-armed desc[0] and did link.start. That
     * is the imx233 model, whose DMA really does treat NULL as a stop. Its cost
     * was the second crackle: one 5.8ms descriptor in flight, and a restart gap
     * opened on the I2S bus inside every interrupt 172 times a second -- OUT_DONE
     * means "transferred to the peripheral", not "played", and the I2S FIFO is
     * only ~256 bytes (1.45ms), so nothing bridges that gap.
     *
     * The shipping combination is auto-writeback ON with next chained through all
     * four slots and every slot owned and filled at all times, guaranteed by
     * start_tx()'s preroll, the ISR's refill-the-completed-slot rule and
     * tx_recover(). Depth is what makes the refill race winnable: the engine
     * takes three more descriptors (17ms) between a slot's DONE and the moment it
     * needs that slot again. next = 0 with either setting is the combination that
     * cannot work. */
    wr(GDMA_TX_CONF0, (1u << 4) | (1u << 2));   /* outdscr_burst_en | out_auto_wrback */
    /* conf1: check the owner bit when mounting buffers */
    wr(GDMA_TX_CONF1, (1u << 12));               /* out_check_owner */

    /* Install the completion handler through ESP-IDF.
     *
     * This used to route the interrupt by hand -- writing the per-source slot
     * register at INTERRUPT_CORE0 and setting INTENABLE directly. That is the
     * bare-metal shape, and it cannot work here: IDF owns the level-1 vector
     * table and dispatches from its own allocation table, so a slot claimed this
     * way has no handler registered with it. Worse, nothing in the image
     * referenced gdma_irq(), so --gc-sections discarded the function outright
     * (verified: it was absent from the linked ELF). The hardware unmasked an
     * interrupt whose service routine had been optimised away, refill() never
     * ran, the descriptor ring starved after two buffers, and playback produced
     * neither sound nor a moving position.
     *
     * esp_intr_alloc() does both halves: it picks a slot IDF will dispatch, and
     * it takes the handler's address, which keeps the symbol alive.
     *
     * The source is ETS_DMA_OUT_CH0_INTR_SOURCE. On ESP32-S3 the GDMA sources
     * carry no "GDMA" in their name -- they are ETS_DMA_OUT_CH0_/ETS_DMA_IN_CH0_
     * -- which is worth stating because the obvious grep finds nothing.
     *
     * No ESP_INTR_FLAG_IRAM: the handler lives in the instruction-cache window
     * via ICODE_ATTR and is not guaranteed IRAM-resident, so it must not be
     * called with the flash cache disabled.
     */
    if (!tx_intr)
    {
        esp_err_t err = esp_intr_alloc(ETS_DMA_OUT_CH0_INTR_SOURCE,
                                       ESP_INTR_FLAG_LEVEL1, gdma_tx_isr,
                                       NULL, &tx_intr);
        if (err != ESP_OK)
            panicf("i2s: cannot allocate the GDMA TX interrupt (%d)", err);
    }

    /* out_eof and out_dscr_err. sink_lock/sink_unlock toggle the out_eof bit
     * from here on. */
    wr(GDMA_TX_INT_ENA, GDMA_INT_DONE | GDMA_INT_DSCR_ERR);
    wr(GDMA_TX_INT_CLR, 0xFF);

    for (int i = 0; i < XT_TX_RING; i++)
    {
        desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
        desc[i].next   = (uint32_t)(unsigned long)&desc[(i + 1) % XT_TX_RING];
        desc[i].dw0 = 0;   /* start in the CPU's hands; see sink_play() */
    }
    commit_dcache_range((const void *)desc, sizeof(desc));

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
}


void i2s_init(void)
{
    /* Peripheral clock and reset. IDF does this in periph_ctrl; entering from the
     * ROM loader means doing it here. Both halves are required -- with the reset
     * bit still asserted the block discards every write and reads back zero. */
    wr(SYSTEM_PERIP_CLK_EN0, rd(SYSTEM_PERIP_CLK_EN0) | CLK_EN_I2S1);
    wr(SYSTEM_PERIP_CLK_EN1, rd(SYSTEM_PERIP_CLK_EN1) | CLK_EN_DMA);
    wr(SYSTEM_PERIP_RST_EN0, rd(SYSTEM_PERIP_RST_EN0) & ~CLK_EN_I2S1);
    wr(SYSTEM_PERIP_RST_EN1, rd(SYSTEM_PERIP_RST_EN1) & ~CLK_EN_DMA);

    i2s_hal_init(&tx_hal, XT_I2S_TX_PORT);

    /* Route the signals through the GPIO matrix using the same ROM helper the
     * IDF driver uses, so the pad configuration matches what IDF expects. */
    esp_rom_gpio_connect_out_signal(PIN_I2S_MCLK, SIG_I2S1_MCLK_OUT, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_BCLK, SIG_I2S1O_BCK_OUT, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_WS,   SIG_I2S1O_WS_OUT, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_DOUT, SIG_I2S1O_SD_OUT, false, false);

    gpio_set_direction(PIN_I2S_MCLK, 1);
    gpio_set_direction(PIN_I2S_BCLK, 1);
    gpio_set_direction(PIN_I2S_WS,   1);
    gpio_set_direction(PIN_I2S_DOUT, 1);

    gdma_init();

#ifdef XT_AUDIO_TRACE
    /* i2s_init() can be reached again, and tick_add_task() would add a second
       copy of the same task each time. */
    static bool trace_added;

    if (!trace_added) {
        trace_added = true;
        tick_add_task(audio_trace_tick);
    }
#endif

    /* The session stats line is printed here rather than in start_tx(), which can
       be reached from the GDMA handler. One task, checked every tick, printing
       only when a session has started. */
    static bool stat_task_added;

    if (!stat_task_added) {
        stat_task_added = true;
        tick_add_task(i2s_stat_tick);
    }
}

#ifdef HAVE_RECORDING

/* The recording session's IDF channel has to be created at the rate the bus is
 * running: the codecs share one MCLK, so a receiver configured for a different
 * one would be clocked against a frame the ADC was not expecting. */
unsigned long i2s_rx_sample_rate(void)
{
    return rx_rate ? rx_rate : hw_freq_sampr[builtin_pcm_sink.caps.default_freq];
}

/* The rate the transmitter was last programmed to. rx_rate is written by
 * i2s_set_frequency(), which programs both ports from one call, so this is the
 * playback bus rate as much as the recording one -- what matters when comparing
 * against the mixer's idea of the sample rate. */
unsigned long i2s_tx_sample_rate(void)
{
    return rx_rate ? rx_rate : hw_freq_sampr[builtin_pcm_sink.caps.default_freq];
}

void i2s_tx_stats(unsigned int *sessions, unsigned int *err,
                  unsigned int *recover, unsigned int *silence)
{
    *sessions = stat_sessions;
    *err = stat_err;
    *recover = stat_recover;
    *silence = stat_silence;
}

#ifdef XT_AUDIO_TRACE
/* One line every 250 ms of everything the transmit path believes, so a garbled
 * window can be read from the CDC console instead of described.
 *
 * It runs as a tick task: interrupts enabled, never inside the GDMA handler and
 * never inside ops.play, so it cannot reorder the path it is watching. About 200
 * characters at 4 lines/s is six bulk-IN packets a second on a 16-word endpoint,
 * well inside what a DAC session already tolerates. The codec registers are read
 * once every eight lines because each read costs 1.6 ms of polled I2C.
 *
 * What the columns are for:
 *   hw vs pend/cfg  -- programmed rate against pcm's bookkeeping. If pend != cfg
 *                      while audio is wrong, nothing will repair it: apply_settings
 *                      only acts on a difference.
 *   start/act/idle  -- whether the port is started, whether its clock generator
 *                      runs, and the transmitter's own idle flag. A session with
 *                      play=1 and start=0 is a handoff that never completed.
 *   arm           -- the four descriptors' armed length as the engine sees them: bits
 *                    23:12 of dw0. The size field (bits 11:0) is always XT_TX_SLOT, so
 *                    reading it as a length is a mistake this probe briefly made and
 *                    reported 2048 for every slot at every rate.
 *   flen          -- the mixer frame size for the rate in effect. arm != flen on a slot
 *                    the engine has not retired yet is a rate change mid-session.
 *   usb/und/drop    -- what the host thinks the rate is, and whether the isochronous
 *                      side is underflowing or dropping frames.
 */
extern unsigned long usb_audio_trace_rate(void);
extern int usb_audio_trace_underflow(void);
extern int usb_audio_trace_overflow(void);
extern int usb_audio_trace_dropped(void);
extern int usb_audio_trace_pending(void);
extern int usb_audio_trace_rates(void);
extern int es8311_trace_read(int reg);

static void audio_trace_tick(void)
{
    static unsigned int div;
    static unsigned int slow;
    unsigned int lens[XT_TX_RING];
    char codec[32] = "";

    if (++div < (HZ / 4))
        return;
    div = 0;

    /* The engine clears owner bits through the bus, so read what is really there.
       Discarding is safe here: every write to desc[] is committed by fill() before
       the slot is handed over, so no CPU-side value is lost. */
    discard_dcache_range((const void *)desc, sizeof(desc));

    unsigned int own = 0;

    for (int i = 0; i < XT_TX_RING; i++) {
        own |= ((unsigned int)(desc[i].dw0 >> 31) & 1u) << i;
        lens[i] = (unsigned int)((desc[i].dw0 >> 12) & 0xfff);
    }

    if (++slow >= 8) {
        slow = 0;
        snprintf(codec, sizeof codec, " cd=%02x%02x%02x%02x%02x",
                 es8311_trace_read(0x00), es8311_trace_read(0x01),
                 es8311_trace_read(0x06), es8311_trace_read(0x07),
                 es8311_trace_read(0x0d));
    }

    logf("TRC hw=%lu pend=%d cfg=%d play=%d start=%d act=%d idle=%d "
         "ena=%08x gst=%08x link=%08x fifo=%08x gstate=%08x own=%04x "
         "arm=%u,%u,%u,%u flen=%lu done=%d str=%d pval=%d que=%d "
         "usb=%lu und=%d ovf=%d drop=%d rxq=%d rates=%d%s",
         i2s_tx_sample_rate(),
         (int)builtin_pcm_sink.pending_freq,
         (int)builtin_pcm_sink.configured_freq,
         playing ? 1 : 0,
         (rd(I2S_TX_CONF) & TX_CONF_START) ? 1 : 0,
         (rd(I2S_TX_CLKM_CONF) & CLKM_CONF_CLK_ACTIVE) ? 1 : 0,
         (rd(I2S_TX_STATE) & TX_STATE_IDLE) ? 1 : 0,
         rd(GDMA_TX_INT_ENA), rd(GDMA_TX_INT_ST), rd(GDMA_TX_LINK),
         rd(GDMA_TX_FIFO_STATUS), rd(GDMA_TX_STATE),
         own, lens[0], lens[1], lens[2], lens[3],
         tx_frame_len, done_slot, dscr_streak, pend_valid, queued,
         usb_audio_trace_rate(), usb_audio_trace_underflow(),
         usb_audio_trace_overflow(),
         usb_audio_trace_dropped(), usb_audio_trace_pending(),
         usb_audio_trace_rates(), codec);
}
#endif /* XT_AUDIO_TRACE */

/* Hand the three shared clock pads to whichever port is master right now.
 * esp_rom_gpio_connect_out_signal() just rewrites the pad's OUT_SEL, so this is
 * cheap and can be done on every mode switch. See the note in i2s_set_frequency()
 * for why the two ports cannot both drive these pads. */
static void i2s_route_clock_pads(int mclk_sig, int bck_sig, int ws_sig)
{
    esp_rom_gpio_connect_out_signal(PIN_I2S_MCLK, mclk_sig, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_BCLK, bck_sig, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_WS,   ws_sig,   false, false);
}

void i2s_rx_pads_take(void)
{
    i2s_route_clock_pads(SIG_I2S0_MCLK_OUT, SIG_I2S0I_BCK_OUT, SIG_I2S0I_WS_OUT);
}

void i2s_rx_pads_release(void)
{
    i2s_route_clock_pads(SIG_I2S1_MCLK_OUT, SIG_I2S1O_BCK_OUT, SIG_I2S1O_WS_OUT);
}
#endif /* HAVE_RECORDING */

static void start_tx(void)
{
    /* Start from an empty path. Without this the first descriptor of a session is
       appended to whatever survived the previous one, and a leftover odd number of
       bytes shifts every sample by half a stereo frame. */
    tx_flush();
    stat_sessions++;

    /* Preroll every slot before the walk begins, so the engine never reaches a
     * descriptor the CPU owns. Ask the core at most once per slot and stop
     * asking at the first decline -- driving pcm_play_dma_complete_callback()
     * repeatedly at start makes the swvol path call pcm_play_stop_int() once it
     * runs out, which re-enters sink_stop() mid-preroll. Silence for the rest is
     * correct regardless: the invariant is "owned and filled", not "real audio". */
    {
        bool core_exhausted = false;

        for (int i = 0; i < XT_TX_RING; i++)
        {
            if (!pend_valid && !core_exhausted)
            {
                refill();
                if (!pend_valid)
                    core_exhausted = true;
            }

            if (pend_valid)
            {
                fill(i, (const void *)pend_addr, (unsigned long)pend_size);
                pend_valid = 0;
            }
            else
                fill_silence(i);
        }

        done_slot = 0;
        dscr_streak = 0;
    }

    /* The ring is fully armed and the walk has not begun, so this is the only
     * safe moment to clear what the previous session left pending and put the
     * error interrupt back. Clearing while running would drop a completion that
     * fired between reading INT_ST and writing INT_CLR. */
    wr(GDMA_TX_INT_CLR, GDMA_INT_DONE | GDMA_INT_DSCR_ERR | GDMA_INT_TOTAL_EOF);
    wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DSCR_ERR);

    i2s_ll_tx_start(tx_hal.dev);

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start -- once */

    /* One line per session, printed from tick context: err/recover/silence since
       boot are the only way to tell a starvation-recovery from a misalignment after
       the fact. */
    stat_log_pending = true;
}

/* Runs in the tick thread, where the console can drain. Prints once per session
 * start, not on an interval. */
static void i2s_stat_tick(void)
{
    if (!stat_log_pending)
        return;

    stat_log_pending = false;

    logf("i2s: session %u err=%u rec=%u sil=%u rate=%lu",
         stat_sessions, stat_err, stat_recover, stat_silence,
         i2s_tx_sample_rate());
}

static void stop_tx(void)
{
    /* Stopping leaves every descriptor CPU-owned with length zero while
     * GDMA_INT_DSCR_ERR is still armed, so this is the moment the ring is most
     * fragile. */

    /* Mask the descriptor-error interrupt before the ring is emptied on purpose.
     *
     * sink_stop() hands every descriptor back to the CPU (dw0 = 0) immediately
     * after this returns. With out_auto_wrback set the engine mounts the next
     * descriptor the instant the current one retires, and out_check_owner then
     * rejects a slot we have just taken away: out_dscr_err. That is the teardown
     * doing what it was designed to do, not a fault -- and it was being counted as
     * one. This is where the transition bursts came from, including the nine
     * entries measured in the very first session before any audio had completed,
     * and a burst with no DONE between the entries is what trips tx_recover().
     *
     * Masked for the stopped period, re-armed in start_tx(). A genuine error
     * during playback is still reported; a designed one during teardown is not. */
    wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) & ~GDMA_INT_DSCR_ERR);

    /* Flush rather than merely halt: the bytes left in the I2S FIFO and in the
       channel are the ones that misalign the next session. Clearing the latched
       bits belongs to the flush, and is safe here because the transport is
       stopped, so a completion from the previous session has nothing to refill. */
    tx_flush();
}

/* Nothing is started or stopped for the receiver in this file any more: it is an
 * ESP-IDF I2S channel, enabled and disabled by pcm_rec_dma_start() /
 * pcm_rec_dma_stop(). Two hand-rolled attempts lived here and both are worth
 * remembering for what they taught:
 *
 *   i2s_tx_clock_run() borrowed the transmitter's clock domain for recording. It
 *   asserted tx_start, which runs the DAC's port against an empty FIFO, and still
 *   produced no usable clock -- an I2S transmitter with no GDMA feeding it hangs
 *   and stops clocking, which is what the tx_hung interrupt exists to report.
 *
 *   i2s_rx_start() set rx_start directly and never pulsed rx_update, so the
 *   receiver's configuration (master role, clock source, slot width) stayed in the
 *   APB domain and was never latched into the sampling domain. Measured: rx_conf
 *   read 00009404 with slave=0 and rx_update clear, and GDMA never took a
 *   descriptor. That is the same double-buffered-configuration trap as LEDC's
 *   conf0.low_speed_update and I2C's CONF_UPGATE.
 *
 * What the capture path still needs from here is only the shared clock pads. */

/* cur is the half that is free to write. The DMA mounts a descriptor only when the
 * owner bit says it owns it, so the half it is about to need must already be armed
 * when the current one finishes - which is why every write advances cur. */
/* GDMA's size field is 12 bits, so one descriptor carries at most 4095 bytes.
 * XT_TX_SLOT is the mixer's largest frame for the rates HW_SAMPR_CAPS allows, and
 * SAMPR_CAP_ALL_96 makes that 2048 bytes -- comfortably under the limit. Raising
 * the cap past 96 kHz takes the multiplier to 4, fails here, and means the ring
 * needs chained descriptors rather than a silent truncation. */
_Static_assert(XT_TX_SLOT <= GDMA_MAX_DESC_LEN,
               "the mixer's largest frame exceeds one GDMA descriptor");


static void refill(void)
{
    const void *addr;
    size_t size;


    /* Report the consumed buffer before anything else. This is the order every
     * working sink driver uses -- see firmware/target/arm/imx233/pcm-imx233.c,
     * whose ISR ends with pcm_play_dma_status_callback(PCM_DMAST_STARTED).
     * Telling the layer above that a buffer was consumed is what makes the mixer
     * produce the next one and hand it down through builtin_pcm_sink.ops.play,
     * which lands in the pending slot. */
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);

    if (pend_valid)
    {
        return;     /* ops.play already handed us the next buffer */
    }

    /* Nothing arrived, so ask explicitly. This is the fallback, not the normal
     * path, and it is also the underrun path: the caller re-arms the released
     * slot empty and the engine walks past it. */
    if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
    {
        return;
    }

    pend_addr = addr;
    pend_size = size;
    pend_valid = 1;
}

/* Runs on the interrupted thread's stack, so it has to stay small. The handler in
 * vectors.S has already saved a0 and a12-a15; this is the C part of the frame. */

void ICODE_ATTR gdma_irq(void)
{
    unsigned int st = rd(GDMA_TX_INT_ST);

    /* DSCR_ERR is handled BEFORE DONE, and the order is the bug this fixes.
     *
     * Both branches act on the same st, captured once at entry. When DONE and
     * DSCR_ERR are pending together -- which is exactly what "the engine ran off
     * the end of the list while the refill was still in here" looks like -- the
     * DONE path used to restart the walk and the DSCR_ERR path then stopped it
     * again in the same handler. A stopped outlink raises no completions, so
     * nothing restarted it: one transient became a silent transport until some
     * later event kicked it. Measured rate was about one DSCR_ERR every two
     * frames (err climbing ~173 per 345 interrupts).
     *
     * Handling it first means DONE's restart is the last word on the link, which
     * is what we want: DONE has just armed a real buffer. */
    if (st & GDMA_INT_DSCR_ERR)
    {
        stat_err++;

        /* Owner error or a bad descriptor word.

           This handler used to reset cur to 0 as well as re-pointing the link.
           That is what turned a single transient into a permanent fault: cur is
           the driver's idea of which slot is free, and forcing it to 0 while the
           engine was somewhere else in the ring meant every later arm() targeted
           a slot the engine had already passed. Measured directly -- write_desc
           armed slot 0 on both of its calls and slot 1 was never armed.

           With every slot owned from init onward this should not fire at all, so
           leave the ring alone and only restart the walk -- which is what this
           comment has always said, and what the code did not do: it wrote
           link.stop, guaranteeing the deadlock described above. Now it starts. */
        /* The storm guard. One DSCR_ERR is a transient and restarting is right.
         * Two in a row with no DONE between means the engine is sitting on a slot
         * the CPU owns, so restarting onto it just errors again -- and the handler
         * re-enters itself forever. Recover properly instead: stop, re-arm the
         * whole ring so no slot can be unowned, start once. */
        /* Only restart if the core actually wants a transport running.
         *
         * This was the burst. err steps to +52, +10 and +16 landed on exactly
         * the samples where (refill - irq) steps by 4 -- one start_tx() preroll,
         * i.e. a playback session starting -- and never on samples with heavy
         * volume or touch traffic and no session start. At a track switch
         * stop_tx() halts the walk, sink_stop() then clears every descriptor to
         * CPU-owned with zero length, and DSCR_ERR stays armed. Any error
         * pending across that teardown fired this branch, which started a
         * transport the core had just stopped -- straight onto the cleared
         * descriptors, producing more errors, which tripped tx_recover(), which
         * also started. Self-sustaining, anchored at the transition. */
        if (!playing)
        {
            /* Stopped: nothing to restart. Clear the pending error and stay down. */
            dscr_streak = 0;
        }
        else if (++dscr_streak >= 2)
            tx_recover();
        else
            wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));  /* link.start */
    }

    if (st & GDMA_INT_DONE)
    {
        /* Teardown guard. sink_stop() clears playing and then hands every
         * descriptor back to the CPU; a completion that arrives during that window
         * must not arm anything, or the handler would write into slots the stop
         * sequence is clearing. This is the race the completion mask used to paper
         * over, handled at the point where the state is actually known. */
        if (!playing)
        {
            cb_pending = 0;
            goto done_exit;
        }

        /* Take the engine off the list before it can fetch anything else.
         *
         * This is the imx233 model -- that driver's DMA command carries
         * next = NULL, so the transfer ends and the ISR restarts it. Our GDMA is
         * chained and free-running, which means it advances to the next
         * descriptor about a microsecond after retiring the current one, while
         * the refill that would arm it is still inside the interrupt. Measured on
         * this part: reaching a descriptor the CPU owns raises out_dscr_err, it is
         * not a benign park, so "let it park and restart later" is not a strategy
         * here. Stopping the walk removes the race instead of trying to win it.
         *
         * The I2S FIFO bridges the gap; it was measured running full, so there is
         * real capacity between a stopped outlink and the DAC. */
        int slot = done_slot;

        done_slot = (slot + 1) % XT_TX_RING;
        dscr_streak = 0;      /* a DONE proves the walk is healthy again */

        /* Free-running: the engine walks the chained ring by itself, so this
         * handler does not stop, restart, or clear next. done_slot is the
         * position to arm next; the slot that just retired has to be rearmed
         * before the engine wraps back onto it, which is what the runway
         * measured above buys. */

        if (queued > 0)
            queued--;

        /* Deferred is the CORE PULL, not the descriptor arming.
         *
         * This was the steady-state err mechanism, and it was my porting error.
         * The old design's answer to the play lock was to mask GDMA_INT_DONE, so
         * nothing at all happened while it was held -- which is safe with a
         * stop-and-restart transport and actively harmful with a free-running
         * one. The version above inherited that: on a locked completion it set
         * cb_pending and skipped everything, including arming the slot that just
         * retired. That slot stayed CPU-owned, the engine walked onto it under
         * auto-writeback, and out_dscr_err fired. Every locked window longer
         * than the ring's runway produced a wrap, and a wrap produces one error
         * per slot -- which is also why deepening the ring made err look WORSE
         * (257 at seven slots against 19 at four) without changing harm: err
         * counts wraps, and wraps are priced by depth.
         *
         * Arming a descriptor with silence involves the core not at all, so it
         * runs unconditionally. Only pcm_play_dma_complete_callback() is
         * deferred, via cb_pending, and replayed on unlock as as3525 does.
         */
        if (locked)
        {
            cb_pending = 1;

            if (pend_valid)
            {
                /* A buffer already published by ops.play() is ours to transmit;
                 * handing it over is not asking the core for anything. */
                fill(slot, (const void *)pend_addr, (unsigned long)pend_size);
                pend_valid = 0;
            }
            else
                fill_silence(slot);

            goto done_exit;
        }

        refill();

        if (pend_valid)
        {
            fill(slot, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
        }
        else
            fill_silence(slot);

        /* No link.stop and no link.start: the engine is already walking on. The
         * restart that used to sit here is the gap that clicked. */
done_exit:
        ;
        /* Nothing to play: stay stopped. The core's stop path handles the end of
         * stream; a genuine underrun simply pauses the transport rather than
         * feeding the DAC a descriptor the engine is not allowed to read. */
    }

    /* Clear only what fired. int_clr is write-to-clear. */
    wr(GDMA_TX_INT_CLR, st);

}

/* The play lock is a counter only -- it must not mask GDMA_INT_DONE.
 *
 * Masking the completion event stops the handler from re-arming retired slots,
 * but out_auto_wrback means the engine keeps consuming descriptors regardless.
 * After four of them the walk wraps onto a descriptor the CPU owns,
 * out_dscr_err fires, and tx_recover() answers with one repeated frame and three
 * silent slots -- audible as a glitch.
 *
 * That is why the symptom lives at the first frame of a session and not at a
 * particular rate: pcm_mixer.c:359 holds this lock across the whole DSP chain,
 * and the first frame is the expensive one (resampler and channel setup,
 * buffering's first SD read, codec startup). It outlasts the ring's runway of
 * three slots, about 16 ms at 44.1 kHz. Steady-state frames fit, which is why
 * the error counters go flat after the first couple of sessions.
 *
 * The handler already copes with a locked completion: it arms the retired slot
 * unconditionally -- pending audio if ops.play published one, silence otherwise --
 * and defers only the core callback through cb_pending, replayed on unlock below.
 * There is nothing for a mask to protect. What the mask did protect was teardown,
 * and that is now handled where it belongs: the DONE branch declines to arm
 * anything once playing is false. */
static void sink_lock(void)
{
    locked++;
}

static void sink_unlock(void)
{
    /* Clamped at zero, and the clamp still matters now that the count no longer
     * gates an interrupt.
     *
     * sink_stop() sets locked = 0 when a stop ends the session, but the mixer's own
     * outstanding pcm_play_unlock() still runs afterwards. With a bare --locked that
     * took the count to -1 -- measured locked=-1 in 500 of 548 samples during
     * ordinary playback -- and from there it never returns to zero, so the
     * `locked == 0` test that replays a deferred completion would never fire again.
     * Clamping keeps the count equal to the number of locks genuinely held, which is
     * all this counter is used for now. */
    if (locked > 0)
        locked--;

    if (locked == 0 && cb_pending)
    {
        int slot = done_slot;

        cb_pending = 0;
        done_slot = (slot + 1) % XT_TX_RING;
        refill();

        if (pend_valid)
        {
            fill(slot, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
        }
        else
            fill_silence(slot);
    }
}

static void sink_play(const void *addr, size_t size)
{
    /* Nothing here may print. usb_audio.c calls mixer_channel_play_data() inside
       disable_irq_save() (usb_audio.c:1380-1396), and a USB-DAC session is exactly
       a first frame. printf() takes the newlib stdout lock and needs the console to
       drain; with interrupts off on this core it never can. Every Rockbox thread is
       pinned to the same core (thread-idf.c), so the whole player stops -- no tick
       task, no buttons, no backlight, no USB -- and the task watchdog never fires
       because it is itself an interrupt. That is the USB-DAC lock-up. */

    if (!playing)
    {
        /* First frame of a playback. Fill slot 0 with it and every other slot
         * with an owned nothing, so from the instant the engine starts there is
         * no descriptor in the ring it is not allowed to take. Starting with one
         * armed slot and three the engine must not touch is what made the very
         * first hand-off fatal. */
        fill(0, addr, (unsigned long)size);
        pend_valid = 0;
        queued = 1;

        playing = true;
        start_tx();
        wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);

    }
    else
    {
        /* Steady state: the core hands us the next frame while the engine is
         * still playing the current one. The completion interrupt takes it from
         * here into whichever slot it has just released. */
        pend_addr = addr;
        pend_size = size;
        pend_valid = 1;
        queued++;
    }

}

static void sink_stop(void)
{
    playing = false;
    queued = 0;
    stop_tx();

    /* Hand every descriptor back to the CPU. If the DMA left one owned by
     * itself, the engine would suspend on it at the next start and nothing would
     * ever come out.
     *
     * Take the lines from memory before writing them, as fill() does: the engine
     * has been clearing owner bits through the bus, and a blind write here would
     * publish whatever our cache last held rather than what is actually there. */
    discard_dcache_range((const void *)desc, sizeof(desc));
    for (int i = 0; i < XT_TX_RING; i++)
        desc[i].dw0 = 0;
    commit_dcache_range((const void *)desc, sizeof(desc));
    pend_valid = 0;

    /* End of session: drop the lock accounting. A stop abandons any lock still
     * held, and leaving the count nonzero would suppress the cb_pending replay in
     * sink_unlock() forever -- the historical symptom was playback hanging with the
     * buffer full, measured as locked=2, int_ena 0x4 with the completion bit clear,
     * dma irq=0 and pcmbuf 181504/524288 and rising.
     *
     * Asserting GDMA_INT_DONE here is now defensive rather than load-bearing:
     * sink_lock() no longer masks the event, but a stop should leave the transport
     * able to signal completions whatever the interrupt enable register happened to
     * hold. */
    locked = 0;
    wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DONE);
}

static void sink_set_freq(uint16_t freq)
{
    i2s_set_frequency(hw_freq_sampr[freq]);
}
static void sink_init(void)
{
#ifdef HAVE_RECORDING
    /* Bring up IDF's receive channel BEFORE the transmitter is configured.
     *
     * i2s_new_channel() acquires IDF's GDMA group and, on first acquisition,
     * calls gdma_ll_reset_register(group) -- which resets every channel in the
     * block, including the OUT channel that i2s_init() then configures by hand.
     * Going first meant that reset landed on top of our transmitter and nothing
     * ever put it back: GDMA_TX_INT_ENA 0x06 -> 0x00 and the descriptor address
     * 0x0cf700 -> 0, with playback dead for the rest of the boot from the moment
     * recording was first armed. See i2s_rx_chan_create() in pcm-xtensa.c. */
    if (!i2s_rx_chan_create())
        printf("AUDHW: RX channel create failed\n");
#endif

    i2s_init();

    /* Cold-start order: clock generator, then codec reset and power-up, then the
     * codec's clock registers.
     *
     * es8311_init() resets the chip and writes only its port, power and analog
     * registers -- the clock dividers come from es8311_set_clock(). With the old
     * order (codec init, then sink_set_freq()) the chip was reset while MCLK, BCLK
     * and WS were absent, and the clock registers were written *before* that reset
     * and therefore wiped: the codec came up with CLK01 at its reset value against
     * a clock that only appeared afterwards, and the first thing that reprogrammed
     * it was the next i2s_set_frequency() -- the codec applying a file's rate at the
     * start of playback. That is the window in which the first seconds of a cold
     * start sound garbled and then clean up by themselves.
     */
    i2s_program_clock(hw_freq_sampr[builtin_pcm_sink.caps.default_freq]);

    audiohw_preinit();      /* reset and power-up, with clocks present */

    es8311_set_clock(hw_freq_sampr[builtin_pcm_sink.caps.default_freq],
                     hw_freq_sampr[builtin_pcm_sink.caps.default_freq] * MCLK_MULTIPLE);

    /* The ADC is deliberately not brought up here. This is the playback sink:
     * resetting the ES7210 and programming its dividers on every boot -- and on
     * every playback rate change -- and holding the I2C bus across two 10ms delays
     * to do it, belongs to the recording session that needs it and nowhere else. It
     * now happens in pcm_rec_dma_start(). */
    tx_flush();
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type  = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init     = sink_init,
        .postinit = audiohw_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_play,
        .stop     = sink_stop,
    },
};

