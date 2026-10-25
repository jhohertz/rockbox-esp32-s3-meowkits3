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

#include <stdio.h>

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

/* Every call site used to discard this. That is how a dead I2C bus looks
 * identical to a correctly configured codec: the transport runs at exactly the
 * right rate, the DAC is silent, and nothing anywhere reports a failure. */
static int es8311_write_fails;
static int es8311_writes;

/* i2c_writemem(dev, address, buf, count) always puts `address` on the bus first,
 * as the register byte. This driver used to pass address = 0 and put the register
 * in the payload alongside the value, so every write went out as three bytes --
 * 00, reg, val -- instead of reg, val. The codec saw a write to register 0 whose
 * data was the intended register number, then two stray bytes.
 *
 * Every other device on this bus gets it right: pca9557.c and es7210.c both pass
 * the register as `address` and a single data byte. That mismatch is why the
 * expander, the PMU and the ADC all work while the codec never accepted a single
 * command -- and because the return value was discarded at every call site, it
 * looked identical to a configured chip. The values that appeared to match what we
 * wrote were reset defaults.
 *
 * Symptom, for the record: perfect I2S transport at 44.1 kHz, readback showing
 * REG00 = 0x31 (chipen clear, powered down) and REG01 = 0x00 (no clocks enabled),
 * and silence on both the headphone jack and the speaker.
 */
static int write_reg(int reg, int val)
{
    unsigned char v = (unsigned char)val;
    /* i2c_writemem returns the byte count on success and -1 on failure -- not
     * zero. Testing against 0 would report every successful write as a failure
     * and invent a dead bus that is not there. */
    int r = i2c_writemem(ES8311_I2C_ADDR, reg, &v, 1);

    es8311_writes++;
    if (r != 1)
        es8311_write_fails++;

    return r;
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
#ifdef XT_AUDIO_TRACE
/* TEMPORARY PROBE: one register read for the transmit trace. Costs 1.6 ms of
 * polled I2C, so the caller takes it at most every couple of seconds. */
int es8311_trace_read(int reg)
{
    return read_reg(reg);
}
#endif

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

/* Read the codec back over I2C. This is the only way to distinguish "we wrote
 * the right values" from "the chip is configured", and until now nothing in the
 * driver ever asked the chip what it thinks its state is. */
void es8311_dump(const char *tag)
{
    /* One printf, not twenty. The multi-call version this replaces was cut off
     * at twelve registers by interleaving output, which truncated it exactly
     * before dac31/dac32 -- the two that mattered. */
    int chip = read_reg(0x00), clk1 = read_reg(0x01), clk2 = read_reg(0x02);
    int clk3 = read_reg(0x03), clk4 = read_reg(0x04), clk5 = read_reg(0x05);
    int clk6 = read_reg(0x06), clk7 = read_reg(0x07), clk8 = read_reg(0x08);
    int sdpin = read_reg(0x09), sdpo = read_reg(0x0A), sys0d = read_reg(0x0D);
    int sys0e = read_reg(0x0E), sys12 = read_reg(0x12), sys13 = read_reg(0x13);
    int dac31 = read_reg(0x31), dac32 = read_reg(0x32);

    printf("ES8311 %s: w=%d f=%d | chip=%02x clk=%02x %02x %02x %02x %02x "
           "%02x %02x %02x | sdp=%02x %02x | sys=%02x %02x %02x %02x | "
           "dac31=%02x dac32=%02x\n",
           tag, es8311_writes, es8311_write_fails,
           chip & 0xff, clk1 & 0xff, clk2 & 0xff, clk3 & 0xff, clk4 & 0xff,
           clk5 & 0xff, clk6 & 0xff, clk7 & 0xff, clk8 & 0xff,
           sdpin & 0xff, sdpo & 0xff, sys0d & 0xff, sys0e & 0xff, sys12 & 0xff,
           sys13 & 0xff, dac31 & 0xff, dac32 & 0xff);
}

/* The last volume we were asked for, in centibels, or VOL_UNSET before the first
 * call. Needed because sink_init() runs audiohw_preinit() -> es8311_init() when
 * the PCM core comes up, which is after the apps layer has already applied the
 * stored volume. The reset below wipes REG32 to 0 -- -95.5 dB -- and nothing else
 * re-applies it, so the codec came back muted no matter what the UI showed. */
#define VOL_UNSET 0x7fffffff
static int last_vol_cB = VOL_UNSET;

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

    /* REG00 = 0x1F above is a full digital reset, which clears the DAC volume
     * along with everything else. Restore the last value we were given so a reset
     * that happens after the apps layer set the volume does not silently mute the
     * codec. */
    if (last_vol_cB != VOL_UNSET)
        es8311_set_volume(last_vol_cB);
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
/* val is in Rockbox's 0.1 dB unit, matching the AUDIOHW_SETTING bounds of -955 and 0.
 * The register is 0.5 dB per step from a -95.5 dB floor, so:
 *
 *   -955 -> 0   (-95.5 dB, the floor)
 *   -250 -> 141 (-25.0 dB, the default)
 *     0  -> 191 (0 dB, unity -- and the ceiling the range now declares)
 *   +320 -> 255 (+32.0 dB, unreachable: the declared range stops at 0 dB because the
 *                stock firmware caps this amplifier at register 191 for its 1 W rating)
 *
 * The clamp below stays as a guard rather than as the range limit: Rockbox would only
 * reach it if something outside the audiohw setting table handed us a value above the
 * declared maximum.
 *
 * The previous formula divided by 50 after adding 9550, i.e. it treated val as
 * centibels. On the -25.0 dB default that produced 0xBA, which is -2.5 dB -- 22.5
 * dB louder than intended -- and it made the 0.1 dB keypress step land on the
 * same register value over and over. */
void es8311_set_volume(int val)
{
    int reg = (val + 955) / 5;

    last_vol_cB = val;

    if (reg < 0)
        reg = 0;
    if (reg > 255)
        reg = 255;

    /* i2c_writemem returns -1 on a failed transfer. That is the only thing worth
     * saying here: the register value itself is derived, and a volume change is a
     * keypress-rate event, so a trace line for every one of them is noise. */
    if (write_reg(REG_DAC32, reg) < 0)
        printf("ES8311: volume write failed\n");
}

void es8311_mute(int mute)
{
    int reg = read_reg(REG_DAC31);

    if (reg < 0)
        return;

    reg &= 0x9F;
    write_reg(REG_DAC31, mute ? (reg | 0x60) : reg);
}
