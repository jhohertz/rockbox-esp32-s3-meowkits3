/*
 * This config file is for the MeowKit ESP32-S3 (Xtensa LX7).
 *
 * Only the parts that are actually implemented are enabled. Drivers still missing:
 * SDMMC, touchscreen, buttons, PMU, RTC.
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 133
#define MODEL_NAME   "MeowKit ESP32-S3"

#define CONFIG_CPU ESP32S3

/* Core runs at 240 MHz; the APB/peripheral bus is 80 MHz. */
#define CPU_FREQ 240000000

/* LCD: ST7789, 240x320 portrait, RGB565 over SPI. */
#define HAVE_LCD_COLOR
#define HAVE_LCD_BITMAP
#define LCD_WIDTH 240
#define LCD_HEIGHT 320
#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565

/* The framebuffer cannot live in .bss: the static data region is capped at 337 KB
 * and the framebuffer alone is 150 KB. Put it in PSRAM via the linker. */
#define IRAM_LCDFRAMEBUFFER __attribute__ ((section(".framebuffer")))

#define CONFIG_STORAGE STORAGE_SD

#define CONFIG_KEYPAD ESP32S3_PAD

/* I2C is bit-banged on GPIO 1/2, not a peripheral. */
#define CONFIG_I2C I2C_XTENSA

/* Audio: ES8311 codec on I2S1. Every rate Rockbox supports is reachable from the
 * 40 MHz XTAL; only 44100 and 88200 are off by ~7 Hz (0.000064 %). */
#define HW_SAMPR_CAPS SAMPR_CAP_ALL_96

#define HAVE_ES8311

/* One volume register for both channels, so the codec has mono volume. The
 * AUDIOHW_SETTING itself lives in es8311.h, which audiohw.h includes after
 * defining the macro. */
#define AUDIOHW_CAPS MONO_VOL_CAP
