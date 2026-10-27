/***************************************************************************
 * Recording over the I2S0 receiver, through ESP-IDF's I2S driver.
 *
 * This file used to drive the receiver and its DMA by hand: a two-descriptor
 * GDMA ring in internal SRAM, the descriptor words and their owner bits, the
 * cache write-back and invalidate that go around them, the rx_conf and
 * GDMA_IN_LINK programming, the interrupt allocation, and the rx_update strobe
 * that latches the configuration into the sampling domain. Every one of those is
 * something IDF's i2s_channel_* API does, and every one of them was a place this
 * target silently failed -- the last of them being rx_update, whose absence made
 * a correctly configured master receiver capture nothing at all.
 *
 * So the hand-rolled path is gone. What is left here is the part that is
 * genuinely Rockbox's: turning a blocking i2s_channel_read() into the
 * pcm_rec_dma_* contract, which is a callback-driven "fill this buffer, then ask
 * for the next one".
 *
 * The receiver is the MASTER of the I2S bus while recording (the reference
 * firmware for this board does the same: I2S_MODE_MASTER | I2S_MODE_RX). It has
 * to be, because the board wires one MCLK/BCLK/WS set to both codecs, so the
 * clocks exist only if somebody drives them, and playback is stopped before a
 * recording starts. The three shared pads are handed to this port for the
 * duration and given back to the transmitter afterwards -- see
 * i2s_rx_pads_take() / i2s_rx_pads_release() in i2s-xtensa.c.
 *
 * The pins are deliberately NOT handed to IDF's channel configuration: IDF would
 * claim MCLK/BCLK/WS for I2S0 permanently at init, and the transmitter needs them
 * whenever we are not recording. Only the ADC's data line, which nothing else
 * uses, is given over.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "string.h"

#include "i2s-xtensa.h"
#include "es7210.h"
#include "pcm-internal.h"
#include "audiohw.h"
#include "kernel.h"
#include "semaphore.h"
#include "panic.h"

#include <stdio.h>   /* boot and error reporting */

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

/* One read chunk. IDF sizes its DMA buffers from dma_desc_num * dma_frame_num
 * below; this is how much the recorder is handed per completion, and it is
 * deliberately smaller than a whole DMA buffer so the recorder's own buffering
 * stays responsive. */
#define RX_CHUNK 4096

/* IDF's DMA ring: 4 descriptors of 512 frames, matching what the reference
 * firmware's Mic_Class uses (dma_buf_count = 4, dma_buf_len = 512). At 16-bit
 * stereo that is 4 x 2048 bytes = 8 KB, allocated by IDF in internal RAM. */
#define RX_DMA_DESC_NUM  4
#define RX_DMA_FRAME_NUM 512

static i2s_chan_handle_t rx_chan;
/* The channel is created once and kept for the lifetime of the boot -- see
   pcm_rec_dma_init(). These track what state it is in so enable and disable are
   issued once each, instead of IDF logging "the channel has not been enabled
   yet" every time a session ends. */
static unsigned long rx_chan_rate;
static bool rx_chan_enabled;
static bool rx_thread_created;

static unsigned char *rec_ptr;
static size_t rec_left;
static int rec_locked;
static volatile bool rx_wanted;
static volatile bool rx_running;
static volatile bool rx_thread_idle = true;

/* Receive-path counters, read by the debug screen's RX row. The distinction that
 * matters is between "the reader asked and nothing arrived" and "the reader asked
 * and the port reported an error": from the outside both look like a plugin that
 * never gets audio, and which one it is decides whether the clock is missing or
 * the configuration is. */
static unsigned int stat_sessions;
static unsigned int stat_reads;
static unsigned int stat_bytes;
static unsigned int stat_timeouts;
static unsigned int stat_errors;
static int stat_last_err;

static struct semaphore rx_sem;
static long rx_thread_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static const char rx_thread_name[] = "i2srx";
static unsigned int rx_thread_id;

/* The recorder is driven by callbacks, not by a blocking reader, so a small
 * thread converts one into the other: read a chunk, advance the destination, and
 * when the current buffer is full ask pcm_record for the next one. A non-OK read
 * is reported and retried rather than treated as fatal, because a timeout during
 * teardown is normal and the loop exits on rx_wanted anyway. */
