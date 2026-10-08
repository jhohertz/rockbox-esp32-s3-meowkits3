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

/* No power button on this board - the PMU owns power. A long press of A is the
 * closest thing to one. */
#define POWEROFF_BUTTON BUTTON_A
#define POWEROFF_COUNT  30

#endif /* _BUTTON_TARGET_H_ */
