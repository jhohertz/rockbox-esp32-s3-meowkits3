/*
 * MeowKit ESP32-S3 spike: does Espressif's own stack bring this board up
 * further and faster than the hand-rolled bare-metal path?
 *
 * Stages run in increasing order of risk. A failure does not abort the run --
 * the point is the whole table at the end, not the first failure.
 *
 * Everything here is deliberately throwaway. See ../README.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_private/periph_ctrl.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/reg_base.h"
#include "soc/soc.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "board_meowkit.h"

static const char *TAG = "spike";

/* ------------------------------------------------------------------ */
/* results table                                                       */
/* ------------------------------------------------------------------ */

typedef enum { ST_SKIPPED, ST_PASS, ST_FAIL } stage_t;

static const char *stage_name[] = {
    "0 heap/psram",
    "1 i2c + pca9557",
    "2 backlight (ledc, active low)",
    "3 st7789 via esp_lcd",
    "4 sd card (1-bit, gpio matrix)",
    "5 axp173 pmu",
    "6 nimble scan",
    "7 spi clock at the pad",
};
#define N_STAGES ((int)(sizeof(stage_name) / sizeof(stage_name[0])))
static stage_t results[N_STAGES];
static char notes[N_STAGES][96];

static void record(int s, stage_t st, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void record(int s, stage_t st, const char *fmt, ...)
{
    va_list ap;
    results[s] = st;
    va_start(ap, fmt);
    vsnprintf(notes[s], sizeof(notes[s]), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "[%s] %s: %s", st == ST_PASS ? "PASS" : "FAIL",
             stage_name[s], notes[s]);
}

/* ------------------------------------------------------------------ */
/* stage 0: memory                                                     */
/* ------------------------------------------------------------------ */

/* Rockbox needs four big regions to exist at all: the audiobuffer, the
 * codec buffer, the plugin buffer and the dircache. On an 8 MB PSRAM part
 * those all live in PSRAM, so the question is not "is PSRAM mapped" but
 * "how much contiguous PSRAM survives IDF's own allocations". */
static void stage_heap(void)
{
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "internal: %u free, largest %u",
             (unsigned)internal, (unsigned)internal_largest);
    ESP_LOGI(TAG, "psram:    %u free, largest %u",
             (unsigned)psram, (unsigned)psram_largest);

    /* A 320x240 RGB565 double framebuffer is 2 x 153600 bytes. If PSRAM is
     * not up, this allocation is what makes it obvious. */
    void *fb = heap_caps_malloc(2u * LCD_H * LCD_V * 2u, MALLOC_CAP_SPIRAM);
    if (fb) {
        memset(fb, 0x5a, 2u * LCD_H * LCD_V * 2u);
        free(fb);
        record(0, ST_PASS, "internal %u B, psram %u B, double fb alloc'd",
               (unsigned)internal, (unsigned)psram);
    } else {
        record(0, ST_FAIL, "psram double-framebuffer alloc failed "
               "(internal %u B, psram %u B)", (unsigned)internal,
               (unsigned)psram);
    }
}

/* ------------------------------------------------------------------ */
/* stage 1: I2C and the IO expander                                    */
/* ------------------------------------------------------------------ */

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t dev_pca9557;
static i2c_master_dev_handle_t dev_axp173;

static esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg,
                              uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 1000);
}

static esp_err_t i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg,
                               uint8_t val)
{
    uint8_t d[2] = { reg, val };
    return i2c_master_transmit(dev, d, sizeof(d), 1000);
}

static esp_err_t expander_write(uint8_t val)
{
    return i2c_reg_write(dev_pca9557, PCA9557_REG_OUTPUT, val);
}

/* Reading the output register only proves we wrote a register. The input
 * register mirrors the actual pin level, including on pins set as outputs, so
 * reading both while a level is being commanded distinguishes "the expander
 * latched the value" from "the pin is at that voltage" -- which is the
 * difference between commanding a reset pulse and the panel ever seeing one. */
static void expander_probe(const char *tag)
{
    uint8_t cfg = 0, out = 0, in = 0;
    i2c_reg_read(dev_pca9557, PCA9557_REG_CONFIG, &cfg, 1);
    i2c_reg_read(dev_pca9557, PCA9557_REG_OUTPUT, &out, 1);
    i2c_reg_read(dev_pca9557, PCA9557_REG_INPUT, &in, 1);
    ESP_LOGI(TAG, "expander %s: cfg=0x%02x out=0x%02x in=0x%02x "
           "rst_pin=%d cs_pin=%d touch_pin=%d",
           tag, cfg, out, in,
           !!(in & (1u << EXP_LCD_RST)), !!(in & (1u << EXP_LCD_CS)),
           !!(in & (1u << EXP_TOUCH_RST)));
}