static void i2s_rx_thread(void)
{
    while (true)
    {
        semaphore_wait(&rx_sem, TIMEOUT_BLOCK);
        rx_thread_idle = false;

        while (rx_wanted && rec_left > 0)
        {
            size_t want;
            size_t got = 0;
            esp_err_t err;

            want = rec_left < RX_CHUNK ? rec_left : RX_CHUNK;
            err = i2s_channel_read(rx_chan, rec_ptr, want, &got, 200);

            if (err != ESP_OK || got == 0)
            {
                /* A timeout during teardown is normal; a timeout while the
                 * recorder still wants data is retried, not treated as the end of
                 * the capture. */
                stat_last_err = (int)err;

                if (err == ESP_ERR_TIMEOUT || got == 0)
                    stat_timeouts++;
                else
                    stat_errors++;

                if (!rx_wanted)
                    break;
                continue;
            }

            stat_reads++;
            stat_bytes += got;

            rec_ptr += got;
            rec_left -= got;

            /* pcm_rec_lock() masks the recording completion, not the transfer --
             * imx233 does it by disabling the DMA channel interrupt, and this
             * driver's predecessor did it by masking IN_DONE. The DMA kept
             * filling either way. So keep reading into the caller's buffer and
             * hold back only the hand-off, which is what the interrupt would have
             * been. Stopping the read instead (an earlier version here did) parks
             * the thread while the recorder waits for data that can never arrive,
             * because pcm_record_data() starts us while it still holds the lock. */
            if (rec_left == 0 && !rec_locked)
            {
                void *addr;
                size_t size;
                bool again;

                again = pcm_rec_dma_complete_callback(PCM_DMAST_OK, &addr, &size);

                if (!again)
                {
                    rx_wanted = false;
                    break;
                }

                rec_ptr = (unsigned char *)addr;
                rec_left = size;
            }

            /* Yield to the threads that have to run for the hand-off above to
             * mean anything. i2s_channel_read() returns as soon as a DMA buffer
             * completes -- about 23 ms for 4096 bytes at 44.1 kHz stereo 16-bit --
             * and this thread sits at PRIORITY_USER_INTERFACE, the same priority
             * as the codec and recorder threads that consume what we just handed
             * over. A loop that blocks only briefly and never sleeps can starve
             * exactly the consumers it is waiting on. sleep(0) waits for the next
             * tick (10 ms), which still allows ~400 KB/s against the 176 KB/s the
             * stream needs, so it costs nothing. */
            sleep(0);
        }

        rx_running = rx_wanted;
        rx_thread_idle = true;
    }
}

/* Create the IDF receive channel.
 *
 * This MUST run before the transmitter's hand-built GDMA setup, and that ordering
 * is not stylistic. i2s_new_channel() acquires IDF's GDMA group for the first
 * time, and gdma_acquire_group_handle() does:
 *
 *     gdma_ll_enable_bus_clock(group_id, true);
 *     gdma_ll_reset_register(group_id);
 *
 * -- resetting EVERY channel in the block, including the OUT channel the
 * transmitter drives with raw registers. Our transmitter never registers with
 * IDF's GDMA driver, so it holds no reference and IDF has no idea that channel is
 * in use. Measured directly: at boot GDMA_TX_INT_ENA is 0x06 and GDMA_TX_LINK
 * holds the descriptor address 0x0cf700; by the time the first recording session
 * reaches its first line of code -- before pads_take, before chan_enable, before
 * anything that could physically touch the bus -- they read 0x00 and 0. The only
 * thing that ran in between was i2s_new_channel().
 *
 * That is why playback died permanently the first time recording was ARMED rather
 * than during a session, and why it never came back: nothing re-initialises the
 * transmitter afterwards. Creating the channel at boot, ahead of gdma_init(),
 * puts IDF's block reset first so our configuration lands after it and survives.
 *
 * The channel is also never deleted, for the mirror-image reason:
 * gdma_release_group_handle() drops the group refcount to zero and calls
 * gdma_ll_enable_bus_clock(group_id, false), gating the block's bus clock
 * outright. */
