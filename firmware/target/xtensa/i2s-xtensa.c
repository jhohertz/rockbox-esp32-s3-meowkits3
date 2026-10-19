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

/* Stage 1a of the esp_driver_i2s migration. Off by default: with this undefined
 * the file behaves exactly as the frozen baseline in TX-IDF-MIGRATION.md, and
 * the switch is the revert. Flip it to test the next stage; see that file for
 * why the cutover itself cannot be staged any finer. */
#define TX_USE_IDF_DRIVER

#ifdef TX_USE_IDF_DRIVER
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#endif
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
 * calls it from thread context without taking the lock, and the probe reads it from
 * a third context. Without the qualifier the compiler may cache it across those
 * boundaries. An earlier note here claimed the interrupt mask made it safe, but
 * sink_play takes no lock at all. */
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
 * Owning every slot removes the race instead of trying to win it. A slot with no
 * data yet is armed with zero length, which the engine walks past, so there is
 * never a descriptor it is not allowed to take and therefore no error and no
 * park. Depth 4 also matches what the board's reference firmware uses
 * (dma_buf_count = 4), which is the same conclusion reached independently. */
#define XT_TX_RING 4

static volatile struct gdma_desc desc[XT_TX_RING] __attribute__((aligned(32)));
/* GDMA requires a 4-byte aligned buffer address */
/* Sized to the mixer's actual largest frame, not to GDMA_MAX_DESC_LEN. Four
 * 4096-byte slots is 16 KB of internal DRAM, which is the region this target has
 * no room to give: dram0_0_seg overflowed at 4 x 4096 and fits comfortably at
 * 4 x 512. The bound below is the same one the static assert in write_desc()
 * enforces, so the slot can never be smaller than what we are handed. */
#define XT_TX_SLOT (MIX_FRAME_SAMPLES * 4 * 2)

static unsigned char dma_buf[XT_TX_RING][XT_TX_SLOT] __attribute__((aligned(4)));

/* cur is the slot that is free to write. When DONE fires, the slot that just
 * finished transmitting is the one retire points at, so the mixer's next frame
 * goes into cur. Both advance modulo XT_TX_RING -- with a plain xor the upper
 * slots of the ring would never be armed at all. */
/* One buffer handed down by the core and not yet placed in a descriptor. The
 * core pushes through sink_play(); the completion interrupt pulls. */
static volatile const void *pend_addr;
static volatile size_t pend_size;
static volatile int pend_valid;
/* Guards a hardware interrupt mask, and is read from the probe while the mixer
 * holds it, so it must not be cached in a register across the read-modify-write
 * of GDMA_TX_INT_ENA. */
static volatile int locked;
static bool playing;
/* Descriptors armed and awaiting transmission. The ISR decrements it as each one
 * retires; refill() only asks the core for a buffer when the ring has run dry,
 * because in the normal case reporting the completion causes ops.play to hand us
 * the next one before we ever need to ask. */
static volatile int queued;

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
       -> abort. gdma_irq() already counts entries in xt_dma_irq_n, which
       i2s_audio_probe() reports from thread context, so nothing is lost by
       keeping quiet. */
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

/* TEMPORARY (transitional measurement only): the registers as IDF's HAL left
 * them, so they can be diffed against the values this file used to write by
 * hand. Remove together with the last hand-rolled setup. */
static void i2s_reg_snapshot(const char *tag);

/* TEMPORARY: one counter per link in the playback chain, so "it does not play"
 * becomes a statement about where the chain stops rather than which link is
 * suspected. Printed from the button probe, which is known to emit. Remove once
 * playback works.
 *
 * init  : sink_init() ran, so i2s_init() and audiohw_preinit() ran
 * play  : the PCM layer handed us a buffer to transmit
 * stop  : the PCM layer told us to stop
 * irq   : the GDMA out_eof interrupt actually fired and we refilled
 * lastsz: size of the most recent play() buffer, to show the mixer's frame shape
 */
volatile unsigned long xt_sink_init_n;
volatile unsigned long xt_sink_play_n;
volatile unsigned long xt_sink_stop_n;
volatile unsigned long xt_sink_irq_n;
volatile unsigned long xt_sink_last_size;
/* TEMPORARY: refill() path trace -- see the comment on refill(). */
volatile unsigned long xt_refill_n;
volatile int xt_dscr_err_n;
volatile int xt_refill_branch;
/* What write_desc() actually did, rather than what its caller implies it did.
 * Every attempt to reconstruct the stall from side effects has produced a
 * contradiction, so record the action itself: which slot was armed, the dw0
 * value written, and how many times the routine ran. */
volatile int  xt_wd_n;
volatile int  xt_wd_last_idx = -1;
volatile unsigned long xt_wd_last_dw0;
volatile int  xt_wd_idx_hist[4] = { -1, -1, -1, -1 };
volatile unsigned long xt_wd_dw0_hist[4];

