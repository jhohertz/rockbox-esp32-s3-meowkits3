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
/* Mic gain in dB, on the scale the Rockbox recording setting uses. Applies the
 * analog PGA code and the fixed digital calibration together; see es7210.c. */
void es7210_set_gain(int db);
void es7210_start(void);
void es7210_stop(void);
/* Read the gain, bias and clock registers back off the chip, for Debug -> View HW
 * info. A write that landed and a write the chip ignored look the same from every
 * consumer of the audio, so this is the only way to tell them apart. */
void es7210_stat(char *buf, int bufsz);

#endif /* ES7210_H */
