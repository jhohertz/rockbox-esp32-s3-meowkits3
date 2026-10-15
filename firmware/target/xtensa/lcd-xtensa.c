#include "config.h"
#include <string.h>
#include "bootlog.h"
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

/* CS is asserted once in st7789_init() and left asserted; see the comment there.
 * Nothing in the draw path touches the expander.
 */

void lcd_write_command(int byte)
{
    st7789_write_command((unsigned char)byte);
}

/* Raw byte write is not usable on its own: see write_cmd_args below. */

void lcd_write_data(const fb_data *p_bytes, int count)
{
    /* One GPSPI FIFO fill is 64 bytes, so 32 RGB565 pixels. A full 320x240
     * frame is 2400 transfers at this size instead of 3200 at 24. */
    static const int chunk = SPI_BUF_BYTES / 2;
    unsigned char buf[SPI_BUF_BYTES];
    int i;

    static unsigned long long copy_cyc, spi_cyc, chunks;

    rtc_bump(24);                      /* lcd_write_data entries */
    rtc_set(31, (unsigned)count);      /* the count we were handed */
    while (count > 0) {
        int n = count > chunk ? chunk : count;
        rtc_bump(23);                  /* pixel-chunk iterations */
        int j = 0;
        unsigned int c0, c1, c2;

        asm volatile ("rsr %0, ccount" : "=r"(c0));
        for (i = 0; i < n; i++) {
            unsigned short pixel = p_bytes[i];
            buf[j++] = (unsigned char)(pixel >> 8);
            buf[j++] = (unsigned char)(pixel & 0xFF);
            *(volatile unsigned int *)0x50001b48 = (unsigned int)i;
        }
        asm volatile ("rsr %0, ccount" : "=r"(c1));
        *(volatile unsigned int *)0x50001b4c = 0xBEEF0000u + (unsigned)j;

        st7789_write_data(buf, j);
        *(volatile unsigned int *)0x50001b4c = 0xCAFE0000u + (unsigned)chunks;

        asm volatile ("rsr %0, ccount" : "=r"(c2));
        copy_cyc += c1 - c0;
        spi_cyc += c2 - c1;
        chunks++;
        *(volatile unsigned int *)0x50001b40 = (unsigned int)chunks;
        *(volatile unsigned int *)0x50001b44 = (unsigned int)(p_bytes - (const fb_data *)lcd_framebuffer);

        p_bytes += n;
        count -= n;
    }
}

/* The ST7789 takes a command and its parameters in one CS-low phase. CS is held
 * asserted for the whole session, so command and arguments naturally share that
 * phase; the helper keeps the pairing explicit.
 */
static void write_cmd_args(unsigned char cmd, const unsigned char *args, int n)
{
    st7789_write_command(cmd);
    if (n > 0)
        st7789_write_data(args, n);
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

    write_cmd_args(0x2A, caset, 4);   /* CASET */
    write_cmd_args(0x2B, raset, 4);   /* RASET */

    /* RAMWR starts the write. CASET/RASET only set the address window; the panel
     * does not accept pixel data until 0x2C has been issued. LovyanGFX's
     * set_window_8() ends with writeCommand(cmd, 8) and every caller passes
     * CMD_RAMWR. Omitting it leaves GRAM untouched, which looks like noise that
     * never changes rather than a wrong image.
     */
    st7789_write_command(0x2C);
}

/* Bring-up pattern, written through the framebuffer rather than straight to the
 * panel. That is the path Rockbox actually uses, so it tests the whole chain:
 * framebuffer in the PSRAM window -> lcd_update_rect -> set_window -> SPI.
 * Each fill is a different window, so the geometry is visible if it is right.
 */
static void fb_fill(int x, int y, int w, int h, unsigned short rgb)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            *LCD_FRAMEBUF_ADDR(col, row) = rgb;
}

/* Geometry probe: alternating single rows, then single columns. If the panel's
 * window is the 320x240 we asked for, this reads as a fine comb. If the window is
 * actually 240 wide, each 320-pixel row spills 80 pixels into the next one and the
 * comb turns into a diagonal smear.
 *
 * The panel has been confirmed to display, so the multi-frame pattern and its
 * one-second pauses are no longer needed; they only delay boot by several seconds
 * and fill the trace ring before the interesting part of boot runs.
 */
static void test_pattern(void)
{
    fb_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0xF800);
    lcd_update();
}

/* .framebuffer is NOLOAD and sits in the PSRAM window; crt0 zeroes .bss and
 * .srambss but not it, so at first use it holds whatever PSRAM happened to
 * contain. That is the "noise that looks like a data area".
 */
static void dump_framebuffer(void)
{
}

/* Two things can make the panel show GRAM garbage while the SPI itself is fine:
 * CS never actually asserting (it lives on the expander, not a GPIO), or DC stuck
 * low so every byte is read as a command. Both are checkable from the outside:
 * the PCA9557 input register mirrors the state of its own output pins, and a pad
 * driven through the GPIO matrix reads back in GPIO_IN.
 */
static void probe_control_lines(void)
{
    unsigned dcbit = 1u << (PIN_LCD_DC - 32);

    (void)dcbit;
}

__attribute__((section(".srambss"))) volatile unsigned int xt_lcd_rect_n;
__attribute__((section(".srambss"))) volatile unsigned int xt_lcd_data_n;

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

    rtc_bump(25);                      /* lcd_update_rect entries */
    set_window(x, x_end - 1, y, y_end - 1);

    for (int row = y; row < y_end; row++) {
        lcd_write_data(LCD_FRAMEBUF_ADDR(x, row), width);
        rtc_bump(26);                  /* rows pushed */
    }
    boot_trace('g');
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_init_device(void)
{
    /* Forced by the hardware: CS and RST are expander outputs, so the expander has
     * to be reachable before the panel can be reset or selected. */
    boot_trace('c');
    pca9557_init();
    boot_trace('e');
    boot_trace('!');
    st7789_init();
    boot_trace('D');

    /* .framebuffer is NOLOAD in the PSRAM window and crt0 only zeroes .bss,
     * .psrambss and .srambss, so the first lcd_update() would push whatever
     * PSRAM happened to hold. Clear it before anything draws.
     */
    boot_trace('U');
    dump_framebuffer();
    boot_trace('V');
    memset(lcd_framebuffer, 0, FRAMEBUFFER_SIZE);
    boot_trace('Y');
    probe_control_lines();
    boot_trace('Z');

    /* lcd_update_rect drops the draw when the panel is inactive, so the flag has
     * to be set before anything is drawn - not after.
     */
    lcd_on = true;
    test_pattern();

    boot_trace('F');
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