/* TEMPORARY: the whole playback chain as one aligned readout, the way the
 * scheduler was finally diagnosed -- every stage's own view of what it is doing,
 * sampled at the same instant and emitted as a single printf so no other thread
 * can land between two stages. The answer is the seam where two adjacent stages
 * disagree, never one counter on its own:
 *
 *   insert climbing, pcmbuf used climbing, mixer cb static
 *        -> the mixer is not being driven at all
 *   mixer cb climbing, next non-zero, sink play static
 *        -> buffers are being mixed but pcm_play_data() is not starting the DMA
 *   core more=(nil)
 *        -> pcm_play_stop_int() has latched the chain off; nothing can resume it
 *   sink play climbing, dma irq static
 *        -> the ring is fed but the transmitter is not completing
 *
 * locked, int_ena and int_raw are included because sink_lock() masks out_eof and
 * sink_unlock() unmasks it only when the count returns to zero. If the play lock
 * is ever left held the consumer is muted for good: int_raw keeps the pending
 * bit, int_ena has it clear, and the PCM buffer fills with nothing able to drain
 * it -- which is what a stalled consumer looks like from upstream.
 *
 * pcmbuf used/free is the pressure quantity: it is what should decide whether
 * there is anything to hand down. Clock and reset state are included because a
 * peripheral held in reset reads back zero and is otherwise indistinguishable
 * from a wrong base address.
 */
/* TEMPORARY: the whole playback chain as one aligned readout -- every stage's
 * own view, sampled at the same instant and emitted as a single printf so no
 * other thread can land between two stages. The answer is the seam where two
 * adjacent stages disagree, never one counter on its own.
 *
 * Hardware flags are printed DECODED. Two separate misreads cost most of a day
 * here: out_eof was taken for bit 0 when the BIT() define says bit 1, and the
 * GDMA FIFO status bits were read as full when bits 0/2/4 are full and 1/3/5 are
 * empty -- both from parsing the comment block instead of the defines. Nothing
 * below asks the reader to know a bit position.
 */
void i2s_audio_probe(void)
{
    extern volatile unsigned long xt_codec_run_n;
    extern volatile int xt_codec_in_run;
    extern volatile unsigned long xt_pcminsert_n, xt_bufget_n, xt_startfill_n;
    extern volatile unsigned long xt_mix_cb_n;
    extern size_t pcm_mixer_probe_next(void);
    extern int pcm_mixer_probe_index(void);
    extern size_t pcmbuf_free(void);
    extern size_t pcmbuf_get_bufsize(void);
    extern int pcmbuf_used_descs(void);
    extern int pcmbuf_descs(void);
    extern volatile pcm_play_callback_type pcm_callback_for_more;
    extern enum pcm_dma_status
        (* volatile pcm_play_status_callback)(enum pcm_dma_status);

    size_t bufsz = pcmbuf_get_bufsize();
    size_t used = bufsz - pcmbuf_free();
    uint32_t f_pll160 = 0, f_xtal = 0;
    unsigned int gs = rd(GDMA_TX_STATE);
    unsigned int fs = rd(GDMA_TX_FIFO_STATUS);

    /* The DMA writes descriptor results back through the bus, not through our
     * D-cache, so the cached line for the ring can still hold what we wrote
     * rather than what the engine left. Drop it first: without this the owner
     * bits printed below describe the CPU's memory, not the hardware's, and can
     * contradict every other reading on this line. */
    discard_dcache_range((const void *)desc, sizeof(desc));

    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_PLL_F160M,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &f_pll160);
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &f_xtal);

    printf("CHAIN: codec run=%lu in=%d | insert=%lu bufget=%lu startfill=%lu\n"
           "CHAIN: pcmbuf used=%lu/%lu descs=%d/%d | mixer cb=%lu next=%lu idx=%d\n"
           "CHAIN: core playing=%d more=%p status=%p | sink play=%lu stop=%lu\n"
           "CHAIN: dma irq=%lu refill=%lu branch=%d queued=%d locked=%d ena=%08x\n"
           "CHAIN: fifo L1 f%d/e%d L2 f%d/e%d L3 f%d/e%d | raw eof=%d dscr=%d tot=%d\n"
           "CHAIN: gdma out=%d dscr=%d park=%d | err=%d | link=%08x own=%d%d%d%d len=%d,%d,%d,%d\n"
           "CHAIN: wd n=%d lastidx=%d wrote=%08x hist=%d,%d,%d,%d pre=%08x\n"
           "CHAIN: i2s idle=%d started=%d | clk sel=%d active=%d en=%d | pll160=%lu xtal=%lu\n",
           xt_codec_run_n, xt_codec_in_run,
           xt_pcminsert_n, xt_bufget_n, xt_startfill_n,
           (unsigned long)used, (unsigned long)bufsz,
           pcmbuf_used_descs(), pcmbuf_descs(),
           xt_mix_cb_n, (unsigned long)pcm_mixer_probe_next(),
           pcm_mixer_probe_index(),
           pcm_playing, (void *)pcm_callback_for_more,
           (void *)pcm_play_status_callback,
           xt_sink_play_n, xt_sink_stop_n,
           xt_sink_irq_n, xt_refill_n, xt_refill_branch, queued, locked,
           (unsigned)rd(GDMA_TX_INT_ENA),
           /* GDMA_OUTFIFO_FULL_Ln = BIT(0/2/4), EMPTY_Ln = BIT(1/3/5) */
           (int)((fs >> 0) & 1), (int)((fs >> 1) & 1),
           (int)((fs >> 2) & 1), (int)((fs >> 3) & 1),
           (int)((fs >> 4) & 1), (int)((fs >> 5) & 1),
           /* GDMA_OUT_EOF = BIT(1), DSCR_ERR = BIT(2), TOTAL_EOF = BIT(3) */
           (int)((rd(GDMA_TX_INT_RAW) >> 1) & 1),
           (int)((rd(GDMA_TX_INT_RAW) >> 2) & 1),
           (int)((rd(GDMA_TX_INT_RAW) >> 3) & 1),
           /* GDMA_OUT_STATE = 3 bits at 20, OUT_DSCR_STATE = 2 bits at 18 */
           (int)((gs >> 20) & 0x7), (int)((gs >> 18) & 3),
           (int)((rd(GDMA_TX_LINK) >> 23) & 1),
           xt_dscr_err_n, (unsigned)rd(GDMA_TX_LINK),
           (int)((desc[0].dw0 >> 31) & 1), (int)((desc[1].dw0 >> 31) & 1),
           (int)((desc[2].dw0 >> 31) & 1), (int)((desc[3].dw0 >> 31) & 1),
           (int)((desc[0].dw0 >> 12) & 0xfff), (int)((desc[1].dw0 >> 12) & 0xfff),
           (int)((desc[2].dw0 >> 12) & 0xfff), (int)((desc[3].dw0 >> 12) & 0xfff),
           xt_wd_n, xt_wd_last_idx, (unsigned)xt_wd_last_dw0,
           xt_wd_idx_hist[0], xt_wd_idx_hist[1],
           xt_wd_idx_hist[2], xt_wd_idx_hist[3],
           (unsigned)(xt_wd_n > 0 && xt_wd_n <= 4
                          ? xt_wd_dw0_hist[xt_wd_n - 1] : 0),
           (rd(I2S_TX_STATE) & TX_STATE_IDLE) ? 1 : 0,
           (rd(I2S_TX_CONF) & TX_CONF_START) ? 1 : 0,
           (int)((rd(I2S_TX_CLKM_CONF) >> 27) & 3),
           (rd(I2S_TX_CLKM_CONF) & CLKM_CONF_CLK_ACTIVE) ? 1 : 0,
           (rd(I2S_TX_CLKM_CONF) & CLKM_CONF_CLK_EN) ? 1 : 0,
           (unsigned long)f_pll160, (unsigned long)f_xtal);
}


