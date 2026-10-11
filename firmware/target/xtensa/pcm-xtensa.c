/***************************************************************************
 * Recording over the I2S0 receiver.
 *
 * The transmitter (I2S1) owns the clock: it drives MCLK, BCLK and WS out to both
 * codecs. The receiver is put in slave mode and fed those same two signals back
 * through the GPIO matrix, so the ADC's frames and the DAC's frames are the same
 * frames rather than two oscillators that happen to run at the same rate.
 *
 * Data arrives through GDMA receive channel 0 into a two-descriptor ring in
 * internal SRAM. Each descriptor is 4092 bytes, which is the largest whole number
 * of words under GDMA's 4095-byte-per-descriptor limit, i.e. 1023 stereo frames.
 * While one half is being copied into the recorder's buffer the DMA is filling the
 * other, so the I2S FIFO never has to be drained by the CPU in real time.
 *
 * Internal SRAM is served by the L1 D-cache and GDMA does not go through it, so
 * every descriptor is written back before the engine can mount it, and every
 * buffer is discarded before the CPU reads what the engine put there.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "string.h"

#include "xtensa.h"
#include "i2s-xtensa.h"
#include "gpio-xtensa.h"
#include "es7210.h"
#include "pcm-internal.h"
#include "audiohw.h"
#include "kernel.h"

/* MeowKit wiring (bsp/config.h): the ES7210's data out is GPIO 3, and its
 * BCLK/WS are the same pins the ES8311 is driven from. */

static volatile struct gdma_desc rx_desc[2] __attribute__((aligned(4), section(".srambss")));
static unsigned char rx_buf[2][GDMA_RX_CHUNK] __attribute__((aligned(4), section(".srambss")));

/* The descriptor the engine is expected to be filling. Re-armed immediately after
 * it is retired, which is always before the ring wraps back around to it. */
static int rx_cur;
static bool rx_running;
static int rec_locked;

/* Where the recorder wants the next bytes, and how many are left in that buffer. */
static unsigned char *rec_ptr;
static size_t rec_left;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

static void rx_arm(int i, size_t len)
{
    rx_desc[i].buffer = (uint32_t)(unsigned long)rx_buf[i];
    rx_desc[i].next   = (uint32_t)(unsigned long)&rx_desc[i ^ 1];
    rx_desc[i].dw0 = GDMA_RX_CHUNK | (len << 12) | DESC_OWNER;
    commit_dcache_range((const void *)&rx_desc[i], sizeof(rx_desc[i]));
}

static size_t chunk_for_left(void)
{
    return rec_left < GDMA_RX_CHUNK ? rec_left : GDMA_RX_CHUNK;
}

static void rx_dma_init(void)
{
    wr(SYSTEM_PERIP_CLK_EN1, rd(SYSTEM_PERIP_CLK_EN1) | CLK_EN_DMA);

    /* GDMA receive channel 0 -> I2S0 */
    wr(GDMA_RX_PERI_SEL, GDMA_TRIG_I2S0);
    wr(GDMA_RX_CONF0, (1u << 0) | (1u << 2) | (1u << 3));  /* in_rst, indscr_burst_en, in_data_burst_en */
    wr(GDMA_RX_CONF0, (1u << 2) | (1u << 3));              /* clear the self-clearing reset bit */
    wr(GDMA_RX_CONF1, (1u << 12));                        /* in_check_owner */

    wr(XT_INTERRUPT_CORE0_BASE + 4 * XT_SRC_GDMA_IN_CH0, XT_SLOT_GDMA_IN_CH0);
    set_intenable(get_intenable() | (1u << XT_SLOT_GDMA_IN_CH0));

    wr(GDMA_RX_INT_ENA, GDMA_INT_IN_DONE | GDMA_INT_IN_DSCR_ERR | GDMA_INT_IN_DSCR_EMPTY);
    wr(GDMA_RX_INT_CLR, 0xFF);

    rx_desc[0].buffer = (uint32_t)(unsigned long)rx_buf[0];
    rx_desc[0].next   = (uint32_t)(unsigned long)&rx_desc[1];
    rx_desc[1].buffer = (uint32_t)(unsigned long)rx_buf[1];
    rx_desc[1].next   = (uint32_t)(unsigned long)&rx_desc[0];
    rx_desc[0].dw0 = 0;
    rx_desc[1].dw0 = 0;
    commit_dcache_range((const void *)rx_desc, sizeof(rx_desc));

    wr(GDMA_RX_LINK, (uint32_t)(unsigned long)&rx_desc[0] & 0xFFFFF);
}

