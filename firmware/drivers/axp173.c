/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
/*
 * AXP173 PMU. It is not a battery gauge only: on this board it is the power
 * supply. LDO1 feeds the RTC, DCDC1 feeds the SoC, and the power button is
 * wired to the chip's PEK input, so "power off" is a command to this device
 * rather than something the SoC can do by itself.
 *
 * The ADC registers are pairs: the high byte holds the top 8 bits and the low
 * byte the remainder, so a value is (hi << 4) | (lo & 0x0f) for voltages and
 * (hi << 5) | (lo & 0x1f) for currents.
 */

#include "config.h"
#include "axp173.h"
#include "power.h"
#include "system.h"
#include "i2c.h"
#include <limits.h>

int axp173_read_reg(unsigned reg)
{
    unsigned char val;

    if (i2c_readmem(AXP173_ADDR, reg, &val, 1) != 1)
        return -1;

    return val;
}

bool axp173_write_reg(unsigned reg, unsigned char value)
{
    return i2c_writemem(AXP173_ADDR, reg, &value, 1) == 1;
}

bool axp173_modify_reg(unsigned reg, unsigned char clr, unsigned char set)
{
    int val = axp173_read_reg(reg);

    if (val < 0)
        return false;

    return axp173_write_reg(reg, (unsigned char)((val & ~clr) | set));
}

static int adc_read_pair(unsigned reg, int num, int den)
{
    unsigned char buf[2];

    if (i2c_readmem(AXP173_ADDR, reg, buf, 2) != 2)
        return INT_MIN;

    /* Currents use five low bits, everything else four. */
    int raw = (reg == 0x7a || reg == 0x7c) ? (buf[0] << 5) | (buf[1] & 0x1f)
                                          : (buf[0] << 4) | (buf[1] & 0x0f);

    return raw * num / den;
}

bool axp173_present(void)
{
    /* REG 0x00 (power status) is not usable for detection: on pure battery boot it
     * legitimately reads 0x00. REG 0x12 (output control) has a power-on default of
     * 0x03 and is never 0xFF, so it distinguishes "absent" from "idle".
     */
    int r = axp173_read_reg(AXP_REG_OUTPUTCTRL);

    return r >= 0 && r != 0xff;
}

