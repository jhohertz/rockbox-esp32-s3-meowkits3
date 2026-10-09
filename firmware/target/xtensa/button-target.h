/***************************************************************************
 * MeowKit ESP32-S3 buttons: A, B and a 4-direction joystick, all active-low.
 *
 * Pin numbers from the reference firmware's bsp/config.h.
 ****************************************************************************/
#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

#define BUTTON_A     0x00000001
#define BUTTON_B     0x00000002
#define BUTTON_UP    0x00000004
#define BUTTON_DOWN  0x00000008
#define BUTTON_LEFT  0x00000010
#define BUTTON_RIGHT 0x00000020

#define BUTTON_MAIN  (BUTTON_A | BUTTON_B | BUTTON_UP | BUTTON_DOWN | \
                      BUTTON_LEFT | BUTTON_RIGHT)

/* The FT6336 is reported as a 3x3 grid of virtual buttons when the touchscreen is
 * in button mode. These codes are unused by the physical keys.
 */
#define BUTTON_TOPLEFT       0x00000040
#define BUTTON_TOPMIDDLE     0x00000080
#define BUTTON_TOPRIGHT      0x00000100
#define BUTTON_MIDLEFT       0x00000200
#define BUTTON_CENTER        0x00000400
#define BUTTON_MIDRIGHT      0x00000800
#define BUTTON_BOTTOMLEFT    0x00001000
#define BUTTON_BOTTOMMIDDLE  0x00002000
#define BUTTON_BOTTOMRIGHT   0x00004000

/* No power button among the GPIO keys: the PMU owns power, and its PEK key is
 * handled in button-xtensa.c by polling the PMU IRQ line. POWEROFF_BUTTON is
 * only read by firmware/drivers/button.c under HAVE_SW_POWEROFF, which this
 * target does not define. */

#endif /* _BUTTON_TARGET_H_ */