/* Best rational approximation of freq_error / exp_freq with denominator < max.
 * Ported from ESP-IDF's hal_utils_calc_clk_div_frac_accurate. */


/* i2s_ll_tx_set_mclk's mapping from (integer, num/den) to the four registers. */

void i2s_set_frequency(unsigned long rate)
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

    i2s_reg_snapshot("after hal config");

    es8311_set_clock(rate, rate * MCLK_MULTIPLE);

    /* The ADC shares the transmitter's MCLK, so it has to be reprogrammed
     * whenever the rate changes.
     *
     * The receiver is no longer configured here. It is a full ESP-IDF I2S channel
     * created per recording session in pcm-xtensa.c, which sets its own clock and
     * slot. All this file still owes the capture path is the sample rate the
     * session must match, recorded below, and the shared clock pads. */
#ifdef HAVE_RECORDING
    rx_rate = rate;
    es7210_set_frequency(rate);
#endif
}

static void arm(int i, unsigned long len)
{
    desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
    /* Zero terminates the link list: the engine stops rather than wrapping.
     *
     * Three variants, and the record matters because two of them were conflated
     * for a long time:
     *
     *  - next = &desc[i] (SELF-linked) was tried and is genuinely bad: the engine
     *    follows one slot forever, giving one out_dscr_err per frame (err tracked
     *    irq exactly, +1) and a noticeable lag on pause while it replayed the last
     *    buffer. That measurement is real and is what motivated this line.
     *
     *  - next = &desc[(i+1) % XT_TX_RING] (a real ring, which is what gdma_init()
     *    already builds before the first arm() call un-chains it) was tried on the
     *    reasoning that self-linking does not predict it -- the engine walks
     *    forward instead of looping, and the owner bit already gates every slot.
     *    Result: no improvement. Clicks at frame boundaries persisted, and the CHAIN
     *    probe captured no non-zero gdma counters during playback at all, so there
     *    is no evidence the ring ever engaged, and no evidence that it did not.
     *    Reverted. This is NOT a refutation of the ring; it is an inconclusive
     *    experiment, and the reason is the next bullet.
     *
     *  - next = 0 (what is here) is the encoding of imx233's `dma.next = NULL`:
     *    the transfer ends and the driver restarts it deliberately.
     *
     * The blocker on all of this is observability, not the descriptor link. The
     * CHAIN dump does not sample during steady playback, so a per-frame property
     * like slot occupancy cannot be measured by it -- every comparison so far has
     * been against a snapshot taken at some other moment. Before this line is
     * changed again, the probe needs a periodic sample while audio is running;
     * otherwise any result here is unfalsifiable. */
    desc[i].next   = 0;
    /* len 0 keeps the slot owned by the DMA with nothing to move: the engine
     * takes it, transfers nothing, and moves on. That is what makes an empty
     * slot safe rather than fatal. */
    desc[i].dw0 = XT_TX_SLOT | (len << 12) | DESC_SUC_EOF | DESC_OWNER;
}

/* Place one frame into slot i and hand it to the DMA. len 0 leaves the slot
 * owned with nothing to move, which is what keeps the ring fully owned between
 * hand-offs. */
/* TEMPORARY: sample-width capture. fill() runs inside gdma_tx_isr and may not
 * print, so it stores the frame head here and i2s_probe_tick() prints it from
 * thread context. ready is set last and cleared by the printer, so a frame
 * captured mid-print is simply reported next time round. */
