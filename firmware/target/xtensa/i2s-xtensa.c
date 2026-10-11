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

#ifdef HAVE_RECORDING
#include "es7210.h"
#endif

/* MCLK, SLOT_BITS, SLOT_NUM, I2S_BCLK_DIV, the pin map and the descriptor layout
 * all come from i2s-xtensa.h. */

/* XTAL is the clock source. APLL would be more accurate but needs the clock
 * tree, which Rockbox does not own. */
#define XTAL_HZ 40000000u
#define MAX_FRACT 512
#define MAX_INTEG 256

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/* Ring of two descriptors, one per half of the ping-pong pair. */
/* .srambss, not .bss: GDMA takes the descriptor address from a 20-bit register
 * based at 0x3FC00000, so a descriptor in the PSRAM window loses its high bits.
 * app.lds puts .srambss in internal SRAM.
 *
 * volatile: the DMA writes the descriptor back after it transmits the buffer, so the
 * owner bit changes through a store the compiler cannot see. The handler reads it to
 * decide whether a half is free, and without the qualifier that test can be hoisted.
 *
 * cur is not volatile: it is only touched by the handler and by sink_play, and the
 * interrupt mask means the handler cannot run while sink_play is inside the lock. */
static volatile struct gdma_desc desc[2] __attribute__((aligned(4), section(".srambss")));
/* GDMA requires a 4-byte aligned buffer address */
static unsigned char dma_buf[2][GDMA_MAX_DESC_LEN + 1] __attribute__((aligned(4), section(".srambss")));

/* cur is the half that is free to write. When DONE fires, the half that just
 * finished transmitting is cur ^ 1, so the mixer's next frame goes into cur. */
static int cur;
static int locked;
static bool playing;

static int div_integ, div_num, div_denom;

#ifdef HAVE_RECORDING
static void program_rx_div(void);
#endif

/* Best rational approximation of freq_error / exp_freq with denominator < max.
 * Ported from ESP-IDF's hal_utils_calc_clk_div_frac_accurate. */
static void calc_div(unsigned long exp_freq)
{
    div_integ = XTAL_HZ / exp_freq;
    unsigned long rem = XTAL_HZ % exp_freq;

    if (!rem) { div_num = 0; div_denom = 1; return; }

    /* Carry to the next integer if the fraction is closer to 1 than to the
     * best fraction reachable under the denominator bound. */
    if (rem >= exp_freq - exp_freq / (MAX_FRACT - 1) * 2) {
        div_integ++;
        div_num = 0;
        div_denom = 1;
        return;
    }

    unsigned long long best = 1ULL << 63;
    div_denom = 1;
    div_num = 0;
    for (unsigned long a = 2; a < MAX_FRACT; a++) {
        unsigned long b = (a * rem + exp_freq / 2) / exp_freq;
        unsigned long long sub = exp_freq * b > rem * a ? exp_freq * b - rem * a
                                                        : rem * a - exp_freq * b;
        if (sub < best) {
            best = sub;
            div_denom = a;
            div_num = b;
        }
    }
}

static void set_raw_div(unsigned integ, unsigned x, unsigned y, unsigned z, unsigned yn1)
{
    wr(I2S_TX_CLKM_CONF, CLKM_CONF_CLK_EN | CLKM_CONF_CLK_ACTIVE |
                         (CLKM_CONF_CLK_SEL_XTAL << 27) | integ);
    wr(I2S_TX_CLKM_DIV_CONF, (yn1 << 27) | (x << 18) | (y << 9) | z);

    /* tx_update latches the APB writes into the I2S clock domain. It has to be a
     * read-modify-write: a bare write of this register also clears tx_start, and
     * the tx_stop_en/tx_pcm_bypass/tx_left_align reset defaults that the standard
     * mode depends on.
     */
    wr(I2S_TX_CONF, rd(I2S_TX_CONF) | TX_CONF_UPDATE);
    while (rd(I2S_TX_CONF) & TX_CONF_UPDATE)
        ;
}

