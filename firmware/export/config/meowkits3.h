/*
 * This config file is for the MeowKit ESP32-S3 (Xtensa LX7).
 *
 * Only the parts that are actually implemented are enabled.
 */

/* For Rolo and boot loader */
#define MODEL_NUMBER 133
#define MODEL_NAME   "MeowKit ESP32-S3"

#define CONFIG_CPU ESP32S3

/* HAVE_SYSTEM_LIBC -- ESP-IDF supplies the C library (newlib/picolibc) and links
 * it normally, so Rockbox's own libc must not be built alongside it: it defines
 * the same names (memcpy, strlen, qsort, mktime, errno accessors and the rest),
 * and with both archives on the link line the implementation a call lands on
 * depends on archive order rather than on intent, which is the kind of bug that
 * shows up as behaviour rather than as a link error.
 *
 * It is deliberately not defined here. A config header is pulled in by every
 * domain in the build, and this target has two: the firmware, which links the
 * platform library, and codecs and plugins, which are separate executables with no
 * C library in their link at all, whose C library is the codec_api / plugin_api
 * jump table. Defining it here made the freestanding domain behave as though it
 * linked picolibc too -- firmware/include/file.h reached for <unistd.h>, which
 * resolved to the toolchain's copy inside a -ffreestanding translation unit whose
 * include order shadows the sys/cdefs.h that copy depends on, and one header line
 * turned into thousands of parse errors. tools/root.make supplies it to the
 * firmware domain only.
 */

/* Core runs at 240 MHz; the APB/peripheral bus is 80 MHz.
 *
 * This has to agree with CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ in the IDF project,
 * because IDF is what actually sets the clock. It cannot be derived from
 * sdkconfig.h here: this header is pulled in by every domain and by the
 * SOURCES preprocessor, and IDF's CONFIG_* names must not reach either (see
 * the PPCFLAGS note in tools/root.make). system-xtensa.c asserts the match
 * instead, in a firmware-domain TU where sdkconfig.h is reachable.
 */
#define CPU_FREQ 240000000

/* Bring-up instrumentation for the dircache scan fault and the Ext_RAM MMU
 * faults: the STRACE() calls in firmware/common/dircache.c and firmware/kernel/,
 * the .noinit ring in firmware/target/xtensa/scan_trace.c, and the sampling in
 * vApplicationTickHook.
 *
 * Off now, not deleted -- the questions it answered are settled (the PSRAM MMU
 * indexing, the Ext_RAM read/write asymmetry, the codec I2C addressing), and
 * leaving it on costs real time on a hot path. With it defined, the 1 kHz tick
 * reads intenable plus two EXTMEM status registers every eighth tick, and every
 * sixty-fourth walks 128 volatile MMU entries at 0x600C5000. That is what was
 * left running on the tick after the button-tick probe was removed.
 *
 * Uncomment to re-arm everything at once. The ring is .noinit, so a trace
 * survives a reset and is printed by the boot log on the next start. */
/* #define ROCKBOX_SCAN_TRACE */

/* Not set, on purpose. Rockbox's cpu_boost() exists to call
 * set_cpu_frequency(), and on this SoC the clock tree belongs to ESP-IDF:
 * IDF moves the analog regulator bias (dbias) along with the core clock via
 * REGI2C and the ROM's PVT calibration, which Rockbox cannot reproduce. Hand
 * writing CPUPERIOD_SEL and the LDO slave count is the kind of change that
 * browns out the chip instead of failing visibly. Power management, if wanted,
 * goes through IDF's esp_pm DFS rather than around it. */

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

/*
 * Big statics go to PSRAM.
 *
 * Both of these used to name sections of Rockbox's own linker script
 * (.framebuffer, .psrambss), which this path does not have. Naming a section
 * nothing in the script claims does not fail: the linker keeps it as an orphan
 * and drops it wherever its default rules happen to put it, which on this
 * script was between IDF's flash regions -- and IDF's script then refuses the
 * result with "the gap between .flash.rodata and .flash.tdata must not exist".
 * That is the shape every one of these mistakes takes here, so the rule is:
 * name a section IDF's script knows about, or name none.
 *
 * .ext_ram.bss is that section: IDF's script places it in PSRAM and its startup
 * zeroes it, provided CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is set in the
 * IDF project.
 *
 * The 337 KB internal DRAM cannot hold the framebuffer (150 KB) or the locale
 * and unicode tables, and taking them out is what leaves room for the transfer
 * buffers and DMA rings, which do have to stay internal because GDMA's
 * descriptor window cannot reach PSRAM.
 *
 * The simulator has no PSRAM, so there the attribute stays empty.
 */
#ifndef SIMULATOR
#define PSRAM_BSS_ATTR      __attribute__ ((section(".ext_ram.bss")))
#define IRAM_LCDFRAMEBUFFER __attribute__ ((section(".ext_ram.bss")))
#endif

#define CONFIG_STORAGE STORAGE_SD

