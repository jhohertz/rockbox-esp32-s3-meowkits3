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
    static const int pins[] = { PIN_BTN_A, PIN_BTN_B, PIN_JOY_UP,
                                PIN_JOY_DOWN, PIN_JOY_LEFT, PIN_JOY_RIGHT };

    /* GPIO_ENABLE only turns the output driver on or off. The pad still has to be
     * given the GPIO function (GPIO17 and GPIO18 default to DAC outputs) and have
     * its input enable set, otherwise GPIO_IN reads nothing.
     *
     * The lines are active low, so they need a pull-up. ESP32-S3 has pull-ups, but
     * in the IO MUX (FUN_PU, bit 8), not in the GPIO block - a search of
     * gpio_reg.h/gpio_struct.h finds nothing because they are not there.
     */
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        io_mux_select_gpio(pins[i]);
        gpio_input_enable(pins[i], 1);
        gpio_pullup(pins[i], 1);
        gpio_set_direction(pins[i], 0);
    }
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
