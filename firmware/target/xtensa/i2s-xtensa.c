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
#include <esp_rom_gpio.h>
#include <esp_clk_tree.h>

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
static volatile int cur;
/* The slot the engine is expected to retire next. The ring is walked in order,
 * so this advances one per completion regardless of whether that slot carried
 * anything. */
static volatile int retire;
static int locked;
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
    gdma_irq();
}


/* IDF's HAL context for the transmitter. The HAL is a thin wrapper over the
 * register block, so the context is just the peripheral base address. */
static i2s_hal_context_t tx_hal;
#ifdef HAVE_RECORDING
static i2s_hal_context_t rx_hal;
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
           "CHAIN: gdma out=%d dscr=%d park=%d | err=%d | link=%08x cur=%d own=%d%d%d%d len=%d,%d,%d,%d\n"
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
           (unsigned)rd(GDMA_TX_LINK), cur,
           xt_dscr_err_n,
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

    /* The ADC is clocked from the same MCLK the transmitter drives out, so it
     * has to be reprogrammed whenever the rate changes. */
#ifdef HAVE_RECORDING
    {
        /* The receiver is the slave on this bus: it takes BCLK and WS back from
         * the transmitter rather than generating its own, so the ADC and the DAC
         * share one frame instead of two oscillators that happen to agree. */
        i2s_hal_set_rx_clock(&rx_hal, &clk, I2S_CLK_SRC_DEFAULT, &mclk_div);
        i2s_hal_std_set_rx_slot(&rx_hal, true, &slot);
    }
    es7210_set_frequency(rate);
#endif
}

static void arm(int i, unsigned long len)
{
    desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
    desc[i].next   = (uint32_t)(unsigned long)&desc[(i + 1) % XT_TX_RING];
    /* len 0 keeps the slot owned by the DMA with nothing to move: the engine
     * takes it, transfers nothing, and moves on. That is what makes an empty
     * slot safe rather than fatal. */
    desc[i].dw0 = XT_TX_SLOT | (len << 12) | DESC_SUC_EOF | DESC_OWNER;
}

static void gdma_init(void)
{
    /* GDMA channel 0 -> I2S1 */
    wr(GDMA_TX_PERI_SEL, GDMA_TRIG_I2S1);

    /* conf0: auto write-back so the DMA hands the descriptor back to the CPU
     * after it has transmitted the buffer. Without this the owner bit stays
     * set to DMA forever and the ring never frees. */
    wr(GDMA_TX_CONF0, (1u << 2) | (1u << 4));   /* out_auto_wrback, outdscr_burst_en */
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
        /* Owned and empty. The engine may walk here at any time and must find a
         * descriptor it is allowed to take. */
        desc[i].dw0 = XT_TX_SLOT | DESC_SUC_EOF | DESC_OWNER;
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

#ifdef HAVE_RECORDING
    i2s_rx_init();
#endif
}

#ifdef HAVE_RECORDING

void i2s_rx_init(void)
{
    wr(SYSTEM_PERIP_CLK_EN0, rd(SYSTEM_PERIP_CLK_EN0) | CLK_EN_I2S0);
    /* Same requirement as the transmit side: clock enable without a reset release
     * leaves the receiver in reset and its registers read zero. */
    wr(SYSTEM_PERIP_RST_EN0, rd(SYSTEM_PERIP_RST_EN0) & ~CLK_EN_I2S0);

    /* A pad can drive a matrix input while still being driven as an output, so
     * pins 13 and 14 do double duty: the transmitter drives them and the receiver
     * reads them back. */
    gpio_input_enable(PIN_I2S_DIN, 1);
    gpio_input_enable(PIN_I2S_WS, 1);
    gpio_input_enable(PIN_I2S_BCLK, 1);

    gpio_matrix_in(PIN_I2S_DIN,  SIG_I2S0I_SD_IN);
    gpio_matrix_in(PIN_I2S_WS,   SIG_I2S0I_WS_IN);
    gpio_matrix_in(PIN_I2S_BCLK, SIG_I2S0I_BCK_IN);

    /* The block itself is configured by IDF's HAL, in the transmitter's own
     * context order: init here, then clock and slot on every rate change from
     * i2s_set_frequency(), which is where the slave role is declared. Doing the
     * slot here as well would be overwritten by that call anyway. */
    i2s_hal_init(&rx_hal, XT_I2S_RX_PORT);
}
#endif /* HAVE_RECORDING */

