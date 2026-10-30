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
#include "i2s-esp32s3.h"
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
 * audiohw_enable_speaker(), and nothing ever turned it off: a class-D stage
 * with nothing to amplify still draws. So does a powered codec. With the
 * frequency seam and light sleep in place the core is asleep most of the time
 * -- measured, not assumed -- which means what is left at idle is exactly this
 * kind of board current.
 *
 * The policy runs as a tick task rather than from the sink, because the sink
 * cannot do it: usb_audio.c calls mixer_channel_play_data() inside
 * disable_irq_save() (see sink_play()), and the amp and the codec are both I2C.
 * tick_add_task() runs in the tick thread with interrupts enabled, which is the
 * same arrangement power-meowkits3.c and usb-esp32s3.c already use.
 *
 * The state is i2s_tx_playing(), not audio_status(): a USB-DAC stream is the
 * host driving the mixer and Rockbox's audio state does not report it, so
 * keying off audio_status() would mute a stream the user can hear. That is the
 * one way this change could break audio rather than merely save it.
 *
 * Order matters on both edges. Going idle drops the amp before muting the
 * codec, so the codec's own mute transient is never amplified; coming back
 * unmutes the codec before enabling the amp, and by then the transmit ring is
 * already carrying silence -- idle_tx() keeps it armed from boot -- so the amp
 * switches on into a quiet line. The 250 ms grace after playback stops is what
 * keeps the fade tail audible instead of clipping it.
 */
#define PA_IDLE_TICKS (HZ / 4)   /* 250 ms at HZ */
/* Two separate states, because on this board they mean different things:
 * pa_wanted is what the core asked for through audiohw_enable_speaker() --
 * i.e. Settings -> Speaker -- and pa_on is what the expander actually holds.
 * Keeping them apart is what makes the setting mean something: the policy
 * used to re-enable the amp on every playback start regardless of the user's
 * choice, so "Speaker: Off" was display-only. It also matters for recording,
 * apps/recorder/recording.c:1018 asks for the speaker off while the mic is
 * open and restores it at :1818, and the old policy undid that as soon as
 * the transmit path carried a frame. */
static bool pa_wanted = true;
static bool pa_on = true;
/* Inverted name so the initial state reads true with the codec open at boot. */
static bool codec_open = true;
static unsigned int pa_idle_ticks;

static void audiohw_pa_idle_tick(void)
{
    if (i2s_tx_playing())
    {
        pa_idle_ticks = 0;
        /* The codec has to be open whenever audio is moving, whether or not the
         * amplifier is: the jack is fed from the same headphone driver, so
         * muting the codec would take the headphones down with the amp. */
        if (!codec_open)
        {
            es8311_mute(0);
            codec_open = true;
        }
        if (pa_wanted && !pa_on)
        {
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
        pa_on = false;
    }
    if (codec_open)
    {
        es8311_mute(1);
        codec_open = false;
    }
}
#endif /* HAVE_SPEAKER */

void audiohw_postinit(void)
{
    /* Volume is applied by the sound settings, not here. */
    es8311_mute(0);
#ifdef HAVE_SPEAKER
    codec_open = true;
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
/* The NS4150B amplifier is enabled by PCA9557 IO3, not by a GPIO: the expander
 * is the only way to reach it, and it is high-active with a 10K pull-down.
 * Leaving it low means the DAC produces a signal that never reaches the
 * speaker.
 */
void audiohw_enable_speaker(bool on)
{
    /* The core decides when; the target only obeys. imx233-codec.c:104 and
     * speaker-onda_vx747.c:26 are both pure -- neither has a policy of its own
     * -- so recording the request rather than acting-and-forgetting is the
     * shape that keeps the idle policy from overriding it later.
     *
     * Turning the amp off does not mute the codec: with Speaker set to Off the
     * jack still has to carry sound.
     */
    pa_wanted = on;
    pa_idle_ticks = 0;

    if (on && !pa_on)
    {
        pca9557_write(IOEXP_PA_EN, 1);
        pa_on = true;
    }
    else if (!on && pa_on)
    {
        pca9557_write(IOEXP_PA_EN, 0);
        pa_on = false;
    }
}
#endif /* HAVE_SPEAKER */

#if INPUT_SRC_CAPS != 0
/* There is one recording source and it is hard-wired to the receiver half of
 * the I2S bus, so selecting it is a no-op. The function still has to exist:
 * audio_path.c calls it whenever a source is chosen.
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
/* The ADC's gain is the only recording gain on the board. Rockbox hands the
 * setting value here in dB, so it goes to es7210_set_gain() as dB: that
 * function maps the analog PGA code from the datasheet ladder and adds the
 * fixed digital calibration. Passing the value straight through as a
 * register code used to write the undefined code 15 for the setting's 30 dB,
 * and 34.5 dB for its 12 dB. */
void audiohw_set_recvol(int left, int right, int type)
{
    (void)right;

    if (type == AUDIO_GAIN_MIC)
        es7210_set_gain(left);
}
#endif /* HAVE_RECORDING */