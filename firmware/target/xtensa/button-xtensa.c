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
#include "mrsw_lock.h"     /* TEMPORARY filesystem-lock probe */
#include <stdio.h>         /* TEMPORARY bring-up logging */
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
    /* TEMPORARY: once a second, report who holds the filesystem lock. The play
     * attempt deadlocks with no panic and no output at all, which means the
     * only way to see it is to ask the lock directly while it is wedged. The
     * dircache thread takes the writer lock and then blocks on its queue with
     * TIMEO_BLOCK, and nothing in this tree releases held MRSW locks across a
     * wait -- so an idle dircache thread should show count<0, owner dircache,
     * and a growing waiter list. If instead count is 0 and the waiters are
     * empty, the lock is innocent and the stall is elsewhere. */
    {
        static int fsdiag;
        extern struct mrsw_lock file_internal_mrsw;

        if (++fsdiag >= 1)   /* every call: this path is rare */
        {
            char owner[20];
            int waiters = 0;
            int count;

            fsdiag = 0;
            /* Which threads are awake, and what each is queued on. Printed on
             * every fourth sample: the play stall showed the lock free and the
             * codec thread never running, so the thread states are the evidence,
             * not the lock counters. */
            {
                static char th[400];
                static int thdiv;
                extern int thread_debug_dump(char *out, int sz);
                if (++thdiv >= 10)
                {
                    static char sc[200];
                    static char os[400];
                    extern int scheduler_debug_state(char *out, int sz);
                    extern int rb_thread_os_states(char *out, int sz);

                    thdiv = 0;
                    thread_debug_dump(th, sizeof th);
                    printf("THREADS: %s\n", th);
                    /* The same threads as FreeRTOS sees them. A name that is
                     * unremarkable above and BLOCKED here is inside a driver,
                     * not awaiting its gate -- the distinction every reading
                     * so far has been unable to make. */
                    rb_thread_os_states(os, sizeof os);
                    printf("OS:      %s\n", os);
                    /* The other half of the same question: what the scheduler
                     * believes, as opposed to what each thread is doing. */
                    scheduler_debug_state(sc, sizeof sc);
                    printf("SCHED: %s\n", sc);
                }
            }
            extern volatile int mrsw_null_self;
            extern volatile int mrsw_null_self_read;
            extern volatile int mrsw_recursion_hits;

            count = mrsw_debug_state(&file_internal_mrsw, owner, sizeof owner,
                                     &waiters);
            extern int mrsw_debug_trace(char *out, int sz);
            static char tr[256];

            printf("FSLOCK: count=%d owner=%s waiters=%d "
                   "nullW=%d nullR=%d recur=%d\n",
                   count, owner, waiters,
                   mrsw_null_self, mrsw_null_self_read, mrsw_recursion_hits);
            /* Worth the bytes whenever anything is outstanding. Gating this on
             * count != 0 was a mistake: the state that matters is a FREE lock
             * with permanent waiters, which count alone does not flag. */
            if (count != 0 || waiters != 0)
            {
                extern int mrsw_debug_report(const struct mrsw_lock *,
                                             char *, int);
                static char rep[160];
                mrsw_debug_report(&file_internal_mrsw, rep, sizeof rep);
                printf("  who : %s\n", rep);
                mrsw_debug_trace(tr, sizeof tr);
                printf("  hist: %s\n", tr);
            }
        }
    }

    int b = 0;

    /* Active low: pressed reads 0 */
    if (!gpio_read_pin(PIN_BTN_A))     b |= BUTTON_A;
    if (!gpio_read_pin(PIN_BTN_B))     b |= BUTTON_B;
    if (!gpio_read_pin(PIN_JOY_UP))    b |= BUTTON_UP;
    if (!gpio_read_pin(PIN_JOY_DOWN))  b |= BUTTON_DOWN;
    if (!gpio_read_pin(PIN_JOY_LEFT))  b |= BUTTON_LEFT;
    if (!gpio_read_pin(PIN_JOY_RIGHT)) b |= BUTTON_RIGHT;

#ifdef HAVE_TOUCHSCREEN
    /* The read queued on the previous tick has usually finished by now; the driver's
     * callback has already filled ft6x06_state. If it has not, the state still holds
     * the last completed read, which is what a polled panel means in practice.
     */
    /* The panel is polled, not interrupt-driven, but a 14-byte read at 100 kHz
     * costs about 1.5 ms and this runs inside the level-1 handler. Polling every
     * tick would spend a third of the CPU in the interrupt; every fifth tick is
     * plenty for a finger.
     */
    /* Bring-up: the touch poll is disabled so the tick does not perform I2C.
     * Re-enable once the main-thread stall is resolved.
     */
    if (0)
        ft6x06_irq_handler();

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
