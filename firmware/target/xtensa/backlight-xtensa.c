/***************************************************************************
 * MeowKit ESP32-S3 backlight.
 *
 * The panel's backlight is an active-low GPIO (42). The pin itself is handled by
 * the LCD driver, so this is just the Rockbox-side entry point.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "lcd.h"

void backlight_hw_on(void)
{
    lcd_backlight(true);
}

void backlight_hw_off(void)
{
    lcd_backlight(false);
}