static void stage_i2c_expander(void)
{
    /* Before touching the controller: are SDA and SCL electrically idle high?
     * A line stuck low means a device is mid-transaction (needs the nine-clock
     * recovery) or the rail feeding the 10K pull-ups is off. Distinguishing an
     * electrical fault from a driver fault is worth two lines, and the driver's
     * ESP_ERR_INVALID_STATE does not tell you which you have. */
    for (int i = 0; i < 2; i++) {
        int pin = i == 0 ? PIN_I2C_SDA : PIN_I2C_SCL;
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    int sda = gpio_get_level(PIN_I2C_SDA);
    int scl = gpio_get_level(PIN_I2C_SCL);
    ESP_LOGI(TAG, "bus idle: SDA(%d)=%d SCL(%d)=%d", PIN_I2C_SDA, sda,
             PIN_I2C_SCL, scl);
    if (!sda || !scl) {
        /* Nine clocks on SCL with SDA released frees a slave that latched
         * mid-transfer. The schematic has no bus switch, so this only helps a
         * stuck slave, not an unpowered one. */
        ESP_LOGW(TAG, "line held low, trying nine-clock recovery");
        gpio_set_direction(PIN_I2C_SCL, GPIO_MODE_OUTPUT);
        for (int n = 0; n < 9; n++) {
            gpio_set_level(PIN_I2C_SCL, 0);
            vTaskDelay(pdMS_TO_TICKS(5));
            gpio_set_level(PIN_I2C_SCL, 1);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        gpio_set_direction(PIN_I2C_SCL, GPIO_MODE_INPUT);
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_LOGI(TAG, "after recovery: SDA=%d SCL=%d", gpio_get_level(PIN_I2C_SDA),
                 gpio_get_level(PIN_I2C_SCL));
    }

    i2c_master_bus_config_t buscfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Left at 0 the driver runs fully synchronously, and every multi-op
         * transaction (anything but probe's START/STOP pair) returns
         * ESP_ERR_INVALID_STATE immediately. Giving it a queue depth makes the
         * calls return ESP_OK but the received bytes come back wrong (two
         * different devices both read 0xa1), and the driver itself warns that
         * asynchronous mode "cannot get bus error from API". So stay
         * synchronous and find the real fault instead of papering over it. */
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&buscfg, &i2c_bus);
    if (err != ESP_OK) {
        record(1, ST_FAIL, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "i2c bus %d created", (int)I2C_PORT);

    i2c_device_config_t devcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR_PCA9557,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(i2c_bus, &devcfg, &dev_pca9557);
    if (err != ESP_OK) {
        record(1, ST_FAIL, "add pca9557 device: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "pca9557 device added at 0x%02x", ADDR_PCA9557);

    /* Register the PMU here rather than at the end of this stage: the expander
     * may be unreachable and that must not cost us the power/charge data. */
    i2c_device_config_t axpcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR_AXP173,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(i2c_bus, &axpcfg, &dev_axp173);

    /* Isolate which transfer shape the synchronous path actually completes.
     * probe() is a 2-op START/STOP pair and works; a register read is a 6-op
     * list containing a zero-length READ (total_bytes = read_size - 1) when
     * read_size is 1. Test each in turn, resetting the FSM in between so one
     * failure cannot poison the next result. */
    {
        /* Which devices ACK tells us which rails are up. The pull-ups sit on
         * EXP_3V3, so lines can idle high while the expander itself is
         * unpowered -- healthy lines plus no ACK is exactly that signature.
         * The AXP173 normally runs straight off the battery, so if it ACKs and
         * the expander does not, the expander's rail is the thing that is off. */
        /* Full 7-bit scan: if the expander answers somewhere other than 0x19
         * that is an addressing bug of ours; if it answers nowhere while every
         * other device does, its reset or supply rail is the suspect. */
        int found = 0;
        for (int a = 0x08; a <= 0x77; a++) {
            esp_err_t p = i2c_master_probe(i2c_bus, (uint8_t)a, 100);
            if (p == ESP_OK) {
                ESP_LOGI(TAG, "  ACK at 0x%02x", a);
                found++;
            }
        }
        ESP_LOGI(TAG, "bus scan: %d device(s)", found);
    }

    /* The expander is absent from the scan while six other devices answer. It
     * is clearly powered enough to keep driving the LCD CS it was last told to
     * assert (the panel takes data), so the question is whether its supply is
     * actually at 3.3 V. The AXP173's voltage registers POR to values far below
     * that and the reference BSP writes them explicitly before enabling.
     *
     * REG 0x26 DCDC1, 0x27 LDO4: 700-3500 mV, 25 mV steps -> 3300 = 0x68
     * REG 0x28 LDO2/LDO3: 1800-3300 mV, 100 mV steps -> 3300 = 0xFF
     * REG 0x12 output enable: bit0 DCDC1, 1 LDO4, 2 LDO2, 3 LDO3, 4 DCDC2, 6 EXTEN
     */
    {
        i2c_device_config_t axp_early = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADDR_AXP173,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t axp;
        if (i2c_master_bus_add_device(i2c_bus, &axp_early, &axp) == ESP_OK) {
            static const uint8_t dump[] = {
                0x00, 0x01, 0x12, 0x23, 0x26, 0x27, 0x28, 0x30, 0x31, 0x32
            };
            for (unsigned i = 0; i < sizeof(dump); i++) {
                uint8_t v = 0;
                if (i2c_master_transmit_receive(axp, &dump[i], 1, &v, 1, 1000) == ESP_OK) {
                    ESP_LOGI(TAG, "AXP REG %02x = %02x", dump[i], v);
                }
            }

            /* Match the reference BSP exactly, then re-scan. */
            static const uint8_t set[][2] = {
                { 0x26, 0x68 }, { 0x27, 0x68 }, { 0x28, 0xFF },
            };
            for (unsigned i = 0; i < sizeof(set) / sizeof(set[0]); i++) {
                i2c_master_transmit(axp, set[i], 2, 1000);
            }
            /* REG 0x12 bit 7 is VBUSHC, the VBUS hardware current limit. It is
             * NOT a supply enable and setting it clamps VBUS to ~100 mA, which
             * browns out the ESP32-S3 and drops the USB link outright -- this
             * cost a board re-plug to recover. Never write bit 7 here; the
             * highest legal value is 0x4F (EXTEN). */
            static const uint8_t tries[] = { 0x0F, 0x4F, 0x0F };
            for (unsigned i = 0; i < sizeof(tries); i++) {
                uint8_t d[2] = { 0x12, tries[i] };
                i2c_master_transmit(axp, d, 2, 1000);
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_err_t p = i2c_master_probe(i2c_bus, ADDR_PCA9557, 300);
                ESP_LOGI(TAG, "REG12=0x%02x -> pca9557 %s", tries[i],
                         p == ESP_OK ? "ACK" : "no ack");
            }

            /* Power-cycle the rail: a chip in a latched state only recovers if
             * its supply actually drops. */
            uint8_t off[2] = { 0x12, 0x03 };
            i2c_master_transmit(axp, off, 2, 1000);
            vTaskDelay(pdMS_TO_TICKS(100));
            uint8_t on[2] = { 0x12, 0x0F };
            i2c_master_transmit(axp, on, 2, 1000);
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "after rail power-cycle: pca9557 %s",
                     i2c_master_probe(i2c_bus, ADDR_PCA9557, 300) == ESP_OK
                         ? "ACK" : "no ack");

            /* LDO4 is the expander's supply: it came up at 0x14 = 1200 mV and
             * the expander did not ACK until it was raised to 0x68 = 3300 mV.
             * Confirm by driving it back down and watching the ACK track the
             * rail -- but only as far as 2.5 V. Dropping it to its boot value of
             * 1.2 V risks brown-ing whatever else hangs off LDO4, and the board
             * has already been off USB once this session. */
            static const uint8_t ldo4[][2] = {
                { 0x27, 0x48 }, { 0x27, 0x68 }, { 0x27, 0x48 }, { 0x27, 0x68 },
            };
            for (unsigned i = 0; i < sizeof(ldo4) / sizeof(ldo4[0]); i++) {
                uint16_t mv = 700 + (uint16_t)ldo4[i][1] * 25;
                i2c_master_transmit(axp, ldo4[i], 2, 1000);
                vTaskDelay(pdMS_TO_TICKS(60));
                ESP_LOGI(TAG, "LDO4=%u mV -> pca9557 %s", mv,
                         i2c_master_probe(i2c_bus, ADDR_PCA9557, 300) == ESP_OK
                             ? "ACK" : "no ack");
            }
        }
    }

    /* Presence must be tested on REG_CONFIG, not on a status register: the
     * expander powers up with all bits set as inputs. */
    uint8_t cfg = 0;
    err = i2c_reg_read(dev_pca9557, PCA9557_REG_CONFIG, &cfg, 1);
    if (err != ESP_OK) {
        record(1, ST_FAIL, "read CONFIG: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "pca9557 CONFIG = 0x%02x", cfg);

    /* Make LCD reset, LCD CS and the amp enable outputs. */
    uint8_t out = 0;
    i2c_reg_read(dev_pca9557, PCA9557_REG_OUTPUT, &out, 1);
    out &= ~(1u << EXP_LCD_RST);           /* hold in reset: /RES is active low */
    out &= ~(1u << EXP_LCD_CS);            /* CS asserted: only device on bus */
    out &= ~(1u << EXP_AMP_EN);            /* amp off for now */
    err = i2c_reg_write(dev_pca9557, PCA9557_REG_CONFIG,
                        cfg & ~((1u << EXP_LCD_RST) | (1u << EXP_LCD_CS) |
                                (1u << EXP_AMP_EN)));
    if (err == ESP_OK) {
        err = expander_write(out);
    }
    if (err != ESP_OK) {
        record(1, ST_FAIL, "CONFIG/OUTPUT write: %s", esp_err_to_name(err));
        return;
    }

    uint8_t back = 0;
    i2c_reg_read(dev_pca9557, PCA9557_REG_CONFIG, &back, 1);

    record(1, (back & (1u << EXP_LCD_RST)) == 0 ? ST_PASS : ST_FAIL,
           "CONFIG 0x%02x -> 0x%02x", cfg, back);
}

/* ------------------------------------------------------------------ */
/* stage 2: backlight                                                  */
/* ------------------------------------------------------------------ */

/* The LED is ACTIVE LOW: a full-high duty turns it OFF. The reference
 * firmware writes duty 512 to switch the display off and 0 for full
 * brightness. Getting this backwards looks exactly like a dead panel. */
static void set_backlight(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void stage_backlight(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_9_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        record(2, ST_FAIL, "ledc_timer_config: %s", esp_err_to_name(err));
        return;
    }

    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 511,          /* active low: 511 is essentially OFF */
        .hpoint = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        record(2, ST_FAIL, "ledc_channel_config: %s", esp_err_to_name(err));
        return;
    }

    set_backlight(0);        /* full brightness */
    vTaskDelay(pdMS_TO_TICKS(200));
    set_backlight(256);      /* half */
    record(2, ST_PASS, "GPIO %d at 50%% duty (active low)", PIN_LCD_BL);
}

/* ------------------------------------------------------------------ */
/* stage 3: the panel                                                  */
/* ------------------------------------------------------------------ */

static esp_lcd_panel_handle_t panel;

/* GPIO 39, 40 and 41 are the JTAG pads -- MTCK, MTDO and MTDI -- and their IO
 * MUX default (MCU_SEL = 0) hands them to the JTAG block, not to the GPIO
 * matrix. Routing FSPICLK/FSPIQ into the matrix is only half the path: the pad
 * mux still has to be switched to the GPIO function, or the pin never moves no
 * matter what the matrix or the output enable say. gpio_set_direction() does
 * not touch MCU_SEL, and gpio_reset_pin() puts the pad back to its default,
 * which here is JTAG. This was already found once on the bare-metal branch. */
static unsigned pad_mcu_sel(int pin)
{
    volatile uint32_t *r = (volatile uint32_t *)(DR_REG_IO_MUX_BASE + 0x04 + 4 * pin);
    return (*r >> 12) & 7u;
}

/* Compare the hardware state between the path that paints and the path that
 * does not. Everything here is read from the peripheral, not from what we
 * asked for: the IO MUX pad register (function select, input enable, pulls,
 * drive strength), the GPIO matrix output-select entry (function, invert,
 * output-enable source), the output-enable bit, the actual pad level, and the
 * matrix input slots for the GPSPI signals. The input slots matter because
 * writing only the output half leaves the controller unable to see the bus. */
static void pin_state_dump(const char *tag)
{
    static const int pins[] = { PIN_LCD_DC, PIN_LCD_MOSI, PIN_LCD_SCLK, PIN_LCD_BL };
    ESP_LOGI(TAG, "pin state (%s):", tag);
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        int p = pins[i];
        uint32_t mux = *(volatile uint32_t *)(uintptr_t)(DR_REG_IO_MUX_BASE + 0x04 + 4 * p);
        const volatile uint32_t *osc =
            (const volatile uint32_t *)(uintptr_t)&GPIO.func_out_sel_cfg[p];
        unsigned en = p < 32 ? (unsigned)((GPIO.enable >> p) & 1u)
                             : (unsigned)((GPIO.enable1.val >> (p - 32)) & 1u);
        unsigned ov = p < 32 ? (unsigned)((GPIO.out >> p) & 1u)
                             : (unsigned)((GPIO.out1.val >> (p - 32)) & 1u);
        unsigned iv = p < 32 ? (unsigned)((GPIO.in >> p) & 1u)
                             : (unsigned)((GPIO.in1.val >> (p - 32)) & 1u);
        ESP_LOGI(TAG, "  pin %2d mux=0x%08x mcu_sel=%u ie=%u pu=%u pd=%u drv=%u"
               " | out_sel=0x%08x func=%u oen_sel=%u | en=%u out=%u in=%u",
                 p, mux, (unsigned)((mux >> 12) & 7u), (unsigned)((mux >> 9) & 1u),
                 (unsigned)((mux >> 8) & 1u), (unsigned)((mux >> 7) & 1u),
                 (unsigned)((mux >> 10) & 3u), *osc, (unsigned)(*osc & 0x1FFu),
                 (unsigned)((*osc >> 10) & 1u), en, ov, iv);
    }
    static const int sigs[] = { FSPICLK_OUT_IDX, FSPID_OUT_IDX };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        const volatile uint32_t *isc =
            (const volatile uint32_t *)(uintptr_t)&GPIO.func_in_sel_cfg[sigs[i]];
        ESP_LOGI(TAG, "  signal %d in_sel=0x%02x in_inv=%u", sigs[i],
                 (unsigned)(*isc & 0x3Fu), (unsigned)((*isc >> 6) & 1u));
    }
    if (dev_pca9557) {
        uint8_t cfg = 0, outb = 0, inb = 0;
        i2c_reg_read(dev_pca9557, PCA9557_REG_CONFIG, &cfg, 1);
        i2c_reg_read(dev_pca9557, PCA9557_REG_OUTPUT, &outb, 1);
        i2c_reg_read(dev_pca9557, PCA9557_REG_INPUT, &inb, 1);
        ESP_LOGI(TAG, "  expander cfg=0x%02x out=0x%02x in=0x%02x rst=%d cs=%d",
                 cfg, outb, inb, !!(inb & (1u << EXP_LCD_RST)),
                 !(inb & (1u << EXP_LCD_CS)));
    }
}

static void claim_lcd_pads(const char *tag)
{
    const int pins[3] = { PIN_LCD_DC, PIN_LCD_MOSI, PIN_LCD_SCLK };
    unsigned before[3];

    for (int i = 0; i < 3; i++) {
        before[i] = pad_mcu_sel(pins[i]);
        esp_rom_gpio_pad_select_gpio((uint32_t)pins[i]);   /* MCU_SEL -> GPIO */
    }
    ESP_LOGI(TAG, "%s pads: MCU_SEL dc %u->%u  mosi %u->%u  sclk %u->%u",
             tag, before[0], pad_mcu_sel(pins[0]),
             before[1], pad_mcu_sel(pins[1]),
             before[2], pad_mcu_sel(pins[2]));
}

/* Count SCLK edges on the pad while a transfer is running. The counter must
 * not touch the pad's direction -- gpio_set_direction() re-routes the pad to
 * SIG_GPIO_OUT_IDX and would disconnect the very signal being measured -- so
 * only gpio_input_enable() is used, which sets the IO MUX input bit and leaves
 * the output routing alone. Pinned to the other core so it runs in parallel
 * with the transfer instead of starving it. At 80 MHz a polling loop misses
 * most edges, so the number is a lower bound; zero versus non-zero is the
 * signal being looked for. */
static volatile int s_cnt_run;
static volatile int s_cnt_done;
static volatile int s_cnt_edges;

static void edge_counter_task(void *arg)
{
    (void)arg;
    gpio_input_enable((gpio_num_t)PIN_LCD_SCLK);
    int last = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
    int n = 0;
    while (s_cnt_run) {
        int now = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
        if (now != last) { n++; last = now; }
    }
    s_cnt_edges = n;
    s_cnt_done = 1;
    vTaskDelete(NULL);
}

static void stage_lcd(void)
{
    /* The earlier "esp_lcd emits no clock edges at the pad" verdict was an
     * artefact of the probe: it called gpio_set_direction() immediately before
     * counting, which re-routes the pad to SIG_GPIO_OUT_IDX and disconnects it
     * from the peripheral being measured. With the pixel contract now settled,
     * this is the path the port is meant to use. */
    if (!dev_pca9557) {
        record(3, ST_SKIPPED, "needs the expander");
        return;
    }

    claim_lcd_pads("lcd");

    /* The reference drives SCLK LOW before it touches the pad's direction or
     * matrix routing: "gpio_lo(spi_sclk); // prevent a HIGH pulse being
     * produced by the pinMode change (countermeasure for panels without CS)".
     * That caveat is exactly our wiring -- CS is held asserted by the expander
     * for the life of the bus, so the panel is always listening. An edge
     * produced while the pad is being reconfigured is clocked in and shifts the
     * panel's bit alignment by one, after which every command and every
     * argument decodes as garbage while each transfer still reports ESP_OK.
     * This is why driving the same pins by hand worked and esp_lcd did not, at
     * any clock rate. */
    for (int i = 0; i < 3; i++) {
        const int p[3] = { PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_DC };
        gpio_set_direction((gpio_num_t)p[i], GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)p[i], 0);
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    /* The reference's expander sequence, in its order. Three things we were
     * doing differently: CS is left DESELECTED across the entire reset pulse
     * and only asserted after the panel comes out of reset; touch reset is
     * pulsed together with the panel's; and the amp enable is driven low.
     * Holding CS asserted while /RES is low lets the interface latch the idle
     * clock/data state, which is precisely the kind of failure that looks like
     * a dead panel while every SoC-side register reads correct. */
    expander_write((1u << EXP_LCD_CS) | (0u << EXP_LCD_RST) |
                   (0u << EXP_AMP_EN) | (0u << EXP_TOUCH_RST));
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_write((1u << EXP_LCD_CS) | (1u << EXP_LCD_RST) |
                   (0u << EXP_AMP_EN) | (1u << EXP_TOUCH_RST));
    vTaskDelay(pdMS_TO_TICKS(120));
    expander_write((0u << EXP_LCD_CS) | (1u << EXP_LCD_RST) |
                   (0u << EXP_AMP_EN) | (1u << EXP_TOUCH_RST));
    vTaskDelay(pdMS_TO_TICKS(5));

    spi_bus_config_t sbus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H * LCD_V * 2 + 8,
    };
    esp_err_t err = spi_bus_initialize(SPI_LCD_HOST, &sbus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        record(3, ST_FAIL, "spi_bus_initialize: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_spi_config_t iocfg = {
        .dc_gpio_num = PIN_LCD_DC,
        /* CS lives on the I2C expander, so there is no GPIO to hand the
         * driver. We hold CS asserted for the life of the bus instead, which
         * is safe because the panel is the only device on GPSPI2. */
        .cs_gpio_num = -1,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        /* Must be >= 1. Zero is not a legal value here: spi_bus_add_device()
         * calls xQueueCreate(queue_size, ...) unconditionally and FreeRTOS
         * asserts on a zero-length queue (queue.c:573), which rebooted the
         * board. The colour path always queues, so the thing to get right is
         * keeping the buffer alive until the transfer really finishes. */
        .trans_queue_depth = 4,
    };
    esp_lcd_panel_io_handle_t io;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_LCD_HOST,
                                   &iocfg, &io);
    if (err != ESP_OK) {
        record(3, ST_FAIL, "new_panel_io_spi: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,     /* already handled via the expander */
        /* BGR, not RGB. Measured: with the RGB ordering green and white are
         * correct but red and blue are exchanged -- MADCTL bit 3 alone. The
         * reference asks for the same thing via rgb_order = false. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &pcfg, &panel);
    if (err != ESP_OK) {
        record(3, ST_FAIL, "new_panel_st7789: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);

    /* esp_lcd's ST7789 driver sends only SLPOUT, MADCTL, COLMOD and RAMCTRL.
     * The reference firmware (LovyanGFX Panel_ST7789) also runs the power
     * sequencing and gamma block below. Without VCOMS / VRHS / VDV and gate
     * control the source and gate drivers have no bias, so the controller
     * happily accepts every pixel and the glass stays dark -- which is exactly
     * the failure this looked like: backlight on, every call ESP_OK, black
     * screen. Values taken verbatim from the reference init list.
     * RAMCTRL is left alone: esp_lcd pairs it with its own byte order. */
    static const struct {
        uint8_t cmd;
        uint8_t n;
        uint8_t p[14];
    } power_seq[] = {
        { 0xB2,  5, { 0x0c, 0x0c, 0x00, 0x33, 0x33 } },   /* PORCTRL  */
        { 0xB7,  1, { 0x35 } },                           /* GCTRL    */
        { 0xBB,  1, { 0x28 } },                           /* VCOMS    */
        { 0xC0,  1, { 0x0C } },                           /* LCMCTRL  */
        { 0xC2,  2, { 0x01, 0xFF } },                     /* VDVVRHEN */
        { 0xC3,  1, { 0x10 } },                           /* VRHS     */
        { 0xC4,  1, { 0x20 } },                           /* VDVSET   */
        { 0xC6,  1, { 0x0f } },                           /* FRCTR2   */
        { 0xD0,  2, { 0xa4, 0xa1 } },                     /* PWCTRL1  */
        { 0xE0, 14, { 0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32,
                      0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17 } }, /* PVGAM */
        { 0xE1, 14, { 0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31,
                      0x54, 0x47, 0x0e, 0x1c, 0x17, 0x1b, 0x1e } }, /* NVGAM */
    };
    for (unsigned i = 0; i < sizeof(power_seq) / sizeof(power_seq[0]); i++) {
        esp_lcd_panel_io_tx_param(io, power_seq[i].cmd,
                                  power_seq[i].p, power_seq[i].n);
    }
    esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0);         /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(130));
    /* ST7789 needs RGB inversion. The GRAM is 240x320 and we drive it as
     * 320x240, hence swap_xy plus a y offset of 320-240. */
    /* Measured contract is MADCTL 0xA8 = MV | MY | BGR. The driver composes
     * that value from rgb_ele_order (the BGR bit, set above), swap_xy (MV) and
     * mirror (MX/MY), so the three calls below are what produce 0xA8. */
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    /* No offset: the panel's GRAM is a full 240x320 and we drive it as
     * 320x240, so with MV set CASET spans the 320 axis and RASET the 240 one.
     * The 80 that shows up in 240x240 ST7789 recipes does not apply here, and
     * asking for RASET 80..319 runs 80 rows past the end of the range. */
    esp_lcd_panel_set_gap(panel, 0, 0);
    esp_lcd_panel_disp_on_off(panel, true);

    uint16_t *fb = heap_caps_malloc((size_t)LCD_H * LCD_V * 2,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const char *fbmem = "internal DMA";
    if (!fb) {
        fb = heap_caps_malloc((size_t)LCD_H * LCD_V * 2, MALLOC_CAP_SPIRAM);
        fbmem = "psram";
    }
    if (!fb) {
        record(3, ST_FAIL, "framebuffer alloc failed");
        return;
    }

    /* Confirm the expander is really driving the lines we think it is:
     * IO0 (reset) must read 1 = released, IO1 (CS) must read 0 = asserted.
     * The PCA9557 input register mirrors the pin level even for pins set as
     * outputs, so reading both registers distinguishes "we wrote the register"
     * from "the pin is actually at that level" -- at power-on every expander
     * pin is an input, which leaves LCD reset and CS floating. */
    uint8_t outb = 0, inb = 0;
    i2c_reg_read(dev_pca9557, PCA9557_REG_OUTPUT, &outb, 1);
    i2c_reg_read(dev_pca9557, PCA9557_REG_INPUT, &inb, 1);
    ESP_LOGI(TAG, "expander OUT=0x%02x IN=0x%02x rst_pin=%d cs_pin=%d fb=%s",
             outb, inb,
             !!(inb & (1u << EXP_LCD_RST)), !(inb & (1u << EXP_LCD_CS)), fbmem);

    /* Content bisect. The pin state is now known to be equivalent between the
     * two paths, so the remaining variable is what esp_lcd puts in the
     * transfer. Make this path byte-for-byte equivalent to the driver that
     * paints, changing only the transport. Three things differ from esp_lcd's
     * defaults:
     *   - RAMCTRL: IDF sends 0x00,0xF0 ("big endian" per its own comment);
     *     the reference and the working driver send 0x00,0xC0.
     *   - MADCTL: written directly as the measured 0xA8 instead of composed
     *     from swap_xy / mirror / rgb_ele_order.
     *   - pixel bytes: esp_lcd_panel_io_tx_color ships the raw memory of a
     *     uint16_t buffer, which on little-endian Xtensa is LOW byte first.
     *     The working driver sent high byte first, so the buffer is built here
     *     as explicit big-endian pairs.
     * If this paints, the transport is fine and our esp_lcd configuration was
     * the fault. If it stays black, the difference is IDF's spi_device + DMA
     * path itself. */
    static const struct {
        uint8_t cmd;
        uint8_t n;
        uint8_t p[14];
    } ref_seq[] = {
        { 0xB2,  5, { 0x0c, 0x0c, 0x00, 0x33, 0x33 } },   /* PORCTRL  */
        { 0xB7,  1, { 0x35 } },                           /* GCTRL    */
        { 0xBB,  1, { 0x28 } },                           /* VCOMS    */
        { 0xC0,  1, { 0x0C } },                           /* LCMCTRL  */
        { 0xC2,  2, { 0x01, 0xFF } },                     /* VDVVRHEN */
        { 0xC3,  1, { 0x10 } },                           /* VRHS     */
        { 0xC4,  1, { 0x20 } },                           /* VDVSET   */
        { 0xC6,  1, { 0x0f } },                           /* FRCTR2   */
        { 0xD0,  2, { 0xa4, 0xa1 } },                     /* PWCTRL1  */
        { 0xE0, 14, { 0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32,
                      0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17 } },
        { 0xE1, 14, { 0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31,
                      0x54, 0x47, 0x0e, 0x1c, 0x17, 0x1b, 0x1e } },
        { 0x3A,  1, { 0x55 } },                           /* COLMOD   */
        { 0xB0,  2, { 0x00, 0xC0 } },                     /* RAMCTRL, reference value */
        { 0x36,  1, { 0xA8 } },                           /* MADCTL, measured value */
        { 0x21,  0, { 0x00 } },                           /* INVON    */
        { 0x2A,  4, { 0x00, 0x00, 0x01, 0x3F } },         /* CASET 0..319 */
        { 0x2B,  4, { 0x00, 0x00, 0x00, 0xEF } },         /* RASET 0..239 */
        { 0x11,  0, { 0x00 } },                           /* SLPOUT   */
        { 0x29,  0, { 0x00 } },                           /* DISPON   */
    };
    for (unsigned i = 0; i < sizeof(ref_seq) / sizeof(ref_seq[0]); i++) {
        esp_lcd_panel_io_tx_param(io, ref_seq[i].cmd,
                                  ref_seq[i].n ? ref_seq[i].p : NULL,
                                  ref_seq[i].n);
    }
    vTaskDelay(pdMS_TO_TICKS(130));

    /* Big-endian pixel pairs written straight into the buffer esp_lcd ships
     * verbatim, matching the working driver's byte order. */
    uint8_t *be = (uint8_t *)fb;
    for (int y = 0; y < LCD_V; y++) {
        for (int x = 0; x < LCD_H; x++) {
            uint16_t c;
            if      (y < LCD_V / 2 && x < LCD_H / 2) c = 0xF800;
            else if (y < LCD_V / 2)                  c = 0x07E0;
            else if (x < LCD_H / 2)                  c = 0x001F;
            else                                     c = 0xFFFF;
            be[(y * LCD_H + x) * 2]     = (uint8_t)(c >> 8);
            be[(y * LCD_H + x) * 2 + 1] = (uint8_t)(c & 0xFF);
        }
    }
    s_cnt_edges = 0;
    s_cnt_done = 0;
    s_cnt_run = 1;
    xTaskCreatePinnedToCore(edge_counter_task, "edgecnt", 3072, NULL, 12, NULL, 1);
    err = esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H, LCD_V, fb);
    s_cnt_run = 0;
    while (!s_cnt_done) { vTaskDelay(1); }
    ESP_LOGI(TAG, "esp_lcd draw_bitmap: %d sclk edges counted (lower bound)",
             s_cnt_edges);
    /* The colour path queues, so draw_bitmap can return while the DMA is
     * still reading. Hold the buffer until the pixels are on the glass. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Everything reports success, so check the thing that actually matters:
     * are the GPSPI signals wired to the pads the panel is wired to. DC should
     * read SIG_GPIO_OUT_IDX (256 = driven from GPIO_OUT_REG, i.e. a plain GPIO
     * level), SCLK should be FSPICLK_OUT_IDX (101) and MOSI FSPIQ_OUT_IDX (102).
     * enable1 bits 7/8/9 are the output drivers for GPIO 39/40/41. */
    ESP_LOGI(TAG, "routing: dc(39)=%u mosi(40)=%u sclk(41)=%u enable1=0x%08x",
             (unsigned)GPIO.func_out_sel_cfg[PIN_LCD_DC].func_sel,
             (unsigned)GPIO.func_out_sel_cfg[PIN_LCD_MOSI].func_sel,
             (unsigned)GPIO.func_out_sel_cfg[PIN_LCD_SCLK].func_sel,
             (unsigned)GPIO.enable1.val);

    free(fb);
    fb = NULL;

    pin_state_dump("esp_lcd, black screen");
    record(3, err == ESP_OK ? ST_PASS : ST_FAIL,
           "esp_lcd quadrants %dx%d @ %d MHz (%s)",
           LCD_H, LCD_V, LCD_PCLK_HZ / 1000000, esp_err_to_name(err));
}

/* ------------------------------------------------------------------ */
/* stage 4: SD card                                                    */
/* ------------------------------------------------------------------ */

static void stage_sd(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    /* Only D0 is wired on this board, so 1-bit is the ceiling, not a
     * fallback. The pins are non-default, which is why
     * CONFIG_SDMMC_USE_GPIO_MATRIX is set. */
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;
    slot.d1  = -1;
    slot.d2  = -1;
    slot.d3  = -1;
    slot.cd  = -1;
    slot.wp  = -1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mcfg,
                                            &card);
    if (err != ESP_OK) {
        record(4, ST_FAIL, "mount: %s", esp_err_to_name(err));
        return;
    }

    /* Round-trip a file so we know the data path, not just enumeration. */
    const char *path = "/sdcard/spike.txt";
    const char *body = "meowkit idf spike\n";
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        ok = fwrite(body, 1, strlen(body), f) == strlen(body);
        fclose(f);
    }
    if (ok) {
        char rb[32] = { 0 };
        f = fopen(path, "rb");
        if (f) {
            ok = fread(rb, 1, sizeof(rb) - 1, f) == strlen(body) &&
                 strcmp(rb, body) == 0;
            fclose(f);
        }
    }
    remove(path);

    /* capacity is in sectors; multiplying by 512 in 32 bits overflows on any
     * card over 4 GB and prints nonsense. */
    unsigned long long mb = (unsigned long long)card->csd.capacity *
                            card->csd.sector_size / (1024ull * 1024ull);
    record(4, ok ? ST_PASS : ST_FAIL, "%s, %llu MB, %s",
           card->cid.name, mb,
           ok ? "write/read verified" : "write/read failed");

    if (!ok) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
    }
}

