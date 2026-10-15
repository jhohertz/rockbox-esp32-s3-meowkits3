/***************************************************************************
 * GPSPI2 master, polled, for the ST7789 panel.
 *
 * Three things are easy to get wrong on this peripheral:
 *
 * 1. The clock does not come from the 240 MHz CPU clock. The master clock
 *    source is selected in CLK_GATE: bit 2 chooses PLL_CLK_80M, cleared gives
 *    XTAL (40 MHz). CLK_EQU_SYSCLK in the CLOCK register makes spi_clk equal
 *    that source directly, which is the fastest setting available - and the
 *    reference firmware drives this panel at 80 MHz.
 *
 * 2. GPSPI has no "general read/write" mode bit. DMA mode is off unless
 *    dma_tx_ena/dma_rx_ena are set, so the CPU-controlled 16-word buffer at
 *    0x98 is the normal path.
 *
 * 3. The buffer is a little-endian byte array: the first byte to go out sits
 *    in bits 0-7 of SPI_W0. Both ESP-IDF's spi_ll_write_buffer_byte() and the
 *    Arduino ESP32 SPI driver place byte 0 in the low byte.
 *
 * The CONF-state registers (USER, CLOCK, CTRL, MS_DLEN) live in the APB domain
 * and have to be latched into the SPI clock domain with CMD.UPDATE before
 * CMD.USR starts the transfer.
 *
 * The pins are not enough on their own. GPIO 40 and 41 are MTDO and MTDI in
 * the IO MUX, and GPIO 39 is MTCK; all three default to MCU_SEL = 0, the JTAG
 * function. Routing FSPICLK/FSPIQ to "GPIO 41"/"GPIO 40" in the matrix does
 * nothing while the pad is muxed elsewhere - the transfer completes and the
 * clock line never moves. gpio_set_direction() now selects the GPIO function
 * as ESP-IDF's gpio_output_enable() does.
 ****************************************************************************/
#include "config.h"
#include "xtensa.h"
#include "bootlog.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)reg = v; }

static void spi_apply_config(void)
{
    wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_UPDATE);

    for (int i = 0; i < 2000 && (rd(SPI_CMD) & SPI_CMD_UPDATE); i++)
        ;
}

/* GPSPI clock. The module clock is PLL_CLK_80M (CLK_GATE bit 2) and the SPI
 * clock is that divided by (clkdiv_pre+1) * (clkcnt_n+1), so 80/(2*2) = 20 MHz.
 * The panel is specced to 80 MHz and the reference firmware drives it there, but
 * that path uses DMA; the polled CPU-buffer path should be proven at a slower
 * rate before trusting the top of the range.
 */
#define SPI_CLK_1M      ((15u << 0) | (7u << 6) | (15u << 12) | (4u << 18))
#define SPI_CLK_20M     ((1u << 0) | (0u << 6) | (1u << 12) | (1u << 18))

/* Measure the bit stream at the pin. A 64-byte transfer must produce exactly 512
 * clock edges; anything else means MS_DLEN is not being honoured and the panel is
 * receiving a stream that is longer or shorter than the buffer we filled.
 * The clock is dropped to 1 MHz so a software loop can resolve individual edges.
 */
#define SPI_PROBE_1M      ((15u << 0) | (7u << 6) | (15u << 12) | (4u << 18))

void spi_probe_edges(void)
{
    static const unsigned char pat[SPI_BUF_BYTES] = {
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01
    };
    unsigned prev = 0, pm = 0;
    int edges = 0, iters = 0, mtrans = 0;
    char wf[120];
    int wn = 0;

    wr(SPI_CLOCK, SPI_PROBE_1M);
    spi_apply_config();

    for (int i = 0; i < SPI_BUF_WORDS; i++)
        wr(SPI_W0 + 4 * (unsigned)i, (uint32_t)pat[i * 4] |
                                        ((uint32_t)pat[i * 4 + 1] << 8) |
                                        ((uint32_t)pat[i * 4 + 2] << 16) |
                                        ((uint32_t)pat[i * 4 + 3] << 24));

    wr(SPI_MS_DLEN, SPI_BUF_BYTES * 8 - 1);
    spi_apply_config();
    wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_USR);

    while (rd(SPI_CMD) & SPI_CMD_USR) {
        unsigned in1 = gpio_read(GPIO_IN1);
        unsigned s = (in1 >> (PIN_LCD_SCLK - 32)) & 1u;
        unsigned m = (in1 >> (PIN_LCD_MOSI - 32)) & 1u;

        if (s && !prev)
            edges++;
        prev = s;

        if (m != pm) {
            pm = m;
            if (wn < 120) wf[wn++] = m ? '1' : '0';
            mtrans++;
        }

        if (++iters > 200000)
            break;
    }

    wr(SPI_CLOCK, SPI_CLK_20M);
    spi_apply_config();

    boot_trace('E');
    boot_trace_hex((unsigned)edges);      /* clock periods actually emitted */
    boot_trace_hex((unsigned)iters);      /* loop iterations = transfer length */
    boot_trace_hex(rd(SPI_CLOCK));
    boot_trace_hex((unsigned)mtrans);
    boot_trace('W');
    for (int i = 0; i < wn; i++)
        boot_trace(wf[i]);
}