bool i2s_rx_chan_create(void)
{
    if (rx_chan)
        return true;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,
                                                            I2S_ROLE_MASTER);

    chan_cfg.dma_desc_num  = RX_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = RX_DMA_FRAME_NUM;

    esp_err_t e = i2s_new_channel(&chan_cfg, NULL, &rx_chan);

    if (e != ESP_OK)
    {
        printf("RX i2s_new_channel failed: %d\n", (int)e);
        rx_chan = NULL;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(i2s_rx_default_rate()),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            /* Left UNUSED on purpose: the transmitter owns these pads whenever
             * we are not recording, and configuring them here would hand them to
             * I2S0 for good. i2s_rx_pads_take() moves them for the duration of a
             * recording instead. */
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws   = I2S_GPIO_UNUSED,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false,
                              .ws_inv = false },
        },
    };

    e = i2s_channel_init_std_mode(rx_chan, &std_cfg);

    if (e != ESP_OK)
    {
        printf("RX i2s_channel_init_std_mode failed: %d\n", (int)e);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return false;
    }

    rx_chan_rate = i2s_rx_default_rate();

    return true;
}

/* Program the IDF channel's clock. The channel is never rebuilt -- see
 * i2s_rx_chan_create() -- so a rate change has to be a reconfig, and IDF only
 * accepts that with the channel disabled. The error is returned rather than
 * printed because the caller has to know: a rate the port could not be made to
 * run at shows up downstream as a reader that never receives a frame, which looks
 * exactly like an absent ADC. */
static esp_err_t rx_chan_set_rate(unsigned long rate)
{
    if (!rx_chan)
        return ESP_ERR_INVALID_STATE;

    if (rx_chan_rate == rate)
        return ESP_OK;

    if (rx_chan_enabled)
    {
        i2s_channel_disable(rx_chan);
        rx_chan_enabled = false;
    }

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    esp_err_t e = i2s_channel_reconfig_std_clock(rx_chan, &clk);

    if (e == ESP_OK)
        rx_chan_rate = rate;

    return e;
}

void pcm_rec_dma_init(void)
{
    /* Normally already created at boot by sink_init(); this is the fallback for a
       boot that skipped it. It does not configure anything about the rate: see the
       note at the end of this function. */
    if (!i2s_rx_chan_create())
        return;

    if (!rx_thread_created)
    {
        rx_thread_created = true;
        semaphore_init(&rx_sem, 1, 0);
        rx_thread_id = create_thread(i2s_rx_thread, &rx_thread_stack,
                                     sizeof(rx_thread_stack), 0, rx_thread_name
                                     IF_PRIO(, PRIORITY_USER_INTERFACE));
    }
    /* Nothing here touches the channel's clock or the ADC. pcm_init_recording()
       runs before pcm_apply_settings() puts the requested capture rate on the
       sink, so the rate is not yet known at this point -- reading it here is what
       left the receiver clocked at whatever playback last asked for while the ADC
       was configured for something else. pcm_rec_dma_start() does both, in that
       order, once the rate exists. */
}

void pcm_rec_dma_close(void)
{
    es7210_stop();

    /* Same rule as stop, and same bound: never delete a channel a task may be
     * blocked on, and never spin here without saying so. */
    {
        int spins = 0;

        while (!rx_thread_idle && spins < 100)
        {
            sleep(1);
            spins++;
        }

        if (!rx_thread_idle)
        {
            printf("RX close: reader still blocked, leaking the channel\n");
            return;
        }
    }

    if (rx_chan && rx_chan_enabled)
    {
        /* Disable, but never delete: see rx_chan_set_rate(). Deleting this
           channel gates the clock on the whole GDMA block and kills the
           transmitter for the rest of the boot. */
        i2s_channel_disable(rx_chan);
        rx_chan_enabled = false;
    }

    /* Belt and braces: pcm_close_recording() always calls this, so making the
     * handback idempotent here means the transmitter can never be left without
     * its clocks however the session ended. */
    i2s_rx_release_bus();
}