/* i2s_ll_tx_set_mclk's mapping from (integer, num/den) to the four registers. */
static void program_div(void)
{
    unsigned yn1 = div_num * 2 > div_denom;
    unsigned z = yn1 ? div_denom - div_num : div_num;
    unsigned x = z ? div_denom / z - 1 : 0;
    unsigned y = z ? div_denom % z : 0;

    /* ESP-IDF's workaround for the divider latching inaccurately when the ratio
     * jumps from a low sample rate to a high one: write a coefficient set that no
     * real division could produce, then the target. (7, 317, 7, 3, 0) is the value
     * ESP-IDF uses; it corresponds to a division of 7 + 3/3 * (317 + 1) which is
     * not reachable from any integer/numerator/denominator triple.
     */
    set_raw_div(7, 317, 7, 3, 0);
    set_raw_div(div_integ, x, y, z, yn1);
}

void i2s_set_frequency(unsigned long rate)
{
    calc_div(rate * MCLK_MULTIPLE);
    program_div();
    es8311_set_clock(rate, rate * MCLK_MULTIPLE);

    /* The ADC is clocked from the same MCLK the transmitter drives out, so it
     * has to be reprogrammed whenever the rate changes. */
#ifdef HAVE_RECORDING
    program_rx_div();
    es7210_set_frequency(rate);
#endif
}

static void arm(int i, unsigned long len)
{
    desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
    desc[i].next   = (uint32_t)(unsigned long)&desc[i ^ 1];
    desc[i].dw0 = GDMA_MAX_DESC_LEN | (len << 12) | DESC_SUC_EOF | DESC_OWNER;
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

    /* Route the channel's interrupt to a level-1 slot and unmask the slot. The
     * per-event mask in GDMA_TX_INT_ENA is what sink_lock/sink_unlock toggle. */
    wr(XT_INTERRUPT_CORE0_BASE + 4 * XT_SRC_GDMA_OUT_CH0, XT_SLOT_GDMA_OUT_CH0);
    set_intenable(get_intenable() | (1u << XT_SLOT_GDMA_OUT_CH0));

    /* DONE and DSCR_ERR. sink_lock/sink_unlock toggle the DONE bit from here on. */
    wr(GDMA_TX_INT_ENA, GDMA_INT_DONE | GDMA_INT_DSCR_ERR);
    wr(GDMA_TX_INT_CLR, 0xFF);

    desc[0].buffer = (uint32_t)(unsigned long)dma_buf[0];
    desc[0].next   = (uint32_t)(unsigned long)&desc[1];
    desc[1].buffer = (uint32_t)(unsigned long)dma_buf[1];
    desc[1].next   = (uint32_t)(unsigned long)&desc[0];
    desc[0].dw0 = 0;
    desc[1].dw0 = 0;
    commit_dcache_range((const void *)desc, sizeof(desc));

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
}

