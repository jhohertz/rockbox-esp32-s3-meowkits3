/*
 * ST7789 display, MeowKit wiring.
 *
 * The panel is on GPSPI2 with DC as a plain GPIO, and its CS and /RES lines are
 * outputs of the PCA9557 I2C expander rather than GPIOs -- which is why the
 * expander has to be up before the panel can be reset or selected.
 *
 * The transfer path is the one the reference firmware uses: IDF brings up the
 * bus and routes the pins, and the actual writes program GPSPI directly. It is
 * not a workaround. At 80 MHz with the output-DMA burst enable set -- which IDF
 * sets and the caller has no lever on -- a transfer whose length is not a
 * multiple of four has its tail corrupted. A FIFO-only driver has no DMA read
 * to corrupt. See the "Why the reference bypasses IDF's transaction layer"
 * section of AGENTS.md.
 *
 * Pixel format, measured on the glass rather than read off the datasheet:
 * COLMOD 0x55, MADCTL 0xA8 (MV|MY|BGR), 16-bit RGB565 high byte first, CASET
 * 0..319 and RASET 0..239, INVON on. Getting COLMOD wrong is not subtle: the
 * panel powers up at 66h (18-bit) and shows three equal thirds with the palette
 * flipped.
 */

#include "config.h"
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "lcd-target.h"
#include "backlight.h"          /* lcd_awake() is prototyped there */
#include "backlight-target.h"
#include "pca9557.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include "st7789.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include <stdio.h>              /* TEMPORARY bring-up logging */

/* lcd_on and lcd_active() belong to drivers/lcd-memframe.c, which owns the
 * active flag. The functions are prototyped in lcd.h; the variable itself has
 * no header, and AGENTS.md is right that a target driver has to declare it. */
extern bool lcd_on;

void lcd_write_command(int byte)
{
    st7789_write_command((unsigned char)byte);
}

/*
 * Pack RGB565 pixels into the GPSPI FIFO's byte order and push them.
 *
 * One FIFO fill is 64 bytes, so 32 pixels; a full 320x240 frame is 2400
 * transfers at this size instead of 3200 at the old 24. The high byte goes
 * first -- measured, not assumed.
 */
