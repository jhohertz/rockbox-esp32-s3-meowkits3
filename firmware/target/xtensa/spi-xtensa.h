#ifndef SPI_XTENSA_H
#define SPI_XTENSA_H

#include <stdint.h>

/* GPSPI2, verified from components/soc/esp32s3/include/soc/spi_reg.h */
#define SPI2_BASE 0x60024000

#define SPI_CMD        (SPI2_BASE + 0x00)
#define SPI_ADDR       (SPI2_BASE + 0x04)
#define SPI_CTRL       (SPI2_BASE + 0x08)
#define SPI_CLOCK      (SPI2_BASE + 0x0C)
#define SPI_USER       (SPI2_BASE + 0x10)
#define SPI_USER1      (SPI2_BASE + 0x14)
#define SPI_USER2      (SPI2_BASE + 0x18)
#define SPI_MS_DLEN    (SPI2_BASE + 0x1C)
#define SPI_MISC       (SPI2_BASE + 0x20)
#define SPI_DMA_CONF   (SPI2_BASE + 0x30)
#define SPI_CLK_GATE   (SPI2_BASE + 0xE8)
#define SPI_W0         (SPI2_BASE + 0x98)   /* CPU-controlled buffer, 16 words */

#define SPI_CMD_CONF_BITLEN (0x3FFFFu)
#define SPI_CMD_UPDATE      (1u << 23)
#define SPI_CMD_USR         (1u << 24)

#define SPI_CLK_EQU_SYSCLK (1u << 31)

#define SPI_USR_MOSI        (1u << 27)

#define SPI_CLK_EN          (1u << 0)
#define SPI_MST_CLK_ACTIVE  (1u << 1)
#define SPI_MST_CLK_SEL     (1u << 2)   /* 1 = PLL 80 MHz, 0 = XTAL 40 MHz */

#define SYSTEM_SPI2_CLK_EN  (1u << 6)

/* The CPU-controlled buffer holds 64 bytes; MS_DLEN is (bits - 1). */
#define SPI_BUF_WORDS 16
#define SPI_BUF_BYTES (SPI_BUF_WORDS * 4)

/* MeowKit pins */
#define PIN_LCD_SCLK 41
#define PIN_LCD_MOSI 40
#define PIN_LCD_DC   39

void spi_init(void);
void spi_write(const unsigned char *data, int len);

/* TEMPORARY bring-up: count SCLK/MOSI edges at the pad during a slow transfer,
   then validate the counter by hand-toggling. */
void spi_edge_probe_run(void);
void spi_probe_edges(void);

/* Set to 1 to have the next full 64-byte transfer sampled at the pin. */

/* Transfer counters, for bring-up diagnostics: xt_spi_calls counts spi_write()
 * entries and xt_spi_stuck counts transfers whose USR bit never cleared, i.e.
 * the controller started a transfer that did not finish. */
extern volatile unsigned int xt_spi_calls;
extern volatile unsigned int xt_spi_stuck;

#endif
