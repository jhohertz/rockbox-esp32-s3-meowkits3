#ifndef SPI_XTENSA_H
#define SPI_XTENSA_H

#include <stdint.h>

/* GPSPI2, verified from components/soc/esp32s3/register/soc/reg_base.h */
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
#define SPI_W0         (SPI2_BASE + 0x98)   /* CPU-controlled TX FIFO, 12 words */

#define SPI_USR_MOSI        (1u << 27)
#define SPI_GRL_MODE        (1u << 1)        /* DMA_CONF.grl: CPU-controlled FIFO */
#define SPI_CMD_UPDATE      (1u << 18)
#define SPI_CMD_USR         (1u << 19)

/* MeowKit pins */
#define PIN_LCD_SCLK 41
#define PIN_LCD_MOSI 40
#define PIN_LCD_DC   39

void spi_init(void);
void spi_write(const unsigned char *data, int len);

#endif
