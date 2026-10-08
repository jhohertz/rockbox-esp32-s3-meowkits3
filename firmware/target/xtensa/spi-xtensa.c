#include "config.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)reg = v; }

void spi_init(void)
{
    /* Route the controller signals to the pins through the GPIO matrix. DC is a
     * plain GPIO, not a controller signal. */
    gpio_matrix_out(PIN_LCD_SCLK, SIG_FSPICLK_OUT);
    gpio_matrix_out(PIN_LCD_MOSI, SIG_FSPIQ_OUT);

    gpio_set_direction(PIN_LCD_SCLK, 1);
    gpio_set_direction(PIN_LCD_MOSI, 1);
    gpio_set_direction(PIN_LCD_DC, 1);

    /* CPU-controlled FIFO mode. */
    wr(SPI_DMA_CONF, SPI_GRL_MODE);

    /* Write-only master transfer, MSB first. */
    wr(SPI_USER, SPI_USR_MOSI);
    wr(SPI_CTRL, 0);                 /* WR_BIT_ORDER = 0 -> MSB first */

    /* 80 MHz from a 240 MHz system clock: 240 / (pre+1) / (cnt_N+1) = 240/1/3.
     * CLKCNT_L must equal CLKCNT_N, and CLKCNT_H = floor((N+1)/2 - 1). */
    wr(SPI_CLOCK, (0u << 31) | (0u << 18) | (2u << 12) | (1u << 6) | 2u);

    /* Full 12-word FIFO per transfer. MS_DLEN is (bits - 1). */
    wr(SPI_MS_DLEN, 32 * 12 - 1);
}

void spi_write(const unsigned char *data, int len)
{
    while (len > 0) {
        int words = len > 48 ? 12 : (len + 3) / 4;

        for (int i = 0; i < 12; i++) {
            uint32_t w = 0;

            if (i < words) {
                /* MSB first within each word, so assemble 4 bytes big-endian. */
                for (int b = 0; b < 4; b++) {
                    int idx = i * 4 + b;
                    w = (w << 8) | (idx < len ? data[idx] : 0);
                }
            }

            wr(SPI_W0 + 4 * (unsigned)i, w);
        }

        wr(SPI_MS_DLEN, 32 * words - 1);
        wr(SPI_CMD, SPI_CMD_UPDATE);
        wr(SPI_CMD, SPI_CMD_UPDATE | SPI_CMD_USR);

        /* USR clears itself when the transfer completes. */
        while (rd(SPI_CMD) & SPI_CMD_USR) { }

        len -= words * 4;
        if (len < 0)
            break;

        data += words * 4;
    }
}
