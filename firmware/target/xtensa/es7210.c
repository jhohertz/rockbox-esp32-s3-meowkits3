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

    /* 16-bit samples, standard I2S format, two channels on SDOUT1. */
    reg_write(REG_SDP_IF1, (16 << 5) | 0x00);
    reg_write(REG_SDP_IF2, 0x00);

    /* MIC1 and MIC2 only: SDOUT1 carries two channels, and Rockbox records
     * stereo. */
    reg_update(REG_MIC1_GAIN, GAIN_ENABLE, 0);
    reg_update(REG_MIC2_GAIN, GAIN_ENABLE, 0);
    reg_write(REG_MIC12_POWER, 0xff);
    reg_write(REG_MIC34_POWER, 0xff);

    reg_update(REG_CLOCK_OFF, CLOCK_OFF_ADC12, 0);
    reg_write(REG_MIC12_POWER, 0x00);
    reg_update(REG_MIC1_GAIN, GAIN_ENABLE, GAIN_ENABLE);
    reg_update(REG_MIC2_GAIN, GAIN_ENABLE, GAIN_ENABLE);
    reg_update(REG_MIC1_GAIN, GAIN_MASK, gain_code(global_settings.rec_mic_gain));
    reg_update(REG_MIC2_GAIN, GAIN_MASK, gain_code(global_settings.rec_mic_gain));

    reg_write(REG_ADC_AUTOMUTE, 0x00);

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
    unsigned char off;

    /* The clock-off register is read back so that stop() can restore exactly
     * the channels that were enabled, rather than assuming a pattern. */
    if (reg_read(REG_CLOCK_OFF, &off) && off != 0x7f && off != 0xff)
        reg_write(REG_CLOCK_OFF, off);

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
