#ifndef LCD_TARGET_H
#define LCD_TARGET_H

/* The framebuffer the drawing code owns.
 *
 * lcd-color-common.c defines a static array and wraps it in
 * lcd_framebuffer_default. On the bare-metal branch the target had its own
 * symbol, lcd_framebuffer, and app.lds pointed it at the start of the section
 * holding that static array -- which is the only reason the two were ever the
 * same memory. There is no Rockbox linker script on this path to perform that
 * aliasing, and defining a second buffer here would leave the UI drawing into
 * one array while the panel was fed from the other: a black screen with no
 * error anywhere.
 *
 * Going through the struct is also correct in the case the alias was not: a
 * viewport change (backdrop, plugin, lcd-memframe double buffering) moves
 * fb_ptr, and a fixed address would keep pushing the old buffer.
 *
 * Requires lcd.h to have been included first, which lcd-meowkits3.c does.
 */
extern struct frame_buffer_t lcd_framebuffer_default;

#define FRAME (lcd_framebuffer_default.fb_ptr)

#define LCD_FRAMEBUF_ADDR(col, row) (FRAME + (row) * LCD_WIDTH + (col))

/* We transmit on update rather than having memory-mapped display memory, so the
 * generic lcd-memframe.c copy path is not used. */
#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT

/* Same reasoning for the YUV path: the generic lcd_blit_yuv in
 * drivers/lcd-memframe.c only fills the shadow framebuffer, and mpegplayer
 * never issues an update for the video band itself -- it blits through this
 * entry and then pushes only its OSD strip (mpegplayer.c:1665) and, when the
 * FPS overlay is on, the FPS rect. So on a panel with no CPU-writable
 * display memory the video area stays as whatever the last full update left.
 * Defining this makes the target own the function, which is the contract
 * targets with a real YUV path use (firmware/export/config/3ds.h, the as3525
 * and ipod LCD drivers). */
#define LCD_OPTIMIZED_BLIT_YUV

#endif /* LCD_TARGET_H */
