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
#define AXP_REG_SHUTDOWN        0x32
#define AXP_REG_CHARGECTRL      0x33
#define AXP_REG_TIMING          0x36
#define AXP_REG_IRQENABLE1      0x40
#define AXP_REG_IRQSTATUS1      0x44
#define AXP_REG_ADCENABLE1      0x82
#define AXP_REG_ADCENABLE2      0x83
#define AXP_REG_ADCRATE         0x84
#define AXP_REG_CC_CHARGE       0xb0
#define AXP_REG_CC_DISCHARGE    0xb4
#define AXP_REG_CC_CTRL         0xb8

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

/* IRQENABLE1 (0x40) / IRQSTATUS1 (0x44). The status bits latch until written
   back as 1; they are not a level. */
#define AXP_IRQ_LONG_PRESS  (1u << 0)
#define AXP_IRQ_SHORT_PRESS (1u << 1)
#define AXP_IRQ_ACIN_OFF    (1u << 2)
#define AXP_IRQ_ACIN_ON     (1u << 3)
#define AXP_IRQ_VBUS_OFF    (1u << 4)
#define AXP_IRQ_VBUS_ON     (1u << 5)
#define AXP_IRQ_BATT_LOW    (1u << 6)
#define AXP_IRQ_CHG_STAT    (1u << 7)

/* TIMING (0x36): bits 7:6 power-on press duration, bits 1:0 the hold time
   before the chip cuts its own output. 3 is the longest, 10 s. */
#define AXP_PEK_ON_SHIFT    6
#define AXP_PEK_OFF_SHIFT   0

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

bool axp173_charging(void);
bool axp173_battery_connected(void);

/* Bitfield of POWER_INPUT_* from power.h. */
unsigned int axp173_input_status(void);

/* Shut the chip down. Everything loses power, including the SoC. */
void axp173_power_off(void);

/* True if the PEK power key was pressed since the last call. Consumes the
   latched status bits so the same press is not reported twice. */
bool axp173_power_key_pressed(void);

#endif /* AXP173_H */