void axp173_init(void)
{
    if (!axp173_present())
        return;

    /* VBUS-to-IPSOUT path: let VBUS power the load while the battery charges, with
     * the hold voltage capped so plugging in cannot push the rail above the cell.
     * This is the power-on default, written explicitly so the state is known. */
    axp173_write_reg(0x30, 0x60);

    /* Shut down at 2.9 V. The default 3.0 V is close enough to the point where
     * removing VBUS momentarily droops IPSOUT that the chip trips on unplugging. */
    axp173_write_reg(AXP_REG_VOFF, (2900 - 2600) / 100);

    /* Rails are set before they are enabled. DCDC1 and LDO4 are 700-3500 mV in
     * 25 mV steps (0x26 and 0x27); LDO2 and LDO3 share 0x28, 1800-3300 mV in
     * 100 mV steps, LDO2 in the high nibble. DCDC2 is unused on this board.
     */
    axp173_write_reg(0x26, (3300 - 700) / 25);                        /* DCDC1 */
    axp173_write_reg(0x27, (3300 - 700) / 25);                        /* LDO4 */
    axp173_write_reg(0x28, (((3300 - 1800) / 100) << 4) |            /* LDO2 */
                         ((3300 - 1800) / 100));                      /* LDO3 */

    /* One write rather than four read-modify-writes: the reference firmware found
     * that the individual updates can lose bits on a busy bus.
     */
    axp173_write_reg(AXP_REG_OUTPUTCTRL,
                     AXP_OUT_DCDC1 | AXP_OUT_LDO2 | AXP_OUT_LDO3 | AXP_OUT_LDO4);

    /* REG 33H has three fields and the middle one is easy to lose. Bits 6:5 are the
     * charge target voltage (00: 4.10 V, 01: 4.15 V, 10: 4.20 V, 11: 4.36 V) and reset to
     * 10; writing `0x80 | 4` looked like "enable, 450 mA" but drove those two bits to 00,
     * i.e. a 4.10 V ceiling. That is what made an overnight charge end at ~4.0 V under
     * load and the gauge read 83 % -- the chip was never asked to fill the cell. The
     * reference firmware sidesteps it by setting bit 7 and bits 3:0 through two separate
     * read-modify-writes; naming the target explicitly is shorter and shows intent.
     */
    axp173_write_reg(AXP_REG_CHARGECTRL,
                     AXP_CHG_ENABLE | AXP_CHG_TARGET_4V20 | AXP_CHG_CURRENT_450MA);

    /* ADC channels. Battery voltage and current plus chip temperature are what this
     * port reads for power management. VBUS current and the TS (temperature socket)
     * channel are enabled to match the reference firmware, which turns on the same set:
     * VBUS current is the number that tells us whether a 450 mA charge plus the system
     * load is asking a USB 2.0 port for more than its 500 mA budget.
     *
     * TS is the thermistor input. Whether this board actually wires one is unverified --
     * the schematic does not show an NTC, but the datasheet's charge and discharge
     * temperature thresholds (REG 38H/39H and 3CH/3DH) act on that input, so a floating
     * TS pin is not a harmless unread channel: it feeds the chip's charge-suspend and
     * discharge-suspend decisions. Nothing here treats the value as data, and Debug ->
     * View HW info prints the raw reading so the question can be settled on the device
     * instead of by argument.
     */
    axp173_write_reg(AXP_REG_ADCENABLE1,
                     AXP_ADC_BATT_V | AXP_ADC_BATT_C | AXP_ADC_VBUS_V |
                     AXP_ADC_VBUS_C | AXP_ADC_TS);
    axp173_write_reg(AXP_REG_ADCENABLE2, AXP_ADC_TEMP);

    /* REG 84H is not just the sample rate. Bits 7:6 are the rate, but bits 5:4 set the
     * TS pin's output current (00: 20 uA, 01: 40, 10: 60, 11: 80), bit 2 selects between
     * thermistor and independent ADC input, and bits 1:0 say when that current flows
     * (00: never, 01: while charging, 10: during sampling, 11: always). Writing the bare
     * rate -- 0 -- therefore asked for 25 Hz and also switched the TS drive off and its
     * current down to 20 uA, which is the datasheet default of 0x32 restated as 0x00.
     * Write the default explicitly: 25 Hz, 80 uA, thermistor mode, drive during sampling.
     * The reference firmware drives TS at 80 uA too.
     */
    axp173_write_reg(AXP_REG_ADCRATE, (AXP_ADC_RATE_25HZ << 6) | (3u << 4) | 2u);

    /* Charge integration: enable counting, then clear both counters once so the first
     * reading is relative to this boot rather than to whatever the previous firmware
     * accumulated.
     *
     * Both writes have to preserve the enable bit. Bit 6 is a pause, not a calibration
     * base -- the earlier name for it was an assumption, and the pulse on bit 5 that
     * clears the counters, written as a whole-register value of 0x20, also cleared bit 7
     * and switched the coulometer off for the rest of the boot. That is the whole reason
     * CC read 0/0 no matter what the device had been doing.
     */
    axp173_coulomb_enable(true);
    axp173_coulomb_clear();

    /* PEK timings. REG 36H resets to 0x5D: ONLEVEL 512 ms, IRQLEVEL 1.5 s, auto-off on,
     * PWROK 64 ms, OFFLEVEL 6 s. We ask for a 1 s press to power the chip on and a 1 s
     * long-press threshold, so the IRQ23 latch arrives as early as this chip allows, and
     * OFFLEVEL at its longest, 10 s, while the software path is being proven.
     *
     * Bit 3 must stay set: it is what makes the chip cut its outputs when the press
     * outlasts OFFLEVEL. The reset value has it set, and a bare write of only the on/off
     * fields would clear it, leaving no hardware safety net for a hung application.
     *
     * OFFLEVEL is a press-duration limit, not a countdown that survives a release: the
     * datasheet's auto-off condition is "PEK longer than OFFLEVEL", and entering shutdown
     * turns off every output except LDO1. So this 10 s is precisely what forces the user
     * to keep holding, and it is our choice -- the chip resets to 6 s and the vendor uses
     * 4 s.
     */
    axp173_write_reg(AXP_REG_TIMING, (2u << AXP_PEK_ON_SHIFT) |
                                     AXP_PEK_AUTOOFF |
                                     AXP_PEK_PWROK |
                                     (3u << AXP_PEK_OFF_SHIFT));

    /* Only the PEK long press is worth signalling. REG 42H resets to 0x3B, with the short
     * press and several rail-monitoring IRQs enabled, and the IRQ pin reflects every
     * *enabled* pending event -- so leaving those enabled holds the pin low for events
     * nobody acts on. The status bits latch independently of the masks, which is why
     * button_read_device() polls the latch instead of the pin; the pin's high level comes
     * from an external 51K pull-up per section 9.10 and we do not depend on it.
     * Enabling IRQ23 anyway keeps the pin meaningful for a board that wants it.
     */
    axp173_write_reg(AXP_REG_IRQEN1, 0);
    axp173_write_reg(AXP_REG_IRQEN2, 0);
    axp173_write_reg(AXP_REG_IRQEN3, AXP_IRQ_PEK_LONG);
    axp173_write_reg(AXP_REG_IRQEN4, 0);
    axp173_write_reg(AXP_REG_IRQEN5, 0);

    /* A press, or an event, before Rockbox started must not shut the player down
     * on its first button poll. */
    axp173_write_reg(AXP_REG_IRQST1, 0xff);
    axp173_write_reg(AXP_REG_IRQST2, 0xff);
    axp173_write_reg(AXP_REG_IRQST3, 0xff);
    axp173_write_reg(AXP_REG_IRQST4, 0xff);
    axp173_write_reg(AXP_REG_IRQST5, 0xff);
}