void spi_init(void)
{
    wr(XT_SYSTEM_BASE + 0x18, rd(XT_SYSTEM_BASE + 0x18) | SYSTEM_SPI2_CLK_EN);
    wr(SPI_CLK_GATE, SPI_CLK_EN | SPI_MST_CLK_ACTIVE | SPI_MST_CLK_SEL);

    /* GPSPI does not drive its own pins; the signals have to be routed through
     * the GPIO matrix. DC is a plain GPIO, not a controller signal. */
    gpio_matrix_out(PIN_LCD_SCLK, SIG_FSPICLK_OUT);
    gpio_matrix_out(PIN_LCD_MOSI, SIG_FSPID_OUT);

    gpio_set_direction(PIN_LCD_SCLK, 1);
    gpio_set_direction(PIN_LCD_MOSI, 1);
    gpio_set_direction(PIN_LCD_DC, 1);

    wr(SPI_DMA_CONF, 0);

    /* Write-only master transfer. WR_BIT_ORDER = 0, RD_BIT_ORDER = 0. */
    wr(SPI_CTRL, 0);
    wr(SPI_USER, SPI_USR_MOSI);
    wr(SPI_USER1, 0);

    /* spi_clk = the 80 MHz source, divided. 80/(2*2) = 20 MHz. The reference
     * drives this panel at 80 MHz, but that path uses DMA; the polled CPU-buffer
     * path is proven at 20 MHz first.
     */
    wr(SPI_CLOCK, SPI_CLK_20M);

    wr(SPI_MS_DLEN, SPI_BUF_BYTES * 32 - 1);
    spi_apply_config();
}

__attribute__((section(".srambss"))) volatile unsigned int xt_spi_calls;
__attribute__((section(".srambss"))) volatile unsigned int xt_spi_chunks;
__attribute__((section(".srambss"))) volatile unsigned int xt_spi_stuck;
__attribute__((section(".srambss"))) volatile unsigned int xt_spi_spins;
__attribute__((section(".srambss"))) volatile unsigned int xt_spi_maxlen;
__attribute__((section(".srambss"))) volatile unsigned int xt_spi_iter_max;
__attribute__((section(".srambss"))) volatile unsigned int rtc_maxlen;

void spi_write(const unsigned char *data, int len)
{
    static unsigned stuck;

    xt_spi_calls++;
    rtc_bump(27);                      /* spi_write entries */
    rtc_set(30, (unsigned)len);        /* last len seen */
    rtc_set(19, 0);                    /* iterations of this call */
    rtc_set(18, 0);                    /* max len seen so far */
    *(volatile unsigned int *)0x50001b50 = (unsigned)xt_spi_calls;
    *(volatile unsigned int *)0x50001b64 = 1;   /* entered */
    if ((unsigned)len > xt_spi_maxlen)
        xt_spi_maxlen = (unsigned)len;
    if (xt_spi_calls < 8) {
        boot_trace('x');
        boot_trace_hex((unsigned)len);
    }
    while (len > 0) {
        int words = (len + 3) / 4;
        if (words > SPI_BUF_WORDS)
            words = SPI_BUF_WORDS;

        /* The FIFO is addressed in whole words, but the transfer length is not:
         * MS_DLEN is an exact bit count. Rounding it up to a multiple of 32 pads
         * every command with 24 zero bits, which the panel reads as argument
         * bytes, so CASET/RASET never receive the window they were given.
         */
        int n = len < words * 4 ? len : words * 4;
        rtc_set(21, (unsigned)len);    /* remaining len at this iteration */
        rtc_set(22, (unsigned)n);      /* what this iteration consumes */
        rtc_bump(19);                  /* iterations of this call */
        if ((unsigned)len > rtc_maxlen) rtc_maxlen = (unsigned)len;
        rtc_bump(28);                  /* FIFO fills issued */

        for (int i = 0; i < words; i++) {
            uint32_t w = 0;

            for (int b = 0; b < 4; b++) {
                int idx = i * 4 + b;
                if (idx < n)
                    w |= (uint32_t)data[idx] << (8 * b);
            }

            wr(SPI_W0 + 4 * (unsigned)i, w);
        }

        wr(SPI_MS_DLEN, (unsigned)n * 8u - 1u);
        spi_apply_config();
        *(volatile unsigned int *)0x50001b54 = rd(SPI_CMD);
        wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_USR);
        *(volatile unsigned int *)0x50001b58 = rd(SPI_CMD);
        *(volatile unsigned int *)0x50001b64 = 2;   /* transfer started */

        /* USR is self-clearing: it drops when the transfer completes. Bound the
         * wait so a stalled transfer shows up as a trace instead of a silent hang;
         * the unbounded loop is what makes "boot stops after the first frame"
         * indistinguishable from "boot never got here".
         */
        unsigned spins = 0;
        while (rd(SPI_CMD) & SPI_CMD_USR) {
            if (++spins > 200000) {
                stuck++;
                xt_spi_stuck++;
                boot_trace('s');
                boot_trace_hex(rd(SPI_CMD));
                boot_trace_hex(rd(SPI_MS_DLEN));
                break;
            }
        }
        rtc_bump(29);                  /* transfers retired */
        *(volatile unsigned int *)0x50001b64 = 3;   /* transfer finished */
        *(volatile unsigned int *)0x50001b68 = spins;
        xt_spi_chunks++;
        xt_spi_spins += spins;
        {
            unsigned it = xt_spi_chunks - (xt_spi_calls ? 0 : 0);
            (void)it;
        }

        len -= n;
        data += n;
    }
}