void pcm_rec_dma_start(void *addr, size_t size)
{
    if (!rx_chan)
    {
        printf("RX start with no channel\n");
        return;
    }

    rec_ptr = (unsigned char *)addr;
    rec_left = size;
    stat_sessions++;

    /* The rate pcm.c just applied. pcm_record_data() calls pcm_apply_settings()
     * immediately before this, which is the first moment the requested capture
     * rate exists as hardware state rather than as a mixer field: a file at
     * 44.1 kHz, a recorder set to 16 kHz, or pitch_detector asking for 8 kHz.
     * Programming the receiver here -- and the ADC at the same rate, in the same
     * call -- is what makes the capture rate the caller's rather than a side
     * effect of whatever playback last did.
     */
    unsigned long rate = i2s_rx_default_rate();

    esp_err_t e = rx_chan_set_rate(rate);

    if (e != ESP_OK)
        printf("RX clock %lu Hz failed: %d\n", rate, (int)e);

    /* Brings the ADC up at `rate` and hands the receiver the clock pads with it.
     * The result is checked: with the return value dropped, an ADC that never
     * answered on the bus is indistinguishable from one that configured
     * perfectly, and every symptom downstream reads as a fault in the capture
     * path rather than an absent device. The pads come with it because the
     * receiver cannot clock anything out of pads it does not own, and both must
     * happen before the channel is enabled. */
    if (!i2s_rx_take_bus(rate))
        printf("ES7210: init FAILED (no answer at 0x41?)\n");

    e = i2s_channel_enable(rx_chan);
    if (e != ESP_OK)
        printf("RX i2s_channel_enable failed: %d\n", (int)e);
    else
        rx_chan_enabled = true;

    rx_wanted = true;
    rx_running = true;
    semaphore_release(&rx_sem);

    pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
}

void pcm_rec_dma_stop(void)
{
    if (!rx_running)
    {
        /* Still give the clock pads back. The reader clears rx_running itself
         * when it leaves its loop, so a stop that arrives after it will take this
         * branch -- and if the pads were left pointing at I2S0, the transmitter
         * can never clock the codecs again, which kills playback for the rest of
         * the session. Releasing is just rewriting three OUT_SEL registers, so
         * doing it unconditionally is free. */
        i2s_rx_release_bus();
        return;
    }

    rx_wanted = false;
    /* Wake the thread so it observes rx_wanted and leaves its read loop. */
    semaphore_release(&rx_sem);

    /* Wait for it to actually leave before touching the channel: disabling or
     * deleting an IDF I2S channel while a task is blocked inside
     * i2s_channel_read() on it is a deadlock.
     *
     * Bounded, and it reports when the bound expires. An earlier version spun
     * here with no limit and no output, which turned "the reader is stuck" into an
     * unexplained freeze of the whole recorder -- the hang looked like a UI
     * problem rather than a thread that never came back. */
    {
        int spins = 0;

        while (!rx_thread_idle && spins < 100)   /* ~1 s at 10 ms/tick */
        {
            sleep(1);
            spins++;
        }

        if (!rx_thread_idle)
            printf("RX stop: reader still blocked after %d ticks, "
                   "leaving channel enabled\n", spins);
    }

    if (rx_thread_idle && rx_chan && rx_chan_enabled)
    {
        i2s_channel_disable(rx_chan);
        rx_chan_enabled = false;
    }

    rx_running = false;
    i2s_rx_release_bus();
}

const void *pcm_rec_dma_get_peak_buffer(void)
{
    return rec_ptr;
}

/* pcm_rec_lock() masks the recording completion, not the transfer -- so the reader
 * keeps pulling data while locked and only defers the hand-off. That is what
 * imx233 does by disabling the DMA channel interrupt, and what this driver's
 * predecessor did by masking GDMA_INT_IN_DONE. */
void pcm_rec_lock(void)
{
    rec_locked++;
}

void pcm_rec_unlock(void)
{
    if (rec_locked > 0)
        rec_locked--;
}

void pcm_rec_dma_stats(struct pcm_rx_stats *s)
{
    s->sessions = stat_sessions;
    s->reads    = stat_reads;
    s->bytes    = stat_bytes;
    s->timeouts = stat_timeouts;
    s->errors   = stat_errors;
    s->last_err = stat_last_err;
    s->rate     = rx_chan_rate;
    s->chan     = rx_chan != NULL;
    s->enabled  = rx_chan_enabled;
    s->running  = rx_running;
    s->bus      = i2s_rx_owns_bus();
}
