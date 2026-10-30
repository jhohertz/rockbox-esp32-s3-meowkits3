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
#include "paniclog.h"
#include "button.h"
#include "tick.h"     /* current_tick and HZ: the report below is timed in ticks */
#include "gpio-xtensa.h"
#ifdef HAVE_AXP173
#include "axp173.h"
#include "powermgmt.h"
#ifdef HAVE_USBSTACK
#include "usb.h"           /* usb_host_session(): Rockbox declines a poweroff in a session */
#endif
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

    /* GPIO_ENABLE only turns the output driver on or off. The pad still has to
     * be given the GPIO function (GPIO17 and GPIO18 default to DAC outputs) and
     * have its input enable set, otherwise GPIO_IN reads nothing.
     *
     * The lines are active low, so they need a pull-up. ESP32-S3 has pull-ups,
     * but in the IO MUX (FUN_PU, bit 8), not in the GPIO block - a search of
     * gpio_reg.h/gpio_struct.h finds nothing because they are not there.
     */
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        io_mux_select_gpio(pins[i]);
        gpio_input_enable(pins[i], 1);
        gpio_pullup(pins[i], 1);
        gpio_set_direction(pins[i], 0);
    }

#ifdef HAVE_AXP173
    /* Pad 43 is the AXP173 IRQ output. It is configured because the line is
     * cheap to set up and a future board may route PEK events to it, but
     * nothing reads it: see the poll in button_read_device() for why the pin
     * cannot be trusted here.
     */
    io_mux_select_gpio(PIN_PMU_IRQ);
    gpio_input_enable(PIN_PMU_IRQ, 1);
    gpio_pullup(PIN_PMU_IRQ, 1);
    gpio_set_direction(PIN_PMU_IRQ, 0);
#endif

#ifdef HAVE_TOUCHSCREEN
    /* The INT pin is not wired, so there is no interrupt to attach: the panel
     * is read on every button tick instead. ft6x06_irq_handler() is the
     * driver's "a touch happened, read it" entry point, and it queues with
     * I2C_Q_ONCE, so calling it unconditionally cannot pile up transactions.
     */
    ft6x06_init();
    ft6x06_enable(true);

    /* 0xFE is the auto-sleep control. Left at its default the controller drops
     * into idle mode after a few seconds without contact and only comes back
     * when it is touched, which makes the first touch after an idle period
     * invisible.
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

int button_hw_any_pressed(void)
{
    /* Deliberately narrower than button_read_device(): the panic path runs with
     * interrupts masked, so it cannot use the tick, the debounce state or the
     * touchscreen read (which goes through I2C). Any active-low level counts. */
    static const int pins[] = { PIN_BTN_A, PIN_BTN_B, PIN_JOY_UP,
                                PIN_JOY_DOWN, PIN_JOY_LEFT, PIN_JOY_RIGHT };
    int i;

    for (i = 0; i < (int)ARRAYLEN(pins); i++)
        if (!gpio_read_pin(pins[i]))
            return 1;
    return 0;
}

