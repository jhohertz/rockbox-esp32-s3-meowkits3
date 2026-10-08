#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

#define LCD_WIDTH   240
#define LCD_HEIGHT  320

void st7789_init(void);
void st7789_write_command(uint8_t cmd);
void st7789_write_data(const uint8_t *data, int len);

#endif