void i2s_init(void)
{
    wr(SYSTEM_PERIP_CLK_EN0, rd(SYSTEM_PERIP_CLK_EN0) | CLK_EN_I2S1);
    wr(SYSTEM_PERIP_CLK_EN1, rd(SYSTEM_PERIP_CLK_EN1) | CLK_EN_DMA);

    /* GPSPI does not drive its own pins; neither does I2S once the signal is
     * routed through the GPIO matrix. */
    gpio_matrix_out(PIN_I2S_MCLK, SIG_I2S1_MCLK_OUT);
    gpio_matrix_out(PIN_I2S_BCLK, SIG_I2S1O_BCK_OUT);
    gpio_matrix_out(PIN_I2S_WS,   SIG_I2S1O_WS_OUT);
    gpio_matrix_out(PIN_I2S_DOUT, SIG_I2S1O_SD_OUT);

    gpio_set_direction(PIN_I2S_MCLK, 1);
    gpio_set_direction(PIN_I2S_BCLK, 1);
    gpio_set_direction(PIN_I2S_WS,   1);
    gpio_set_direction(PIN_I2S_DOUT, 1);

    /* TX_CONF1: BCLK divider, 16-bit channel, Phillips standard. Every "bits" and
     * "div" field here is stored as value-minus-one, so the divider that yields
     * BCLK = MCLK/8 is written as 7.
     */
    wr(I2S_TX_CONF1, ((SLOT_BITS - 1) << 13) |          /* tx_bits_mod */
                     ((SLOT_BITS - 1) << 24) |           /* tx_tdm_chan_bits */
                     ((SLOT_BITS - 1) << 18) |           /* tx_half_sample_bits */
                     ((I2S_BCLK_DIV - 1) << 7) |         /* tx_bclk_div_num */
                     (1u << 29));                        /* tx_msb_shift */

    /* Standard mode runs through the TDM path with two slots. The reset defaults
     * of tx_stop_en, tx_pcm_bypass and tx_left_align are 1 and a bare write of
     * this register clears them: with tx_pcm_bypass = 0 and tx_pcm_conf = 0 the
     * data goes through the A-law compressor.
     */
    wr(I2S_TX_TDM_CTRL, ((SLOT_NUM - 1) << 16) | 0x3);  /* tot_chan_num, chan mask */

    wr(I2S_TX_CONF, (1u << 19) |                    /* tx_tdm_en */
                    (1u << 15) |                    /* tx_left_align */
                    (1u << 13) |                    /* tx_stop_en */
                    (1u << 12) |                    /* tx_pcm_bypass */
                    (1u << 8));                     /* tx_update */
    while (rd(I2S_TX_CONF) & TX_CONF_UPDATE)
        ;

    gdma_init();

#ifdef HAVE_RECORDING
    i2s_rx_init();
#endif
}

#ifdef HAVE_RECORDING
/*
 * The receiver half of the bus. It lives on I2S0 because the ESP32-S3 signal map
 * gives a single data signal per port (I2S1I_SD_IN and I2S1O_SD_OUT are both 30),
 * so a second data line has to come from the other port.
 *
 * The receiver runs in slave mode and is fed the transmitter's BCLK and WS back
 * through the GPIO matrix, which keeps the ADC's frames and the DAC's frames in
 * lock-up rather than two free-running dividers that merely agree on frequency.
 */
static void program_rx_div(void)
{
    unsigned yn1 = div_num * 2 > div_denom;
    unsigned z = yn1 ? div_denom - div_num : div_num;
    unsigned x = z ? div_denom / z - 1 : 0;
    unsigned y = z ? div_denom % z : 0;

    wr(I2S0_RX_CLKM_CONF, CLKM_CONF_CLK_EN | CLKM_CONF_CLK_ACTIVE |
                          (CLKM_CONF_CLK_SEL_XTAL << 27) | div_integ);
    wr(I2S0_RX_CLKM_DIV_CONF, (yn1 << 27) | (x << 18) | (y << 9) | z);
    wr(I2S0_RX_CONF, rd(I2S0_RX_CONF) | RX_CONF_UPDATE);
    while (rd(I2S0_RX_CONF) & RX_CONF_UPDATE)
        ;
}

