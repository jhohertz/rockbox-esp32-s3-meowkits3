/***************************************************************************
 * Audio hardware API for the MeowKit ESP32-S3 target.
 *
 * The codec is an ES8311 on the hardware I2C master; the transport is I2S1.
 * Volume is hardware, so PCM_NATIVE_VOLUME_TYPE resolves to PCM_SINK_HWVOL.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "audio.h"
#include "audiohw.h"
#include "pcm_sampr.h"
#include "es8311.h"
#include "i2s-xtensa.h"
#include "pca9557.h"
#ifdef HAVE_RECORDING
#include "es7210.h"
#endif

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

#ifdef HAVE_SPEAKER
/* The NS4150B amplifier is enabled by PCA9557 IO3, not by a GPIO: the expander is
 * the only way to reach it, and it is high-active with a 10K pull-down. Leaving it
 * low means the DAC produces a signal that never reaches the speaker.
 */
void audiohw_enable_speaker(bool on)
{
    pca9557_write(IOEXP_PA_EN, on ? 1 : 0);
}
#endif /* HAVE_SPEAKER */

#if INPUT_SRC_CAPS != 0
/* There is one recording source and it is hard-wired to the receiver half of the
 * I2S bus, so selecting it is a no-op. The function still has to exist: audio_path.c
 * calls it whenever a source is chosen.
 */
void audio_input_mux(int source, unsigned flags)
{
    (void)source;
    (void)flags;
}

/* Likewise there is nothing to route: playback always goes to the codec and the
 * ADC's output is not monitored through the speaker. */
void audio_set_output_source(int source)
{
    (void)source;
}
#endif /* INPUT_SRC_CAPS */

#ifdef HAVE_RECORDING
/* The ADC's gain is the only recording gain on the board. left is the hardware
 * code (0 = 0 dB, 10 = 30 dB) that es7210.c writes into REG43/REG44. */
void audiohw_set_recvol(int left, int right, int type)
{
    (void)right;

    if (type == AUDIO_GAIN_MIC)
        es7210_set_gain(left);
}
#endif /* HAVE_RECORDING */