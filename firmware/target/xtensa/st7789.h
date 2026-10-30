#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

/* Panel dimensions come from config.h (LCD_WIDTH/LCD_HEIGHT); the driver
 * does not need its own copy. */

void st7789_init(void);
void st7789_write_command(uint8_t cmd);
void st7789_write_data(const uint8_t *data, int len);

/* Display on/off keeps the driver powered; sleep is deeper and needs a
 * SLPOUT plus the same recovery delay as after a reset. */
void st7789_display_off(void);
void st7789_display_on(void);
void st7789_sleep(void);
void st7789_wake(void);

#endif
