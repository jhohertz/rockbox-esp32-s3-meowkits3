/***************************************************************************
 * ES8311 codec control over the hardware I2C master.
 *
 * Register values and the clock-divider mapping are taken from the MeowKit
 * reference firmware (src/bsp/audio/ES8311_Class.cpp), which carries the
 * datasheet's per-rate table. The volume range comes from esp_codec_dev's
 * es8311 vol_range: reg 0x00 = -95.5 dB, reg 0xFF = +32.0 dB, i.e. 0.5 dB per
 * step.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "i2c.h"
#include "es8311.h"
#include "i2s-xtensa.h"

#define REG_RESET    0x00
#define REG_CLK01    0x01
#define REG_CLK02    0x02
#define REG_CLK03    0x03
#define REG_CLK04    0x04
#define REG_CLK05    0x05
#define REG_CLK06    0x06
#define REG_CLK07    0x07
#define REG_CLK08    0x08
#define REG_SDPIN    0x09
#define REG_SDPOUT   0x0A
#define REG_SYS0D    0x0D
#define REG_SYS0E    0x0E
#define REG_SYS12    0x12
#define REG_SYS13    0x13
#define REG_SYS14    0x14
#define REG_ADC1C    0x1C
#define REG_DAC31    0x31
#define REG_DAC32    0x32
#define REG_DAC37    0x37

static int write_reg(int reg, int val)
{
    unsigned char buf[2];

    buf[0] = (unsigned char)reg;
    buf[1] = (unsigned char)val;

    return i2c_writemem(ES8311_I2C_ADDR, 0, buf, 2);
}

static int read_reg(int reg)
{
    unsigned char val;

    if (i2c_readmem(ES8311_I2C_ADDR, reg, &val, 1) != 1)
        return -1;

    return val;
}

/* The codec's internal clock is C = MCLK * 2^pre_multi / pre_div, and the bit
 * clock it produces is BCLK = C / (2 * bclk_div). The datasheet table keeps C at
 * 256 * rate for every rate Rockbox supports, so the pair (pre_multi, pre_div) is
 * not a function of the MCLK multiple alone: it has to satisfy that invariant.
 *
 * With MCLK = 256 * rate the answer is pre_div = 1, pre_multi = 0, bclk_div = 4,
 * which is exactly the {11289600, 44100, 1, 0, 1, 1, 0, 0, 0xff, 4, 16, 16} entry
 * in the reference table.
 */
void es8311_set_clock(unsigned long rate, unsigned long mclk)
{
    unsigned long target = 256UL * rate;
    int pre_multi = 0;
    unsigned long pre_div = 1;
    bool found = false;

    for (int m = 0; m <= 3 && !found; m++) {
        unsigned long num = mclk * (1u << m);
        unsigned long d = num / target;

        if (num % target == 0 && d >= 1 && d <= 8) {
            pre_multi = m;
            pre_div = d;
            found = true;
        }
    }

    unsigned long c = mclk * (1u << pre_multi) / pre_div;
    unsigned long bclk = (unsigned long)SLOT_BITS * SLOT_NUM * rate;
    int bclk_div = (int)(c / (2 * bclk));

    if (bclk_div < 1)
        bclk_div = 1;

    write_reg(REG_CLK01, 0x3F);                                   /* enable every clock, MCLK from the pin, not inverted */
    write_reg(REG_CLK02, ((pre_div - 1) << 5) | (pre_multi << 3));
    write_reg(REG_CLK03, 16);                                    /* fs_mode = 0, adc_osr = 16 */
    write_reg(REG_CLK04, 16);                                    /* dac_osr = 16 */
    write_reg(REG_CLK05, 0);                                     /* adc_div = 1, dac_div = 1 */

    /* 0x06 is "sclk inverter, bclk divider" and 0x07 is "tri-state, lrck divider".
     * The reference firmware read-modify-writes both so the bits it does not own
     * keep their reset values.
     */
    int reg06 = read_reg(REG_CLK06);
    if (reg06 >= 0)
        write_reg(REG_CLK06, (reg06 & 0xE0) | (bclk_div - 1));

    int reg07 = read_reg(REG_CLK07);
    if (reg07 >= 0)
        write_reg(REG_CLK07, reg07 & 0xC0);                      /* lrch = 0 */

    write_reg(REG_CLK08, 255);                                   /* lrcl = 255 */
}

void es8311_init(void)
{
    /* Reset to defaults, then power on */
    write_reg(REG_RESET, 0x1F);
    mdelay(20);
    write_reg(REG_RESET, 0x00);
    write_reg(REG_RESET, 0x80);

    /* Slave serial port, I2S format, 16-bit in and out. Bit 7 of REG00 is the
     * power-on command, so read-modify-write to clear only bit 6. */
    int reg00 = read_reg(REG_RESET);
    if (reg00 >= 0)
        write_reg(REG_RESET, reg00 & 0xBF);  /* clear bit 6: slave mode */
    write_reg(REG_SDPIN, 3 << 2);            /* 16-bit */
    write_reg(REG_SDPOUT, 3 << 2);           /* 16-bit */

    /* Power up */
    write_reg(REG_SYS0D, 0x01);              /* analog circuitry */
    write_reg(REG_SYS0E, 0x02);              /* analog PGA, ADC modulator */
    write_reg(REG_SYS12, 0x00);              /* DAC power up */
    write_reg(REG_SYS13, 0x10);              /* output to HP drive */
    write_reg(REG_ADC1C, 0x6A);              /* bypass ADC equalizer */
    write_reg(REG_DAC37, 0x08);              /* bypass DAC equalizer */
}

void es8311_postinit(void)
{
    /* Nothing beyond init on this codec. */
}

void es8311_close(void)
{
    write_reg(REG_SYS13, 0x00);              /* HP drive off */
    write_reg(REG_SYS12, 0x01);              /* DAC power down */
    write_reg(REG_SYS0D, 0x00);              /* analog circuitry off */
}

/* Rockbox always passes centibels. reg = round((db + 95.5) / 0.5). */
void es8311_set_volume(int centibels)
{
    int reg = (centibels + 9550 + 25) / 50;

    if (reg < 0)
        reg = 0;
    if (reg > 255)
        reg = 255;

    write_reg(REG_DAC32, reg);
}

void es8311_mute(int mute)
{
    int reg = read_reg(REG_DAC31);

    if (reg < 0)
        return;

    reg &= 0x9F;
    write_reg(REG_DAC31, mute ? (reg | 0x60) : reg);
}
