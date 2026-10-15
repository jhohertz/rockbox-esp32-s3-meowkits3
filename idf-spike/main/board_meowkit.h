/*
 * MeowKit ESP32-S3-WROOM-1-N16R8 pin map.
 *
 * Taken from the stock firmware (src/bsp/config.h) and verified against the
 * schematic during the bare-metal bring-up. This is board truth: it does not
 * change whether Rockbox's own drivers or IDF's drive the silicon.
 *
 * The two things a pin list does not tell you:
 *   - half the control signals live on an I2C IO expander, so the display is
 *     not a plain SPI device;
 *   - the audio path is an I2S DAC with no controller of its own, and the
 *     speaker amp is enabled by an expander pin.
 */
#pragma once

/* ---- I2C bus: everything except the display and SD hangs off this ---- */
#define PIN_I2C_SDA         1
#define PIN_I2C_SCL         2
#define I2C_PORT            I2C_NUM_0

#define ADDR_PCA9557        0x19    /* IO expander  */
#define ADDR_AXP173         0x34    /* PMU          */
#define ADDR_FT6336         0x38    /* capacitive touch */
#define ADDR_ES8311         0x18    /* audio DAC    */
#define ADDR_ES7210         0x41    /* audio ADC    */
#define ADDR_PCF8563        0x51    /* RTC          */

/* ---- PCA9557 registers: 0x00 input, 0x01 output, 0x02 inversion,
 *      0x03 configuration where 1 = input ---- */
#define PCA9557_REG_INPUT   0x00
#define PCA9557_REG_OUTPUT  0x01
#define PCA9557_REG_INV     0x02
#define PCA9557_REG_CONFIG  0x03

/* Expander bit assignments. */
#define EXP_LCD_RST         0
#define EXP_LCD_CS          1
#define EXP_FLASH_CS        2
#define EXP_AMP_EN          3       /* NS4150B enable, active high, 10K pull-down */
#define EXP_TOUCH_RST       6

/* ---- Display: ST7789 IPS, physically 240x320 GRAM, driven 320x240 ---- */
#define PIN_LCD_MOSI        40
#define PIN_LCD_SCLK        41
#define PIN_LCD_DC          39
/* CS and RST are NOT GPIOs -- they are expander bits above. */
#define SPI_LCD_HOST        SPI2_HOST
/* Full speed, matching the stock firmware: display.hpp cfg.freq_write =
 * 80000000. The reference proves the panel and the wiring accept 80 MHz on
 * this board, so anything slower is a handicap we impose on ourselves. */
#define LCD_PCLK_HZ         (80 * 1000 * 1000)
#define LCD_H               320
#define LCD_V               240

/* ---- Backlight: LEDC on GPIO 42, ACTIVE LOW.
 *      duty 512 (all high) = OFF; duty 0 = full brightness. ---- */
#define PIN_LCD_BL          42

/* ---- SD card: SDMMC 1-bit. Only D0 is wired, so 1-bit is the ceiling. ---- */
#define PIN_SD_CLK          47
#define PIN_SD_CMD          48
#define PIN_SD_D0           21

/* ---- Audio out: ES8311 over I2S -> NS4150B amp -> 1 W speaker ---- */
#define PIN_I2S_MCLK        15
#define PIN_I2S_BCLK        14
#define PIN_I2S_WS          13
#define PIN_I2S_DOUT        16

/* ---- Audio in: ES7210, MEMS mic ---- */
#define PIN_I2S_DIN         3

/* ---- AXP173 power key IRQ, active low. Pad 43 defaults to UART0 TX in the
 *      IO MUX, so it has to be switched to the GPIO function before use. ---- */
#define PIN_AXP_IRQ         43

/* ---- Buttons: all active-low with internal pull-ups, 80-100 ms debounce ---- */
#define PIN_BTN_A           6
#define PIN_BTN_B           4
#define PIN_JOY_UP          12
#define PIN_JOY_DOWN        18
#define PIN_JOY_LEFT        17
#define PIN_JOY_RIGHT       8

/* ---- PCA9557 presence check. REG_CONFIG powers up as 0xFF (all inputs) and
 *      is never 0x00 or 0xFF-on-a-bad-bus ambiguity the way REG 0x00 is. ---- */
#define PCA9557_CONFIG_POR  0xFF
