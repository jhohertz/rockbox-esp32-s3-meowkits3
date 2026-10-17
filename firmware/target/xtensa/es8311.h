#ifndef ES8311_H
#define ES8311_H

/* MeowKit ESP32-S3: ES8311 on the hardware I2C master, address 0x18 */
#define ES8311_I2C_ADDR 0x18

/* DAC volume register: 0x00 = -95.5 dB, 0xFF = +32.0 dB, 0.5 dB per step.
 *
 * With numdecimals=1 the unit Rockbox passes to audiohw_set_volume() is 0.1 dB,
 * not 1 dB and not centibels -- which is why the bounds are -955 and 320 rather
 * than -95.5 and 32.0. steps must therefore be 5 to move one hardware quantum
 * (0.5 dB) per keypress. Every other numdecimals=1 target in the tree uses 5;
 * the 1 that was here made each press worth 0.1 dB, which the 0.5 dB register
 * quantum usually rounds away, so the volume keys appeared to do nothing. */
AUDIOHW_SETTING(VOLUME, "dB", 1, 5, -955, 320, -250)

/*
 * The recording ADC (ES7210) hangs off the same I2S bus as this codec. Rockbox
 * keeps microphone gain in the audiohw setting table, so it has to be declared
 * here even though the chip it applies to is not the codec.
 */
#ifdef HAVE_RECORDING
#define AUDIOHW_HAVE_MIC_GAIN
AUDIOHW_SETTING(MIC_GAIN, "dB", 0, 3, 0, 30, 30)
#endif

void es8311_init(void);
/* Read the codec back over I2C and print what it reports, with a count of the
 * writes we attempted and how many were NAKed. */
void es8311_dump(const char *tag);
void es8311_postinit(void);
void es8311_close(void);
void es8311_set_clock(unsigned long rate, unsigned long mclk);
void es8311_set_volume(int val);   /* Rockbox 0.1 dB unit */
void es8311_mute(int mute);

#endif /* ES8311_H */
