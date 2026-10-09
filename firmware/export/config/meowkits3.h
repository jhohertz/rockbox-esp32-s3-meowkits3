/*
 * This config file is for the MeowKit ESP32-S3 (Xtensa LX7).
 *
 * Only the parts that are actually implemented are enabled. Drivers still missing:
 * touchscreen (FT6336), USB.
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 133
#define MODEL_NAME   "MeowKit ESP32-S3"

#define CONFIG_CPU ESP32S3

/* Core runs at 240 MHz; the APB/peripheral bus is 80 MHz. */
#define CPU_FREQ 240000000

/* LCD: ST7789. The panel's GRAM is 240x320, but the reference firmware drives it
 * rotated 90 degrees (LovyanGFX offset_rotation = 3), so it is used as 320x240. */
#define HAVE_LCD_COLOR
#define HAVE_LCD_BITMAP
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565
/* 320x240 across a 2.0" diagonal is 400 pixels diagonal, i.e. 200 dpi. */
#define LCD_DPI    200

/* Three display power states. The backlight dominates the panel, but the panel
 * keeps driving its gates as long as it is enabled, so DISPOFF follows the
 * backlight and SLPIN is reached only after the backlight has been off for a
 * while. Rockbox asks for both: lcd_enable() is the shallow state the backlight
 * driver uses, lcd_sleep() the deeper one the timeout reaches.
 */
#define HAVE_LCD_ENABLE
#define HAVE_LCD_SLEEP
#define LCD_SLEEP_TIMEOUT (2*HZ)
#define HAVE_LCD_SHUTDOWN

#define HAVE_BACKLIGHT
/* GPIO 42 is routed through the GPIO matrix to LEDC channel 7, so the backlight is
 * a PWM duty rather than a level. The LED is active low. */
#define HAVE_BACKLIGHT_BRIGHTNESS
#define MIN_BRIGHTNESS_SETTING     0
#define MAX_BRIGHTNESS_SETTING     255
#define DEFAULT_BRIGHTNESS_SETTING 200

/* The framebuffer cannot live in .bss: the static data region is capped at 337 KB
 * and the framebuffer alone is 150 KB. Put it in PSRAM via the linker. */
#define IRAM_LCDFRAMEBUFFER __attribute__ ((section(".framebuffer")))

#define CONFIG_STORAGE STORAGE_SD

/* One SDHOST slot, wired for 1-bit. There is no card-detect line, so the drive is
 * not removable: a card has to be present at boot. */
#define SDMMC_HOST_NUM_SD_CONTROLLERS 1

#define BOOTDIR "/.rockbox"

/* No plugin loader exists for this core, but apps/plugin.c and apps/codecs.c are
 * built unconditionally and every target config defines these. Codecs are real:
 * they are loaded from storage into CODEC_SIZE worth of PSRAM at runtime. */
#define CODEC_SIZE 0x100000 /* 1 MiB */
#define PLUGIN_BUFFER_SIZE 0x80000

#define CONFIG_KEYPAD ESP32S3_PAD

/* PCF8563 on the I2C bus at 0x51. It is powered from the PMU's LDO1, so it keeps
 * running while the SoC is off. */
#define CONFIG_RTC RTC_PCF8563

/* AXP173 at 0x34: supplies the board, measures the battery, and owns the power
 * button. Charging is controlled by the chip; Rockbox only monitors it. */
#define CONFIG_CHARGING CHARGING_MONITOR
#define CONFIG_BATTERY_MEASURE (VOLTAGE_MEASURE | CURRENT_MEASURE)
#define HAVE_AXP173

#define BATTERY_CAPACITY_DEFAULT 1400 /* mAh, per the board's cell */
#define BATTERY_CAPACITY_MIN     1400
#define BATTERY_CAPACITY_MAX     1400
#define BATTERY_CAPACITY_INC     0


/* I2C uses the hardware master peripheral at 0x60013000, on GPIO 1/2. */
#define CONFIG_I2C I2C_XTENSA

/* Audio: ES8311 codec on I2S1. Every rate Rockbox supports is reachable from the
 * 40 MHz XTAL; only 44100 and 88200 are off by ~7 Hz (0.000064 %). */
#define HW_SAMPR_CAPS SAMPR_CAP_ALL_96

#define HAVE_ES8311

/* The ES7210 ADC sits on the same I2S bus as the codec, on the receiver half of a
 * second I2S port. See firmware/target/xtensa/pcm-xtensa.c. */
#define HAVE_RECORDING

/* The ES7210 is the only input the board has. */
#define INPUT_SRC_CAPS SRC_CAP_MIC

/* Recording shares the I2S clock with playback, so the reachable rates are the
 * same set. */
#define REC_SAMPR_CAPS HW_SAMPR_CAPS

/* The NS4150B amplifier sits between the codec and the speaker and is enabled from
 * the PCA9557 expander. Without it there is no sound at all. */
#define HAVE_SPEAKER

/* One volume register for both channels, so the codec has mono volume. The
 * AUDIOHW_SETTING itself lives in es8311.h, which audiohw.h includes after
 * defining the macro. */
#define AUDIOHW_CAPS MONO_VOL_CAP