static void start_tx(void)
{
    i2s_ll_tx_start(tx_hal.dev);
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start */
}

static void stop_tx(void)
{
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */
    i2s_ll_tx_stop(tx_hal.dev);
}

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

static void write_desc(const void *addr, size_t size)
{
    if (size > XT_TX_SLOT)
    {
        /* Unreachable by the assertion above. Reaching it means the PCM layer is
         * handing us something the mixer was never designed to produce, and
         * quietly dropping the tail would play a corrupted frame rather than
         * tell us anything.
         */
        panicf("i2s: %lu byte DMA frame exceeds a staging slot",
               (unsigned long)size);
    }

    memcpy(dma_buf[cur], addr, size);

    /* Take the line from memory before looking at it or modifying it. The engine
     * may have cleared this descriptor's owner, or its neighbour's, since we last
     * looked; arming into a stale line and committing it would hand that stale
     * state back to the DMA as if it were new. The line is clean at this point
     * because every arm() is committed immediately below. */
    discard_dcache_range((const void *)&desc[cur], sizeof(desc[cur]));

    {
        const int wd_idx = cur;
        const unsigned long wd_before = desc[wd_idx].dw0;

        /* TEMPORARY: the slot armed, and what dw0 held once the line had been
         * taken from memory. If wd_before already shows the owner bit set, the
         * previous hand-off never completed and the ring is a step out of phase
         * with the engine. */
        xt_wd_n++;
        xt_wd_last_idx = wd_idx;
        if (xt_wd_n <= 4) {
            xt_wd_idx_hist[xt_wd_n - 1] = wd_idx;
            xt_wd_dw0_hist[xt_wd_n - 1] = wd_before;
        }
    }

    arm(cur, size);
    /* GDMA fetches the descriptor and the buffer from memory, not from the
     * cache, so both have to be committed before the engine can mount them. */
    commit_dcache_range(dma_buf[cur], size);
    commit_dcache_range((const void *)&desc[cur], sizeof(desc[cur]));
    xt_wd_last_dw0 = desc[cur].dw0;
    cur = (cur + 1) % XT_TX_RING;
    queued++;

    /* Arming a descriptor is not enough on its own.
     *
     * When the engine reaches a descriptor the CPU still owns, it parks its
     * descriptor FSM and stops. From that point a later arm() is invisible: the
     * engine is no longer walking the list, so it never notices the descriptor it
     * now owns. The link has to be restarted. That is what gdma_ll_tx_restart()
     * is for -- "restart a new outlink right after the last descriptor" -- and
     * what IDF's gdma_append() does on every hand-off.
     *
     * This is exactly the state the readout showed: a descriptor armed and owned
     * by the DMA (d0own=1), the FIFO empty, no interrupt pending, and out_state
     * reporting the engine idle. out_eof fired once for the first descriptor,
     * refill armed the second, and nothing ever came back for it -- so the
     * transmitter starved, tx_idle stayed asserted, and upstream the mixer sat on
     * a ready buffer with half a megabyte of PCM queued behind it.
     *
     * Bit positions from gdma_reg.h: outlink.stop BIT(20), start BIT(21),
     * restart BIT(22), park BIT(23). start and restart are both R/W/SC.
     */
    unsigned int link = rd(GDMA_TX_LINK);

    if (link & (1u << 23))      /* outlink.park: the descriptor FSM is idle */
        wr(GDMA_TX_LINK, link | (1u << 22) | (1u << 21));   /* restart | start */
}

