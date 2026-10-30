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
/* The one line in power_off() that reports what the PMU's shutdown register
 * ended on is worth having permanently; autoconf.h leaves ROCKBOX_HAS_LOGF
 * undefined and logf.h makes logf() a no-op in any file that does not ask
 * for it. */
#define LOGF_ENABLE

#include "config.h"

#include "system.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "tick.h"
#include "logf.h"
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
    /* The bootloader enables the RTC watchdog with a timeout sized for
     * itself. Keeping it and feeding it from the tick is the only freeze
     * protection this target has. */
    if (rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & RWDT_EN_BIT)
        tick_add_task(feed_rwdt);

    /* The PMU is the power supply, so this is not a monitoring call that can be
     * skipped: cold, the chip leaves LDO4 at 1.2 V, and LDO4 feeds the PCA9557
     * that holds the LCD reset and chip select -- below about 2 V the expander
     * does not answer on the bus at all, so every display and touch operation
     * that follows fails in a way that looks like a display problem. DCDC1
     * feeds the SoC and LDO2/LDO3 the rest of the board, including the card
     * slot.
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

    /* REG 0x32 bit 7 is the chip's software shutdown: it drops every output
     * except LDO1, and DCDC1 is what feeds this SoC. axp173_power_off() is a
     * read-modify-write that gives up silently if its read fails, and our I2C
     * path returns an error rather than waiting when the queue is busy -- so
     * retry, and read the register back to see whether the bit actually took.
     * Without this the board sat in the halt below until the chip's own
     * OFFLEVEL counter expired, which is the ten-second delay this replaces.
     */
    int tries;
    for (tries = 0; tries < 5; tries++)
    {
        axp173_power_off();

        int r = axp173_read_reg(AXP_REG_SHUTDOWN);

        if (r < 0 || (r & (1 << 7)))
            break;   /* written, or the chip is gone -- either way the rails are coming down */
        sleep(HZ / 100);
    }

    logf("power_off: REG32=%d after %d attempt(s), halting",
         axp173_read_reg(AXP_REG_SHUTDOWN), tries + 1);

    /* In case the PMU does not shut down (no battery, or VBUS holding IPSOUT
     * up), do not spin: the watchdog is off, so this is a real halt. */
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
    /* The chip's current-direction bit rather than charging_state(): REG 01H
     * bit 6 reads 0 both when the charger is idle and when the cell is
     * finished, so keying the register choice off it can sample the
     * discharge register while current is flowing the other way, and report
     * a negative-looking charge as a discharge. */
    return axp173_current_is_charging() ? axp173_charge_current()
                                        : axp173_discharge_current();
}
#endif

#if CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE
/* Battery percentage from the PMU's coulomb counters, replacing the
 * voltage-table mapping.
 *
 * The counters are cleared by axp173_init() at boot, so they measure this
 * session only and cannot know what the cell held when the player started. That
 * is what the voltage tables are still for: the first reading anchors the
 * coulometer to the voltage-derived percent, and every reading after it moves
 * from that anchor by the charge actually transferred. The anchor is a guess
 * with a short life; the deltas are measurements. The reason to bother is
 * linearity -- on the 2026-10-28 run, 81 mAh left the cell while the voltage
 * gauge moved 93% to 88%, because the top of a Li-ion curve is stretched:
 * roughly 16 mAh per gauge percent there, against the ~14 mAh a 1400 mAh cell
 * implies. Charge and discharge are separate cumulative registers, so charging
 * is counted rather than guessed, and the capacity used here is
 * `get_battery_capacity()`, the same number battery_time() divides by, so the
 * bar and the estimate cannot drift apart.
 *
 * Returning -1 is deliberate and safe: powermgmt.c falls back to
 * voltage_to_battery_level() whenever the target has nothing to report
 * (powermgmt.c:388-392), and the shutoff and disksafe thresholds stay
 * voltage-based either way (powermgmt.c:503, :537), so a failed read costs
 * accuracy, not protection.
 */
