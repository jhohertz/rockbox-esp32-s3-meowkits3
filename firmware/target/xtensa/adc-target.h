/* Nothing here uses the ADC: the buttons are plain GPIO inputs and the battery
 * is not measurable on this board. adc.h includes adc-target.h unconditionally,
 * so the file has to exist even when it is empty. */