/* No SDMMC_HOST_NUM_SD_CONTROLLERS here on purpose. That define is what pulls in
 * Rockbox's generic SDHOST protocol layer (drivers/sdmmc_host.c) and expects the
 * target to supply a set of controller ops. On this target the controller is
 * driven by ESP-IDF's own SD driver, so target/xtensa/sd-idf.c implements the
 * sd_* device entry points directly and the generic layer is not built. */

#define BOOTDIR "/.rockbox"

/* No plugin loader exists for this core, but apps/plugin.c and apps/codecs.c are
 * built unconditionally and every target config defines these. Codecs are real:
 * they are loaded from storage into CODEC_SIZE worth of PSRAM at runtime. */
/* A synchronous dircache build takes audio_buffer_available() outright -- the
 * whole arena -- capped only by DIRCACHE_LIMIT. This target's arena is 4 MB
 * (XT_AUDIOBUF_SIZE, half of PSRAM), so the default 6 MB limit lets the cache
 * claim all of it and everything allocated afterwards from the same arena
 * fails: playlist_init()'s 117 KB fileref table came back as
 * BUFLIB_ERRCODE_MEMBLOCK_ALLOC_FAILED, and because buflib reports failure as a
 * negative code while the caller only tests for zero, that surfaced three
 * frames later as a NULL write. Cap the cache below the arena so the rest of
 * Rockbox still has somewhere to allocate. */
#define DIRCACHE_LIMIT   (1024*1024*2) /* 2 MB of the 4 MB arena */

/* TEMPORARY: Rockbox's own thread invariants, off by default and compiled out
 * as `do {} while (0)`. One of them -- "UPPT->wrong thread" in
 * wakeup_thread_transfer() -- asserts precisely what we are crashing on: that
 * the thread releasing a priority-inheriting object is the thread recorded as
 * its owner. With the check off that violation walks a garbage pointer and dies
 * as a bare LoadProhibited; with it on the kernel names the thread itself.
 * Remove once the thread seam is settled. */
/* Off for now, and not because the checks are wrong. Honoured since thread.c
 * stopped clobbering it, enabling these immediately turned a known port defect
 * fatal: thread_exit_final() ends in THREAD_PANICF("thread_exit->K:*R") because
 * in the gated-task model switch_thread() legitimately returns to a thread that
 * has exited -- its FreeRTOS task is still alive, pending on its gate -- which
 * upstream treats as "must never be reached". Until thread_exit is taught the
 * task model, enabling this panic-loops the device instead of informing us.
 *
 * Worth recording what it did find on the way: with these checks on, the
 * runnable list was observed to contain scroll/tick/power/dircache five or six
 * times each, i.e. threads are added to core_entry.rtr without their previous
 * entry being removed. That duplication is the leading candidate for the play
 * hang, since it lets switch_thread() select one thread twice and hand it a
 * second gate. */
#define THREAD_EXTRA_CHECKS 0

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

/* The peripheral is a command-list machine: a transaction is programmed whole and
 * executed by the CMD_Controller. That is exactly the shape of an i2c_descriptor,
 * so the asynchronous layer is the natural fit and the synchronous API is built on
 * it. One bus is wired to the outside world.
 */
#define HAVE_I2C_ASYNC

/* FT6336 capacitive touch at 0x38. Its INT pin is left unconnected on this board,
 * so the controller is polled from the button tick instead of interrupting.
 */
#define HAVE_TOUCHSCREEN
#define HAVE_FT6x06
#define FT6x06_NUM_POINTS 2
#define HAVE_BUTTON_DATA

/* The controller's own axes are the panel's short side first: registers 0x03/0x04
 * run 0..240 and 0x05/0x06 run 0..320, while the panel is driven as 320x240. The
 * reference firmware uses display x = 320 - raw_y, display y = raw_x.
 */
#define DEFAULT_TOUCHSCREEN_CALIBRATION { .A=0, .B=-1, .C=LCD_WIDTH, .D=1, .E=0, .F=0, .divider=1 }

/*
 * USB: not built.
 *
 * The board's usable USB is the USB-Serial-JTAG block, which is what the host
 * already uses to flash and to read the bring-up trace; it enumerates as CDC
 * with no firmware involvement. The SoC also has a DWC-derived OTG block, but
 * its GUSBCFG is not the classic DWC2 layout: phyif sits at bit 3, ulpiutmisel
 * at 4, fsintf at 5 and physel at 6, and ESP-IDF's own driver has to set
 * phyif = 1 for this core. Rockbox's usb-designware.c writes the whole register
 * with only the PHY field and then compares it back in full, so the bring-up
 * panics with "PHY type not supported" before anything else runs.
 *
 * Nothing on this target needs the OTG core: charging is monitored through the
 * AXP173 (CONFIG_CHARGING + HAVE_AXP173 report VBUS), and storage is the SD card.
 * Revisit only with a driver that matches this core's register layout.
 *
 * USB_NONE is what makes this stick. Without it main() runs the
 * "if USB is plugged in, enter USB mode before mounting" loop, and because the
 * AXP173 reports VBUS present whenever the board is powered from the host, that
 * loop never terminates: boot stops there and nothing else is ever initialised.
 */
#define USB_NONE

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