void lcd_write_data(const fb_data *p_bytes, int count)
{
    static const int chunk = SPI_BUF_BYTES / 2;
    unsigned char buf[SPI_BUF_BYTES];

    while (count > 0) {
        int n = count > chunk ? chunk : count;
        int j = 0;

        for (int i = 0; i < n; i++) {
            unsigned short pixel = p_bytes[i];
            buf[j++] = (unsigned char)(pixel >> 8);
            buf[j++] = (unsigned char)(pixel & 0xFF);
        }

        st7789_write_data(buf, j);

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

void lcd_update_rect(int x, int y, int width, int height)
{
    int x_end = x + width;
    int y_end = y + height;

    /* TEMPORARY bring-up counters. "Black screen" has at least three distinct
       causes on this path -- the flush never being called, it being called but
       dropped because the panel is flagged inactive, and it running with the
       right pixel count but the wrong bytes -- and only the last one is visible
       on the glass. Count all three so the console says which. */
    static unsigned long upd_n, upd_px, upd_drop;
    char dbg[96];

    /* The panel is powered down; the framebuffer keeps accumulating and is
     * pushed in full when the panel comes back. */
    if (!lcd_active())
    {
        if ((++upd_drop & 63u) == 1)
        {
            snprintf(dbg, sizeof dbg, "LCD drop: inactive, %lu calls so far\n",
                     upd_drop);
            printf("%s", dbg);
        }
        return;
    }

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

    upd_n++;
    upd_px += (unsigned long)width * height;
    if (upd_n == 1 || (upd_n & 255u) == 0)
    {
        snprintf(dbg, sizeof dbg,
                 "LCD update #%lu: %dx%d px_total=%lu\n",
                 upd_n, width, height, upd_px);
        printf("%s", dbg);
    }
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_init_device(void)
{
    /* Forced by the hardware: CS and RST are expander outputs, so the expander
     * has to be reachable before the panel can be reset or selected. */
    pca9557_init();
    pca9557_dump("after pca9557_init");
    st7789_init();

    /* The framebuffer is in PSRAM and is not part of the image, so the first
     * lcd_update() would push whatever PSRAM happened to hold. Clear it before
     * anything draws. */
    memset(FRAME, 0, FRAMEBUFFER_SIZE);

    /* lcd_update_rect drops the draw when the panel is inactive, so the flag
     * has to be set before anything is drawn - not after.
     */
    lcd_on = true;

    /* TEMPORARY: direct proof on the glass, with nothing between it and the
       panel -- no UI, no skin, no font, no backlight policy, no framebuffer
       content from Rockbox's drawing code. The update counter says a full
       320x240 flush happens, but a counter is not the glass, and the panel has
       not been visually confirmed since the thread model changed. Solid red is
       RGB565 0xF800 written high byte first, which is the byte order settled on
       the spike. If this is red, the panel, the expander CS/reset, the SPI path
       and the backlight are all fine and the black screen is a rendering or
       power-state problem above this line. If it is black, it is not. */
    {
        unsigned char *fb = (unsigned char *)FRAME;
        unsigned i;

        for (i = 0; i + 1 < FRAMEBUFFER_SIZE; i += 2)
        {
            fb[i]     = 0xF8;
            fb[i + 1] = 0x00;
        }
        /* The pad picture, for pins 39/40/41 (DC, MOSI, SCLK). All three are
           above 31, so they live in the OUT1/ENABLE1/IN1 group, and all three
           are JTAG pads that default to MCU_SEL = 0 rather than the GPIO
           function. For each: the IO MUX must select GPIO (bits 14:12 == 1),
           func_out_sel_cfg says which signal drives the pad (bits 8:0; 256 =
           the GPIO output register) and whether output enable comes from
           GPIO_ENABLE (bit 10), and ENABLE1/OUT1 say whether the pad is driven
           and to what. A pad can be perfectly programmed in the controller and
           still be tri-stated here, which is the "transfers complete, no edges
           at the pin" signature. */
        {
            static const int dbg_pins[3] = { 39, 40, 41 };
            const char *dbg_names[3] = { "DC ", "MOSI", "SCLK" };
            int k;

            for (k = 0; k < 3; k++)
            {
                int pin = dbg_pins[k];
                unsigned mux = *(volatile unsigned *)IO_MUX_PIN(pin);
                unsigned osel = *(volatile unsigned *)GPIO_FUNC_OUT_SEL(pin);
                unsigned en1 = *(volatile unsigned *)0x6000402C;
                unsigned out1 = *(volatile unsigned *)0x60004010;

                printf("pad %s(%d): mux=0x%04x mcu=%d ie=%d osel=0x%03x "
                       "func=%u oen=%d en1=%d out1=%d\n",
                       dbg_names[k], pin, mux, (int)((mux >> 12) & 7),
                       (int)((mux >> 9) & 1), osel, osel & 0x1FFu,
                       (int)((osel >> 10) & 1), (en1 >> (pin - 32)) & 1,
                       (out1 >> (pin - 32)) & 1);
            }
        }

        unsigned before = xt_spi_stuck, calls_before = xt_spi_calls;
        lcd_update();
        /* xt_spi_stuck counts transfers whose USR bit never cleared. If it moves
           here, the controller started transfers that did not finish -- no clock
           at the pad, which is a wiring or clock-source fault. If it stays at
           zero while the calls count climbs, the transfers completed and the
           panel is being clocked, which moves the suspicion to CS, DC, the
           expander, or the init sequence rather than the SPI path. */
        printf("LCD red: calls+%u stuck+%u on=%d fb=%p\n",
               xt_spi_calls - calls_before, xt_spi_stuck - before,
               (int)lcd_on, (void*)fb);
    }
}

/*
 * Three power states, matching what Rockbox expects from a target that declares
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
