/***************************************************************************
 * ES7210 ADC driver, register-level, over the target's synchronous I2C API.
 *
 * Register numbering follows the ES7210 datasheet: REG00 is the reset register,
 * REG02 the main clock divider, REG04/05 the LRCK divider, REG11 the SDP
 * interface format, REG43..REG46 the per-microphone gain.
 *
 * The clock setup is derived rather than tabulated. The I2S transmitter drives
 * MCLK at 256 * rate (see i2s-xtensa.h), and the ADC's internal clock is
 *
 *     adc_clk = MCLK * 2^doubler / adc_div
 *
 * which the chip wants to see as 512 * rate below 64 kHz and 256 * rate above it
 * (double-speed mode). Checking that against the coefficient table the reference
 * firmware ships:
 *
 *   44100 Hz  MCLK 11289600 div 1 doubler 1 -> 22579200 = 512 * 44100
 *   16000 Hz  MCLK  4096000 div 1 doubler 1 ->  8192000 = 512 * 16000
 *   8000 Hz   MCLK 12288000 div 3 doubler 0 ->  4096000 = 512 *  8000
 *   88200 Hz  MCLK 11289600 div 1 doubler 1 -> 22579200 = 256 * 88200
 *
 * so with MCLK = 256 * rate the divider is 1 and the doubler is on only in
 * single-speed mode. LRCK_DIV is MCLK / rate, i.e. 256 for every rate we can
 * generate.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "i2c.h"
#include "settings.h"

#include "es7210.h"

#include <stdio.h>   /* es7210_stat(), for the debug screen */

#define REG_RESET          0x00
#define REG_CLOCK_OFF      0x01
#define REG_MAINCLK        0x02
#define REG_LRCK_DIVH      0x04
#define REG_LRCK_DIVL      0x05
#define REG_POWER_DOWN     0x06
#define REG_OSR            0x07
#define REG_MODE_CONFIG    0x08
#define REG_TIME_CTRL0     0x09
#define REG_TIME_CTRL1     0x0A
#define REG_SDP_IF1        0x11
#define REG_SDP_IF2        0x12
#define REG_ADC_AUTOMUTE   0x13
#define REG_ALC_SELECT     0x16
#define REG_ADC2_MAXGAIN   0x1D
#define REG_ADC1_MAXGAIN   0x1E
#define REG_ADC34_HPF2     0x20
#define REG_ADC34_HPF1     0x21
#define REG_ADC12_HPF1     0x22
#define REG_ADC12_HPF2     0x23
#define REG_ANALOG         0x40
#define REG_MIC12_BIAS     0x41
#define REG_MIC34_BIAS     0x42
#define REG_MIC1_GAIN      0x43
#define REG_MIC2_GAIN      0x44
#define REG_MIC1_POWER     0x47
#define REG_MIC2_POWER     0x48
#define REG_MIC3_POWER     0x49
#define REG_MIC4_POWER     0x4A
#define REG_MIC12_POWER    0x4B
#define REG_MIC34_POWER    0x4C

#define MODE_MASTER        (1u << 0)   /* clear: WS/BCLK come from the SoC */
/* SPEED_MODE, not bit 6. Bits 7:4 of REG08 are LRCK_RATE_MODE and only mean
 * anything when SDOUT_MODE = 11 (multi-LRCK TDM); writing bit 6 as the speed flag
 * configured a channel count and left the chip in single-speed mode at 88.2 and
 * 96 kHz, where the datasheet says the ADC clock has to be 256 * rate rather than
 * 512. Single speed is the reset default, which is why every rate below 64 kHz was
 * fine and the mistake stayed invisible. */
#define MODE_DOUBLE_SPEED  (1u << 1)

/* Bits 0, 1 and 3 are the ADC1/ADC2 clock gates. Clearing them is what lets the
 * selected channels run. */
#define CLOCK_OFF_ADC12    0x0b

#define GAIN_ENABLE        (1u << 4)   /* SELMICn: select MICnP/MICnN as the input */
#define GAIN_MASK          0x0f

