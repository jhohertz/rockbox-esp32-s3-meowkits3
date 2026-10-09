/***************************************************************************
 * ES7210 four-channel ADC on the I2C bus at 0x41.
 *
 * The chip is an I2S slave: it takes MCLK, BCLK and WS from the pins the I2S
 * transmitter already drives, and puts MIC1 and MIC2 on its SDOUT line.
 ****************************************************************************/
#ifndef ES7210_H
#define ES7210_H

#include <stdbool.h>

#define ES7210_I2C_ADDR 0x41

bool es7210_present(void);
bool es7210_init(unsigned long rate);
void es7210_set_frequency(unsigned long rate);
void es7210_set_gain(int code);
void es7210_start(void);
void es7210_stop(void);

#endif /* ES7210_H */
