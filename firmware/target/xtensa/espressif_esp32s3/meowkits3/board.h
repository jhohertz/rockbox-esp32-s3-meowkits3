#ifndef MEOWKIT3_BOARD_H
#define MEOWKIT3_BOARD_H

/* Shared bring-up for the board's I2C master and PMU rails.

   The AXP173's LDO4 feeds the PCA9557 -- which holds the LCD reset and chip
   select -- and below about 2 V the expander does not answer on the bus at
   all. That makes this a prerequisite of the display and the touchscreen, not
   a power-management step, so it has to run before pca9557_init() rather than
   in power_init(), which apps/main.c reaches only after lcd_init().

   Idempotent: whichever init path runs first does the work, and the other
   becomes a no-op. */
void meowkit3_board_init(void);

#endif
