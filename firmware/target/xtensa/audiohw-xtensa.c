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
#include "tick.h"
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

#ifdef HAVE_SPEAKER
/* The NS4150B between the codec and the speaker is enabled once, at
 * audiohw_enable_speaker(), and nothing ever turned it off: a class-D stage with
 * nothing to amplify still draws. So does a powered codec. With the frequency seam
 * and light sleep in place the core is asleep most of the time -- measured, not
 * assumed -- which means what is left at idle is exactly this kind of board current.
 *
 * The policy runs as a tick task rather than from the sink, because the sink cannot
 * do it: usb_audio.c calls mixer_channel_play_data() inside disable_irq_save()
 * (see sink_play()), and the amp and the codec are both I2C. tick_add_task() runs in
 * the tick thread with interrupts enabled, which is the same arrangement
 * power-xtensa.c and usb-xtensa.c already use.
 *
 * The state is i2s_tx_playing(), not audio_status(): a USB-DAC stream is the host
 * driving the mixer and Rockbox's audio state does not report it, so keying off
 * audio_status() would mute a stream the user can hear. That is the one way this
 * change could break audio rather than merely save it.
 *
 * Order matters on both edges. Going idle drops the amp before muting the codec, so
 * the codec's own mute transient is never amplified; coming back unmutes the codec
 * before enabling the amp, and by then the transmit ring is already carrying
 * silence -- idle_tx() keeps it armed from boot -- so the amp switches on into a
 * quiet line. The 250 ms grace after playback stops is what keeps the fade tail
 * audible instead of clipping it.
 */
#define PA_IDLE_TICKS (HZ / 4)   /* 250 ms at HZ */
static bool pa_on = true;
static unsigned int pa_idle_ticks;

static void audiohw_pa_idle_tick(void)
{
    if (i2s_tx_playing())
    {
        pa_idle_ticks = 0;
        if (!pa_on)
        {
            es8311_mute(0);
            pca9557_write(IOEXP_PA_EN, 1);
            pa_on = true;
        }
        return;
    }

    if (++pa_idle_ticks < PA_IDLE_TICKS)
        return;

    if (pa_on)
    {
        pca9557_write(IOEXP_PA_EN, 0);
        es8311_mute(1);
        pa_on = false;
    }
}
#endif /* HAVE_SPEAKER */

void audiohw_postinit(void)
{
    /* Volume is applied by the sound settings, not here. */
    es8311_mute(0);
#ifdef HAVE_SPEAKER
    tick_add_task(audiohw_pa_idle_tick);
#endif
}

void audiohw_close(void)
{
    es8311_close();
}

void audiohw_set_volume(int val)
{
    /* An I2C write to the codec, so it shares the bus with the touch poll, the
     * PMU and the IO expander. Rapid file cycling means rapid fades. */
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
#ifdef HAVE_SPEAKER
    /* Keep the idle policy's idea of the state honest, or its next transition
     * would be decided from a stale flag. */
    pa_on = on;
    pa_idle_ticks = 0;
#endif
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