void i2s_rx_init(void)
{
    wr(SYSTEM_PERIP_CLK_EN0, rd(SYSTEM_PERIP_CLK_EN0) | CLK_EN_I2S0);

    /* A pad can drive a matrix input while still being driven as an output, so
     * pins 13 and 14 do double duty: the transmitter drives them and the receiver
     * reads them back. */
    gpio_input_enable(PIN_I2S_DIN, 1);
    gpio_input_enable(PIN_I2S_WS, 1);
    gpio_input_enable(PIN_I2S_BCLK, 1);

    gpio_matrix_in(PIN_I2S_DIN,  SIG_I2S0I_SD_IN);
    gpio_matrix_in(PIN_I2S_WS,   SIG_I2S0I_WS_IN);
    gpio_matrix_in(PIN_I2S_BCLK, SIG_I2S0I_BCK_IN);

    wr(I2S0_RX_CONF1, ((SLOT_BITS - 1) << 13) |       /* rx_bits_mod */
                      ((SLOT_BITS - 1) << 24) |       /* rx_tdm_chan_bits */
                      ((SLOT_BITS - 1) << 18) |       /* rx_half_sample_bits */
                      ((I2S_BCLK_DIV - 1) << 7) |     /* rx_bclk_div_num */
                      (1u << 29));                    /* rx_msb_shift */

    wr(I2S0_RX_TDM_CTRL, ((SLOT_NUM - 1) << 16) | 0x3);

    /* rx_stop_en, rx_pcm_bypass and rx_left_align are 1 at reset and a bare write
     * of this register clears them; with rx_pcm_bypass = 0 and rx_pcm_conf = 0 the
     * data goes through the A-law compressor.
     */
    wr(I2S0_RX_CONF, (1u << 19) |                    /* rx_tdm_en */
                     RX_CONF_SLAVE |
                     RX_CONF_LEFT_ALIGN |
                     RX_CONF_STOP_MODE |
                     RX_CONF_PCM_BYPASS);
    wr(I2S0_RX_CONF, rd(I2S0_RX_CONF) | RX_CONF_UPDATE);
    while (rd(I2S0_RX_CONF) & RX_CONF_UPDATE)
        ;

    program_rx_div();
}
#endif /* HAVE_RECORDING */

static void start_tx(void)
{
    wr(I2S_TX_CONF, rd(I2S_TX_CONF) | TX_CONF_START);
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start */
}

static void stop_tx(void)
{
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */
    wr(I2S_TX_CONF, rd(I2S_TX_CONF) & ~TX_CONF_START);
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
    if (size > GDMA_MAX_DESC_LEN)
    {
        /* Unreachable by the assertion above. Reaching it means the PCM layer is
         * handing us something the mixer was never designed to produce, and
         * quietly dropping the tail would play a corrupted frame rather than
         * tell us anything.
         */
        panicf("i2s: %lu byte DMA frame exceeds a GDMA descriptor",
               (unsigned long)size);
    }

    memcpy(dma_buf[cur], addr, size);
    arm(cur, size);
    /* GDMA fetches the descriptor and the buffer from memory, not from the
     * cache, so both have to be committed before the engine can mount them. */
    commit_dcache_range(dma_buf[cur], size);
    commit_dcache_range((const void *)&desc[cur], sizeof(desc[cur]));
    cur ^= 1;
}

static void refill(void)
{
    const void *addr;
    size_t size;

    if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
        return;

    write_desc(addr, size);
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

/* Runs on the interrupted thread's stack, so it has to stay small. The handler in
 * vectors.S has already saved a0 and a12-a15; this is the C part of the frame. */
void ICODE_ATTR gdma_irq(void)
{
    unsigned int st = rd(GDMA_TX_INT_ST);

    if (st & GDMA_INT_DONE)
        refill();

    if (st & GDMA_INT_DSCR_ERR)
    {
        /* Owner error or a bad descriptor word. The ring is broken, so stop the
         * channel, point it back at the start and begin again. */
        wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));      /* link.stop */
        wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
        cur = 0;
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
    write_desc(addr, size);

    if (!playing) {
        playing = true;
        start_tx();
    }

    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

static void sink_stop(void)
{
    playing = false;
    stop_tx();

    /* The link register still points at desc[0], so the ring restarts there on the
     * next play. If the DMA left desc[0] owned by itself, the engine would suspend
     * on it and nothing would ever come out. Hand both halves back to the CPU.
     */
    desc[0].dw0 = 0;
    desc[1].dw0 = 0;
    commit_dcache_range((const void *)desc, sizeof(desc));
    cur = 0;
}

static void sink_set_freq(uint16_t freq)
{
    i2s_set_frequency(hw_freq_sampr[freq]);
}

static void sink_init(void)
{
    i2s_init();

    audiohw_preinit();

    /* Reset and configure the ADC before the first rate is programmed, so the
     * clock registers land on a chip that has actually been reset. */
#ifdef HAVE_RECORDING
    es7210_init(hw_freq_sampr[builtin_pcm_sink.caps.default_freq]);
#endif

    sink_set_freq(builtin_pcm_sink.caps.default_freq);
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