/*
 * REG41/REG42, mic bias. Bits 6:4 set the bias voltage (111 = 2.87 V with VDDM =
 * 3.3 V) and bits 1:0 are ADCnBIAS_SWH, whose reset value is 01. The reference
 * driver -- and this driver, until it was measured -- writes 0x70, which leaves
 * that switch at level 0.
 *
 * Measured, this build vs the previous one, both at the setting's 30 dB: captured
 * RMS went from -60.9 dBFS to -23.9 dBFS, and the 0 dB vs 30 dB span went from
 * 3.3 dB (a ladder that does nothing) to 32 dB (the ladder's real 31.6x). The PGA
 * code was 10 in both builds, so the analog path changed state through this byte
 * alone. The schematic shows the mic's own VDD fed from AUDIO_3V3 through 0 ohm
 * (R15/R16/R29/R31), which is why an external bias looked like it made REG41
 * irrelevant -- it doesn't: ADCnBIAS_SWH belongs to the PGA/ADC bias, not to the
 * mic's supply.
 */
#define MIC_BIAS_VALUE     0x71

/*
 * REG1B..REG1E are ADCn_DIRECT_DB: the post-ADC digital gain when ALC is off
 * (REG16 resets to 0, so ALC is off). 0xBF is 0 dB and each step is 0.5 dB, up to
 * 0xFF = +32 dB. Neither this port nor the reference driver has ever touched these,
 * which is why the board's own firmware multiplies the samples by 8 in software
 * (Mic_Class: magnification 16 with oversampling 2, f_gain = 16/(os<<1)) to get a
 * usable level out of a 2 Vrms full-scale input fed by a -38 dBV/Pa MEMS element.
 *
 * The analog PGA tops out at 37.5 dB, and speech 300 mm from this mic needs roughly
 * 35 dB to land near -18 dBFS, so a fixed slice of digital gain is board
 * calibration rather than a preference. 12 dB is deliberately modest: it is what
 * makes a quiet mic audible without clipping, and if the PGA turns out to work after
 * the bias fix, 12 + 30 dB still leaves headroom for loud input.
 */
#define ADC12_DIGITAL_GAIN_DB 12
#define DIGITAL_GAIN_UNITY 0xbf   /* 0 dB */
#define DIGITAL_GAIN_MAX   0xff   /* +32 dB, 0.5 dB per step */

static unsigned char digital_code(int db)
{
    unsigned int steps = (unsigned int)db * 2;   /* 0.5 dB per step */
    unsigned int code = DIGITAL_GAIN_UNITY + steps;

    if (code > DIGITAL_GAIN_MAX)
        code = DIGITAL_GAIN_MAX;

    return (unsigned char)code;
}

/*
 * REG43..REG46: bit 4 is SELMICn and must be 1 or MICnP/MICnN is not the input at
 * all. Bits 3:0 are the PGA gain, and the ladder is NOT linear past 30 dB:
 *
 *   0..10 -> 0..30 dB in 3 dB steps
 *   11    -> 33 dB
 *   12    -> 34.5 dB
 *   13    -> 36 dB
 *   14    -> 37.5 dB
 *
 * Code 15 is undefined in the datasheet. audiohw_set_recvol() used to pass the
 * setting value straight through as a code, so the setting's 30 dB became 30,
 * clamped to 15 -- an undefined gain state -- and the setting's 12 dB became
 * 34.5 dB. Whole dB are what the UI offers, so the top of the ladder is reached in
 * 1 dB increments that land on the nearest defined code.
 */
static unsigned char gain_code(int db)
{
    if (db <= 0)
        return 0;
    if (db <= 30)
        return (unsigned char)(db / 3);
    if (db <= 32)
        return 11;      /* 33 dB */
    if (db <= 35)
        return 12;      /* 34.5 dB */
    if (db <= 36)
        return 13;      /* 36 dB */

    return 14;          /* 37.5 dB */
}

static bool reg_write(int reg, unsigned char val)
{
    return i2c_writemem(ES7210_I2C_ADDR, reg, &val, 1) == 1;
}

static bool reg_read(int reg, unsigned char *val)
{
    return i2c_readmem(ES7210_I2C_ADDR, reg, val, 1) == 1;
}

static bool reg_update(int reg, unsigned char mask, unsigned char val)
{
    unsigned char old;

    if (!reg_read(reg, &old))
        return false;

    return reg_write(reg, (old & ~mask) | (mask & val));
}

bool es7210_present(void)
{
    unsigned char v;

    return reg_read(REG_OSR, &v);
}

/*
 * The ADC clock has to be a fixed multiple of the sample rate, and the multiple
 * changes at 64 kHz because the chip then switches to double-speed mode.
 */
