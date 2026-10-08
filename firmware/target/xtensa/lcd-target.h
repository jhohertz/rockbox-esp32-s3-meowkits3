#ifndef LCD_TARGET_H
#define LCD_TARGET_H

/* The framebuffer lives in PSRAM, placed by the linker (see app.lds). It cannot be
 * a static array: .bss is in the 337 KB static DRAM region, which cannot hold
 * 300 KB of framebuffer plus everything else. */
extern char lcd_framebuffer[];

#define FRAME ((fb_data *)lcd_framebuffer)

#define LCD_FRAMEBUF_ADDR(col, row) (FRAME + (row) * LCD_WIDTH + (col))

/* We transmit on update rather than having memory-mapped display memory, so the
 * generic lcd-memframe.c copy path is not used. */
#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT

#endif /* LCD_TARGET_H */
