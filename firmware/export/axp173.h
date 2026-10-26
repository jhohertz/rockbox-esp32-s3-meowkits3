/***************************************************************************
 * AXP173 power management unit.
 *
 * Register set and ADC scaling are the same as the AXP192 driver in
 * drivers/axp-pmu.c, but that one is written against the asynchronous I2C API.
 * This is the synchronous version, for targets that only have
 * i2c_readmem()/i2c_writemem().
 ****************************************************************************/
#ifndef AXP173_H
#define AXP173_H

#include <stdbool.h>
#include <stdint.h>

#define AXP173_ADDR 0x34

/* Registers */
#define AXP_REG_POWERSTATUS     0x00
#define AXP_REG_CHARGESTATUS    0x01
#define AXP_REG_OUTPUTCTRL      0x12
#define AXP_REG_VOFF            0x31
#define AXP_REG_SHUTDOWN        0x32   /* bit 7 = software shutdown: drops every output
                                         except LDO1. DCDC1 feeds this SoC, so writing
                                         it takes the app with it (datasheet section 9.2,
                                         which expects the write from the PEK handler). */
#define AXP_REG_CHARGECTRL      0x33
#define AXP_REG_TIMING          0x36
#define AXP_REG_ADCENABLE1      0x82
#define AXP_REG_ADCENABLE2      0x83
#define AXP_REG_ADCRATE         0x84
#define AXP_REG_CC_CHARGE       0xb0
#define AXP_REG_CC_DISCHARGE    0xb4
#define AXP_REG_CC_CTRL         0xb8

/* CC_CTRL (0xb8). The counters themselves are two 32-bit big-endian values: charge
   at 0xb0-0xb3, discharge at 0xb4-0xb7. Bit 5 clears both and is self-clearing, so a
   write of it is a pulse rather than a state. Bit 6 selects the calibration base the
   chip integrates against; the value is a scale factor for the raw count and is left at
   the reference firmware's setting rather than guessed at here. */
#define AXP_CC_ENABLE           (1u << 7)
#define AXP_CC_PIECE_BASE       (1u << 6)
#define AXP_CC_CLEAR            (1u << 5)

/* POWERSTATUS (0x00) */
#define AXP_EXTEN_PRESENT   (1u << 7)
#define AXP_ACIN_PRESENT    (1u << 6)
#define AXP_VBUS_PRESENT    (1u << 5)
#define AXP_ACIN_AVAILABLE  (1u << 4)
#define AXP_VBUS_AVAILABLE  (1u << 3)
#define AXP_BATT_PRESENT    (1u << 2)

/* CHARGESTATUS (0x01) */
#define AXP_CHARGING        (1u << 6)
#define AXP_BATT_CONNECTED  (1u << 5)
#define AXP_CHG_CURRENT_LOW (1u << 2)

/* OUTPUTCTRL (0x12) */
#define AXP_OUT_DCDC1 (1u << 0)
#define AXP_OUT_LDO4  (1u << 1)
#define AXP_OUT_LDO2  (1u << 2)
#define AXP_OUT_LDO3  (1u << 3)
#define AXP_OUT_DCDC2 (1u << 4)
#define AXP_OUT_EXTEN (1u << 6)

/* ADCENABLE1 (0x82). The AXP173 has one battery-current ADC, shared by the
 * charge and discharge registers. */
#define AXP_ADC_TS      (1u << 0)
#define AXP_ADC_APS_V   (1u << 1)
#define AXP_ADC_VBUS_C  (1u << 2)
#define AXP_ADC_VBUS_V  (1u << 3)
#define AXP_ADC_ACIN_C  (1u << 4)
#define AXP_ADC_ACIN_V  (1u << 5)
#define AXP_ADC_BATT_C  (1u << 6)
#define AXP_ADC_BATT_V  (1u << 7)
#define AXP_ADC_TEMP    (1u << 7)   /* ADCENABLE2 */

/* IRQ enables live in five registers (0x40-0x43 and 0x4A) and the matching
   status registers are 0x44-0x47 and 0x4D. The PEK key is not in the first pair:
   its bits are in IRQEN3/IRQST3. The datasheet's status table is explicit -
   REG46H[1] is the short press and REG46H[0] the long press - and every status
   bit latches until written back as 1. The IRQ pin is pulled high only when no
   status bit is set at all, so an event nobody acts on holds the line low.

   Note that the reference firmware on this board reads 0x44 for the PEK status.
   That register holds the ACIN/VBUS events; the PEK bits are in 0x46.
*/
#define AXP_REG_IRQEN1      0x40
#define AXP_REG_IRQEN2      0x41
#define AXP_REG_IRQEN3      0x42
#define AXP_REG_IRQEN4      0x43
#define AXP_REG_IRQEN5      0x4A
#define AXP_REG_IRQST1      0x44
#define AXP_REG_IRQST2      0x45
#define AXP_REG_IRQST3      0x46
#define AXP_REG_IRQST4      0x47
#define AXP_REG_IRQST5      0x4D

