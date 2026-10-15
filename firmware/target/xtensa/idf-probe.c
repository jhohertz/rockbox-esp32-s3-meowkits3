/*
 * idf-probe.c -- build-integration probe, not a driver.
 *
 * Its only job is to answer, in one translation unit, whether Rockbox's config
 * and ESP-IDF's config can coexist: Rockbox's system.h (and therefore config.h
 * and autoconf.h) and IDF's headers, compiled by Rockbox's build with IDF's own
 * flags. Every real driver on this target will include both, so this has to be
 * settled before writing any of them.
 *
 * The specific things being tested:
 *  - both sides define CONFIG_* macros; neither may disturb the other
 *  - Rockbox's bool/NULL/MIN/MAX must not break IDF's headers, and vice versa
 *  - IDF's types (esp_err_t, TickType_t, gpio_num_t) must be usable
 *  - the call must survive into an IDF symbol, i.e. the ABI matches
 *
 * Delete once the drivers are ported.
 */

#include "system.h"
#include "config.h"
#include "tick.h"     /* HZ */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"

/* Rockbox's own config must still be readable in the same TU. */
_Static_assert(LCD_WIDTH == 320, "Rockbox config lost after IDF headers");
_Static_assert(LCD_HEIGHT == 240, "Rockbox config lost after IDF headers");

/* IDF's config must be readable too, and its own derived constants intact. */
_Static_assert(SOC_GPIO_PIN_COUNT == 49, "IDF config lost");

/* Rockbox's MIN/MAX and IDF's headers in one TU. */
static int probe_minmax(void)
{
    return MIN(3, 5) + MAX(3, 5);
}

/*
 * Return a value only IDF can produce, so the link proves the call actually
 * reaches IDF's code rather than being folded away.
 */
int rockbox_idf_probe(void)
{
    esp_err_t err = esp_timer_init();
    int64_t now = esp_timer_get_time();
    TickType_t ticks = xTaskGetTickCount();
    int sum = 0;

    ESP_LOGI("rbprobe", "esp_timer_init: %s, tick=%u, Rockbox HZ=%d",
             esp_err_to_name(err), (unsigned)ticks, HZ);

    /* A GPIO call through IDF's driver layer, to prove the driver archives are
     * reachable and the ABI matches, not just the header layer. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 39,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io);
    sum += (err == ESP_OK) ? 1 : 0;
    sum += (now > 0) ? 1 : 0;
    sum += probe_minmax() > 0 ? 1 : 0;

    return sum;
}
