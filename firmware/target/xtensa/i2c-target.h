/***************************************************************************
 * I2C bus layout for the ESP32-S3 targets.
 *
 * The ESP32-S3 has two I2C master controllers, but only I2C0 (0x60013000) is
 * wired to the external bus on the MeowKit board: SDA on GPIO1, SCL on GPIO2.
 * Everything on that bus - the PCA9557 expander, the ES8311 codec, the ES7210
 * ADC, the AXP173 PMU, the PCF8563 RTC and the FT6336 touch controller - is
 * reached through one bus number, so the async layer needs only one bus.
 ****************************************************************************/
#ifndef __I2C_TARGET_H__
#define __I2C_TARGET_H__

#define I2C_ASYNC_BUS_COUNT  1
#define I2C_ASYNC_QUEUE_SIZE 4

#define FT6x06_BUS  0
#define FT6x06_ADDR 0x38

#endif /* __I2C_TARGET_H__ */