static struct {
    volatile int ready;
    volatile int n;
    volatile int nonzero;
    volatile unsigned long len;
    volatile unsigned long filln;
    unsigned short data[128];
} xt_width;

static void fill(int i, const void *src, unsigned long len)
{
    memcpy(dma_buf[i], (const void *)src, len);

    /* What width is actually on the wire? The mixer frame is
     * MIX_FRAME_SAMPLES*4 = 1024 bytes, which reads as 32-bit samples, but
     * i2s_init() configures I2S_DATA_BIT_WIDTH_16BIT in
     * I2S_SLOT_BIT_WIDTH_16BIT slots. If both are true the DAC is fed the low half
     * of every 32-bit sample as a separate channel: near zero and harmless when
     * quiet, garbage when loud. That is amplitude-dependent distortion with a
     * compressed character, which is what it sounds like.
     *
     * This is in fill(), not sink_play(), and that took four attempts to get
     * right. In Rockbox's buffered model ops.play() fires ONCE per playback to arm
     * the initial buffer; every later frame arrives through the pcm_play_data
     * callback, which lands here. A probe in sink_play() therefore samples one
     * frame per session -- which is exactly why it printed nothing across 18k
     * frames while the counter never reached its threshold.
     *
     * It may not PRINT, though. fill() is reached from refill() inside
     * gdma_tx_isr, and the comment at the top of that handler says outright that
     * nothing may print there: printf takes the newlib stdout lock and aborts.
     * Putting it here froze the device about 1.2 s in, at the sample point, which
     * was the probe and not the audio. So the ISR only copies; i2s_probe_tick()
     * prints it from thread context. */
    {
        const unsigned short *w = (const unsigned short *)src;
        static unsigned long fill_n;
        int i2, nonzero = 0;
        int n = 128;

        if ((unsigned long)n > len / 2)
            n = (int)(len / 2);

        fill_n++;
        if (!xt_width.ready && fill_n >= 200 && n > 0)
        {
            for (i2 = 0; i2 < n; i2++)
            {
                if (w[i2] != 0)
                    nonzero++;
            }

            for (i2 = 0; i2 < n; i2++)
                xt_width.data[i2] = w[i2];

            xt_width.n = n;
            xt_width.nonzero = nonzero;
            xt_width.len = len;
            xt_width.filln = fill_n;
            xt_width.ready = 1;     /* last: the printer reads this first */
        }
    }

    /* Take the line from memory before modifying it, then push it back. The
     * engine wrote the owner bit clear through the bus, and our cached copy of
     * this line still has whatever we put there. */
    discard_dcache_range((const void *)&desc[i], sizeof(desc[i]));
    arm(i, len);
    commit_dcache_range(dma_buf[i], len ? len : XT_TX_SLOT);
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

    /* conf0: auto write-back so the DMA hands the descriptor back to the CPU
     * after it has transmitted the buffer. Without this the owner bit stays
     * set to DMA forever and the ring never frees. */
    /* conf0: outdscr_burst_en only.
     *
     * out_auto_wrback (bit 2) is deliberately NOT set, and that single bit is
     * what the whole crackle came from. arm() writes next = 0 on the stated
     * understanding that "zero terminates the link list: the engine stops rather
     * than wrapping". This GDMA has no such convention. With auto-writeback
     * enabled the engine follows next unconditionally the moment a descriptor
     * retires, so next = 0 told it to fetch its next descriptor from address
     * 0x00000000 -- garbage, owner clear, length zero -- and raised out_dscr_err.
     * Measured: err tracked irq exactly 1:1 (360 per 379 interrupts), i.e. one
     * descriptor fault per audio frame, 172 times a second. Each one stalls the
     * transport until the handler restarts it. That stall is the crackle.
     *
     * Without auto-writeback the engine halts at the end of the descriptor it was
     * given and waits for software, which is the behaviour the DONE handler has
     * always assumed: complete, re-arm desc[0], link.start. The imx233 model this
     * file is written against gets the same effect from a DMA command carrying
     * next = NULL, because that peripheral really does treat NULL as a stop.
     *
     * If the ring is ever wanted for real, the change is the other way round:
     * keep auto-writeback on, chain next through all four slots, and keep every
     * slot owned by the DMA and filled. What cannot work is either setting with
     * next = 0. */
    wr(GDMA_TX_CONF0, (1u << 4));            /* outdscr_burst_en */
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

/* The transitional register dump declared above. */
static void i2s_reg_snapshot(const char *tag)
{
    printf("I2SREG %s: conf=%08x conf1=%08x tdm=%08x clkm=%08x div=%08x state=%08x\n",
           tag, (unsigned)rd(I2S_TX_CONF), (unsigned)rd(I2S_TX_CONF1),
           (unsigned)rd(I2S_TX_TDM_CTRL), (unsigned)rd(I2S_TX_CLKM_CONF),
           (unsigned)rd(I2S_TX_CLKM_DIV_CONF), (unsigned)rd(I2S_TX_STATE));
}

#ifdef TX_USE_IDF_DRIVER
/* The IDF transmit channel, created but deliberately NOT yet configured.
 *
 * Stopping at i2s_new_channel() is the whole of Stage 1a, and it is a real
 * boundary rather than an arbitrary one. From IDF source:
 *
 *   i2s_new_channel()            -> i2s_acquire_controller_obj() ->
 *                                   i2s_hal_init(), which is only
 *                                   hal->dev = I2S_LL_GET_HW(port) (i2s_hal.c:64)
 *                                   plus heap, a queue and two semaphores.
 *                                   No GDMA allocation. No register writes.
 *
 *   i2s_channel_init_std_mode()  -> i2s_init_dma_intr() (i2s_common.c:804) ->
 *                                   gdma_new_ahb_channel() ->
 *                                   do_allocate_gdma_channel() ->
 *                                   gdma_acquire_group_handle() ->
 *                                   gdma_ll_reset_register() (gdma.c:696)
 *
 * So the block reset that pcm-xtensa.c:228 documents is real, but it belongs to
 * init_std_mode, not to new_channel. That matters here because our
 * hand-rolled transmitter owns GDMA OUT channel 0 (GDMA_TX_CONF0 =
 * GDMA_BASE + 0x60) and IDF has no idea: the first time init_std_mode runs for
 * a TX channel it will allocate the first free OUT channel and take that one.
 * There is therefore no state in which both transmitters are live, which is why
 * the migration is a switch and an atomic cutover rather than a gradual
 * handover.
 *
 * Creating the channel here is still worth doing on its own: it proves IDF will
 * claim the I2S1 controller while our HAL still drives it, and the probe in
 * sink_init() checks afterwards that the live GDMA configuration survived.
 * That is the claim this stage rests on, so it gets measured rather than
 * trusted. */
static i2s_chan_handle_t tx_chan_idf;

bool i2s_tx_chan_create(void)
{
    if (tx_chan_idf)
        return true;

    /* Sized to match the hand-rolled path for now: one descriptor is
     * dma_frame_num * 2 slots * 2 bytes = 1024 bytes, which is exactly one
     * Rockbox mixer frame. desc_num is the runway-vs-report-accuracy knob and
     * is swept in Stage 3 -- see the table in TX-IDF-MIGRATION.md for why 8
     * made the previous attempt bursty. */
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,
                                                       I2S_ROLE_MASTER);
    cfg.dma_desc_num  = 2;
    cfg.dma_frame_num = 256;

    esp_err_t e = i2s_new_channel(&cfg, &tx_chan_idf, NULL);

    if (e != ESP_OK)
    {
        printf("TX1a: i2s_new_channel failed: %d\n", (int)e);
        tx_chan_idf = NULL;
        return false;
    }

    printf("TX1a: IDF TX channel created %p (not configured, not routed)\n",
           (void *)tx_chan_idf);
    return true;
}
#endif /* TX_USE_IDF_DRIVER */


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
}

