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
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

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

static void stage_i2c_expander(void)
{
    i2c_master_bus_config_t buscfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&buscfg, &i2c_bus);
    if (err != ESP_OK) {
        record(1, ST_FAIL, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return;
    }

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

    /* Presence must be tested on REG_CONFIG, not on a status register: the
     * expander powers up with all bits set as inputs. */
    uint8_t cfg = 0;
    err = i2c_reg_read(dev_pca9557, PCA9557_REG_CONFIG, &cfg, 1);
    if (err != ESP_OK) {
        record(1, ST_FAIL, "read CONFIG: %s", esp_err_to_name(err));
        return;
    }

    /* Make LCD reset, LCD CS and the amp enable outputs. */
    uint8_t out = 0;
    i2c_reg_read(dev_pca9557, PCA9557_REG_OUTPUT, &out, 1);
    out |= (1u << EXP_LCD_RST);            /* hold in reset first */
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

    /* Add the PMU device now too; stage 5 uses it. */
    i2c_device_config_t axp = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR_AXP173,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_add_device(i2c_bus, &axp, &dev_axp173);

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

static void stage_lcd(void)
{
    if (!dev_pca9557) {
        record(3, ST_SKIPPED, "needs the expander");
        return;
    }

    /* Reset the panel through the expander: assert reset, wait, release,
     * then wait the ~120 ms the ST7789 needs after release. */
    expander_write((1u << EXP_LCD_RST) | (0u << EXP_LCD_CS));
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_write((0u << EXP_LCD_RST) | (0u << EXP_LCD_CS));
    vTaskDelay(pdMS_TO_TICKS(120));

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
        .trans_queue_depth = 10,
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
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &pcfg, &panel);
    if (err != ESP_OK) {
        record(3, ST_FAIL, "new_panel_st7789: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    /* ST7789 needs RGB inversion. The GRAM is 240x320 and we drive it as
     * 320x240, hence swap_xy plus a y offset of 320-240. */
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, false);
    esp_lcd_panel_set_gap(panel, 0, 80);
    esp_lcd_panel_disp_on_off(panel, true);

    uint16_t *fb = heap_caps_malloc((size_t)LCD_H * LCD_V * 2,
                                    MALLOC_CAP_SPIRAM);
    if (!fb) {
        record(3, ST_FAIL, "framebuffer alloc failed");
        return;
    }

    static const uint16_t bars[] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xF81F, 0x07FF, 0xFB00
    };
    for (int y = 0; y < LCD_V; y++) {
        uint16_t c = bars[(y * 8) / LCD_V];
        for (int x = 0; x < LCD_H; x++) {
            fb[y * LCD_H + x] = c;
        }
    }
    err = esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H, LCD_V, fb);
    free(fb);

    record(3, err == ESP_OK ? ST_PASS : ST_FAIL,
           "%dx%d bars @ %d MHz, CS held via expander (%s)",
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

    record(4, ok ? ST_PASS : ST_FAIL, "%s, %u MB, %s",
           card->cid.name,
           (unsigned)(card->csd.capacity * card->csd.sector_size / 1024 / 1024),
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

void app_main(void)
{
    ESP_LOGI(TAG, "MeowKit ESP32-S3 spike, IDF %s", esp_get_idf_version());

    stage_heap();
    stage_i2c_expander();
    stage_backlight();
    stage_lcd();
    stage_sd();
    stage_pmu();
    stage_ble();

    summary();

    /* Leave the console alive so the panel state can be inspected on the
     * bench and heap fragmentation can be re-read. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