int axp173_battery_voltage(void)
{
    return adc_read_pair(0x78, 11, 10);   /* 1.1 mV per LSB */
}

int axp173_charge_current(void)
{
    return adc_read_pair(0x7a, 1, 2);     /* 0.5 mA per LSB */
}

int axp173_discharge_current(void)
{
    return adc_read_pair(0x7c, 1, 2);
}

int axp173_vbus_voltage(void)
{
    return adc_read_pair(0x5a, 17, 10);   /* 1.7 mV per LSB */
}

int axp173_vbus_current(void)
{
    return adc_read_pair(0x5c, 3, 8);     /* 0.375 mA per LSB */
}

int axp173_ts_temperature(void)
{
    return adc_read_pair(0x62, 4, 5);     /* TS input, 0.8 degrees per LSB */
}

bool axp173_coulomb_enable(bool on)
{
    return axp173_modify_reg(AXP_REG_CC_CTRL, AXP_CC_ENABLE,
                             on ? AXP_CC_ENABLE : 0);
}

bool axp173_coulomb_clear(void)
{
    int r = axp173_read_reg(AXP_REG_CC_CTRL);

    if (r < 0)
        return false;

    /* Keep bit 7. Writing AXP_CC_CLEAR alone is a whole-register write and drops the
     * enable, which stops counting from that moment onward. */
    if (!axp173_write_reg(AXP_REG_CC_CTRL,
                          (unsigned char)((r & AXP_CC_ENABLE) | AXP_CC_CLEAR)))
        return false;

    /* Bit 5 is self-clearing, and bit 7 must still be set: a clear that turned the
     * counter off is worse than no clear at all. */
    r = axp173_read_reg(AXP_REG_CC_CTRL);
    return r >= 0 && (r & AXP_CC_CLEAR) == 0 && (r & AXP_CC_ENABLE) != 0;
}

