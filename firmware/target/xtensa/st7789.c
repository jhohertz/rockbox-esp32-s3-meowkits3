#include "config.h"
#include "system.h"
#include "pca9557.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include "st7789.h"

/* Command values and the init list are taken from the reference firmware's
 * LovyanGFX panel driver, not from memory. */
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_SLPIN   0x10
#define CMD_SLPOUT  0x11
#define CMD_DISPOFF 0x28
#define CMD_DISPON  0x29
#define CMD_IDMOFF  0x38
#define CMD_COLMOD  0x3A
#define CMD_MADCTL  0x36

#define COLMOD_RGB565 0x55
/* LovyanGFX's rotation table, entry 3 (the panel's offset_rotation), with
 * rgb_order = false contributing MAD_BGR = 0x00:
 *   MAD_MV(0x20) | MAD_MY(0x80) | MAD_ML(0x10) = 0xB0
 * The GRAM is 240x320; this is what makes it behave as a 320x240 panel. */
#define MADCTL_VALUE  0xB0

/* The panel only needs a lower bound on the reset and sleep-out timings, so a
 * generous delay is fine; udelay() is bounded by the core clock rather than by
 * how many cycles the compiler happens to put in the loop body. */
static void delay_ms(int ms)
{
    udelay(ms * 1000);
}

/* CS lives on the PCA9557, not on a GPIO. Toggling it per transfer costs a full
 * I2C transaction (~1.6 ms at 100 kHz) and makes every pixel write depend on the
 * bus having finished. The reference firmware never toggles CS either: LovyanGFX is
 * configured with pin_cs = -1 and the expander just holds CS asserted for the whole
 * session. Do the same - assert once, after the reset, and never touch it again.
 */
static void cs_assert(void)
{
    pca9557_write(IOEXP_LCD_CS, 0);   /* expander bit low = CS active */
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
    st7789_write_command(c);

    if (n > 0)
        st7789_write_data(args, n);
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

    cs_assert();
    /* Expected: bit0 RST=1 (released), bit1 CS=0 (asserted), bit6 TOUCH_RST=1,
       and CONFIG bits 0,1,2,3,6 clear -- set means input, and an input cannot
       drive CS or RST however OUTPUT was written. */
    pca9557_dump("after cs_assert");

    /* The expander's input register mirrors the state of every pin, outputs
     * included, so this is the only external proof that the CS/RST writes landed.
     * Expected: bit0 RST=1, bit1 CS=0, bit3 PA=0, bit6 TOUCH_RST=1.
     */

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

void st7789_display_off(void)
{
    cmd(CMD_DISPOFF, 0, 0);
}

void st7789_display_on(void)
{
    cmd(CMD_DISPON, 0, 0);
}

void st7789_sleep(void)
{
    cmd(CMD_SLPIN, 0, 0);
}

void st7789_wake(void)
{
    cmd(CMD_SLPOUT, 0, 0);
    delay_ms(130);
}
