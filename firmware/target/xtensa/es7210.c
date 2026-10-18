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
#include <stdio.h> /* TEMPORARY: register readback probe in es7210_init */

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
#define MODE_DOUBLE_SPEED  (1u << 6)

/* Bits 0, 1 and 3 are the ADC1/ADC2 clock gates. Clearing them is what lets the
 * selected channels run. */
#define CLOCK_OFF_ADC12    0x0b

#define GAIN_ENABLE        (1u << 4)
#define GAIN_MASK          0x0f

/*
 * REG43..REG46: bit 4 enables the channel, bits 3:0 are the gain. The steps are
 * 3 dB up to 30 dB and 1.5 dB above that; Rockbox only ever asks for whole dB
 * within the 0..30 range declared in es8311.h, so the 3 dB ladder is enough.
 */
static unsigned char gain_code(int db)
{
    if (db < 0)
        return 0;
    if (db > 30)
        db = 30;

    return (unsigned char)(db / 3);
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
    reg_write(REG_MIC12_BIAS, 0x70);
    reg_write(REG_MIC34_BIAS, 0x70);

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
    reg_update(REG_MIC1_GAIN, GAIN_MASK, gain_code(global_settings.rec_mic_gain));
    reg_update(REG_MIC2_GAIN, GAIN_MASK, gain_code(global_settings.rec_mic_gain));

    reg_write(REG_ADC_AUTOMUTE, 0x00);

    /* TEMPORARY: read the chip back. Writing a register and reading it are two
       different claims -- the ES7210 has been accepting every other write
       happily, so if REG11 or the gain bytes come back wrong the write is being
       dropped or overwritten, and if they come back right then the low level is
       analogue (bias, input select, the mic itself) and no further register work
       will help. */
    {
        unsigned char r11 = 0, r43 = 0, r44 = 0, r41 = 0, r40 = 0;

        reg_read(REG_SDP_IF1, &r11);
        reg_read(REG_MIC1_GAIN, &r43);
        reg_read(REG_MIC2_GAIN, &r44);
        reg_read(REG_MIC12_BIAS, &r41);
        reg_read(REG_ANALOG, &r40);
        printf("ES7210 readback: REG11=%02x (want 60) REG43=%02x REG44=%02x "
               "(want 1e/1e) BIAS41=%02x ANALOG40=%02x gain=%d\n",
               r11, r43, r44, r41, r40, global_settings.rec_mic_gain);
    }

    return true;
}

void es7210_set_frequency(unsigned long rate)
{
    program_clock(rate);
}

void es7210_set_gain(int code)
{
    if (code < 0)
        code = 0;
    if (code > 15)
        code = 15;

    reg_update(REG_MIC1_GAIN, GAIN_MASK, (unsigned char)code);
    reg_update(REG_MIC2_GAIN, GAIN_MASK, (unsigned char)code);
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
    es7210_set_gain(gain_code(global_settings.rec_mic_gain));
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