#ifdef HAVE_RECORDING

/* The recording session's IDF channel has to be created at the rate the bus is
 * running: the codecs share one MCLK, so a receiver configured for a different
 * one would be clocked against a frame the ADC was not expecting. */
unsigned long i2s_rx_sample_rate(void)
{
    return rx_rate ? rx_rate : hw_freq_sampr[builtin_pcm_sink.caps.default_freq];
}

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
    i2s_ll_tx_start(tx_hal.dev);
    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start */
}

static void stop_tx(void)
{
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */
    i2s_ll_tx_stop(tx_hal.dev);
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
 * The mixer's largest frame is MIX_FRAME_SAMPLES samples times 4 bytes times the
 * high-rate multiplier, and the multiplier only reaches 2 here because
 * HW_SAMPR_CAPS caps at 96 kHz. That is 2048 bytes, comfortably under the limit.
 * If the cap ever rises, the ring needs chained descriptors rather than one per
 * half, not a silent truncation. */
_Static_assert(MIX_FRAME_SAMPLES * 4 * 2 <= GDMA_MAX_DESC_LEN,
               "the mixer's largest frame exceeds one GDMA descriptor");


static void refill(void)
{
    const void *addr;
    size_t size;

    xt_refill_n++;

    /* Report the consumed buffer before anything else. This is the order every
     * working sink driver uses -- see firmware/target/arm/imx233/pcm-imx233.c,
     * whose ISR ends with pcm_play_dma_status_callback(PCM_DMAST_STARTED).
     * Telling the layer above that a buffer was consumed is what makes the mixer
     * produce the next one and hand it down through builtin_pcm_sink.ops.play,
     * which lands in the pending slot. */
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);

    if (pend_valid)
    {
        xt_refill_branch = 1;
        return;     /* ops.play already handed us the next buffer */
    }

    /* Nothing arrived, so ask explicitly. This is the fallback, not the normal
     * path, and it is also the underrun path: the caller re-arms the released
     * slot empty and the engine walks past it. */
    if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
    {
        xt_refill_branch = 2;
        return;
    }

    xt_refill_branch = 3;
    pend_addr = addr;
    pend_size = size;
    pend_valid = 1;
}

/* Runs on the interrupted thread's stack, so it has to stay small. The handler in
 * vectors.S has already saved a0 and a12-a15; this is the C part of the frame. */
/* TEMPORARY: how long the transmit path is torn down for.
 *
 * The design stops the outlink on every completion, runs the mixer through
 * pcm_play_dma_status_callback(), memcpy's the next frame in, and restarts the
 * walk -- 172 times a second at 1024-byte frames. The only thing between a
 * stopped outlink and silence at the DAC is the I2S FIFO, so the question that
 * decides whether this is the click is simply how long that window is, and how
 * much of it belongs to the mixer rather than the copy.
 *
 * Measured in core cycles because that needs no assumptions; i2s_probe_tick()
 * converts. Nothing prints here -- see the note on gdma_tx_isr(). */
