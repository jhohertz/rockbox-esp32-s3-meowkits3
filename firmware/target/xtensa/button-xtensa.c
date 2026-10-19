/***************************************************************************
 * MeowKit ESP32-S3 GPIO buttons.
 *
 * Six active-low GPIO inputs, plus an FT6336 capacitive touch panel on the I2C
 * bus. Rockbox's common button code does the debouncing and repeat handling, so
 * this file only reports the raw levels and the touch state.
 ****************************************************************************/
#include "config.h"
#include "xtensa.h"

#include "system.h"
#include "button.h"
#include "gpio-xtensa.h"
#ifdef HAVE_AXP173
#include "axp173.h"
#include "powermgmt.h"
#endif
#ifdef HAVE_TOUCHSCREEN
#include "i2c-async.h"
#include "ft6x06.h"
#include "touchscreen.h"
#endif

#define PIN_BTN_A     6
#define PIN_BTN_B     4
#define PIN_JOY_UP    12
#define PIN_JOY_DOWN  18
#define PIN_JOY_LEFT  17
#define PIN_JOY_RIGHT 8
#ifdef HAVE_AXP173
/* AXP173 IRQ output. Active low while an enabled IRQ is latched. */
#define PIN_PMU_IRQ   43
#endif

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

#ifdef HAVE_AXP173
    /* Pad 43 defaults to UART0 TX in the IO MUX, so it has to be switched to the
     * GPIO function before it can be read. The IRQ line is active low, so it
     * needs a pull-up.
     */
    io_mux_select_gpio(PIN_PMU_IRQ);
    gpio_input_enable(PIN_PMU_IRQ, 1);
    gpio_pullup(PIN_PMU_IRQ, 1);
    gpio_set_direction(PIN_PMU_IRQ, 0);
#endif

#ifdef HAVE_TOUCHSCREEN
    /* The INT pin is not wired, so there is no interrupt to attach: the panel is
     * read on every button tick instead. ft6x06_irq_handler() is the driver's "a
     * touch happened, read it" entry point, and it queues with I2C_Q_ONCE, so
     * calling it unconditionally cannot pile up transactions.
     */
    ft6x06_init();
    ft6x06_enable(true);

    /* 0xFE is the auto-sleep control. Left at its default the controller drops into
     * idle mode after a few seconds without contact and only comes back when it is
     * touched, which makes the first touch after an idle period invisible.
     */
    i2c_reg_write1(FT6x06_BUS, FT6x06_ADDR, 0xFE, 0x07);
#endif
}

#ifdef HAVE_TOUCHSCREEN
void touchscreen_enable_device(bool en)
{
    ft6x06_enable(en);
}
#endif

int button_read_device(int *data)
{
    /* This function is the hot path: the common button code calls it every button
     * tick, at HZ. Until a moment ago it carried the bring-up diagnostics -- the
     * filesystem-lock state, thread dumps, scheduler state, the playback-chain
     * probe (twice, from a bad merge) and buffering_probe -- on every call. That
     * is well over a kilobyte of console output per tick, roughly a hundred
     * milliseconds of blocking UART at 115200 baud, emitted by the very thread
     * that services the buttons.
     *
     * It is why input felt laggy and erratic and why the player appeared to lock
     * up with no panic and no fault: 98% of the session log was this probe
     * printing itself. The comment on the block claimed "this path is rare".
     *
     * The deadlock it was written to chase is long since found and fixed -- the
     * ES8311 was being addressed with a spurious leading register byte, so the
     * codec had never accepted a command. Nothing here needs to ask the
     * filesystem lock anything any more. */

    int b = 0;

    /* Active low: pressed reads 0 */
    if (!gpio_read_pin(PIN_BTN_A))     b |= BUTTON_A;
    if (!gpio_read_pin(PIN_BTN_B))     b |= BUTTON_B;
    if (!gpio_read_pin(PIN_JOY_UP))    b |= BUTTON_UP;
    if (!gpio_read_pin(PIN_JOY_DOWN))  b |= BUTTON_DOWN;
    if (!gpio_read_pin(PIN_JOY_LEFT))  b |= BUTTON_LEFT;
    if (!gpio_read_pin(PIN_JOY_RIGHT)) b |= BUTTON_RIGHT;

#ifdef HAVE_TOUCHSCREEN
    /* The panel is polled, not interrupt-driven: FT6336's INT pin is not wired
     * on this board. ft6x06_irq_handler() only queues the next read
     * (i2c_async_queue with TIMEOUT_NOBLOCK and I2C_Q_ONCE), so it never blocks
     * here; the 14-byte transfer happens on the I2C controller's own interrupt
     * and its callback fills ft6x06_state whenever it lands. The read queued on
     * the previous tick has usually finished by now, and if it has not, the state
     * still holds the last completed read -- which is what a polled panel means
     * in practice.
     *
     * This call was `if (0)` for a long time on the stated grounds that
     * button_read_device() runs inside the level-1 handler and a 1.5 ms transfer
     * every tick would burn a third of the CPU in the interrupt. That premise was
     * wrong, and main.c says so: the gptimer alarm advances current_tick only,
     * and the tick tasks -- including button_tick, registered by button_init via
     * tick_add_task -- run in tick_task_thread, ordinary thread context. So there
     * is no ISR cost to pay and no reason to throttle. If touch ever stops
     * responding again, check that this call is live before suspecting the panel,
     * the calibration, or the driver.
     */
    /* Throttled to every fifth tick, and the reason is bus load, not the ISR
     * story that used to be written here (that was wrong -- button_read_device()
     * runs in tick_task_thread, see apps/main.c). A 14-byte read at 100 kHz is
     * about 1.1 ms, so polling every tick at HZ=100 keeps the I2C controller busy
     * roughly a tenth of the time and issues 100 transactions a second on a bus
     * shared with the codec, the PMU and the IO expander. Every fifth tick is
     * 20 Hz, which is far faster than a finger moves and cuts that to ~2%. */
    {
        static unsigned poll_div;
        if (++poll_div >= 5)
        {
            poll_div = 0;
            ft6x06_irq_handler();
        }
    }

    if (touchscreen_get_mode() == TOUCHSCREEN_POINT) {
        /* Point mode can only carry one coordinate pair per button event. */
        const struct ft6x06_point *p = &ft6x06_state.points[0];

        if (p->event == FT6x06_EVT_PRESS || p->event == FT6x06_EVT_CONTACT)
            b |= touchscreen_to_pixels(p->pos_x, p->pos_y, data);
    } else {
        for (int i = 0; i < ft6x06_state.nr_points; i++) {
            const struct ft6x06_point *p = &ft6x06_state.points[i];

            if (p->event == FT6x06_EVT_PRESS || p->event == FT6x06_EVT_CONTACT)
                b |= touchscreen_to_pixels(p->pos_x, p->pos_y, NULL);
        }
    }
#endif

#ifdef HAVE_AXP173
    /* The PEK power key is the only way to turn the board off, and the chip
     * latches the press rather than reporting a level. Reading the IRQ pin first
     * keeps this off the I2C bus unless something actually happened.
     *
     * sys_poweroff() is the clean path: it runs the shutdown sequence (flush the
     * audiobuffer, save settings, close the card) and only then reaches
     * power_off(), which commands the PMU. A raw hardware PEK shutdown does none
     * of that.
     */
    if (!gpio_read_pin(PIN_PMU_IRQ) && axp173_power_key_pressed())
        sys_poweroff();
#endif

    return b;
}
