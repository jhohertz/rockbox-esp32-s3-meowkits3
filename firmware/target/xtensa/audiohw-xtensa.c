/***************************************************************************
 * Audio hardware API for the MeowKit ESP32-S3 target.
 *
 * The codec is an ES8311 on the bit-banged I2C bus; the transport is I2S1.
 * Volume is hardware, so PCM_NATIVE_VOLUME_TYPE resolves to PCM_SINK_HWVOL.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "audiohw.h"
#include "pcm_sampr.h"
#include "es8311.h"
#include "i2s-xtensa.h"

void audiohw_preinit(void)
{
    es8311_init();
}

void audiohw_init(void)
{
    audiohw_preinit();
    i2s_set_frequency(hw_freq_sampr[HW_FREQ_DEFAULT]);
    audiohw_postinit();
}

void audiohw_postinit(void)
{
    /* Volume is applied by the sound settings, not here. */
    es8311_mute(0);
}

void audiohw_close(void)
{
    es8311_close();
}

void audiohw_set_volume(int val)
{
    es8311_set_volume(val);
}

void audiohw_set_frequency(int fsel)
{
    i2s_set_frequency(hw_freq_sampr[fsel]);
}