static void program_clock(unsigned long rate)
{
    bool double_speed = rate >= 64000;
    unsigned doubler = double_speed ? 0 : 1;
    unsigned adc_div = 1;
    unsigned dll = 1;
    unsigned lrck_div = 256;   /* MCLK / rate */

    reg_write(REG_MAINCLK, (dll << 7) | (doubler << 6) | adc_div);
    reg_write(REG_OSR, 0x20);
    reg_write(REG_LRCK_DIVH, (lrck_div >> 8) & 0xff);
    reg_write(REG_LRCK_DIVL, lrck_div & 0xff);
    reg_update(REG_MODE_CONFIG, MODE_DOUBLE_SPEED,
               double_speed ? MODE_DOUBLE_SPEED : 0);
}

/* The last CLOCK_OFF pattern known to enable the channels we use. Kept across
   sessions on purpose: es7210_stop() writes 0x7f, so the next start() reads the
   power-down pattern back and cannot recover the real value from the chip. The
   reference driver holds the same thing in a _clk_reg_save member for exactly
   this reason. Without it the ADC clocks are never restored, the converter
   silently stays off, and gain changes do nothing -- measured here as the first
   session capturing at peak 439-519 and every later one sitting at 12-14. */
static unsigned char clk_off_save = 0x00;

bool es7210_init(unsigned long rate)
{
    if (!es7210_present())
        return false;

    /* REG00 is a reset hold, not a pulse: 0xff holds every block in reset, and
     * the chip only starts running when the hold is released. The reference
     * firmware does exactly this, with a delay between the two writes.
     */
    reg_write(REG_RESET, 0xff);
    udelay(10000);
    reg_write(REG_RESET, 0x41);
    udelay(10000);

    reg_write(REG_CLOCK_OFF, 0x1f);
    reg_write(REG_TIME_CTRL0, 0x30);
    reg_write(REG_TIME_CTRL1, 0x30);

    /* DC blocking in front of each ADC pair. */
    reg_write(REG_ADC12_HPF2, 0x2a);
    reg_write(REG_ADC12_HPF1, 0x0a);
    reg_write(REG_ADC34_HPF2, 0x0a);
    reg_write(REG_ADC34_HPF1, 0x2a);

    reg_update(REG_MODE_CONFIG, MODE_MASTER, 0);

    reg_write(REG_ANALOG, 0x43);
    reg_write(REG_MIC12_BIAS, MIC_BIAS_VALUE);
    reg_write(REG_MIC34_BIAS, MIC_BIAS_VALUE);

    program_clock(rate);

    /* 16-bit samples, standard I2S (REG11 bits 1:0 = 0), two channels on SDOUT1.
     *
     * Bits 7:6 select the word length and the encoding is NOT a bit count:
     * 0x60 is 16-bit, 0x00 is 24-bit and 0x80 is 32-bit. This was written as
     * (16 << 5), which is 0x200 -- and reg_write() takes an unsigned char, so it
     * silently landed as 0x00, configuring the ADC for 24-bit. The receiver then
     * sampled 16 of every 24-bit word, which is why captured peaks sat around 90
     * out of 32767 no matter what the mic gain was set to. The truncation is also
     * why the mistake was invisible: the expression looks deliberate and no
     * warning is possible on a constant argument to a narrowing parameter. */
    reg_write(REG_SDP_IF1, 0x60);
    reg_write(REG_SDP_IF2, 0x00);

    /* MIC1 and MIC2 only: SDOUT1 carries two channels, and Rockbox records
     * stereo. */
    reg_update(REG_MIC1_GAIN, GAIN_ENABLE, 0);
    reg_update(REG_MIC2_GAIN, GAIN_ENABLE, 0);
    reg_write(REG_MIC12_POWER, 0xff);
    reg_write(REG_MIC34_POWER, 0xff);

    reg_update(REG_CLOCK_OFF, CLOCK_OFF_ADC12, 0);
    /* Remember what actually enabled the channels, so start() has something
       valid to restore after a stop() has written 0x7f. */
    {
        unsigned char now;
        if (reg_read(REG_CLOCK_OFF, &now) && now != 0x7f && now != 0xff)
            clk_off_save = now;
    }
    reg_write(REG_MIC12_POWER, 0x00);
    reg_update(REG_MIC1_GAIN, GAIN_ENABLE, GAIN_ENABLE);
    reg_update(REG_MIC2_GAIN, GAIN_ENABLE, GAIN_ENABLE);
    es7210_set_gain(global_settings.rec_mic_gain);

    reg_write(REG_ADC_AUTOMUTE, 0x00);

    return true;
}

