#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

/* Panel dimensions come from config.h (LCD_WIDTH/LCD_HEIGHT); the driver does not
 * need its own copy. */

void st7789_init(void);
void st7789_write_command(uint8_t cmd);
void st7789_write_data(const uint8_t *data, int len);

#endif