/* IRQEN3 / IRQST3 -- datasheet REG 42H (default 0x3B) and REG 46H. PEK long press is
   IRQ23 = bit 0 of both, short press is IRQ22 = bit 1. Note that REG 44H bit 0 is
   reserved: the vendor driver for this board reads PEK status there and is wrong. */
#define AXP_IRQ_PEK_LONG    (1u << 0)
#define AXP_IRQ_PEK_SHORT   (1u << 1)

/* REG 36H bit 3: the PEK auto-off function itself. The datasheet calls it
   "按键时长大于关机时长时自动关机功能设置", 0 = off, 1 = on, and it resets to 1.
   Clearing it disables the OFFLEVEL hard cut wholesale; it is a different mechanism from
   the software shutdown in REG 32H bit 7, so axp173_power_off() still works with it off. */
#define AXP_TIMING_AUTOOFF  (1u << 3)
#define AXP_IRQ_LDO4_LOW    (1u << 3)
#define AXP_IRQ_DCDC2_LOW   (1u << 4)
#define AXP_IRQ_DCDC1_LOW   (1u << 5)
#define AXP_IRQ_CHG_UNDER   (1u << 6)
#define AXP_IRQ_OVERTEMP    (1u << 7)

/* TIMING (0x36, default 0x5D): bits 7:6 ONLEVEL the press that powers the chip on,
   bits 5:4 IRQLEVEL the long-press threshold, bit 3 "auto power off when the press
   outlasts OFFLEVEL", bit 2 the PWROK delay, bits 1:0 OFFLEVEL -- the press duration
   beyond which the chip cuts its own outputs (4/6/8/10 s; 3 is the longest, 10 s).
   OFFLEVEL is a duration the user must outlast, not a countdown that survives release. */
#define AXP_PEK_ON_SHIFT    6
#define AXP_PEK_LONG_SHIFT  4
#define AXP_PEK_OFF_SHIFT   0
#define AXP_PEK_AUTOOFF     (1u << 3)
#define AXP_PEK_PWROK       (1u << 2)   /* PWROK pin delay: 0 = 32 ms, 1 = 64 ms */

#define AXP_ADC_RATE_25HZ  0
#define AXP_ADC_RATE_50HZ  1
#define AXP_ADC_RATE_100HZ 2
#define AXP_ADC_RATE_200HZ 3

void axp173_init(void);
bool axp173_present(void);

int axp173_read_reg(unsigned reg);
bool axp173_write_reg(unsigned reg, unsigned char value);
bool axp173_modify_reg(unsigned reg, unsigned char clr, unsigned char set);

/* ADC values, already scaled: volts are millivolts, amps are milliamps. */
int axp173_battery_voltage(void);
int axp173_charge_current(void);
int axp173_discharge_current(void);
int axp173_vbus_voltage(void);
int axp173_vbus_current(void);
int axp173_ts_temperature(void);

/* Coulomb counters. Returned raw: this driver deliberately does not turn them into
   mAh, because the scale depends on the calibration base in CC_CTRL and there is no
   measurement here that pins it down. Consumers that want a percentage need to
   establish the scale against a known capacity first. */
bool axp173_coulomb_enable(bool on);
bool axp173_coulomb_clear(void);
void axp173_coulomb_read(uint32_t *charge, uint32_t *discharge);

bool axp173_charging(void);
bool axp173_battery_connected(void);

/* Bitfield of POWER_INPUT_* from power.h. */
unsigned int axp173_input_status(void);

/* Shut the chip down. Everything loses power, including the SoC. */
void axp173_power_off(void);

/* True if the PEK power key has been long-pressed since the last call. Clears the
   long-press bit it reports, so the same press is not seen twice. The short-press bit is
   not enabled in IRQEN3 by axp173_init() and therefore never latches. */
bool axp173_power_key_pressed(void);

/* Enable or disable the PEK auto-off (OFFLEVEL hard cut) without disturbing the
   other fields of REG 36H. */
bool axp173_auto_off_enable(bool on);

/* Discard any latched PEK press, long or short, without reporting it. Returns
   true if something was cleared. */
bool axp173_power_key_cancel(void);

#endif /* AXP173_H */