void es7210_set_frequency(unsigned long rate)
{
    program_clock(rate);
}

/*
 * Set the mic gain in dB, on the scale the Rockbox setting uses (0 dB up to the
 * top of the ladder declared in es8311.h). The analog PGA takes the requested
 * figure and the fixed board calibration goes into the chip's own digital gain
 * registers, so one knob covers both stages instead of leaving the missing half to
 * a software multiplier somewhere above this driver.
 */
void es7210_set_gain(int db)
{
    unsigned char pga = gain_code(db);
    unsigned char dig = digital_code(ADC12_DIGITAL_GAIN_DB);

    reg_update(REG_MIC1_GAIN, GAIN_MASK, pga);
    reg_update(REG_MIC2_GAIN, GAIN_MASK, pga);

    /* ADC1 is the left slot and ADC2 the right on SDOUT1, and ALC is off, so these
     * are direct dB rather than ALC ceilings. */
    reg_write(REG_ADC1_MAXGAIN, dig);
    reg_write(REG_ADC2_MAXGAIN, dig);
}

void es7210_start(void)
{
    /* The clock-off register is restored from the saved pattern rather than from
       what the chip currently reports, because stop() deliberately leaves 0x7f
       there. Reading it back and writing it unchanged would keep the ADC powered
       down forever after the first session. The readback still refreshes the
       saved value whenever it looks like a real configuration. */
    {
        unsigned char off;

        if (reg_read(REG_CLOCK_OFF, &off) && off != 0x7f && off != 0xff)
            clk_off_save = off;

        reg_write(REG_CLOCK_OFF, clk_off_save);
    }

    reg_write(REG_POWER_DOWN, 0x00);
    reg_write(REG_ANALOG, 0x43);
    reg_write(REG_MIC1_POWER, 0x00);
    reg_write(REG_MIC2_POWER, 0x00);
    reg_write(REG_MIC3_POWER, 0x00);
    reg_write(REG_MIC4_POWER, 0x00);
    reg_write(REG_MIC12_POWER, 0x00);
    reg_write(REG_ADC_AUTOMUTE, 0x00);

    /* Gain is re-applied on every start so a change in the recording settings
     * takes effect without re-initialising the chip. */
    es7210_set_gain(global_settings.rec_mic_gain);
}

/* What the chip actually holds, for the debug screen. The register values are the
 * only way to separate "we wrote the wrong thing" from "the chip ignored what we
 * wrote": a PGA gain setting that changes the captured level by 3 dB instead of 30
 * looks like a broken caller or a broken device, and the two need different fixes.
 * Reads cost six polled I2C transactions and are drawn when the screen opens. */
void es7210_stat(char *buf, int bufsz)
{
    unsigned char v43 = 0, v1e = 0, v1d = 0, v41 = 0, v01 = 0, v08 = 0;

    reg_read(REG_MIC1_GAIN, &v43);
    reg_read(REG_ADC1_MAXGAIN, &v1e);
    reg_read(REG_ADC2_MAXGAIN, &v1d);
    reg_read(REG_MIC12_BIAS, &v41);
    reg_read(REG_CLOCK_OFF, &v01);
    reg_read(REG_MODE_CONFIG, &v08);

    snprintf(buf, bufsz, "ADC 43=%02x 1e=%02x 1d=%02x 41=%02x 01=%02x 08=%02x",
             v43, v1e, v1d, v41, v01, v08);
}

void es7210_stop(void)
{
    reg_write(REG_MIC1_POWER, 0xff);
    reg_write(REG_MIC2_POWER, 0xff);
    reg_write(REG_MIC3_POWER, 0xff);
    reg_write(REG_MIC4_POWER, 0xff);
    reg_write(REG_MIC12_POWER, 0xff);
    reg_write(REG_MIC34_POWER, 0xff);
    reg_write(REG_ANALOG, 0xc0);
    reg_write(REG_CLOCK_OFF, 0x7f);
    reg_write(REG_POWER_DOWN, 0x07);
}
