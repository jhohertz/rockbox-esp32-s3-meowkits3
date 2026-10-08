/***************************************************************************
 * MeowKit ESP32-S3 GPIO buttons.
 *
 * Six active-low inputs. Rockbox's common button code does the debouncing and
 * repeat handling, so this file only reports the raw levels.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "button.h"
#include "gpio-xtensa.h"

#define PIN_BTN_A     6
#define PIN_BTN_B     4
#define PIN_JOY_UP    12
#define PIN_JOY_DOWN  18
#define PIN_JOY_LEFT  17
#define PIN_JOY_RIGHT 8

void button_init_device(void)
{
    /* The ESP32-S3 GPIO block has no pull-up or pull-down registers - checked in
     * the SVD, in ESP-IDF's gpio_reg.h/gpio_struct.h and in soc_caps.h (no
     * SOC_GPIO_SUPPORT_PULL_UP). Only the USB pads have pull control. So the
     * pull-up on these six lines has to be external; Rockbox can set direction
     * and nothing else.
     */
    gpio_set_direction(PIN_BTN_A, 0);
    gpio_set_direction(PIN_BTN_B, 0);
    gpio_set_direction(PIN_JOY_UP, 0);
    gpio_set_direction(PIN_JOY_DOWN, 0);
    gpio_set_direction(PIN_JOY_LEFT, 0);
    gpio_set_direction(PIN_JOY_RIGHT, 0);
}

int button_read_device(void)
{
    int b = 0;

    /* Active low: pressed reads 0 */
    if (!gpio_read_pin(PIN_BTN_A))     b |= BUTTON_A;
    if (!gpio_read_pin(PIN_BTN_B))     b |= BUTTON_B;
    if (!gpio_read_pin(PIN_JOY_UP))    b |= BUTTON_UP;
    if (!gpio_read_pin(PIN_JOY_DOWN))  b |= BUTTON_DOWN;
    if (!gpio_read_pin(PIN_JOY_LEFT))  b |= BUTTON_LEFT;
    if (!gpio_read_pin(PIN_JOY_RIGHT)) b |= BUTTON_RIGHT;

    return b;
}