int axp173_coulomb_control(void)
{
    return axp173_read_reg(AXP_REG_CC_CTRL);
}

/* The coulomb count is a sum of ADC samples, so converting it needs the sample rate.
 * REG 84H bits 7:6 select 25/50/100/200 Hz; read it rather than assume axp173_init()'s
 * choice, because a different rate changes the scale by the same factor. */
static int coulomb_adc_rate_hz(void)
{
    int r = axp173_read_reg(AXP_REG_ADCRATE);

    if (r < 0)
        return 25;

    switch ((r >> 6) & 3)
    {
        case 0:  return 25;
        case 1:  return 50;
        case 2:  return 100;
        default: return 200;
    }
}

static int coulomb_counts_to_mAh(uint32_t counts)
{
    /* Each ADC sample advances the counter by one LSB per 0.5 mA of current, so
     * 1 LSB = 0.5 mA / rate_hz = 0.02 mA.s at 25 Hz, i.e. **180,000 LSB per mAh**
     * (0.00556 mAh per LSB). A dark-idle hour at 44 mA is ~7.9e6 counts; the overnight
     * run's 425.1 mAh would be ~7.7e7, and five minutes of a 450 mA charge ~6.8e6.
     *
     * The reference library multiplies by 65536 at this point
     * (`65536 * current_LSB * counts / 3600 / ADC_rate`), which is off by 65536^2 in the
     * wrong direction: it makes 1 LSB 0.364 mAh, so a 450 mA charge would take ~2.5 hours
     * to reach a million counts. On hardware a few minutes of charging already shows six
     * and seven figures -- that reading is what rules the constant out. Nothing in the
     * stock firmware ever calls those conversions (`setCoulometer(COULOMETER_ENABLE,
     * true)` is enabled in the board setup and the getters have no callers anywhere), so
     * the formula was never exercised there either.
     *
     * Still derived rather than measured. The debug row prints the raw counts beside the
     * estimate so a paired reading can confirm 180,000 LSB/mAh against a known current.
     */
    int rate = coulomb_adc_rate_hz();

    /* mAh = counts * 0.5 / (rate * 3600), rounded: denominator is 2 * rate * 3600,
     * so half of it is rate * 3600. */
    return (int)((uint64_t)counts + (uint64_t)rate * 3600) /
           ((uint64_t)2 * rate * 3600);
}

int axp173_coulomb_charge_mAh(void)
{
    uint32_t charge, discharge;

    axp173_coulomb_read(&charge, &discharge);
    return coulomb_counts_to_mAh(charge);
}

int axp173_coulomb_discharge_mAh(void)
{
    uint32_t charge, discharge;

    axp173_coulomb_read(&charge, &discharge);
    return coulomb_counts_to_mAh(discharge);
}

int axp173_charge_target_mV(void)
{
    int r = axp173_read_reg(AXP_REG_CHARGECTRL);

    if (r < 0)
        return -1;

    switch ((r >> AXP_CHG_TARGET_SHIFT) & 3)
    {
        case 0:  return 4100;
        case 1:  return 4150;
        case 2:  return 4200;
        default: return 4360;
    }
}

int axp173_charge_current_setting_mA(void)
{
    static const short mA[16] = { 100, 190, 280, 360, 450, 550, 630, 700,
                                  780, 880, 960, 1000, 1080, 1160, 1240, 1320 };
    int r = axp173_read_reg(AXP_REG_CHARGECTRL);

    if (r < 0)
        return -1;

    return mA[r & 0x0f];
}

bool axp173_current_is_charging(void)
{
    int r = axp173_read_reg(AXP_REG_POWERSTATUS);

    if (r < 0)
        return axp173_charging();

    return (r & AXP_BATT_CHARGING_DIR) != 0;
}

bool axp173_over_temp(void)
{
    int r = axp173_read_reg(AXP_REG_CHARGESTATUS);

    return r >= 0 && (r & AXP_OVER_TEMP) != 0;
}