static void rx_start(void)
{
    wr(I2S0_RX_CONF, rd(I2S0_RX_CONF) | RX_CONF_FIFO_RESET);
    wr(I2S0_RX_CONF, rd(I2S0_RX_CONF) | RX_CONF_START);
    wr(GDMA_RX_LINK, rd(GDMA_RX_LINK) | GDMA_RX_LINK_START);
}

static void rx_stop(void)
{
    wr(GDMA_RX_LINK, rd(GDMA_RX_LINK) | GDMA_RX_LINK_STOP);
    wr(I2S0_RX_CONF, rd(I2S0_RX_CONF) & ~RX_CONF_START);

    /* Hand both descriptors back to the CPU so the next start does not suspend on
     * one the engine still owns. */
    rx_desc[0].dw0 = 0;
    rx_desc[1].dw0 = 0;
    commit_dcache_range((const void *)rx_desc, sizeof(rx_desc));
}

static void drain(int i)
{
    size_t n = chunk_for_left();

    if (n == 0)
        return;

    /* The engine wrote these bytes behind the cache; drop any stale copy before
     * the recorder reads them. */
    commit_discard_dcache_range(rx_buf[i], n);
    memcpy(rec_ptr, rx_buf[i], n);

    rec_ptr += n;
    rec_left -= n;
}

void ICODE_ATTR gdma_rx_irq(void)
{
    unsigned int st = rd(GDMA_RX_INT_ST);

    if (st & GDMA_INT_IN_DONE) {
        drain(rx_cur);

        if (rec_left == 0) {
            void *addr;
            size_t size;

            if (!pcm_rec_dma_complete_callback(PCM_DMAST_OK, &addr, &size)) {
                rx_running = false;
                rx_stop();
                wr(GDMA_RX_INT_CLR, st);
                return;
            }

            rec_ptr = (unsigned char *)addr;
            rec_left = size;
        }

        rx_arm(rx_cur, chunk_for_left());
        rx_cur ^= 1;
    }

    if (st & (GDMA_INT_IN_DSCR_ERR | GDMA_INT_IN_DSCR_EMPTY)) {
        /* The ring is broken: stop the channel, point it back at the start and
         * re-arm both halves. */
        rx_stop();
        wr(GDMA_RX_LINK, (uint32_t)(unsigned long)&rx_desc[0] & 0xFFFFF);
        rx_cur = 0;
        rx_running = false;
        pcm_rec_dma_status_callback(PCM_DMAST_ERR_DMA);
    }

    wr(GDMA_RX_INT_CLR, st);
}

void pcm_rec_dma_init(void)
{
    rx_dma_init();
    es7210_start();
}

void pcm_rec_dma_close(void)
{
    es7210_stop();
}

void pcm_rec_dma_start(void *addr, size_t size)
{
    rec_ptr = (unsigned char *)addr;
    rec_left = size;
    rx_cur = 0;

    rx_arm(0, chunk_for_left());
    rx_arm(1, chunk_for_left());

    rx_start();
    rx_running = true;

    pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
}

void pcm_rec_dma_stop(void)
{
    if (!rx_running)
        return;

    rx_running = false;
    rx_stop();
}

const void *pcm_rec_dma_get_peak_buffer(void)
{
    return rec_ptr;
}

/* Mask the completion event rather than the interrupt slot, so a buffer that
 * finishes while the recorder is inside a locked section is not lost: int_raw
 * stays set and the interrupt fires as soon as the mask is lifted. */
void pcm_rec_lock(void)
{
    if (rec_locked++ == 0 && rx_running)
        wr(GDMA_RX_INT_ENA, rd(GDMA_RX_INT_ENA) & ~GDMA_INT_IN_DONE);
}

void pcm_rec_unlock(void)
{
    if (--rec_locked == 0 && rx_running)
        wr(GDMA_RX_INT_ENA, rd(GDMA_RX_INT_ENA) | GDMA_INT_IN_DONE);
}
