#include "config.h"
#include "system.h"
#include "lcd.h"
#include "lcd-target.h"
#include "backlight.h"          /* lcd_awake() is prototyped there */
#include "backlight-target.h"
#include "pca9557.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include "st7789.h"

/* lcd-memframe.c owns the panel-enabled flag; lcd_active() reads it. */
extern bool lcd_on;

/* Backlight is active low on this board. */
#define PIN_LCD_BL 42

static void cs(int select)
{
    pca9557_write(IOEXP_LCD_CS, !select);
}

void lcd_write_command(int byte)
{
    cs(1);
    st7789_write_command((unsigned char)byte);
    cs(0);
}

/* Raw byte write, for command arguments. lcd_write_data takes pixels, which is a
 * different thing, so the two cannot share a helper. */
static void write_bytes(const unsigned char *data, int len)
{
    cs(1);
    st7789_write_data(data, len);
    cs(0);
}

void lcd_write_data(const fb_data *p_bytes, int count)
{
    /* One GPSPI FIFO fill is 64 bytes, so 32 RGB565 pixels. A full 320x240
     * frame is 2400 transfers at this size instead of 3200 at 24. */
    static const int chunk = SPI_BUF_BYTES / 2;
    unsigned char buf[SPI_BUF_BYTES];
    int i;

    cs(1);

    while (count > 0) {
        int n = count > chunk ? chunk : count;
        int j = 0;

        for (i = 0; i < n; i++) {
            unsigned short pixel = p_bytes[i];
            buf[j++] = (unsigned char)(pixel >> 8);
            buf[j++] = (unsigned char)(pixel & 0xFF);
        }

        st7789_write_data(buf, j);
        p_bytes += n;
        count -= n;
    }

    cs(0);
}

static void set_window(int x1, int x2, int y1, int y2)
{
    const unsigned char caset[4] = {
        (unsigned char)(x1 >> 8), (unsigned char)(x1 & 0xFF),
        (unsigned char)(x2 >> 8), (unsigned char)(x2 & 0xFF)
    };
    const unsigned char raset[4] = {
        (unsigned char)(y1 >> 8), (unsigned char)(y1 & 0xFF),
        (unsigned char)(y2 >> 8), (unsigned char)(y2 & 0xFF)
    };

    lcd_write_command(0x2A);   /* CASET */
    write_bytes(caset, 4);
    lcd_write_command(0x2B);   /* RASET */
    write_bytes(raset, 4);
}

void lcd_update_rect(int x, int y, int width, int height)
{
    int x_end = x + width;
    int y_end = y + height;

    /* The panel is powered down; the framebuffer keeps accumulating and is
     * pushed in full when the panel comes back. */
    if (!lcd_active())
        return;

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x_end > LCD_WIDTH) { x_end = LCD_WIDTH; }
    if (y_end > LCD_HEIGHT) { y_end = LCD_HEIGHT; }
    width = x_end - x;
    height = y_end - y;

    if (width <= 0 || height <= 0)
        return;

    set_window(x, x_end - 1, y, y_end - 1);

    for (int row = y; row < y_end; row++)
        lcd_write_data(LCD_FRAMEBUF_ADDR(x, row), width);
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_init_device(void)
{
    /* Forced by the hardware: CS and RST are expander outputs, so the expander has
     * to be reachable before the panel can be reset or selected. */
    pca9557_init();

    st7789_init();

    lcd_on = true;
    lcd_update();
}

/* Three power states, matching what Rockbox expects from a target that declares
 * HAVE_LCD_ENABLE and HAVE_LCD_SLEEP:
 *
 *   active  - the panel is driving the framebuffer
 *   off     - DISPOFF: the driver is still powered, waking costs one command
 *   asleep  - SLPIN: the gate driving stops; waking needs SLPOUT plus the same
 *             recovery delay as after a reset
 *
 * lcd_enable() is called from the backlight driver, and lcd_sleep() is reached
 * only after the backlight has been off for LCD_SLEEP_TIMEOUT - it is the deeper
 * state, which is why Rockbox asks for both.
 */
static bool panel_asleep;

static void panel_wake(void)
{
    if (!panel_asleep)
        return;

    st7789_wake();
    st7789_display_on();
    panel_asleep = false;

    /* Whether GRAM survives SLPIN is a panel question we cannot answer from the
     * datasheet alone, so do not rely on it: push the whole frame. lcd_on has to
     * be set first, because lcd_update() drops the draw when the panel is
     * inactive.
     */
    lcd_on = true;
    lcd_update();
}

void lcd_enable(bool enable)
{
    if (enable) {
        panel_wake();
        if (!lcd_active()) {
            st7789_display_on();
            lcd_on = true;
        }
    } else {
        if (lcd_active()) {
            st7789_display_off();
            lcd_on = false;
        }
    }
}

void lcd_sleep(void)
{
    if (panel_asleep)
        return;

    st7789_display_off();
    st7789_sleep();
    panel_asleep = true;
    lcd_on = false;
}

void lcd_awake(void)
{
    panel_wake();
    if (!lcd_active()) {
        st7789_display_on();
        lcd_on = true;
    }
}

void lcd_shutdown(void)
{
    /* The backlight thread locks itself on SYS_POWEROFF so it cannot go out
     * before the rails drop, so the PWM still has to be killed explicitly here. */
    backlight_hw_off();
    st7789_display_off();
    st7789_sleep();
    panel_asleep = true;
    lcd_on = false;
}

/* The backlight is not written here. GPIO 42 is routed to LEDC channel 7 through
 * the GPIO matrix, so a plain GPIO write has no effect on the pin; backlight-xtensa.c
 * owns the PWM. */

