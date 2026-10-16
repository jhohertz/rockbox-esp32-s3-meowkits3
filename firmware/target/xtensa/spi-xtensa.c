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

volatile unsigned int xt_spi_calls;
volatile unsigned int xt_spi_chunks;
volatile unsigned int xt_spi_stuck;
volatile unsigned int xt_spi_spins;
volatile unsigned int xt_spi_maxlen;
volatile unsigned int xt_spi_iter_max;

void spi_write(const unsigned char *data, int len)
{
    while (len > 0)
    {
        int words = (len + 3) / 4;
        if (words > SPI_BUF_WORDS)
            words = SPI_BUF_WORDS;

        /* The FIFO is addressed in whole words, but the transfer length is not:
         * MS_DLEN is an exact bit count. Rounding it up to a multiple of 32 pads
         * every command with 24 zero bits, which the panel reads as argument
         * bytes, so CASET/RASET never receive the window they were given.
         */
        int n = len < words * 4 ? len : words * 4;

        for (int i = 0; i < words; i++)
        {
            uint32_t w = 0;

            for (int b = 0; b < 4; b++)
            {
                int idx = i * 4 + b;
                if (idx < n)
                    w |= (uint32_t)data[idx] << (8 * b);
            }

            wr(SPI_W0 + 4 * (unsigned)i, w);
        }

        wr(SPI_MS_DLEN, (unsigned)n * 8u - 1u);
        spi_apply_config();
        wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_USR);

        /* USR is self-clearing: it drops when the transfer completes. Bounded so
           a stalled transfer is countable rather than a silent hang -- an
           unbounded spin here is indistinguishable from never having arrived. */
        unsigned spins = 0;
        while (rd(SPI_CMD) & SPI_CMD_USR)
        {
            if (++spins > 200000)
            {
                xt_spi_stuck++;
                break;
            }
        }
        xt_spi_calls++;

        len -= n;
        data += n;
    }
}