/* ------------------------------------------------------------------ */
/* stage 5: PMU                                                        */
/* ------------------------------------------------------------------ */

static void stage_pmu(void)
{
    if (!dev_axp173) {
        record(5, ST_SKIPPED, "needs i2c");
        return;
    }

    /* Presence: use REG 0x12 (output control). On a pure-battery boot REG
     * 0x00 legitimately reads 0x00, which looks like an absent device. */
    uint8_t outctl = 0;
    esp_err_t err = i2c_reg_read(dev_axp173, 0x12, &outctl, 1);
    if (err != ESP_OK || outctl == 0xFF) {
        record(5, ST_FAIL, "REG12=0x%02x (%s)", outctl, esp_err_to_name(err));
        return;
    }

    /* Battery voltage: REG 0x78 high, REG 0x79 low nibble, 1.1 mV counts. */
    uint8_t hi = 0, lo = 0;
    i2c_reg_read(dev_axp173, 0x78, &hi, 1);
    i2c_reg_read(dev_axp173, 0x79, &lo, 1);
    unsigned mv = (((unsigned)hi << 4) | (lo & 0x0fu)) * 11u / 10u;

    /* Charge current: REG 0x7A/0x7B, five low bits, 0.5 mA counts. */
    i2c_reg_read(dev_axp173, 0x7A, &hi, 1);
    i2c_reg_read(dev_axp173, 0x7B, &lo, 1);
    unsigned ma = (((unsigned)hi << 5) | (lo & 0x1fu)) / 2u;

    /* The power button is readable over I2C, and the PEK bits are NOT in the
     * first status pair: short press is REG 0x46 bit 1, long press bit 0.
     * The stock MeowKit driver reads 0x44 and therefore never sees a press. */
    uint8_t pek = 0;
    i2c_reg_read(dev_axp173, 0x46, &pek, 1);

    record(5, ST_PASS, "REG12=0x%02x vbat=%u mV ichg=%u mA pek0x46=0x%02x",
           outctl, mv, ma, pek);
}

