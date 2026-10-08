#ifndef ES8311_H
#define ES8311_H

/* MeowKit ESP32-S3: ES8311 on the bit-banged I2C bus */
#define ES8311_I2C_ADDR 0x18

/* DAC volume register: 0x00 = -95.5 dB, 0xFF = +32.0 dB, 0.5 dB per step. */
AUDIOHW_SETTING(VOLUME, "dB", 1, 1, -955, 320, -250)

void es8311_init(void);
void es8311_postinit(void);
void es8311_close(void);
void es8311_set_clock(unsigned long rate, unsigned long mclk);
void es8311_set_volume(int centibels);
void es8311_mute(int mute);

#endif /* ES8311_H */
