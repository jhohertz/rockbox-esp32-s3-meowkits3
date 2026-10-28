#ifndef ES8311_H
#define ES8311_H

/* MeowKit ESP32-S3: ES8311 on the hardware I2C master, address 0x18 */
#define ES8311_I2C_ADDR 0x18

/* DAC volume register: 0x00 = -95.5 dB, and 0.5 dB per step, so 0 dB lands on
 * register 191 and the register keeps counting to 0xFF (+32.0 dB).
 *
 * The declared range deliberately stops at 0 dB rather than at the register's own
 * ceiling. The stock firmware for this exact board drives the same codec with no dB
 * notion at all -- es8311_voice_volume_set() maps a 0..100 percent slider to
 * reg32 = percent*256/100 - 1 -- and caps the speaker at SPK_VOLUME_MAX 75 with the
 * comment "NS4150B 1W speaker ceiling", i.e. register 191. That is the unity point of
 * this mapping, so the vendor ships the amplifier with no digital boost available at
 * all. Advertising +32 dB lets Rockbox, and any USB host whose slider sits at or above
 * unity, command 32 dB past full scale: the DAC clips, the amp clips harder, and because
 * a USB Feature Unit writes straight into the player's own volume, the blowout also
 * survives the DAC session and shows up on local playback.
 *
 * With numdecimals=1 the unit Rockbox passes to audiohw_set_volume() is 0.1 dB, not
 * 1 dB and not centibels -- hence -955 and 0 rather than -95.5 and 0.0. steps is 5 so
 * one keypress moves one hardware quantum (0.5 dB); every other numdecimals=1 target in
 * the tree uses 5, and the 1 that was here made each press worth 0.1 dB, which the 0.5 dB
 * register quantum rounds away, so the volume keys appeared to do nothing.
 */
AUDIOHW_SETTING(VOLUME, "dB", 1, 5, -955, 0, -250)

/*
 * The recording ADC (ES7210) hangs off the same I2S bus as this codec. Rockbox
 * keeps microphone gain in the audiohw setting table, so it has to be declared
 * here even though the chip it applies to is not the codec.
 */
#ifdef HAVE_RECORDING
#define AUDIOHW_HAVE_MIC_GAIN
/* The ES7210 PGA ladder is 0..30 dB in 3 dB steps and then 33, 34.5, 36 and 37.5
 * dB in 1.5 dB steps, with no defined code above that. The whole-dB range the UI
 * offers therefore ends at 37, and es7210.c maps each value to the nearest defined
 * code. A fixed digital calibration (see ADC12_DIGITAL_GAIN_DB) is added on top of
 * whatever this setting asks for, because the chip's full-scale input is about
 * 2 Vrms while the board's MEMS element puts out -38 dBV/Pa. */
AUDIOHW_SETTING(MIC_GAIN, "dB", 0, 3, 0, 37, 30)
#endif

void es8311_init(void);
/* Format the codec's readback state (clock dividers, power, DAC, and the write
 * success counts) into buf for the debug screen. Returns the snprintf length. */
int es8311_stat(char *buf, size_t bufsz);
void es8311_postinit(void);
void es8311_close(void);

void es8311_set_clock(unsigned long rate, unsigned long mclk);
void es8311_set_volume(int val);   /* Rockbox 0.1 dB unit */
void es8311_mute(int mute);

#endif /* ES8311_H */