/* ------------------------------------------------------------------ */
/* stage 6: Bluetooth                                                  */
/* ------------------------------------------------------------------ */

/* This is the whole reason for the transition: the controller is a prebuilt
 * blob (components/bt/controller/lib_esp32c3_family/esp32s3/libbtdm_app.a)
 * and the stack's OSI layer is built on FreeRTOS and esp_timer. There is no
 * route to BLE on this silicon that does not include IDF. */

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static SemaphoreHandle_t bt_synced;
static volatile int bt_addr_count;

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) {
        bt_addr_count++;
        if (bt_addr_count <= 5) {
            const uint8_t *a = ev->disc.addr.val;
            ESP_LOGI(TAG, "  %02x:%02x:%02x:%02x:%02x:%02x rssi=%d",
                     a[5], a[4], a[3], a[2], a[1], a[0], ev->disc.rssi);
        }
    }
    return 0;
}

static void on_sync(void)
{
    xSemaphoreGive(bt_synced);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset, reason %d", reason);
}

void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void stage_ble(void)
{
    bt_synced = xSemaphoreCreateBinary();

    int rc = nimble_port_init();
    if (rc != 0) {
        record(6, ST_FAIL, "nimble_port_init: %d", rc);
        return;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(ble_host_task);

    if (xSemaphoreTake(bt_synced, pdMS_TO_TICKS(5000)) != pdTRUE) {
        record(6, ST_FAIL, "host never synced (controller did not start)");
        return;
    }

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        record(6, ST_FAIL, "no usable identity address");
        return;
    }

    struct ble_gap_disc_params dp = {
        .filter_duplicates = 1,
        .passive = 0,
        /* itvl/window of 0 ask the controller for its default scan settings.
         * This NimBLE revision has no min/max interval fields. */
        .itvl = 0,
        .window = 0,
    };
    rc = ble_gap_disc(own_addr_type, 5000, &dp, gap_event, NULL);
    if (rc != 0) {
        record(6, ST_FAIL, "ble_gap_disc: %d", rc);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(6000));
    ble_gap_disc_cancel();

    record(6, bt_addr_count > 0 ? ST_PASS : ST_FAIL,
           "%d unique advertiser(s) seen", bt_addr_count);
}