static void refill(void)
{
    const void *addr;
    size_t size;

    /* TEMPORARY: which branch refill actually took. Every attempt to reason about
     * the stall from side effects has produced a contradiction -- irq=1 says this
     * ran, yet queued, cur and d1 are all still at their post-sink_play values,
     * and stop stayed 0 which rules out the complete-callback failure path. So
     * record the path instead of inferring it:
     *   1 = reported the completion and ops.play had already queued a buffer
     *   2 = reported, then asked explicitly and was told there is no more
     *   3 = reported, asked, and armed a descriptor
     */
    xt_refill_n++;

    /* Report the consumed buffer BEFORE asking for another one.
     *
     * This is the order every working sink driver uses -- see
     * firmware/target/arm/imx233/pcm-imx233.c, whose ISR ends with
     * pcm_play_dma_status_callback(PCM_DMAST_STARTED). Telling the layer above
     * that a buffer was consumed is what makes the mixer produce the next one
     * and hand it down through builtin_pcm_sink.ops.play, which arms a
     * descriptor for us. In steady state we therefore never need to ask.
     *
     * Asking first was the bug. On the very first completion the mixer had never
     * been told to build a second buffer, so mixer_pcm_callback() returned
     * next_size == 0, pcm_get_more_int() returned false,
     * pcm_play_dma_complete_callback() called pcm_play_stop_int() -- which sets
     * pcm_callback_for_more = NULL -- and playback was latched off for good. The
     * codec then went on decoding into a PCM buffer nothing could ever ask for:
     * measured at 209 inserts with the transmit path idle after a single 1024
     * byte frame.
     */
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);

    if (queued > 0) {
        xt_refill_branch = 1;
        return;     /* ops.play already handed us the next buffer */
    }

    /* Nothing was queued behind it, so ask explicitly. This is the fallback, not
     * the normal path. */
    if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size)) {
        xt_refill_branch = 2;
        return;
    }

    xt_refill_branch = 3;
    write_desc(addr, size);
}

/* Runs on the interrupted thread's stack, so it has to stay small. The handler in
 * vectors.S has already saved a0 and a12-a15; this is the C part of the frame. */
void ICODE_ATTR gdma_irq(void)
{
    unsigned int st = rd(GDMA_TX_INT_ST);

    if (st & GDMA_INT_DONE)
    {
        const int r = retire;
        retire = (retire + 1) % XT_TX_RING;

        /* Which slot retired decides everything here. An empty slot is in the
         * ring precisely so the engine has something owned to take, and it
         * completes like any other -- but nothing was played, so the mixer must
         * not be told a buffer was consumed. Reporting STARTED for an empty slot
         * would advance the mixer over audio nobody heard and drift the stream
         * by a frame every time the pipeline runs dry. */
        discard_dcache_range((const void *)&desc[r], sizeof(desc[r]));

        if ((desc[r].dw0 >> 12) & 0xfff)
        {
            xt_sink_irq_n++;
            if (queued > 0)
                queued--;
            refill();
        }
    }

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
           leave the ring alone and only restart the walk. */
        xt_dscr_err_n++;
        wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));      /* link.stop */
        wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
    }

    /* Clear only what fired. int_clr is write-to-clear. */
    wr(GDMA_TX_INT_CLR, st);
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
    if (--locked == 0)
        wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DONE);
}

static void sink_play(const void *addr, size_t size)
{
    xt_sink_play_n++;
    xt_sink_last_size = size;

    write_desc(addr, size);

    if (!playing) {
        playing = true;
        start_tx();
    }
}

static void sink_stop(void)
{
    xt_sink_stop_n++;

    playing = false;
    queued = 0;
    stop_tx();

    /* The link register still points at desc[0], so the ring restarts there on the
     * next play. If the DMA left desc[0] owned by itself, the engine would suspend
     * on it and nothing would ever come out. Hand both halves back to the CPU.
     */
    for (int i = 0; i < XT_TX_RING; i++)
        desc[i].dw0 = XT_TX_SLOT | DESC_SUC_EOF | DESC_OWNER;
    commit_dcache_range((const void *)desc, sizeof(desc));
    cur = 0;
    retire = 0;
}

static void sink_set_freq(uint16_t freq)
{
    i2s_set_frequency(hw_freq_sampr[freq]);
}

static void sink_init(void)
{
    xt_sink_init_n++;

    i2s_init();

    audiohw_preinit();

    /* Reset and configure the ADC before the first rate is programmed, so the
     * clock registers land on a chip that has actually been reset. */
#ifdef HAVE_RECORDING
    es7210_init(hw_freq_sampr[builtin_pcm_sink.caps.default_freq]);
#endif

    sink_set_freq(builtin_pcm_sink.caps.default_freq);

    /* Snapshot the transmit path the instant initialisation completes. Compared
     * against the same dump later, this says whether the configuration was ever
     * there at all or was taken away afterwards. */
    printf("AUDHW -- immediately after sink_init --\n");
    i2s_audio_probe();
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