#define CC_UPDATE_TICKS      (5 * HZ)   /* the counters integrate over minutes; polling
                                        * them twice a second only burns the shared bus */
#define CC_TAPER_FLOOR_MA    10

static int cc_percent = -1;
static int cc_anchor_percent;
static int cc_anchor_net;
static uint32_t cc_anchor_dis, cc_anchor_chg;
static long cc_last_update;

/* Voltage to percent over this target's own tables. The core's equivalent is
 * static in powermgmt.c, so the rule is repeated here rather than exported:
 * charging uses the charge table and never reads full until charging
 * finishes (powermgmt.c:356-372). Used only as the anchor, so its inaccuracy
 * is bounded by the first update. */
static int volt_percent(void)
{
    int mv = axp173_battery_voltage();
    const unsigned short *table;
    int i;

    if (mv < 0)
        return -1;

    table = axp173_charging() ? percent_to_volt_charge
                              : percent_to_volt_discharge;
    if (mv <= table[0])
        return 0;
    if (mv >= table[10])
        return axp173_charging() ? 99 : 100;

    for (i = 0; i < 10 && table[i + 1] < mv; i++)
        ;

    return i * 10 + (mv - table[i]) * 10 / (table[i + 1] - table[i]);
}

int _battery_level(void)
{
    int chg, dis, capacity, net, percent, taper_ma;

    if (cc_percent >= 0 && current_tick - cc_last_update < CC_UPDATE_TICKS)
        return cc_percent;

    if (!axp173_coulomb_mAh(&chg, &dis))
        return -1;

    capacity = get_battery_capacity();
    if (capacity <= 0)
        return -1;

    net = dis - chg;

    /* First reading, or the counters went backwards. Only a clear or a
     * corrupted read can do that, and both make the anchor meaningless, so
     * take a new one. */
    if (cc_percent < 0 || dis < cc_anchor_dis || chg < cc_anchor_chg) {
        cc_anchor_percent = volt_percent();
        if (cc_anchor_percent < 0)
            return -1;
        cc_anchor_dis  = dis;
        cc_anchor_chg  = chg;
        cc_anchor_net  = net;
    }

    percent = cc_anchor_percent - (100 * (net - cc_anchor_net)) / capacity;

    /* "Full", as far as this chip can say so. It has no charge-done flag --
     * REG 01H bit 6 reads 0 both when the charger is idle and when it has
     * finished -- so the taper is the only signal: the direction bit still
     * says charging while the charge current has fallen to the termination
     * threshold, which for our init is 10% of the programmed current (REG
     * 33H bit 4 left clear, so 45 mA at 450 mA). Not yet confirmed on
     * hardware; see PENDING.md. */
    if (axp173_current_is_charging()) {
        taper_ma = axp173_charge_current_setting_mA() / 10;
        if (taper_ma < CC_TAPER_FLOOR_MA)
            taper_ma = CC_TAPER_FLOOR_MA;

        if (axp173_charge_current() <= taper_ma) {
            cc_anchor_percent = 100;
            cc_anchor_dis  = dis;
            cc_anchor_chg  = chg;
            cc_anchor_net  = net;
            percent = 100;
        }
        if (percent > 99)
            percent = 99;
    }

    if (percent < 0)
        percent = 0;
    else if (percent > 100)
        percent = 100;

    cc_percent = percent;
    cc_last_update = current_tick;
    return percent;
}
#endif /* CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE */

#ifdef BOOTLOADER
/* powermgmt.c only supplies its own stub when PERCENTAGE_MEASURE is unset, so a
 * bootloader build that compiles both files still needs a definition here. */
#if CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE
int _battery_level(void) { return -1; }
#endif
#endif

/* Called from the tick handler. Only does something if the bootloader armed the
 * watchdog, which by default it did. */
void ICODE_ATTR feed_rwdt(void)
{
    if (rd(XT_RTCCNTL_BASE + RWDT_CONFIG0) & RWDT_EN_BIT)
        wr(XT_RTCCNTL_BASE + RWDT_FEED, RWDT_FEED_BIT);
}