/* ------------------------------------------------------------------ */

static void summary(void)
{
    printf("\n\n================ MeowKit ESP-IDF spike ================\n");
    for (int i = 0; i < N_STAGES; i++) {
        const char *s = results[i] == ST_PASS ? "PASS" :
                        results[i] == ST_FAIL ? "FAIL" : "SKIP";
        printf("  %-34s %-4s  %s\n", stage_name[i], s, notes[i]);
    }
    printf("=====================================================\n\n");
}

/* ------------------------------------------------------------------ */
/* stage 7: is there actually a clock coming out of GPSPI2?            */
/* ------------------------------------------------------------------ */

/* Hand-driven pins paint the panel; esp_lcd does not, at 40 MHz or at 1 MHz,
 * on the same pads with the same reset sequence. So the question is what the
 * controller emits. The pad is put in INPUT_OUTPUT so the IO MUX input enable
 * is set and GPIO_IN mirrors the pin the controller is driving; counting edges
 * there measures the real bit stream rather than the register we hoped would
 * produce it. A 8x8 colour draw is 64 pixels = 1024 data bits plus 8 command
 * and 32 window bits, so a few hundred edges means the transfer is running but
 * short, and zero means the clock never leaves the chip. */
__attribute__((unused)) static void stage_spi_probe(void)
{
    if (!panel) {
        record(7, ST_SKIPPED, "no panel to drive");
        return;
    }

    const volatile uint32_t *r =
        (const volatile uint32_t *)(uintptr_t)DR_REG_SPI2_BASE;
    uint32_t cg = r[0xE8 / 4];
    ESP_LOGI(TAG, "GPSPI2 clk_gate=0x%08x (clk_en=%d mst_active=%d mst_sel=%d) "
           "clock=0x%08x ctrl=0x%08x user=0x%08x user1=0x%08x user2=0x%08x mosi_dlen=%u",
           cg, !!(cg & 1), !!(cg & 2), !!(cg & 4),
           r[0x0c / 4], r[0x08 / 4], r[0x10 / 4], r[0x14 / 4], r[0x18 / 4],
           r[0x1c / 4]);
    ESP_LOGI(TAG, "MCU_SEL dc=%u mosi=%u sclk=%u",
             pad_mcu_sel(PIN_LCD_DC), pad_mcu_sel(PIN_LCD_MOSI),
             pad_mcu_sel(PIN_LCD_SCLK));
    ESP_LOGI(TAG, "func_sel dc=%u mosi=%u sclk=%u enable1=0x%08x",
             (unsigned)GPIO.func_out_sel_cfg[PIN_LCD_DC].func_sel,
             (unsigned)GPIO.func_out_sel_cfg[PIN_LCD_MOSI].func_sel,
             (unsigned)GPIO.func_out_sel_cfg[PIN_LCD_SCLK].func_sel,
             (unsigned)GPIO.enable1.val);
    /* func_sel is only part of the output-select entry. oen_sel chooses where
     * the pad's output enable comes from: 0 = GPIO_ENABLE register, 1 = the
     * signal's own output-enable line. With oen_sel set and that line inactive
     * the pad is tri-stated no matter what GPIO_ENABLE1 says, which is exactly
     * the signature we are chasing: registers programmed, transfers completing,
     * zero edges at the pad. */
    for (int i = 0; i < 3; i++) {
        const int p[3] = { PIN_LCD_DC, PIN_LCD_MOSI, PIN_LCD_SCLK };
        const volatile uint32_t *w =
            (const volatile uint32_t *)(uintptr_t)&GPIO.func_out_sel_cfg[p[i]];
        ESP_LOGI(TAG, "pad %d out_sel_cfg=0x%08x func=%u inv=%d oen_sel=%d oen_inv=%d",
                 p[i], *w, (unsigned)(*w & 0x1FFu), (int)((*w >> 9) & 1u),
                 (int)((*w >> 10) & 1u), (int)((*w >> 11) & 1u));
    }

    /* Read the pad back while the controller drives it. INPUT_OUTPUT sets the
     * IO MUX input enable without disturbing the matrix output, so GPIO_IN
     * mirrors the pin. */
    gpio_set_direction((gpio_num_t)PIN_LCD_SCLK, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction((gpio_num_t)PIN_LCD_MOSI, GPIO_MODE_INPUT_OUTPUT);

    /* A second device on the same bus, at a rate the polling loop can resolve
     * with certainty: 200 kHz is a 5 us period, so a register-read loop misses
     * nothing. 1024 bits is 5.12 ms of clock. If this emits edges, the bus and
     * the pads are fine and the fault is in how esp_lcd frames a transfer; if
     * it emits none, the controller is not reaching the pad at all. */
    spi_device_interface_config_t dcfg = {
        .mode = 0,
        .clock_speed_hz = 200 * 1000,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    spi_device_handle_t raw = NULL;
    esp_err_t err = spi_bus_add_device(SPI_LCD_HOST, &dcfg, &raw);
    if (err != ESP_OK || !raw) {
        record(7, ST_FAIL, "spi_bus_add_device: %s", esp_err_to_name(err));
        return;
    }

    static uint8_t buf[128];
    memset(buf, 0xA5, sizeof(buf));
    spi_transaction_t t = {
        .length = sizeof(buf) * 8,
        .tx_buffer = buf,
    };

    int edges = 0, mchg = 0;
    int last = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
    int lastm = (int)((GPIO.in1.val >> (PIN_LCD_MOSI - 32)) & 1u);

    /* Must be the QUEUED form. spi_device_polling_transmit blocks until the
     * transfer is over, so counting after it returns measures a bus that has
     * already gone idle -- which is exactly what the first version of this
     * probe did, and why it reported zero edges. */
    esp_err_t terr = spi_device_queue_trans(raw, &t, 0);

    TickType_t t0 = xTaskGetTickCount();
    while (xTaskGetTickCount() - t0 < 12) {
        int now = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
        if (now != last) { edges++; last = now; }
        int nowm = (int)((GPIO.in1.val >> (PIN_LCD_MOSI - 32)) & 1u);
        if (nowm != lastm) { mchg++; lastm = nowm; }
    }

    spi_transaction_t *done = NULL;
    spi_device_get_trans_result(raw, &done, pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "raw 1024-bit tx @200kHz queued -> %s; sclk edges=%u "
           "(want ~1024), mosi transitions=%u", esp_err_to_name(terr), edges, mchg);

    /* Validate the counter before believing a zero. Reconnect SCLK to the plain
     * GPIO output signal and toggle it by hand; the same loop must see twenty
     * edges. If it does, the counter is sound and the controller really is not
     * reaching the pad. If it does not, the readback path is the problem and
     * every edge count so far has been meaningless. */
    esp_rom_gpio_connect_out_signal((uint32_t)PIN_LCD_SCLK, SIG_GPIO_OUT_IDX,
                                    false, false);
    int vedges = 0;
    last = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
    for (int i = 0; i < 20; i++) {
        uint32_t bit = 1u << (PIN_LCD_SCLK - 32);
        if (i & 1) {
            GPIO.out1_w1tc.val = bit;
        } else {
            GPIO.out1_w1ts.val = bit;
        }
        esp_rom_delay_us(200);
        int now = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
        if (now != last) { vedges++; last = now; }
    }
    ESP_LOGI(TAG, "counter self-test: 20 hand toggles -> %d edges counted", vedges);
    record(7, (edges > 500 && vedges == 20) ? ST_PASS : ST_FAIL,
           "raw tx edges=%u, self-test %d/20", edges, vedges);
}

/* ------------------------------------------------------------------ */
/* stage 3 (direct): GPSPI2 programmed by hand                         */
/* ------------------------------------------------------------------ */

/* Two independent implementations have painted this panel: the reference's
 * LovyanGFX bus and our bare-metal driver. Neither drives pixels through an
 * IDF spi_device transaction -- both use spi_bus_initialize only for pin and
 * clock routing and then write SPI_USER / SPI_MS_DLEN / SPI_CMD themselves.
 * IDF's master returns ESP_OK for every transfer while producing no clock edge
 * at the pad at all (measured: 0 edges, against a self-test that counted
 * 20/20 hand toggles on the same pin), so this stage follows the two
 * implementations that are known to work on this board.
 */

#define D_SPI2      DR_REG_SPI2_BASE
#define D_SPI_CMD   (D_SPI2 + 0x00)
#define D_SPI_CTRL  (D_SPI2 + 0x08)
#define D_SPI_CLK   (D_SPI2 + 0x0C)
#define D_SPI_USER  (D_SPI2 + 0x10)
#define D_SPI_USER1 (D_SPI2 + 0x14)
#define D_SPI_USER2 (D_SPI2 + 0x18)
#define D_SPI_DLEN  (D_SPI2 + 0x1C)
#define D_SPI_DMA   (D_SPI2 + 0x30)
#define D_SPI_GATE  (D_SPI2 + 0xE8)
#define D_SPI_W0    (D_SPI2 + 0x98)   /* CPU buffer, 16 words, per IDF's own header */

#define D_CMD_UPDATE (1u << 23)
#define D_CMD_USR    (1u << 24)
#define D_USR_MOSI   (1u << 27)       /* not bit 6 -- that is the classic ESP32 map */
#define D_USR_MISO   (1u << 28)
#define D_DOUTDIN    (1u << 0)
#define D_GATE_ON    (BIT(0) | BIT(1) | BIT(2))
/* Full speed, matching the reference: cfg.freq_write = 80000000, which its own
 * comment describes as "max 80MHz, rounded to 80MHz divided by an integer" --
 * i.e. divide by one. clk_equ_sysclk (bit 31) selects the source undivided,
 * and D_GATE_ON has already put that source on the 80 MHz PLL.
 *
 * FIFO-only transfers are immune to the 80 MHz DMA tail corruption the
 * reference guards against (Bus_SPI.cpp:561-566), because there is no DMA read
 * to corrupt. If whole frames ever go over GDMA, the burst-enable rule there
 * becomes mandatory. */
#define D_CLK_BUS    (1u << 31)
#define D_BUF_WORDS  16
#define D_BUF_BYTES  (D_BUF_WORDS * 4)
#define SYS_CLK_EN0  (DR_REG_SYSTEM_BASE + 0x18)
#define SYS_SPI2_CLK BIT(6)
static inline uint32_t d_rd(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void d_wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void d_apply(void)
{
    /* CTRL, USER, USER1, MS_DLEN and the clock live in the APB domain; the
     * controller only sees them once UPDATE is pulsed, and UPDATE is
     * self-clearing. */
    d_wr(D_SPI_CMD, d_rd(D_SPI_CMD) | D_CMD_UPDATE);
    for (int i = 0; i < 4000 && (d_rd(D_SPI_CMD) & D_CMD_UPDATE); i++) { }
}

static void d_xfer(const uint8_t *data, int len)
{
    while (len > 0) {
        int n = len > D_BUF_BYTES ? D_BUF_BYTES : len;
        int words = (n + 3) / 4;
        for (int i = 0; i < words; i++) {
            uint32_t w = 0;
            for (int b = 0; b < 4; b++) {
                int idx = i * 4 + b;
                if (idx < n) {
                    w |= (uint32_t)data[idx] << (8 * b);
                }
            }
            d_wr(D_SPI_W0 + 4u * (unsigned)i, w);
        }
        /* An exact bit count. Rounding up to whole words pads every command
         * with zero bits the panel reads as argument bytes. */
        d_wr(D_SPI_DLEN, (uint32_t)n * 8u - 1u);
        d_apply();
        /* LovyanGFX writes *_spi_cmd_reg = SPI_EXECUTE outright rather than
         * read-modify-writing it, so no stale bit rides along. */
        d_wr(D_SPI_CMD, D_CMD_USR);
        unsigned spins = 0;
        while (d_rd(D_SPI_CMD) & D_CMD_USR) {
            if (++spins > 500000) {
                break;
            }
        }
        len -= n;
        data += n;
    }
}

static void d_pin(int pin, int lvl)
{
    uint32_t bit = 1u << (pin - 32);
    if (lvl) {
        GPIO.out1_w1ts.val = bit;
    } else {
        GPIO.out1_w1tc.val = bit;
    }
}

/* LovyanGFX's dc_control() waits for USR to clear before touching the DC pin.
 * Flipping DC while a transfer is still in flight changes the meaning of bits
 * the panel has not sampled yet. */
static void d_idle(void)
{
    while (d_rd(D_SPI_CMD) & D_CMD_USR) { }
}

static void d_cmd(uint8_t c)  { d_idle(); d_pin(PIN_LCD_DC, 0); d_xfer(&c, 1); }
static void d_arg(uint8_t v)  { d_idle(); d_pin(PIN_LCD_DC, 1); d_xfer(&v, 1); }

/* Same transfer, but counting clock edges on the SCLK pad while it runs. The
 * counter was validated separately (20/20 hand toggles), so a zero here means
 * the controller is not reaching the pad even with its registers correctly
 * programmed. Run this at a rate the polling loop can resolve: 80 MHz /
 * ((24+1) * (15+1)) = 200 kHz. */
#define D_CLK_200K  ((24u << 0) | (12u << 6) | (24u << 12) | (15u << 18))

static int d_xfer_counted(const uint8_t *data, int len)
{
    int edges = 0;
    int last = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);

    int words = (len + 3) / 4;
    for (int i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            int idx = i * 4 + b;
            if (idx < len) {
                w |= (uint32_t)data[idx] << (8 * b);
            }
        }
        d_wr(D_SPI_W0 + 4u * (unsigned)i, w);
    }
    d_wr(D_SPI_DLEN, (uint32_t)len * 8u - 1u);
    d_apply();
    d_wr(D_SPI_CMD, d_rd(D_SPI_CMD) | D_CMD_USR);

    while (d_rd(D_SPI_CMD) & D_CMD_USR) {
        int now = (int)((GPIO.in1.val >> (PIN_LCD_SCLK - 32)) & 1u);
        if (now != last) { edges++; last = now; }
    }
    return edges;
}

__attribute__((unused)) static void stage_lcd_direct(void)
{
    if (!dev_pca9557) {
        record(3, ST_SKIPPED, "needs the expander");
        return;
    }

    claim_lcd_pads("direct");
    const int pins[3] = { PIN_LCD_SCLK, PIN_LCD_MOSI, PIN_LCD_DC };
    /* Direction FIRST, matrix second. gpio_set_direction() routes the pad to
     * SIG_GPIO_OUT_IDX as part of making it an output, so calling it after the
     * connect silently undoes the connect: the pad is then driven from the GPIO
     * output register rather than by the peripheral, while every controller
     * register still reads back correct and every transfer still completes.
     * That is the whole of the "no clock at the pad" result. */
    for (int i = 0; i < 3; i++) {
        gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_OUTPUT);
        d_pin(pins[i], 0);          /* clock idle low before the mux changes */
    }
    d_pin(PIN_LCD_DC, 1);           /* Bus_SPI::init() drives DC high first */
    esp_rom_gpio_connect_out_signal((uint32_t)PIN_LCD_SCLK, FSPICLK_OUT_IDX,
                                    false, false);
    esp_rom_gpio_connect_out_signal((uint32_t)PIN_LCD_MOSI, FSPID_OUT_IDX,
                                    false, false);
    esp_rom_gpio_connect_out_signal((uint32_t)PIN_LCD_DC, SIG_GPIO_OUT_IDX,
                                    false, false);

    /* func_sel is only part of the output-select entry. oen_sel chooses where
     * the pad's output enable comes from: 0 = the GPIO_ENABLE register, 1 = the
     * signal's own output-enable line. With oen_sel set and that line inactive
     * the pad is tri-stated no matter what GPIO_ENABLE1 says -- which is the
     * exact signature under investigation: registers programmed, transfers
     * completing, zero edges at the pad. */
    for (int i = 0; i < 3; i++) {
        const volatile uint32_t *w =
            (const volatile uint32_t *)(uintptr_t)&GPIO.func_out_sel_cfg[pins[i]];
        ESP_LOGI(TAG, "pad %d out_sel_cfg=0x%08x func=%u inv=%d oen_sel=%d oen_inv=%d",
                 pins[i], *w, (unsigned)(*w & 0x1FFu), (int)((*w >> 9) & 1u),
                 (int)((*w >> 10) & 1u), (int)((*w >> 11) & 1u));
    }
    ESP_LOGI(TAG, "enable1=0x%08x in1=0x%08x", (unsigned)GPIO.enable1.val,
             (unsigned)GPIO.in1.val);

    /* Writes to a peripheral whose module clock is off are dropped without
     * complaint, which is exactly what the register dump after this shows if
     * it is skipped. Use IDF's own enable rather than a hand-computed SYSTEM
     * offset. */
    periph_module_enable(PERIPH_SPI2_MODULE);
    d_wr(D_SPI_GATE, D_GATE_ON);
    ESP_LOGI(TAG, "after enable: gate=0x%08x (want 0x7)", d_rd(D_SPI_GATE));
    d_wr(D_SPI_DMA, 0);
    d_wr(D_SPI_CTRL, 0);
    /* Reference: *SPI_USER = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN. The
     * MISO phase is left enabled even though nothing is wired to it. */
    d_wr(D_SPI_USER, D_USR_MOSI | D_USR_MISO | D_DOUTDIN);
    d_wr(D_SPI_USER1, 0);
    d_wr(D_SPI_USER2, 0);
    d_wr(D_SPI_CLK, D_CLK_BUS);
    d_apply();

    /* Reset, in the reference's order: CS stays de-selected across the pulse.
     * The pin level is read back at every step, including while reset is being
     * commanded low, because "we wrote the output register" has never by itself
     * shown that the line actually fell. */
    expander_probe("before");
    expander_write((1u << EXP_LCD_CS) | (0u << EXP_LCD_RST) |
                   (0u << EXP_AMP_EN) | (0u << EXP_TOUCH_RST));
    expander_probe("reset LOW cmd");
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_probe("after 20ms");
    expander_write((1u << EXP_LCD_CS) | (1u << EXP_LCD_RST) |
                   (0u << EXP_AMP_EN) | (1u << EXP_TOUCH_RST));
    expander_probe("reset HIGH cmd");
    vTaskDelay(pdMS_TO_TICKS(120));
    expander_write((0u << EXP_LCD_CS) | (1u << EXP_LCD_RST) |
                   (0u << EXP_AMP_EN) | (1u << EXP_TOUCH_RST));
    expander_probe("CS asserted");
    vTaskDelay(pdMS_TO_TICKS(5));

    static const uint8_t init_list[] = {
        0xB2, 5, 0x0c, 0x0c, 0x00, 0x33, 0x33,   /* PORCTRL  */
        0xB7, 1, 0x35,                           /* GCTRL    */
        0xBB, 1, 0x28,                           /* VCOMS    */
        0xC0, 1, 0x0C,                           /* LCMCTRL  */
        0xC2, 2, 0x01, 0xFF,                     /* VDVVRHEN */
        0xC3, 1, 0x10,                           /* VRHS     */
        0xC4, 1, 0x20,                           /* VDVSET   */
        0xC6, 1, 0x0f,                           /* FRCTR2   */
        0xD0, 2, 0xa4, 0xa1,                     /* PWCTRL1  */
        0xB0, 2, 0x00, 0xC0,                     /* RAMCTRL  */
        0xE0, 14, 0xd0,0x00,0x02,0x07,0x0a,0x28,0x32,0x44,0x42,0x06,0x0e,0x12,0x14,0x17,
        0xE1, 14, 0xd0,0x00,0x02,0x07,0x0a,0x28,0x31,0x54,0x47,0x0e,0x1c,0x17,0x1b,0x1e,
    };
    for (unsigned i = 0; i < sizeof(init_list); ) {
        uint8_t c = init_list[i];
        int n = init_list[i + 1];
        d_cmd(c);
        if (n > 0) {
            d_pin(PIN_LCD_DC, 1);
            d_xfer(&init_list[i + 2], n);
        }
        i += 2 + (unsigned)n;
    }

    /* Measure this driver's own bit stream before trusting it. */
    {
        static const uint8_t pat[64] = {
            0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
            0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
            0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
            0xFF,0x00,0xF0,0x0F, 0xC3,0x3C,0x80,0x01,
        };
        /* Enable the input path without touching the output routing --
         * gpio_set_direction() would re-route the pad to SIG_GPIO_OUT_IDX and
         * destroy the very thing being counted. */
        gpio_input_enable((gpio_num_t)PIN_LCD_SCLK);
        d_wr(D_SPI_CLK, D_CLK_200K);
        d_apply();
        int e = d_xfer_counted(pat, (int)sizeof(pat));
        d_wr(D_SPI_CLK, D_CLK_BUS);
        d_apply();
        ESP_LOGI(TAG, "direct transfer self-measure: 512 bits -> %d sclk edges "
               "(want ~512)", e);
    }

    /* Reference geometry: lgfx offset_rotation = 3, i.e. the panel is driven at
     * rotation 3, MADCTL MV|MY = 0xA0. X and Y are swapped, so CASET spans the
     * 320 axis and RASET the 240 one -- the opposite of what we had. And
     * cfg.invert = true, so the reference sends INVON (0x21); we never did. */
    /* COLMOD must be stated. The ST7789 powers up at 66h = 18-bit, and the
     * three-equal-thirds pattern with fine banding is the signature of a
     * 2-byte-per-pixel stream being consumed as 3-byte pixels: the byte
     * grouping re-phases every three rows, which is the banding, and the
     * channel assignment changes with the phase, which is why one region reads
     * CYM and the next RGB. */
    d_cmd(0x3A); d_arg(0x55);                    /* COLMOD: 16-bit, 65K */
    /* Measured on the glass: with 0xA0 the quadrants land in the right places
     * and green and white are correct, but red and blue are exchanged. That is
     * MADCTL bit 3 (BGR/RGB) and nothing else, so the panel wants BGR pixel
     * fields -- which is also what the reference asks for via rgb_order =
     * false. */
    d_cmd(0x36); d_arg(0xA8);                    /* MADCTL: rotation 3 + BGR */
    d_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(130)); /* SLPOUT */
    d_cmd(0x21);                                 /* INVON: cfg.invert = true */
    d_cmd(0x2A); d_arg(0x00); d_arg(0x00); d_arg(0x01); d_arg(0x3F);  /* 0..319 */
    d_cmd(0x2B); d_arg(0x00); d_arg(0x00); d_arg(0x00); d_arg(0xEF);  /* 0..239 */
    d_cmd(0x29);                                 /* DISPON */

    /* Four large quadrants, four different colours. A full-screen solid fill
     * cannot distinguish "wrong colour format" from "corrupted stream" -- both
     * look like a flat field -- and it is invariant to geometry, so stripes
     * during a solid fill mean the data arriving is not the data written.
     * Quadrants make both failures readable: if the stream is intact you get
     * four clean blocks; if it is misaligned you get a pattern that marches. */
    static uint8_t row[320 * 2];
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 320; x++) {
            uint16_t c;
            if      (y < 120 && x < 160) c = 0xF800;   /* TL red   */
            else if (y < 120)            c = 0x07E0;   /* TR green */
            else if (x < 160)            c = 0x001F;   /* BL blue  */
            else                         c = 0xFFFF;   /* BR white */
            row[x * 2]     = (uint8_t)(c >> 8);
            row[x * 2 + 1] = (uint8_t)(c & 0xFF);
        }
        if (y == 0) {
            d_cmd(0x2C);
            d_idle();
            d_pin(PIN_LCD_DC, 1);
        }
        for (int off = 0; off < 640; off += D_BUF_BYTES) {
            int n = (640 - off) < D_BUF_BYTES ? (640 - off) : D_BUF_BYTES;
            d_xfer(&row[off], n);
        }
    }
    ESP_LOGI(TAG, "quadrants: expect TL red, TR green, BL blue, BR white");

    ESP_LOGI(TAG, "direct GPSPI2: cmd=0x%08x ctrl=0x%08x clk=0x%08x user=0x%08x "
           "dlen=%u gate=0x%08x", d_rd(D_SPI_CMD), d_rd(D_SPI_CTRL),
           d_rd(D_SPI_CLK), d_rd(D_SPI_USER), d_rd(D_SPI_DLEN), d_rd(D_SPI_GATE));
    pin_state_dump("direct GPSPI2, paints correctly");
    record(3, ST_PASS, "direct GPSPI2; four quadrants on the glass");
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void)
{
    ESP_LOGI(TAG, "MeowKit ESP32-S3 spike, IDF %s", esp_get_idf_version());

    stage_heap();
    stage_i2c_expander();
    stage_backlight();
    /* Aligned with the reference: IDF owns the bus and the pins, transactions
     * are driven by hand. esp_lcd is parked -- it sits on IDF's transaction
     * layer, which the reference deliberately bypasses because it cannot reach
     * the DMA burst-enable bits that corrupt the tail of a large 80 MHz
     * transfer. */
    stage_lcd_direct();
    stage_sd();
    stage_pmu();
    stage_ble();

    summary();

    /* The board's console is USB-Serial-JTAG, which re-enumerates when the
     * chip resets, so a host that attaches after boot has missed everything.
     * Re-announce periodically: a bench probe should be readable whenever you
     * happen to look at it, not only if you were attached at t=0. */
    for (int round = 0; ; round++) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        printf("\n-- re-announce %d --\n", round);
        summary();
        printf("  heap now: internal %u B, psram %u B\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