static volatile unsigned long xt_isr_n;
static volatile unsigned long xt_isr_cyc_max;      /* entry -> exit */
static volatile unsigned long xt_isr_refill_max;   /* entry -> after refill() */
static volatile unsigned long xt_isr_cyc_sum;
/* Never reset. The per-window max is read by the tick thread as four separate
 * volatile reads while the ISR keeps updating them, so a long sample landing
 * between the read and the reset is lost and max under-reports -- which is how
 * it came to print max=10us against avg=16us. A monotonic worst case cannot be
 * lost that way, so the number that actually matters ("is it ever large?") is
 * race-free. The windowed max only biases low, never high. */
static volatile unsigned long xt_isr_cyc_worst;

void ICODE_ATTR gdma_irq(void)
{
    unsigned int st = rd(GDMA_TX_INT_ST);
    unsigned long t0 = esp_cpu_get_cycle_count();
    unsigned long trefill = t0;

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
        xt_dscr_err_n++;
        wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));      /* link.start */
    }

    if (st & GDMA_INT_DONE)
    {
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
        wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */

        /* desc[0] is the only descriptor in play. The outlink was stopped above
         * before anything else could be fetched, so there is no ring position to
         * track: complete, re-arm, start. */

        /* A completion can only mean audio was played: a slot the CPU owns is not
         * taken by the engine, so it never completes. */
        xt_sink_irq_n++;
        if (queued > 0)
            queued--;

        /* Report the consumption first: that is what makes the mixer build the
         * next frame and hand it down through sink_play(), which lands in the
         * pending slot. */
        refill();
        trefill = esp_cpu_get_cycle_count();

        if (pend_valid)
        {
            fill(0, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
            /* Re-arm and go again. Writing the address re-latches the descriptor
             * list at desc[0]; start begins the walk from there. */
            wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
            wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start */
        }
        /* Nothing to play: stay stopped. The core's stop path handles the end of
         * stream; a genuine underrun simply pauses the transport rather than
         * feeding the DAC a descriptor the engine is not allowed to read. */
    }

    /* Clear only what fired. int_clr is write-to-clear. */
    wr(GDMA_TX_INT_CLR, st);

    {
        unsigned long d = esp_cpu_get_cycle_count() - t0;

        xt_isr_n++;
        xt_isr_cyc_sum += d;
        if (d > xt_isr_cyc_max)
            xt_isr_cyc_max = d;
        if (d > xt_isr_cyc_worst)
            xt_isr_cyc_worst = d;
        d = trefill - t0;
        if (d > xt_isr_refill_max)
            xt_isr_refill_max = d;
    }
}

/* Mask the event rather than the slot, so a completion that happens while the mixer
 * is busy is not lost: int_raw stays set and fires as soon as the mask is lifted. */
static void sink_lock(void)
{
    if (locked++ == 0)
        wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) & ~GDMA_INT_DONE);
}

static void sink_unlock(void)
{
    /* Clamped, and the clamp is the fix for a drift that was easy to miss.
     *
     * sink_stop() sets locked = 0 deliberately (see the note there: a stop that
     * arrives while the mixer still holds the play lock used to leave the count
     * nonzero and GDMA_INT_DONE masked for good). But the mixer's own outstanding
     * pcm_play_unlock() still runs afterwards, and with a bare --locked that took
     * the count to -1 -- where it then stayed: measured locked=-1 in 500 of 548
     * samples during ordinary playback.
     *
     * Once negative, neither transition ever equals zero again. sink_lock()'s
     * "if (locked++ == 0)" never masks and this function's "if (--locked == 0)"
     * never unmasks, so the pair silently stops doing anything and the completion
     * interrupt is live at all times -- including inside the start and stop
     * sequences the play lock exists to protect. That is a race window, not a
     * cosmetic counter bug.
     *
     * Clamping at zero keeps the invariant the two guards depend on: the count is
     * always the number of locks genuinely held, the 0<->1 edges still mean
     * something, and an unlock that arrives after a stop simply re-asserts the
     * unmask, which is what we want anyway. */
    if (locked > 0)
        locked--;

    if (locked == 0)
    {
        /* TEMPORARY: event-driven, so it cannot sample the wrong moment the way
           a dump taken inside sink_play() did -- the mixer holds this lock across
           mixer_start_pcm(), so any snapshot in that call is inside the locked
           region by construction. If "SINK: first play" prints and this never
           does, the play lock is never released: GDMA_INT_DONE stays masked for
           the rest of the session and the codec blocks waiting for pcmbuf to
           drain while the mixer blocks waiting for the codec. */
        static bool unlocked_once = false;
        if (!unlocked_once)
        {
            unlocked_once = true;
            printf("SINK: play lock released (first unlock to zero)\n");
        }
        wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DONE);
    }
}

