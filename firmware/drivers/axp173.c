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

    axp173_write_reg(AXP_REG_CHARGECTRL, 0x80 | 4);  /* enable, 450 mA */

    /* Only the ADCs we actually read. Sampling more costs I2C bandwidth for
     * nothing. */
    axp173_write_reg(AXP_REG_ADCENABLE1,
                     AXP_ADC_BATT_V | AXP_ADC_BATT_C | AXP_ADC_VBUS_V);
    axp173_write_reg(AXP_REG_ADCENABLE2, AXP_ADC_TEMP);
    axp173_write_reg(AXP_REG_ADCRATE, AXP_ADC_RATE_25HZ);
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

void axp173_power_off(void)
{
    axp173_modify_reg(AXP_REG_SHUTDOWN, 0, 1 << 7);
}
