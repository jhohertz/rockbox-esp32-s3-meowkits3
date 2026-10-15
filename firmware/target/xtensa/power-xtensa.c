/***************************************************************************
 * MeowKit ESP32-S3 power management.
 *
 * The AXP173 is the power supply, not just a monitor: DCDC1 feeds the SoC and
 * the power button is the chip's PEK input. That means two things Rockbox
 * normally does not have to think about:
 *
 *   - "power off" has to be a command to the PMU, because the SoC cannot cut
 *     its own supply.
 *   - the RTC watchdog the bootloader left running must be fed, otherwise the
 *     chip resets every few hundred milliseconds.
 *
 * Battery level is derived from voltage. The AXP173 also has a coulomb counter,
 * but it accumulates from whatever state it was in at boot and has no reference
 * point, so it reports a delta rather than a state of charge. Voltage is the
 * honest measurement here.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "tick.h"
#include "axp173.h"
#include "i2c.h"
#include "xtensa.h"

/* milliVolt at 0%, 10%, ... 100% */
unsigned short battery_level_disksafe = 3400;
unsigned short battery_level_shutoff  = 3300;

unsigned short percent_to_volt_discharge[11] =
{
    3300, 3450, 3550, 3620, 3680, 3730, 3790, 3860, 3960, 4080, 4200
};

unsigned short percent_to_volt_charge[11] =
{
    3400, 3700, 3800, 3850, 3900, 3950, 4000, 4080, 4150, 4180, 4200
};

/* RTC watchdog registers (RTC_CNTL base + offset). ESP-IDF's rwdt_ll.h uses the
 * legacy WDT register block, not the newer SWD block at 0xB4. */
#define RWDT_CONFIG0   0x98
#define RWDT_FEED      0xAC
#define RWDT_WPROTECT  0xB0
#define RWDT_EN_BIT    (1u << 31)
#define RWDT_FEED_BIT  (1u << 31)
#define RWDT_KEY       0x50D83AA1u       /* RWDT_LL_WDT_WKEY_VALUE: writing this to
                                         * WPROTECT unlocks the WDT registers; writing
                                         * 0 locks them again. */

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

void ICODE_ATTR feed_rwdt(void);

void power_init(void)
{
    /* The bootloader enables the RTC watchdog with a timeout sized for itself.
     * Keeping it and feeding it from the tick is the only freeze protection this
     * target has. */
    if (rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & RWDT_EN_BIT)
        tick_add_task(feed_rwdt);

    /* The PMU is the power supply, so this is not a monitoring call that can be
     * skipped: cold, the chip leaves LDO4 at 1.2 V, and LDO4 feeds the PCA9557
     * that holds the LCD reset and chip select -- below about 2 V the expander
     * does not answer on the bus at all, so every display and touch operation
     * that follows fails in a way that looks like a display problem. DCDC1 feeds
     * the SoC and LDO2/LDO3 the rest of the board, including the card slot.
     *
     * This call was commented out during bare-metal fault isolation and never
     * put back. The trace line below is why nobody noticed: it printed 'W'
     * ("PMU responding") unconditionally, so the boot log kept reporting a
     * healthy PMU while no rail was being configured.
     *
     * i2c_init() first: nothing else on this target brings the I2C master up
     * before the expander does, and pca9557_init() calls it again for bus
     * recovery, which it is written to tolerate.
     */
    i2c_init();
    axp173_init();

}

void power_off(void)
{
    /* Disable the watchdog first: if it is still armed, halting becomes a reset
     * loop instead of a halt. */
    wr(XT_RTCCNTL_BASE + RWDT_WPROTECT, RWDT_KEY);      /* unlock */
    wr(XT_RTCCNTL_BASE + RWDT_CONFIG0,
       rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & ~RWDT_EN_BIT);
    wr(XT_RTCCNTL_BASE + RWDT_WPROTECT, 0);             /* lock */

    /* The PMU drops every rail, including DCDC1, so the SoC loses power too. */
    axp173_power_off();

    /* In case the PMU does not shut down (no battery, or VBUS holding IPSOUT up),
     * do not spin: the watchdog is off, so this is a real halt. */
    while (1)
        asm volatile ("waiti 0");
}

unsigned int power_input_status(void)
{
    return axp173_input_status();
}

#if CONFIG_CHARGING >= CHARGING_MONITOR
bool charging_state(void)
{
    return axp173_charging();
}
#endif

int _battery_voltage(void)
{
    return axp173_battery_voltage();
}

#if CONFIG_BATTERY_MEASURE & CURRENT_MEASURE
int _battery_current(void)
{
    return charging_state() ? axp173_charge_current()
                            : axp173_discharge_current();
}
#endif

/* Called from the tick handler. Only does something if the bootloader armed the
 * watchdog, which by default it did. */
void ICODE_ATTR feed_rwdt(void)
{
    if (rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & RWDT_EN_BIT)
        wr(XT_RTCCNTL_BASE + RWDT_FEED, RWDT_FEED_BIT);
}