static void sink_play(const void *addr, size_t size)
{
    xt_sink_play_n++;
    xt_sink_last_size = size;

    /* TEMPORARY: the first frame of a playback. The chain dump is taken after
       start_tx() below, not here: the pcm core holds the play lock across the
       whole start sequence, so at this point GDMA_INT_DONE is legitimately
       masked and the transmitter legitimately not started. An early dump cannot
       tell a muted consumer from a normal snapshot. */
    if (xt_sink_play_n == 1)
    {
        printf("SINK: first play addr=%p size=%lu\n", addr,
               (unsigned long)size);

        /* TEMPORARY: playback allocates its file buffer around here, and this is
           where the EOM panic fires. Dumping at the first frame shows what is
           left at the moment the decision gets made, rather than after it has
           already failed. */
        xt_arena_dump("at playback start");
    }

    /* The sample-width probe lives in fill(), not here. sink_play() is
     * ops.play(), which the pcm core calls once per playback to arm the initial
     * buffer; every later frame comes through pcm_play_data into refill() and
     * then fill(). Sampling here sees one frame per session, and it is the silent
     * head of the track. */

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

    /* TEMPORARY: dump from thread context once the engine has had many
       completions, so dma irq, gdma out and the descriptor lengths are live.
       Not on the first play: the mixer still holds the play lock there, so a
       masked OUT_DONE and started=0 are both correct and say nothing. And never
       from the ISR -- a printf there takes the newlib stdout lock and aborts. */
    if (xt_sink_play_n == 16)
    {
        printf("CHAIN: at 16th play --\n");
        i2s_audio_probe();
    }
}

static void sink_stop(void)
{
    xt_sink_stop_n++;

    /* TEMPORARY: read the transmit path out as the session ends, before stop_tx()
       and the descriptor handback below erase the evidence. A playback that never
       completed shows up here as sink play>0 with dma irq=0 and gdma out=0, which
       is the engine refusing to take a descriptor -- a different fault entirely
       from a codec or a data problem. Thread context, so printf is safe. */
    i2s_audio_probe();

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

    /* Restore the interrupt invariant. sink_lock() masks GDMA_INT_DONE on the
     * first lock and sink_unlock() only unmasks it when the count returns to
     * zero, so a stop that arrives while the mixer still holds the play lock
     * leaves the count nonzero and the completion masked for good: the next
     * session's sink_unlock() decrements to a nonzero count and skips the
     * re-enable, no OUT_DONE ever fires, and playback hangs with the buffer
     * full. Measured that way -- locked=2, int_ena 0x4 with the DONE bit clear,
     * dma irq=0, pcmbuf 181504/524288 and rising.
     *
     * A stop ends the session, so any lock still held is moot: drop the
     * accounting and unmask unconditionally. */
    locked = 0;
    wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DONE);
}

static void sink_set_freq(uint16_t freq)
{
    i2s_set_frequency(hw_freq_sampr[freq]);
}

/* TEMPORARY: periodic CHAIN sample.
 *
 * i2s_audio_probe() was only ever called from event sites -- sink_play, sink_stop,
 * pcm close -- so every snapshot in the logs describes the transmitter at some
 * moment other than steady playback. That made a per-frame property like slot
 * occupancy unmeasurable: the ring experiment compared one event snapshot against
 * another and called the difference a verdict, twice, and neither was one.
 *
 * This samples while audio is actually running. It runs from tick_task_thread
 * (the tick tasks are NOT run in the interrupt on this core -- see the note in
 * apps/main.c), so printf is legal here; the ISR must never reach this path.
 * Every 200 ticks at HZ=100 is two seconds, which is often enough to see drift
 * and rare enough not to drown the console. */
static void i2s_probe_tick(void)
{
    static int div;

    if (!pcm_playing)
    {
        div = 0;
        return;
    }

#ifdef XT_PANIC_SELFTEST
    /* Diagnostic bisection, not a feature. The open question is why IDF's panic
     * handler re-enters and aborts on every crash here, printing nothing but
     * "Panic handler entered multiple times". Two causes fit and they need
     * opposite fixes:
     *
     *   - the handler or its console path is broken in general. Then hooking the
     *     exception vector buys nothing, because a hand-written recorder still has
     *     to get its output somewhere across a reboot.
     *   - our crashes corrupt something the handler depends on (stack, or a frame
     *     that ended up in PSRAM). Then a trap from a clean thread context dumps
     *     properly, and a vector hook is exactly the right move.
     *
     * __builtin_trap() emits an illegal instruction from ordinary thread context
     * with a valid stack -- the same exception path, minus whatever a real crash
     * may have already broken. Build with -DXT_PANIC_SELFTEST, play something for
     * ~6 s, and read the dump. Compile-time guarded so it cannot ship.
     *
     * Separate counter on purpose: div resets every 200 ticks to drive the probe,
     * so a threshold above 200 on div would never fire.
     */
    {
        static int selftest_ticks;
        if (++selftest_ticks == 600)
        {
            printf("SELFTEST: trapping now (clean thread context)\n");
            __builtin_trap();
        }
    }
#endif

    if (++div < 200)
        return;

    div = 0;

    /* How long the transport is down, in microseconds. The frame itself is 1024
     * bytes = 5.8 ms at 44.1k stereo 16-bit, and the outlink is stopped for this
     * whole window, so the FIFO has to cover it. Compare the two and the click
     * either is explained or is not. Counters reset each interval so the report is
     * a fresh worst case, not a monotonic mark that only ever shows one old sample. */
    if (xt_isr_n > 0)
    {
        int hz = esp_clk_cpu_freq();
        unsigned long n = xt_isr_n, mx = xt_isr_cyc_max;
        unsigned long rm = xt_isr_refill_max, avg = xt_isr_cyc_sum / n;

        xt_isr_n = 0; xt_isr_cyc_max = 0; xt_isr_refill_max = 0;
        xt_isr_cyc_sum = 0;

        printf("ISR: %lu in window  avg=%luus  max=%luus  worst=%luus"
               "  (refill part max=%luus)  [%d MHz]\n",
               n,
               hz ? avg * 1000000UL / (unsigned long)hz : 0,
               hz ? mx * 1000000UL / (unsigned long)hz : 0,
               hz ? xt_isr_cyc_worst * 1000000UL / (unsigned long)hz : 0,
               hz ? rm * 1000000UL / (unsigned long)hz : 0,
               hz / 1000000);
    }

    /* Sample-width verdict, captured in gdma_tx_isr context by fill() and printed
     * here where printing is legal. */
    if (xt_width.ready)
    {
        int k, nz_even = 0, nz_odd = 0;

        printf("SINK bytes: fill #%lu, len=%lu, first %d half-words"
               " (%d nonzero):\n  ", xt_width.filln, xt_width.len,
               xt_width.n, xt_width.nonzero);
        for (k = 0; k < xt_width.n; k++)
        {
            printf("%04x ", xt_width.data[k]);
            if (xt_width.data[k] != 0x0000 && xt_width.data[k] != 0xffff)
            {
                if (k & 1)
                    nz_odd++;
                else
                    nz_even++;
            }
            if ((k & 7) == 7 && k + 1 < xt_width.n)
                printf("\n  ");
        }
        printf("\nSINK width test: active even-half=%d odd-half=%d of %d"
               "  => %s\n",
               nz_even, nz_odd, xt_width.n / 2,
               xt_width.nonzero == 0
                   ? "HEAD IS SILENCE"
                   : (nz_even > 3 * nz_odd || nz_odd > 3 * nz_even)
                       ? "32-BIT samples: one half of each pair is quiet"
                       : "16-bit PCM: both halves carry signal");
        xt_width.ready = 0;
    }

    i2s_audio_probe();

    /* xt_stack_report() is stood down. It has produced two artefacts in a row:
     * first scanning for DEADBEEF, a fill xTaskCreateStatic overwrites; then with
     * the loop inverted, so it counted the run of fill from the top and reported
     * that as usage -- which is why all eleven threads showed an identical
     * used=320, a property of the buffer layout and not of any thread.
     *
     * Nothing now indicates a stack problem. The reports that motivated this were
     * the canary check reading FreeRTOS's own initial context frame at the bottom
     * of the buffer and concluding the canary had been consumed. With that
     * debunked there is no evidence left to size stacks against, and I have made
     * the measurement wrong twice, so it is not worth a third attempt until a raw
     * word dump establishes where the frame and the fill actually are. */
}

