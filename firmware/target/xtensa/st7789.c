#include "config.h"
#include "system.h"
#include "pca9557.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include "st7789.h"

/* Command values and the init list are taken from the reference firmware's
 * LovyanGFX panel driver, not from memory. */
#define CMD_NOP     0x00
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_SLPOUT  0x11
#define CMD_IDMOFF  0x38
#define CMD_DISPON  0x29
#define CMD_COLMOD  0x3A
#define CMD_MADCTL  0x36

#define COLMOD_RGB565 0x55
/* rotation 3 -> MAD_MY|MAD_ML, plus MAD_BGR since rgb_order is false */
#define MADCTL_VALUE  0x98

/* The panel only needs a lower bound on the reset and sleep-out timings, so a
 * generous delay is fine; udelay() is bounded by the core clock rather than by
 * how many cycles the compiler happens to put in the loop body. */
static void delay_ms(int ms)
{
    udelay(ms * 1000);
}

static void cs(int select)
{
    /* Active low. */
    pca9557_write(IOEXP_LCD_CS, !select);
}

static void dc(int data)
{
    gpio_write_pin(PIN_LCD_DC, data);
}

void st7789_write_command(uint8_t cmd)
{
    dc(0);
    spi_write(&cmd, 1);
}

void st7789_write_data(const uint8_t *data, int len)
{
    dc(1);
    spi_write(data, len);
}

static void cmd(uint8_t c, const uint8_t *args, int n)
{
    cs(1);
    st7789_write_command(c);

    if (n > 0)
        st7789_write_data(args, n);

    cs(0);
}

void st7789_init(void)
{
    static const uint8_t init_list[] = {
        0xB2, 5, 0x0c, 0x0c, 0x00, 0x33, 0x33,   /* PORCTRL */
        0xB7, 1, 0x35,                            /* GCTRL   */
        0xBB, 1, 0x28,                            /* VCOMS   */
        0xC0, 1, 0x0C,                            /* LCMCTRL */
        0xC2, 2, 0x01, 0xFF,                      /* VDVVRHEN*/
        0xC3, 1, 0x10,                            /* VRHS    */
        0xC4, 1, 0x20,                            /* VDVSET  */
        0xC6, 1, 0x0f,                            /* FRCTR2  */
        0xD0, 2, 0xa4, 0xa1,                      /* PWCTRL1 */
        0xB0, 2, 0x00, 0xC0,                      /* RAMCTRL */
        0xE0, 14, 0xd0,0x00,0x02,0x07,0x0a,0x28,0x32,0x44,0x42,0x06,0x0e,0x12,0x14,0x17,
        0xE1, 14, 0xd0,0x00,0x02,0x07,0x0a,0x28,0x31,0x54,0x47,0x0e,0x1c,0x17,0x1b,0x1e,
    };

    /* Reset sequence, in the order the reference firmware performs it. */
    pca9557_write(IOEXP_LCD_CS, 1);        /* deselect */
    pca9557_write(IOEXP_TOUCH_RST, 0);
    pca9557_write(IOEXP_LCD_RST, 0);       /* assert reset */
    delay_ms(20);
    pca9557_write(IOEXP_LCD_RST, 1);       /* release */
    pca9557_write(IOEXP_TOUCH_RST, 1);
    delay_ms(120);                          /* ST7789 needs ~120 ms after release */

    spi_init();

    for (int i = 0; i < (int)sizeof(init_list); ) {
        uint8_t c = init_list[i];
        int n = init_list[i + 1];
        cmd(c, &init_list[i + 2], n);
        i += 2 + n;
    }

    cmd(CMD_SLPOUT, 0, 0);
    delay_ms(130);

    cmd(CMD_IDMOFF, 0, 0);

    cmd(CMD_COLMOD, (const uint8_t[]){COLMOD_RGB565}, 1);
    cmd(CMD_MADCTL, (const uint8_t[]){MADCTL_VALUE}, 1);
    cmd(CMD_INVON, 0, 0);
    cmd(CMD_DISPON, 0, 0);
}
