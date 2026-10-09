/***************************************************************************
 * GPSPI2 master, polled, for the ST7789 panel.
 *
 * Three things are easy to get wrong on this peripheral:
 *
 * 1. The clock does not come from the 240 MHz CPU clock. The master clock
 *    source is selected in CLK_GATE: bit 2 chooses PLL_CLK_80M, cleared gives
 *    XTAL (40 MHz). CLK_EQU_SYSCLK in the CLOCK register makes spi_clk equal
 *    that source directly, which is the fastest setting available.
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
 ****************************************************************************/
#include "config.h"
#include "xtensa.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)reg = v; }

void spi_init(void)
{
    wr(XT_SYSTEM_BASE + 0x18, rd(XT_SYSTEM_BASE + 0x18) | SYSTEM_SPI2_CLK_EN);
    wr(SPI_CLK_GATE, SPI_CLK_EN | SPI_MST_CLK_ACTIVE | SPI_MST_CLK_SEL);

    /* GPSPI does not drive its own pins; the signals have to be routed through
     * the GPIO matrix. DC is a plain GPIO, not a controller signal. */
    gpio_matrix_out(PIN_LCD_SCLK, SIG_FSPICLK_OUT);
    gpio_matrix_out(PIN_LCD_MOSI, SIG_FSPIQ_OUT);

    gpio_set_direction(PIN_LCD_SCLK, 1);
    gpio_set_direction(PIN_LCD_MOSI, 1);
    gpio_set_direction(PIN_LCD_DC, 1);

    wr(SPI_DMA_CONF, 0);

    /* Write-only master transfer. WR_BIT_ORDER = 0, RD_BIT_ORDER = 0. */
    wr(SPI_CTRL, 0);
    wr(SPI_USER, SPI_USR_MOSI);
    wr(SPI_USER1, 0);

    /* spi_clk = the 80 MHz source, undivided. */
    wr(SPI_CLOCK, SPI_CLK_EQU_SYSCLK);

    wr(SPI_MS_DLEN, SPI_BUF_BYTES * 8 - 1);
    wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_UPDATE);
}

void spi_write(const unsigned char *data, int len)
{
    while (len > 0) {
        int words = (len + 3) / 4;
        if (words > SPI_BUF_WORDS)
            words = SPI_BUF_WORDS;

        for (int i = 0; i < SPI_BUF_WORDS; i++) {
            uint32_t w = 0;

            if (i < words) {
                for (int b = 0; b < 4; b++) {
                    int idx = i * 4 + b;
                    if (idx < len)
                        w |= (uint32_t)data[idx] << (8 * b);
                }
            }

            wr(SPI_W0 + 4 * (unsigned)i, w);
        }

        wr(SPI_MS_DLEN, words * 8 - 1);
        wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_UPDATE);
        wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_USR);

        /* USR is self-clearing: it drops when the transfer completes. */
        while (rd(SPI_CMD) & SPI_CMD_USR)
            ;

        len -= words * 4;
        if (len < 0)
            break;

        data += words * 4;
    }
}