static void sink_init(void)
{
    xt_sink_init_n++;

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

#ifdef TX_USE_IDF_DRIVER
    /* Deliberately AFTER i2s_init(): if creating an IDF channel did disturb the
     * GDMA block, we want to see it happen to a transmitter that is already
     * configured, because that is the ordering the real cutover has to survive.
     * The two registers printed are exactly the ones pcm-xtensa.c:240 records as
     * the casualties of a block reset -- INT_ENA 0x06 -> 0x00 and LINK losing
     * the descriptor address -- so this says directly whether the claim in
     * i2s_tx_chan_create() holds on hardware and not just in source. */
    {
        uint32_t ena_before = rd(GDMA_TX_INT_ENA);
        uint32_t link_before = rd(GDMA_TX_LINK);

        bool ok = i2s_tx_chan_create();

        printf("TX1a: GDMA INT_ENA %08x -> %08x   LINK %08x -> %08x  %s\n",
               (unsigned)ena_before, (unsigned)rd(GDMA_TX_INT_ENA),
               (unsigned)link_before, (unsigned)rd(GDMA_TX_LINK),
               (ok && ena_before == rd(GDMA_TX_INT_ENA) &&
                link_before == rd(GDMA_TX_LINK))
                   ? "UNCHANGED (claim holds)" : "DISTURBED (revert stage 1a)");
    }
#endif

    audiohw_preinit();

    /* The capture ADC is deliberately not brought up here. This is the playback
     * sink: resetting the ES7210 and programming its dividers on every boot --
     * and on every playback rate change -- and holding the I2C bus across two
     * 10ms delays to do it, belongs to the recording session that needs it and
     * nowhere else. It now happens in pcm_rec_dma_start(). */
    sink_set_freq(builtin_pcm_sink.caps.default_freq);

    /* TEMPORARY: the boot baseline. Every later arena dump is read as a delta
       from this picture of who holds what. */
    xt_arena_dump("at boot");

    /* Snapshot the transmit path the instant initialisation completes. Compared
     * against the same dump later, this says whether the configuration was ever
     * there at all or was taken away afterwards. */
    printf("AUDHW -- immediately after sink_init --\n");
    i2s_audio_probe();

    /* Periodic playback sample; see i2s_probe_tick(). */
    tick_add_task(i2s_probe_tick);
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

/* TEMPORARY: two registers, on demand, from the recording path. i2s_audio_probe()
 * prints a whole wall of state and is only reachable at boot and at a playback
 * stop that this bug never lets happen, so narrowing it to the two numbers that
 * actually changed -- GDMA_TX_INT_ENA going 0x06 -> 0x00 and GDMA_TX_LINK's
 * address field going 0x0cf700 -> 0x000000 -- and calling it either side of each
 * IDF call in the session lifecycle is what names the offender. */
void i2s_tx_gdma_peek(const char *where)
{
    printf("GDMA %-22s ena=%08x link=%08x\n", where,
           (unsigned)rd(GDMA_TX_INT_ENA), (unsigned)rd(GDMA_TX_LINK));
}
