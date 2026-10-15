#ifndef PCA9557_H
#define PCA9557_H

#include <stdint.h>

/* PCA9557 on the MeowKit board, address 0x19. Register map from the reference
 * firmware (datasheet page 17). */
#define PCA9557_ADDR            0x19

#define PCA9557_REG_INPUT       0x00
#define PCA9557_REG_OUTPUT      0x01
#define PCA9557_REG_INVERSION   0x02
#define PCA9557_REG_CONFIG      0x03   /* 1 = input, 0 = output */

/* Which expander pin drives what, from the reference board config. */
#define IOEXP_LCD_RST           0
#define IOEXP_LCD_CS            1
#define IOEXP_FLASH_CS          2
#define IOEXP_PA_EN             3
#define IOEXP_TOUCH_RST         6

void pca9557_init(void);
int pca9557_write(int pin, int value);
int pca9557_read(int pin);
int pca9557_status(void);

#endif
