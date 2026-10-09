#ifndef BACKLIGHT_TARGET_H
#define BACKLIGHT_TARGET_H

bool backlight_hw_init(void);
void backlight_hw_on(void);
void backlight_hw_off(void);

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
void backlight_hw_brightness(int brightness);
#endif

#endif /* BACKLIGHT_TARGET_H */