void axp173_coulomb_read(uint32_t *charge, uint32_t *discharge)
{
    unsigned char buf[8];

    if (i2c_readmem(AXP173_ADDR, AXP_REG_CC_CHARGE, buf, sizeof buf) != sizeof buf) {
        if (charge)
            *charge = 0;
        if (discharge)
            *discharge = 0;
        return;
    }

    /* Both counters are big-endian 32-bit: charge at 0xb0-0xb3, discharge at
     * 0xb4-0xb7, which is one contiguous 8-byte block. */
    if (charge)
        *charge = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                  ((uint32_t)buf[2] << 8) | buf[3];
    if (discharge)
        *discharge = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] << 8) | buf[7];
}

bool axp173_charging(void)
{
    int r = axp173_read_reg(AXP_REG_CHARGESTATUS);

    return r >= 0 && (r & AXP_CHARGING) != 0;
}

bool axp173_battery_connected(void)
{
    int r = axp173_read_reg(AXP_REG_CHARGESTATUS);

    return r >= 0 && (r & AXP_BATT_CONNECTED) != 0;
}

unsigned int axp173_input_status(void)
{
    int r = axp173_read_reg(AXP_REG_POWERSTATUS);

    if (r < 0)
        return POWER_INPUT_NONE;

    unsigned int status = 0;

    /* USB-C is the only input on this board; ACIN is not wired. */
    if (r & AXP_VBUS_PRESENT)
        status |= POWER_INPUT_USB | POWER_INPUT_USB_CHARGER;

    if (r & AXP_ACIN_PRESENT)
        status |= POWER_INPUT_MAIN | POWER_INPUT_MAIN_CHARGER;

    return status;
}

bool axp173_power_key_pressed(void)
{
    /* PEK status is IRQ STATUS3, REG 0x46: bit 0 is the long press (IRQ23), bit 1 the
     * short press (IRQ22). That is the datasheet's own IRQ table and its REG 46H bit
     * list. REG 44H bit 0 -- which the vendor driver for this board reads -- is marked
     * reserved, so that driver's PEK predicates never see anything; they have no callers
     * in the stock app either, which is why the mistake survives in the field.
     * Only the long press is enabled in IRQEN3, so only bit 0 can latch; writing 1 to a
     * status bit clears it, per the note under REG 46H. */
    int st = axp173_read_reg(AXP_REG_IRQST3);

    if (st < 0)
        return false;

    if ((unsigned int)st & AXP_IRQ_PEK_LONG) {
        /* Status bits are write-1-to-clear. */
        axp173_write_reg(AXP_REG_IRQST3, AXP_IRQ_PEK_LONG);
        return true;
    }

    return false;
}

bool axp173_auto_off_enable(bool on)
{
    /* Only bit 3 of REG 36H; the on/off levels, PWROK delay and OFFLEVEL selection are
       left exactly as axp173_init() programmed them. */
    return axp173_modify_reg(AXP_REG_TIMING,
                             on ? 0 : AXP_TIMING_AUTOOFF,
                             on ? AXP_TIMING_AUTOOFF : 0);
}

bool axp173_power_key_cancel(void)
{
    int st = axp173_read_reg(AXP_REG_IRQST3);

    if (st < 0)
        return false;

    unsigned int bits = (unsigned int)st & (AXP_IRQ_PEK_LONG | AXP_IRQ_PEK_SHORT);

    if (!bits)
        return false;

    /* Write-1-to-clear, so this writes back only the bits that were set. Only the long
       press can latch today -- axp173_init() enables just AXP_IRQ_PEK_LONG in IRQEN3 --
       but both PEK bits are cleared so that a widened IRQ mask elsewhere cannot leave a
       press pending that nothing acts on. */
    axp173_write_reg(AXP_REG_IRQST3, (unsigned char)bits);
    return true;
}

void axp173_power_off(void)
{
    axp173_modify_reg(AXP_REG_SHUTDOWN, 0, 1 << 7);
}
