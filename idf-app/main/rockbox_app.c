/*
 * rockbox_app.c -- the seam between ESP-IDF's startup and Rockbox.
 *
 * IDF's entry chain is call_start_cpu0 -> esp_startup_start_app -> app_main.
 * IDF does not define main() at all, so Rockbox's apps/main.c main() does not
 * collide with anything; it is simply never called unless app_main() calls it.
 *
 * Right now it calls the build-integration probe instead of Rockbox's main(), so
 * that the link itself can be validated before the whole of Rockbox is pulled in.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* From Rockbox's archives. */
extern int rockbox_idf_probe(void);

static const char *TAG = "rockbox";

void app_main(void)
{
    int r = rockbox_idf_probe();

    ESP_LOGI(TAG, "rockbox_idf_probe() -> %d", r);
}
