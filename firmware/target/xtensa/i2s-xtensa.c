/***************************************************************************
 * ESP32-S3 I2S1 transmitter + GDMA polled sink.
 *
 * Two things shape this driver:
 *
 * 1. DMA completion is *polled* by a helper thread rather than signalled. This is a
 *    choice, not a necessity: a handler can be installed on this core by setting
 *    VECBASE and unmasking a slot in INTENABLE (see AGENTS.md), but the polled version
 *    does not depend on the vector table being in place and does not have to fit inside
 *    the interrupt stack.
 *
 * 2. GDMA's descriptor address register is 20 bits wide, so descriptors have to
 *    sit inside the 0x3FC00000 window. That is where .bss is placed, so the
 *    descriptor ring and the DMA buffers are plain .bss. They are also reached
 *    through the non-cached data bus view, which means no cache maintenance is
 *    needed around the DMA - the CPU's writes are already in memory.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "string.h"

#include "i2s-xtensa.h"
#include "gpio-xtensa.h"
#include "es8311.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "audiohw.h"
#include "kernel.h"
#include "pcm_sampr.h"
#include "pcm_mixer.h"

/* MeowKit wiring (bsp/config.h) */
#define PIN_I2S_MCLK 15
#define PIN_I2S_BCLK 14
#define PIN_I2S_WS   13
#define PIN_I2S_DOUT 16

/* MCLK, SLOT_BITS, SLOT_NUM and I2S_BCLK_DIV come from i2s-xtensa.h. */

/* XTAL is the clock source. APLL would be more accurate but needs the clock
 * tree, which Rockbox does not own. */
#define XTAL_HZ 40000000u
#define MAX_FRACT 512
#define MAX_INTEG 256

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

struct gdma_desc {
    uint32_t dw0;      /* size[11:0], length[23:12], err_eof[28], suc_eof[30], owner[31] */
    uint32_t buffer;
    uint32_t next;
};

/* dw0: size[11:0] is the buffer capacity, length[23:12] is what the DMA sends. */
#define DESC_SUC_EOF (1u << 30)
#define DESC_OWNER   (1u << 31)

/* Ring of two descriptors, one per half of the ping-pong pair. */
/* Plain .bss: app.lds places it at DRAMORIG = 0x3FC88000, which is inside the
 * 20-bit window GDMA can address.
 *
 * volatile: the DMA writes the descriptor back after it transmits the buffer,
 * so the owner bit changes outside anything this function does. Without the
 * qualifier GCC folds the spin loop into a single test and never notices. */
static volatile struct gdma_desc desc[2];
static unsigned char dma_buf[2][GDMA_MAX_DESC_LEN + 1];

/* volatile because sink_play runs on the mixer thread and dma_callback on the
 * polling thread, and they hand cur between each other. */
static volatile int cur;          /* descriptor the DMA is working on */
static volatile int locked;
static volatile bool playing;
static volatile bool pending;

static int div_integ, div_num, div_denom;

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

/* i2s_ll_tx_set_mclk's mapping from (integer, num/den) to the four registers. */
static void program_div(void)
{
    unsigned yn1 = div_num * 2 > div_denom;
    unsigned z = yn1 ? div_denom - div_num : div_num;
    unsigned x = z ? div_denom / z - 1 : 0;
    unsigned y = z ? div_denom % z : 0;

    wr(I2S_TX_CLKM_CONF, CLKM_CONF_CLK_EN | CLKM_CONF_CLK_ACTIVE |
                         (CLKM_CONF_CLK_SEL_XTAL << 27) | div_integ);
    wr(I2S_TX_CLKM_DIV_CONF, (yn1 << 27) | (x << 18) | (y << 9) | z);

    /* tx_update latches the APB writes into the I2S clock domain */
    wr(I2S_TX_CONF, TX_CONF_UPDATE);
    while (rd(I2S_TX_CONF) & TX_CONF_UPDATE)
        ;
}