int button_read_device(int *data)
{
    /* This function is the hot path: the common button code calls it every
     * button tick, at HZ. It used to carry the bring-up diagnostics --
     * filesystem-lock state, thread dumps, scheduler state, the playback chain
     * -- on every call. That is well over a kilobyte of console output per
     * tick, roughly a hundred milliseconds of blocking UART at 115200 baud,
     * emitted by the very thread that services the buttons.
     *
     * It is why input felt laggy and erratic and why the player appeared to
     * lock up with no panic and no fault: 98% of the session log was those
     * diagnostics printing themselves. The comment on the block claimed "this
     * path is rare".
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
     * the previous tick has usually finished by now, and if it has not, the
     * state still holds the last completed read -- which is what a polled panel
     * means in practice.
     *
     * This call was `if (0)` for a long time on the stated grounds that
     * button_read_device() runs inside the level-1 handler and a 1.5 ms
     * transfer every tick would burn a third of the CPU in the interrupt. That
     * premise was wrong, and main.c says so: the gptimer alarm advances
     * current_tick only, and the tick tasks -- including button_tick,
     * registered by button_init via tick_add_task -- run in tick_task_thread,
     * ordinary thread context. So there is no ISR cost to pay.
     *
     * There is still a cost, and it is why the call below is throttled: bus
     * load. Removing the wrong reason must not be read as removing a right one
     * that was attached to it -- that is exactly what happened the first time
     * this was un-gated, and the resulting 100 Hz poll was an audible
     * regression on a bus shared with the codec, the PMU and the IO expander.
     * If touch ever stops responding again, check that this call is live before
     * suspecting the panel, the calibration, or the driver.
     */
    /* Throttled to every fifth tick, and the reason is bus load, not the ISR
     * story that used to be written here (that was wrong --
     * button_read_device() runs in tick_task_thread, see apps/main.c). A
     * 14-byte read at 100 kHz is about 1.6 ms, so polling every tick at HZ=100
     * issues 100 transactions a second on a bus shared with the codec, the PMU
     * and the IO expander. Every fifth tick is 20 Hz, far faster than a finger
     * moves.
     *
     * The poll is also only affordable because the transaction it triggers no
     * longer runs under disable_irq_save(): see I2C_ASYNC_SUBMIT_UNMASKED in
     * config/meowkits3.h. With the submit inside the mask, 20 polls a second
     * meant 20 windows of everything-off on the core -- xtensa's
     * disable_irq_save() is PS.INTLEVEL = 15, which blocks the level-2 USB
     * interrupt that feeds the DAC. That cost ~1.8% of audio frames (Filled
     * 18-19 against a gate of 22); with the poll switched off it came to almost
     * nothing, which is how the loss was located. If touch stops responding
     * again, check that this call is live before suspecting the panel, the
     * calibration, or the driver.
     */
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

    /* One-shot crash report for the host.
     *
     * button_read_device() runs in tick_task_thread at HZ -- ordinary thread
     * context, the same place the filesystem's logf() calls happen -- which is
     * what keeps the CDC transport single-producer. The previous version of
     * this report ran from an IDF task and pushed 1.4 KB in a loop; the board
     * reboot-looped.
     *
     * Three seconds is a deliberate wait, not a random one: USB attaches about
     * a second after boot and CDC has to be configured before anything is
     * queued. Sending earlier is harmless -- usb_serial_send() drops the data
     * when the serial function is not active -- but useless, and a useless send
     * reads exactly like a silent console.
     */
    {
        static long report_at;
        static bool reported;

        if (!reported)
        {
            if (report_at == 0)
                report_at = current_tick + HZ * 3;
            else if (current_tick >= report_at)
            {
                reported = true;
                paniclog_crash_report_logf();
            }
        }
    }

#ifdef HAVE_AXP173
    /* The PEK power key is the only way to turn the board off, and the chip
     * latches the press rather than reporting a level: REG 0x46 bit 0 (IRQ23,
     * datasheet IRQ table and REG 46H bit list) sets when the press outlasts
     * the long-press threshold -- 1 s here -- and stays set until written back.
     * The short press is bit 1 of the same register.
     *
     * The IRQ pin gates the read. Per section 9.10 it is an open-drain output
     * that reflects *enabled* pending events, and axp173_init() leaves only
     * IRQ23 (the PEK long press) enabled -- IRQEN0..2 and 4..5 are written 0
     * there, and every status register is cleared -- so a high pin means there
     * is nothing latched to read. The pin is not trusted absolutely: it is
     * pulled high by 51K externally and by the pad's internal pull-up, and if
     * it ever read high while a press *was* latched the key would go dead. So
     * every twentieth pass ignores the pin and reads the latch anyway. The
     * latch survives release, which is what makes that safe: a press missed by
     * the gate is still seen within 2 s, and 2 s is invisible against a
     * threshold measured in seconds.
     *
     * The read costs about 1.6 ms of blocking I2C, so it is taken at most once
     * every 100 ms: this function runs at HZ in the tick thread. That 1.6 ms is
     * not just bus load -- it is subtracted from the window the idle task would
     * otherwise sleep in, and the light-sleep floor is 8 ms of a 10 ms tick
     * period. Gating it is what keeps that margin, which is why it is done
     * before power management is switched on rather than as part of it.
     *
     * sys_poweroff() is the clean path: it queues SYS_POWEROFF, the main thread
     * runs clean_shutdown() (stop the recorder, bookmark, flush settings and
     * the dircache), then shutdown_hw() (close the codec, shut the panel), and
     * only then reaches our power_off(), which writes REG 0x32 bit 7 -- the
     * chip's software shutdown, which drops every output except LDO1, and DCDC1
     * is what feeds this SoC. A hardware PEK shutdown does none of that, so
     * anything that would leave the board to the chip's own auto-off is worth
     * avoiding.
     *
     * The poll stops permanently once it has requested a shutdown. This is a
     * single polled I2C master with no arbitration, and power_off() has to
     * write the PMU's shutdown bit while the main thread halts; a 100 ms reader
     * running concurrently is enough to corrupt that write, which is what let
     * the chip sit on until OFFLEVEL expired.
     *
     * A press during a host session is discarded outright. handle_usb_events()
     * (apps/gui/usb_screen.c) loops until SYS_USB_DISCONNECTED and ignores
     * every other event, so a SYS_POWEROFF posted while the USB screen is up is
     * never dispatched: the shutdown is refused, correctly, because in MSC mode
     * the host owns the card and a forced cut would corrupt the disk. The PMU
     * is made to agree -- the poll disables the PEK auto-off (REG 36H bit 3),
     * clears the latched press and never calls sys_poweroff(), so holding the
     * key during a session does nothing at all, now or later.
     *
     * The predicate is usb_host_session(), not usb_inserted(): the latter is
     * also true for USB_POWERED, where the cable only charges and the ordinary
     * UI is still running. There a shutdown is honoured, the card belongs to
     * nobody, and being able to power off while charging is worth having.
     *
     * The order matters. Clearing the latch is what starts the OFFLEVEL
     * countdown: two builds of this file that differed only in whether they
     * wrote 1 to 0x46 bit 0 showed the one that acked cutting ten seconds after
     * a 2.5 s press, and the one that never acked needing a continuous
     * ten-second hold. Since that countdown cannot be recalled, bit 3 is
     * cleared first so there is no countdown to start. The cost is that a hung
     * application has no hardware kill during a session; VBUS powers the board,
     * the host can unplug it, and the veto is lifted when the session ends. REG
     * 32H bit 7 is a separate mechanism, so the clean shutdown path is
     * unaffected.
     */
    static unsigned int pek_poll_div;
    static unsigned int pek_pin_skips;
    static bool shutdown_requested;
    static long shutdown_request_tick;
    static bool auto_off = true;    /* as programmed by axp173_init() */

    if (shutdown_requested)
    {
        /* clean_shutdown() can refuse: if tagcache_prepare_shutdown() fails
         * while the
           database is committing, apps/misc.c calls cancel_shutdown() and returns false.
           Nothing announces that -- cancel_shutdown() only clears the power thread's
           force-off timer -- so stopping permanently here would leave the key dead until
           reboot. A shutdown that really runs reaches power_off() within a couple of
           seconds; still running half a minute later means it was refused, so arm the key
           again. */
        if (TIME_AFTER(current_tick, shutdown_request_tick + 30 * HZ))
            shutdown_requested = false;
        else
            return b;
    }

    if (++pek_poll_div >= (HZ / 10))
    {
        pek_poll_div = 0;
#ifdef HAVE_USBSTACK
        if (usb_host_session())
        {
            if (auto_off)
            {
                axp173_auto_off_enable(false);
                auto_off = false;
            }
            axp173_power_key_cancel();
            return b;
        }
        if (!auto_off)
        {
            axp173_auto_off_enable(true);
            auto_off = true;
        }
#endif
        /* One GPIO read instead of one I2C transaction, on the 99 passes in 100
         * that
           have nothing latched. See the note above for why the pin may gate the read and
           why it may not be the only witness. */
        if (gpio_read_pin(PIN_PMU_IRQ) && ++pek_pin_skips < 20)
            return b;
        pek_pin_skips = 0;

        if (axp173_power_key_pressed())
        {
            shutdown_requested = true;
            shutdown_request_tick = current_tick;
            sys_poweroff();
        }
    }
#endif

    return b;
}
