/***************************************************************************
 * ESP32-S3 ADC.
 *
 * The SAR ADC peripheral exists on this chip, but nothing on the MeowKit board
 * is measured through it: the buttons are plain GPIO inputs and the battery is
 * not senseable. apps/main.c calls adc_init() unconditionally, so the entry
 * points have to exist.
 *
 * If an ADC ever becomes useful, the peripheral is a command-list machine like
 * the I2C block: a calibration step, then a per-channel configuration, then
 * reading the conversion result from the FIFO.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "adc.h"

void adc_init(void)
{
    /* Nothing to power up: adc_read() has no hardware behind it. */
}

unsigned short adc_read(int channel)
{
    (void)channel;
    return 0;
}