void i2s_set_frequency(unsigned long rate)
{
    calc_div(rate * MCLK_MULTIPLE);
    program_div();
    es8311_set_clock(rate, rate * MCLK_MULTIPLE);
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

    wr(GDMA_TX_INT_CLR, 0xFF);

    desc[0].buffer = (uint32_t)(unsigned long)dma_buf[0];
    desc[0].next   = (uint32_t)(unsigned long)&desc[1];
    desc[1].buffer = (uint32_t)(unsigned long)dma_buf[1];
    desc[1].next   = (uint32_t)(unsigned long)&desc[0];
    desc[0].dw0 = 0;
    desc[1].dw0 = 0;

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

    /* TX_CONF1: BCLK divider, 16-bit channel, Phillips standard */
    wr(I2S_TX_CONF1, ((SLOT_BITS - 1) << 13) |          /* tx_bits_mod */
                     ((SLOT_BITS - 1) << 24) |           /* tx_tdm_chan_bits */
                     ((SLOT_BITS - 1) << 18) |           /* tx_half_sample_bits */
                     (I2S_BCLK_DIV << 7) |               /* tx_bclk_div_num */
                     (1u << 29));                        /* tx_msb_shift */

    /* Standard mode runs through the TDM path with two slots */
    wr(I2S_TX_TDM_CTRL, ((SLOT_NUM - 1) << 16) | 0x3);  /* tot_chan_num, chan mask */

    wr(I2S_TX_CONF, (1u << 19) | (1u << 8));            /* tx_tdm_en, tx_update */
    while (rd(I2S_TX_CONF) & TX_CONF_UPDATE)
        ;

    gdma_init();
}

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

static void dma_callback(void)
{
    if (locked) {
        pending = playing;
        return;
    }

    const void *addr;
    size_t size;

    if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
        return;

    if (size > GDMA_MAX_DESC_LEN)
        size = GDMA_MAX_DESC_LEN;

    memcpy(dma_buf[cur], addr, size);
    arm(cur, size);
    cur ^= 1;

    pcm_play_dma_status_callback(PCM_DMAST_STARTED);
}

/* Polling latency is bounded by the scheduler tick (HZ = 100, so 10 ms), which is
 * longer than one mixer frame at any rate above ~25 kHz. The spin therefore covers
 * a whole frame period at the configured rate: while the DMA is transmitting, the
 * thread sees the completion inside the spin and refills before the next frame is
 * needed. The yield only happens when the DMA has already been idle for a full
 * frame, so it cannot itself cause an underrun.
 *
 * This is the cost of not being able to install an exception handler on this core
 * config: the completion is detected by a thread that burns CPU instead of by an
 * ISR that costs nothing.
 */
static unsigned spin_limit(void)
{
    struct pcm_sink *s = pcm_get_current_sink();
    unsigned long rate = s->configured_freq;

    if (!rate)
        rate = hw_freq_sampr[builtin_pcm_sink.caps.default_freq];

    /* bytes per frame / bytes per second = seconds per frame, times the core
     * frequency, divided by roughly 3 cycles per spin iteration */
    unsigned long long cyc = (unsigned long long)(MIX_FRAME_SAMPLES * SLOT_NUM * SLOT_BITS / 8)
                             * (unsigned long long)cpu_frequency / (4u * rate);
    unsigned n = (unsigned)(cyc / 3u);

    return n ? n : 1;
}

static void dma_thread(void)
{
    while (playing) {
        if (desc[cur].dw0 & DESC_OWNER) {          /* still owned by DMA: not done */
            unsigned spins = spin_limit();
            while (spins-- && (desc[cur].dw0 & DESC_OWNER))
                ;
            if (desc[cur].dw0 & DESC_OWNER) {
                sleep(0);
                continue;
            }
        }
        dma_callback();
    }
}

static void sink_lock(void)   { ++locked; }

static void sink_unlock(void)
{
    if (--locked == 0 && pending) {
        pending = false;
        dma_callback();
    }
}

static void sink_play(const void *addr, size_t size)
{
    if (size > GDMA_MAX_DESC_LEN)
        size = GDMA_MAX_DESC_LEN;

    memcpy(dma_buf[cur], addr, size);
    arm(cur, size);

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
}

static void sink_set_freq(uint16_t freq)
{
    i2s_set_frequency(hw_freq_sampr[freq]);
}

static void sink_init(void)
{
    static unsigned char thread_stack[2048];

    i2s_init();

    audiohw_preinit();
    sink_set_freq(builtin_pcm_sink.caps.default_freq);

    /* Highest priority: this thread stands in for an interrupt handler, and a
     * late completion means an audible gap. */
    create_thread(dma_thread, thread_stack, sizeof(thread_stack), 0,
                  "i2s" IF_PRIO(, PRIORITY_REALTIME) IF_COP(, CPU));
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
