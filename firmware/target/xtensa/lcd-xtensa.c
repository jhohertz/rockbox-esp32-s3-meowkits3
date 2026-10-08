#include "config.h"
#include "system.h"
#include "lcd.h"
#include "lcd-target.h"
#include "pca9557.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include "st7789.h"

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
    unsigned char buf[48];   /* one GPSPI FIFO fill */
    int i;

    cs(1);

    while (count > 0) {
        int n = count > 24 ? 24 : count;
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
    gpio_set_direction(PIN_LCD_BL, 1);

    st7789_init();

    lcd_update();
}

void lcd_backlight(bool on)
{
    /* Active low. */
    gpio_write_pin(PIN_LCD_BL, on ? 0 : 1);
}
