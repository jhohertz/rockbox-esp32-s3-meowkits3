/***************************************************************************
 * ESP32-S3 I2S1 transmitter + GDMA polled sink.
 *
 * Two things shape this driver:
 *
 * 1. DMA completion is signalled by the GDMA OUT_DONE interrupt. OUT_DONE fires when
 *    the last data of a descriptor has actually been transmitted to the peripheral,
 *    which is the point at which the buffer is safe to overwrite. OUT_EOF fires
 *    earlier, when the data has only been read from memory and is still sitting in the
 *    FIFO, so using EOF would let the CPU overwrite audio that is still being played.
 *
 *    The mixer's completion callback is called from the handler, which is the same
 *    arrangement every other Rockbox audio target uses. sink_lock/sink_unlock mask the
 *    interrupt so the callback is never entered while the mixer is busy elsewhere.
 *
 * 2. GDMA's descriptor address register is 20 bits wide, so descriptors have to
 *    sit inside the 0x3FC00000 window. That is where .bss is placed, so the
 *    descriptor ring and the DMA buffers are plain .bss.
 *
 *    Internal SRAM is served by the L1 D-cache, and GDMA does not go through it.
 *    Everything the DMA reads has to be written back first, and anything the DMA
 *    writes has to be invalidated before the CPU reads it back. ESP-IDF does the
 *    same thing on every descriptor it hands to a DMA engine.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "string.h"

#include "xtensa.h"
#include "i2s-xtensa.h"
#include "gpio-xtensa.h"
#include "es8311.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "audiohw.h"
#include "kernel.h"
#include "tick.h"
#include "pcm_sampr.h"
#include "pcm_mixer.h"
#include "panic.h"

#include <esp_intr_alloc.h>
#include <soc/interrupts.h>

/* ESP-IDF's HAL layer: the register-level programming of the I2S block, computed
 * by IDF rather than by us. This is the layer the I2S driver itself calls, so it
 * covers the clock tree, the fractional modulator, the TDM/standard slot layout
 * and the WS width -- the four things this file used to hand-roll and got wrong
 * one at a time. It deliberately does NOT own GDMA, which is why the sink
 * contract and the descriptor ring below can stay exactly as they are. */
#include <hal/i2s_hal.h>
#include <hal/i2s_types.h>
#include <hal/i2s_ll.h>

/* Stage 1a of the esp_driver_i2s migration. Off by default: with this undefined
 * the file behaves exactly as the frozen baseline in TX-IDF-MIGRATION.md, and
 * the switch is the revert. Flip it to test the next stage; see that file for
 * why the cutover itself cannot be staged any finer. */
/* A/B: baseline hand-rolled TX path, with the payload census. */
/* STAGE 1b FAILED -- see TX-IDF-MIGRATION.md. Left in tree, switched off.
 * The IDF writer receives all-zero mixer buffers ~50% of the time versus 1%
 * on this path, with every TX-side metric identical. See the stage log. */
/* #define TX_USE_IDF_DRIVER */

/* Silence the periodic CHAIN/arena dumps while testing whether the probe is
 * what stalls the writer. See the note in i2s_probe_tick(). */
#define TX_PROBE_QUIET

/* Leave undefined for normal builds. The stop-path probe was localised as a
 * crash suspect by the .noinit breadcrumb trail; see sink_stop(). */
/* #define TX_STOP_PROBE */

/* Keep the I2S channel enabled across sink_stop(). See the note in sink_stop().
 * Undefined = old behaviour (disable the channel on every stop). Applies only to
 * the IDF path, which is compiled out; kept so that path stays buildable. */
#define TX_STOP_KEEPS_CHANNEL

/* Full CHAIN dumps at event sites (16th play, boot snapshot). Off by default:
 * the dump is ~900 bytes, holds the stdout lock for ~100ms, and calling it from
 * a teardown path was the crash the breadcrumb trail localised. The tick task
 * still emits the compact i2s_dma_line() every 2s with this off, so quiet does
 * not mean blind. */
/* #define TX_PROBE_FULL */

#ifdef TX_USE_IDF_DRIVER
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "semaphore.h"

/* Ring geometry. desc_num >= 3 is a correctness requirement, not a tuning
 * choice -- see the comment in i2s_tx_chan_create(). frame_num 256 makes one
 * descriptor 256 * 2 slots * 2 bytes = 1024 bytes, exactly one Rockbox mixer
 * frame, so the consumption report has the same granularity as the core's. */
/* Matched to the MeowKit reference firmware's speaker path, which plays
 * continuously on this exact board through IDF's I2S driver:
 * src/bsp/devices.cpp:1207 sets dma_buf_count = 4 and dma_buf_len = 512.
 *
 * frame_num is in frames, so 512 frames * 2 slots * 2 bytes = 2048 bytes per
 * descriptor and an 8192-byte ring (~46ms). The 256 tried first made one
 * descriptor exactly one Rockbox mixer frame (1024 bytes), which is the worst
 * case for IDF's descriptor fetch: every write has to take a fresh descriptor
 * rather than topping up one in progress. desc_num 4 alone did not fix the
 * gaps, so this is the other half of the geometry.
 *
 * Note the reference also differs in two ways we deliberately do NOT copy:
 * use_apll = true and fixed_mclk = rate * 128. Our working baseline runs
 * PLL160M with MCLK = rate * 256 and the ES8311 programmed to match, so
 * changing those would break a clock that is demonstrably correct -- the audio
 * heard on the IDF path is at the right pitch, which is the evidence that the
 * clock is not the fault. */
/* Diagnostic sizing, not tuning.
 *
 * Every phase inside the writer loop has now been measured and none of them
 * accounts for the stall: wait max 0 (the core always has a buffer), nodata 0,
 * pull max ~104us, write max 11.6ms, park n=0 -- yet inter-write gaps reach
 * 799ms. The time is going outside the loop's own work, i.e. the thread is not
 * being scheduled. That is the structural loss in this migration: the old hook
 * was an ISR, which preempts everything, so refill() ran at the hardware moment
 * regardless of what the codec or UI was doing. This is a thread at
 * PRIORITY_USER_INTERFACE, outranked by PRIORITY_PLAYBACK and PRIORITY_SYSTEM.
 *
 * The ring is therefore the only thing between a deschedule and silence. At 512
 * frames (11.6ms) per descriptor, 4 descriptors is 46ms of runway against
 * 118-799ms stalls. 12 gives 139ms. If the gaps shorten in proportion to this
 * number, starvation is confirmed and the real fix is the priority or an
 * interrupt-driven hook, not the ring.
 *
 * Cost: 12 * 2048 = 24KB of internal DMA-capable RAM.
 *
 * SUPERSEDED -- 2 is the correct value, and 12 was an architectural error rather
 * than a tuning choice. Rockbox's contract is that the driver reports a buffer
 * consumed when it has FINISHED TRANSMITTING; pcm_play_dma_complete_callback()
 * is what makes the mixer produce the next one, so the report IS the pacing
 * signal for the whole audio pipeline. A deep ring makes every report a fiction
 * by the depth of the ring (139ms here), and the core's buffering and latency
 * logic trusts it.
 *
 * IDF 5.5 offers no public tx done-callback (no register_tx_callback, no
 * i2s_done_cb_t anywhere in esp_driver_i2s). The only completion signal is a
 * finished buffer pointer returned to dma.msg_queue by i2s_dma_tx_callback()
 * (i2s_common.c:651), and the only way to observe it is i2s_channel_write(),
 * which blocks on that queue. So the queue has to BE our OUT_DONE:
 *
 *   one descriptor == one mixer frame, ring of 2
 *
 * Then write() returns only once a descriptor has been retired by the hardware,
 * which is exactly the moment the hand-rolled ISR used to fire in, and the
 * consumption report that follows is true. The gaps that motivated going deep
 * were never the ring -- they were sink_stop() tearing the channel down on the
 * play/stop pairs the core issues about once a second. See TX_STOP_KEEPS_CHANNEL.
 */
/* Stage 1b is CLOSED as not viable -- see TX-IDF-MIGRATION.md for the proof from
 * IDF source. GDMA exposes one completion slot and esp_driver_i2s owns it, so
 * i2s_channel_write() cannot coexist with the per-frame handover Rockbox's
 * contract requires. Everything guarded by TX_USE_IDF_DRIVER is kept as the record
 * of that attempt, not as a live option; the descriptor-depth arguments that used
 * to sit here (24 versus 2) belonged to the abandoned design. */
#define TX_DMA_DESC_NUM   2
/* 256 frames * 2 slots * 2 bytes = 1024 bytes = exactly one Rockbox mixer
 * frame (MIX_FRAME_SAMPLES * 4). Deliberate: it makes one descriptor and one
 * handover the same unit, so write() returning means "one frame has left the
 * hardware", which is the event pcm_play_dma_complete_callback() is supposed to
 * report. At 512 a descriptor held two mixer frames and the report was off by
 * half a descriptor even at the correct ring depth. */
/* THE MIS-WIRING. dma_frame_num is in frames, and one frame is one stereo
 * sample pair, so 512 made each descriptor 512 * 2 slots * 2 bytes = 2048 bytes
 * while Rockbox hands over MIX_FRAME_SAMPLES*4 = 1024-byte mixer frames.
 *
 * Every descriptor was therefore half filled by us and half filled by IDF's
 * auto_clear zeroing: sound, silence, sound, silence, at descriptor
 * granularity. That is the burst-wait-burst, and it is why the payload census
 * read ~75% silent frames while NO audio was actually missing -- the silence
 * was extra material inserted into the stream, not audio being dropped. It is
 * also why deepening the ring never helped: a bigger ring just meant more
 * half-empty descriptors in the cycle.
 *
 * 256 frames * 2 * 2 = 1024 bytes == exactly one mixer frame, so a handover
 * completes a descriptor instead of half-filling it.
 *
 * The reference firmware's dma_buf_len = 512 was NOT a mistake on its side: it
 * pairs 512-byte... no -- it writes audio in chunks of its own choosing, so its
 * descriptor size matches its chunk size. Ours must match OURS. */
#define TX_DMA_FRAME_NUM  256

/* i2s_set_frequency() sits above the Stage 1b block that defines these. */
static bool tx_set_rate(unsigned long rate);
bool i2s_tx_chan_create(void);
bool i2s_tx_chan_configure(unsigned long rate);
bool i2s_tx_chan_enable(bool en);
void i2s_tx_thread_start(void);
void i2s_tx_probe(void);
#endif
#include <esp_rom_gpio.h>
#include <esp_clk_tree.h>
#include <esp_private/esp_clk.h>
#include <esp_cpu.h>

/* The transmitter port. ESP32-S3 has no soc/i2s_periph.h and therefore no
 * I2S_NUM_1 enumerator; the port index is simply the register block, I2S1 at
 * 0x6002D000. */
#define XT_I2S_TX_PORT 1
/* The receiver lives on I2S0 (0x6000F000): the signal map gives each port a
 * single data signal, so the ADC's data line cannot share I2S1's. */
#define XT_I2S_RX_PORT 0

#ifdef HAVE_RECORDING
#include "es7210.h"
#endif

/* MCLK, SLOT_BITS, SLOT_NUM, I2S_BCLK_DIV, the pin map and the descriptor layout
 * all come from i2s-xtensa.h. */

/* XTAL is the clock source. APLL would be more accurate but needs the clock
 * tree, which Rockbox does not own. */
/* The I2S source clock, matching CLKM_CONF_CLK_SEL_PLL160M. This used to be
 * XTAL_HZ 40 MHz with clk_sel 0; the divider below divides THIS number, so the
 * source selection and the dividend have to move together or MCLK is wrong by
 * a factor of four. */
#define I2S_CLK_SRC_HZ 160000000u
#define MAX_FRACT 512
#define MAX_INTEG 256

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/* Ring of two descriptors, one per half of the ping-pong pair. */
/* GDMA takes the descriptor address from a 20-bit register based at
 * 0x3FC00000, so a descriptor in the PSRAM window loses its high bits. Plain
 * .bss is already the answer under ESP-IDF: it is placed in the internal DRAM
 * segment. The .srambss attribute named a section of Rockbox's own linker
 * script, which this path does not use - and a section name nothing declares
 * is not a request. Same for the buffers below.
 *
 * volatile: the DMA writes the descriptor back after it transmits the buffer, so the
 * owner bit changes through a store the compiler cannot see. The handler reads it to
 * decide whether a half is free, and without the qualifier that test can be hoisted.
 *
 * cur is volatile: refill() calls write_desc() from inside the ISR and sink_play()
 * calls it from thread context without taking the lock, and the probe reads it from
 * a third context. Without the qualifier the compiler may cache it across those
 * boundaries. An earlier note here claimed the interrupt mask made it safe, but
 * sink_play takes no lock at all. */
/* aligned(32), not aligned(4): the struct type asks for a cache line each, but a
 * variable attribute overrides a type attribute, and leaving this at 4 put the
 * array on a 16-byte boundary so the two descriptors still straddled shared
 * lines. GDMA itself only requires 4-byte alignment. */
/* Four slots, and every one of them is always owned by the DMA.
 *
 * Measured, not assumed: with a two-slot ring the engine retires slot 0 and
 * fetches slot 1 within about a microsecond, while the refill that would arm it
 * runs in the out_eof interrupt and has to report the consumption, let the mixer
 * build the next frame, memcpy it and commit the line -- an order of magnitude
 * slower than the fetch it is racing. out_check_owner then makes the engine take
 * that as a descriptor error, and the error handler reset cur to 0, which put the
 * driver and the engine permanently out of phase: write_desc armed slot 0 on both
 * of its calls (hist=0,0) and slot 1 was never armed at all.
 *
 * Ring geometry, measured rather than estimated.
 *
 * Four slots of XT_TX_SLOT each = 23ms of runway. This is the geometry the audio
 * was judged best on, and it independently matches the board's reference firmware
 * (dma_buf_count = 4).
 *
 * Two facts decide depth:
 *
 * 1. OUT_DONE on this GDMA means "transferred to the peripheral", NOT "played by
 *    the DAC". The I2S FIFO holds ~256 bytes = 1.45ms, so it bridges nothing. A
 *    two-slot ring receives the DONE for slot 0 while the engine is already
 *    heading back to it (measured: fifo f0/e1); four gives the engine three more
 *    descriptors -- 17ms -- before the slot just rearmed is needed again.
 *
 * 2. Depth is NOT the lever for DSCR_ERR, and err is not a harm metric. It counts
 *    wraps of the ring, and a wrap costs one error per slot, so seven slots
 *    reported 257 where four reported 19 for comparable audible harm. That is how
 *    a depth experiment came out looking like deeper rings were worse. The actual
 *    cause was in the completion handler: a locked completion deferred the whole
 *    refill, leaving the retired slot CPU-owned in a ring the engine keeps
 *    walking. Arming unconditionally (see gdma_irq) removes that, so a long
 *    sink_lock no longer starves the ring and runway is not the constraint.
 *
 * A slot with no data yet is armed with silence and never left unowned: the
 * engine must always be permitted to take the next descriptor. */
#define XT_TX_RING 4

static volatile struct gdma_desc desc[XT_TX_RING] __attribute__((aligned(32)));
/* GDMA requires a 4-byte aligned buffer address */
/* One mixer frame: MIX_FRAME_SAMPLES * 4 = 1024 bytes, the size the core actually
 * hands over (measured constant across thousands of handovers). The slot was
 * previously twice this; the upper half was never written and never transferred,
 * and that dead 4KB overflowed dram0_0_seg -- a region tight enough that two int
 * diagnostic counters overflowed it by exactly 8 bytes. The static assert in
 * write_desc() bounds the slot to what it enforces, so a larger handover can
 * never silently overrun; it fails the build instead. */
#define XT_TX_SLOT (MIX_FRAME_SAMPLES * 4)

static unsigned char dma_buf[XT_TX_RING][XT_TX_SLOT] __attribute__((aligned(4)));

/* cur is the slot that is free to write. When DONE fires, the slot that just
 * finished transmitting is the one retire points at, so the mixer's next frame
 * goes into cur. Both advance modulo XT_TX_RING -- with a plain xor the upper
 * slots of the ring would never be armed at all. */
/* One buffer handed down by the core and not yet placed in a descriptor. The
 * core pushes through sink_play(); the completion interrupt pulls. */
static volatile const void *pend_addr;
static volatile size_t pend_size;
static volatile int pend_valid;
/* Guards a hardware interrupt mask, and is read from the probe while the mixer
 * holds it, so it must not be cached in a register across the read-modify-write
 * of GDMA_TX_INT_ENA. */
static volatile int locked;
static bool playing;
/* Descriptors armed and awaiting transmission. The ISR decrements it as each one
 * retires; refill() only asks the core for a buffer when the ring has run dry,
 * because in the normal case reporting the completion causes ops.play to hand us
 * the next one before we ever need to ask. */
static volatile int queued;
/* The slot that has just been retired and may be rearmed. The engine retires
 * slots in the order we arm them, so one modulo counter tracks it. */
static volatile int done_slot;
/* Rockbox's own convention for a completion arriving inside the play lock
 * (as3525's play_callback_pending): record it, replay it on unlock. */
static volatile int cb_pending;
/* Length of the last real frame, so an armed silence slot matches the timing. */
static volatile unsigned long tx_frame_len = MIX_FRAME_SAMPLES * 4;
/* DSCR_ERR entries with no intervening DONE. Two in a row means the engine is
 * hitting a slot the CPU owns, and restarting onto it again is a storm -- the
 * interrupt-watchdog panic measured at first play with a free-running ring. */
static volatile int dscr_streak;
/* Splits err by when it happens. The gate experiment showed errors per switch are
 * the same with and without it (38/10/10/28 vs 7/52/10/16), so the source is the
 * transition itself, not a handler restarting a stopped transport. err_startup
 * counts DSCR_ERR entries raised before the current session's first completion:
 * those mean the walk was started over a ring that was not ready. err_late is the
 * remainder -- errors during steady playback, i.e. a refill that lost the race. */
static volatile int err_startup;
static volatile int irq_at_start;
/* Direct session count. Inferring sessions from (refill - irq) proved
 * unreliable: eighteen manual track switches produced only two observed
 * prerolls, so err-per-switch could not be computed and err-per-frame was
 * dominated by how many transitions happened. This counts start_tx() itself,
 * making err/session an exact ratio. It also answers the side question the
 * discrepancy raised: whether a track change stops and restarts the sink at all. */
static volatile int xt_tx_sessions;

/* IDF interrupt handle for the GDMA OUT channel, allocated once in gdma_init().
 * Keeping the handle is what makes the allocation idempotent: i2s_init() can be
 * reached again and a second esp_intr_alloc() on the same source would fail. */
static intr_handle_t tx_intr;

/* gdma_irq() is defined further down with the rest of the transmit path, but the
 * allocation that installs it happens earlier, so it has to be named first. The
 * wrapper exists because esp_intr_alloc() invokes handlers with a void *argument.
 * Referencing gdma_irq() here is also the only thing that keeps it out of
 * --gc-sections, which is what silently deleted it before. */
void ICODE_ATTR gdma_irq(void);

static void gdma_tx_isr(void *arg)
{
    (void)arg;
    /* Nothing may print here. A printf in this handler takes the newlib stdout
       lock, lock_acquire_generic() sees it claimed from interrupt context and
       aborts the process -- measured backtrace: gdma_tx_isr -> printf ->
       _vfprintf_r -> __retarget_lock_acquire_recursive -> lock_acquire_generic
       -> abort. gdma_irq() already counts entries in xt_dma_irq_n, which
       i2s_audio_probe() reports from thread context, so nothing is lost by
       keeping quiet. */
    gdma_irq();
}


/* IDF's HAL context for the transmitter. The HAL is a thin wrapper over the
 * register block, so the context is just the peripheral base address. */
static i2s_hal_context_t tx_hal;

#ifdef HAVE_RECORDING
/* The rate i2s_set_frequency() last programmed; the recording session's IDF
 * channel has to be created at this rate. Declared here because i2s_set_frequency
 * is where it is written, which precedes the receive section of this file. */
static unsigned long rx_rate;
#endif

/* TEMPORARY (transitional measurement only): the registers as IDF's HAL left
 * them, so they can be diffed against the values this file used to write by
 * hand. Remove together with the last hand-rolled setup. */
static void i2s_reg_snapshot(const char *tag);

/* TEMPORARY: one counter per link in the playback chain, so "it does not play"
 * becomes a statement about where the chain stops rather than which link is
 * suspected. Printed from the button probe, which is known to emit. Remove once
 * playback works.
 *
 * init  : sink_init() ran, so i2s_init() and audiohw_preinit() ran
 * play  : the PCM layer handed us a buffer to transmit
 * stop  : the PCM layer told us to stop
 * irq   : the GDMA out_eof interrupt actually fired and we refilled
 * lastsz: size of the most recent play() buffer, to show the mixer's frame shape
 */
volatile unsigned long xt_sink_init_n;
volatile unsigned long xt_sink_play_n;
volatile unsigned long xt_sink_stop_n;
volatile unsigned long xt_sink_irq_n;
volatile unsigned long xt_sink_last_size;
/* TEMPORARY: refill() path trace -- see the comment on refill(). */
volatile unsigned long xt_refill_n;
volatile int xt_dscr_err_n;
volatile int xt_refill_branch;
/* What write_desc() actually did, rather than what its caller implies it did.
 * Every attempt to reconstruct the stall from side effects has produced a
 * contradiction, so record the action itself: which slot was armed, the dw0
 * value written, and how many times the routine ran. */
volatile int  xt_wd_n;
volatile int  xt_wd_last_idx = -1;
volatile unsigned long xt_wd_last_dw0;
volatile int  xt_wd_idx_hist[4] = { -1, -1, -1, -1 };
volatile unsigned long xt_wd_dw0_hist[4];

/* TEMPORARY: the whole playback chain as one aligned readout, the way the
 * scheduler was finally diagnosed -- every stage's own view of what it is doing,
 * sampled at the same instant and emitted as a single printf so no other thread
 * can land between two stages. The answer is the seam where two adjacent stages
 * disagree, never one counter on its own:
 *
 *   insert climbing, pcmbuf used climbing, mixer cb static
 *        -> the mixer is not being driven at all
 *   mixer cb climbing, next non-zero, sink play static
 *        -> buffers are being mixed but pcm_play_data() is not starting the DMA
 *   core more=(nil)
 *        -> pcm_play_stop_int() has latched the chain off; nothing can resume it
 *   sink play climbing, dma irq static
 *        -> the ring is fed but the transmitter is not completing
 *
 * locked, int_ena and int_raw are included because sink_lock() masks out_eof and
 * sink_unlock() unmasks it only when the count returns to zero. If the play lock
 * is ever left held the consumer is muted for good: int_raw keeps the pending
 * bit, int_ena has it clear, and the PCM buffer fills with nothing able to drain
 * it -- which is what a stalled consumer looks like from upstream.
 *
 * pcmbuf used/free is the pressure quantity: it is what should decide whether
 * there is anything to hand down. Clock and reset state are included because a
 * peripheral held in reset reads back zero and is otherwise indistinguishable
 * from a wrong base address.
 */
/* TEMPORARY: the whole playback chain as one aligned readout -- every stage's
 * own view, sampled at the same instant and emitted as a single printf so no
 * other thread can land between two stages. The answer is the seam where two
 * adjacent stages disagree, never one counter on its own.
 *
 * Hardware flags are printed DECODED. Two separate misreads cost most of a day
 * here: out_eof was taken for bit 0 when the BIT() define says bit 1, and the
 * GDMA FIFO status bits were read as full when bits 0/2/4 are full and 1/3/5 are
 * empty -- both from parsing the comment block instead of the defines. Nothing
 * below asks the reader to know a bit position.
 */
void i2s_audio_probe(void)
{
    extern volatile unsigned long xt_codec_run_n;
    extern volatile int xt_codec_in_run;
    extern volatile unsigned long xt_pcminsert_n, xt_bufget_n, xt_startfill_n;
    extern volatile unsigned long xt_mix_cb_n;
    extern size_t pcm_mixer_probe_next(void);
    extern int pcm_mixer_probe_index(void);
    extern size_t pcmbuf_free(void);
    extern size_t pcmbuf_get_bufsize(void);
    extern int pcmbuf_used_descs(void);
    extern int pcmbuf_descs(void);
    extern volatile pcm_play_callback_type pcm_callback_for_more;
    extern enum pcm_dma_status
        (* volatile pcm_play_status_callback)(enum pcm_dma_status);

    size_t bufsz = pcmbuf_get_bufsize();
    size_t used = bufsz - pcmbuf_free();
    uint32_t f_pll160 = 0, f_xtal = 0;
    unsigned int gs = rd(GDMA_TX_STATE);
    unsigned int fs = rd(GDMA_TX_FIFO_STATUS);

    /* The DMA writes descriptor results back through the bus, not through our
     * D-cache, so the cached line for the ring can still hold what we wrote
     * rather than what the engine left. Drop it first: without this the owner
     * bits printed below describe the CPU's memory, not the hardware's, and can
     * contradict every other reading on this line. */
    discard_dcache_range((const void *)desc, sizeof(desc));

    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_PLL_F160M,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &f_pll160);
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &f_xtal);

    printf("CHAIN: codec run=%lu in=%d | insert=%lu bufget=%lu startfill=%lu\n"
           "CHAIN: pcmbuf used=%lu/%lu descs=%d/%d | mixer cb=%lu next=%lu idx=%d\n"
           "CHAIN: core playing=%d more=%p status=%p | sink play=%lu stop=%lu\n"
           "CHAIN: dma irq=%lu refill=%lu branch=%d queued=%d locked=%d ena=%08x\n"
           "CHAIN: fifo L1 f%d/e%d L2 f%d/e%d L3 f%d/e%d | raw eof=%d dscr=%d tot=%d\n"
           "CHAIN: gdma out=%d dscr=%d park=%d | err=%d | link=%08x own=%d%d%d%d len=%d,%d,%d,%d\n"
           "CHAIN: wd n=%d lastidx=%d wrote=%08x hist=%d,%d,%d,%d pre=%08x\n"
           "CHAIN: i2s idle=%d started=%d | clk sel=%d active=%d en=%d | pll160=%lu xtal=%lu\n",
           xt_codec_run_n, xt_codec_in_run,
           xt_pcminsert_n, xt_bufget_n, xt_startfill_n,
           (unsigned long)used, (unsigned long)bufsz,
           pcmbuf_used_descs(), pcmbuf_descs(),
           xt_mix_cb_n, (unsigned long)pcm_mixer_probe_next(),
           pcm_mixer_probe_index(),
           pcm_playing, (void *)pcm_callback_for_more,
           (void *)pcm_play_status_callback,
           xt_sink_play_n, xt_sink_stop_n,
           xt_sink_irq_n, xt_refill_n, xt_refill_branch, queued, locked,
           (unsigned)rd(GDMA_TX_INT_ENA),
           /* GDMA_OUTFIFO_FULL_Ln = BIT(0/2/4), EMPTY_Ln = BIT(1/3/5) */
           (int)((fs >> 0) & 1), (int)((fs >> 1) & 1),
           (int)((fs >> 2) & 1), (int)((fs >> 3) & 1),
           (int)((fs >> 4) & 1), (int)((fs >> 5) & 1),
           /* GDMA_OUT_EOF = BIT(1), DSCR_ERR = BIT(2), TOTAL_EOF = BIT(3) */
           (int)((rd(GDMA_TX_INT_RAW) >> 1) & 1),
           (int)((rd(GDMA_TX_INT_RAW) >> 2) & 1),
           (int)((rd(GDMA_TX_INT_RAW) >> 3) & 1),
           /* GDMA_OUT_STATE = 3 bits at 20, OUT_DSCR_STATE = 2 bits at 18 */
           (int)((gs >> 20) & 0x7), (int)((gs >> 18) & 3),
           (int)((rd(GDMA_TX_LINK) >> 23) & 1),
           xt_dscr_err_n, (unsigned)rd(GDMA_TX_LINK),
           (int)((desc[0].dw0 >> 31) & 1), (int)((desc[1].dw0 >> 31) & 1),
           (int)((desc[2].dw0 >> 31) & 1), (int)((desc[3].dw0 >> 31) & 1),
           (int)((desc[0].dw0 >> 12) & 0xfff), (int)((desc[1].dw0 >> 12) & 0xfff),
           (int)((desc[2].dw0 >> 12) & 0xfff), (int)((desc[3].dw0 >> 12) & 0xfff),
           xt_wd_n, xt_wd_last_idx, (unsigned)xt_wd_last_dw0,
           xt_wd_idx_hist[0], xt_wd_idx_hist[1],
           xt_wd_idx_hist[2], xt_wd_idx_hist[3],
           (unsigned)(xt_wd_n > 0 && xt_wd_n <= 4
                          ? xt_wd_dw0_hist[xt_wd_n - 1] : 0),
           (rd(I2S_TX_STATE) & TX_STATE_IDLE) ? 1 : 0,
           (rd(I2S_TX_CONF) & TX_CONF_START) ? 1 : 0,
           (int)((rd(I2S_TX_CLKM_CONF) >> 27) & 3),
           (rd(I2S_TX_CLKM_CONF) & CLKM_CONF_CLK_ACTIVE) ? 1 : 0,
           (rd(I2S_TX_CLKM_CONF) & CLKM_CONF_CLK_EN) ? 1 : 0,
           (unsigned long)f_pll160, (unsigned long)f_xtal);
}


/* Best rational approximation of freq_error / exp_freq with denominator < max.
 * Ported from ESP-IDF's hal_utils_calc_clk_div_frac_accurate. */


/* i2s_ll_tx_set_mclk's mapping from (integer, num/den) to the four registers. */

void i2s_set_frequency(unsigned long rate)
{
    /* Clock and slot configuration, computed by IDF's HAL.
     *
     * i2s_hal_set_tx_clock() picks the source, programs the integral and
     * fractional modulator and the BCLK divider, and pulses tx_update; it
     * replaces calc_div, set_raw_div and program_div entirely. The slot call sets
     * the sample bits, the WS width (which this file had left at zero, giving a
     * one-BCLK frame) and the TDM channel mask, and selects standard mode via the
     * TDM path.
     */
    i2s_hal_clock_info_t clk = {
        .sclk     = I2S_CLK_SRC_HZ,
        .mclk     = (uint32_t)(rate * MCLK_MULTIPLE),
        .bclk     = (uint32_t)(rate * SLOT_BITS * SLOT_NUM),
        .mclk_div = MCLK_MULTIPLE,
        .bclk_div = I2S_BCLK_DIV,
    };
    hal_utils_clk_div_t mclk_div;

#ifndef TX_USE_IDF_DRIVER
    i2s_hal_set_tx_clock(&tx_hal, &clk, I2S_CLK_SRC_DEFAULT, &mclk_div);

    i2s_hal_slot_config_t slot = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
        .slot_mode      = I2S_SLOT_MODE_STEREO,
        .std = {
            .slot_mask     = I2S_STD_SLOT_BOTH,
            .ws_width      = SLOT_BITS,
            .ws_pol        = false,
            .bit_shift     = true,
            .left_align    = true,
            .big_endian    = false,
            .bit_order_lsb = false,
        },
    };

    /* Master role: this side generates BCLK and WS and feeds MCLK to both codecs. */
    i2s_hal_std_set_tx_slot(&tx_hal, false, &slot);
    i2s_hal_std_enable_tx_channel(&tx_hal);
#else
    (void)clk;
    (void)mclk_div;

    /* IDF programs clock and slot together; tx_set_rate() does disable ->
     * reconfig_std_clock and start_tx() re-enables. */
    tx_set_rate(rate);
#endif

    i2s_reg_snapshot("after hal config");

    es8311_set_clock(rate, rate * MCLK_MULTIPLE);

    /* The ADC shares the transmitter's MCLK, so it has to be reprogrammed
     * whenever the rate changes.
     *
     * The receiver is no longer configured here. It is a full ESP-IDF I2S channel
     * created per recording session in pcm-xtensa.c, which sets its own clock and
     * slot. All this file still owes the capture path is the sample rate the
     * session must match, recorded below, and the shared clock pads. */
#ifdef HAVE_RECORDING
    rx_rate = rate;
    es7210_set_frequency(rate);
#endif
}

static void refill(void);      /* defined with the transmit path below */
static void fill(int i, const void *src, unsigned long len);

static void arm(int i, unsigned long len)
{
    desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
    /* Zero terminates the link list: the engine stops rather than wrapping.
     *
     * Three variants, and the record matters because two of them were conflated
     * for a long time:
     *
     *  - next = &desc[i] (SELF-linked) was tried and is genuinely bad: the engine
     *    follows one slot forever, giving one out_dscr_err per frame (err tracked
     *    irq exactly, +1) and a noticeable lag on pause while it replayed the last
     *    buffer. That measurement is real and is what motivated this line.
     *
     *  - next = &desc[(i+1) % XT_TX_RING] (a real ring, which is what gdma_init()
     *    already builds before the first arm() call un-chains it) was tried on the
     *    reasoning that self-linking does not predict it -- the engine walks
     *    forward instead of looping, and the owner bit already gates every slot.
     *    Result: no improvement. Clicks at frame boundaries persisted, and the CHAIN
     *    probe captured no non-zero gdma counters during playback at all, so there
     *    is no evidence the ring ever engaged, and no evidence that it did not.
     *    Reverted. This is NOT a refutation of the ring; it is an inconclusive
     *    experiment, and the reason is the next bullet.
     *
     *  - next = 0 (what is here) is the encoding of imx233's `dma.next = NULL`:
     *    the transfer ends and the driver restarts it deliberately.
     *
     * The blocker on all of this is observability, not the descriptor link. The
     * CHAIN dump does not sample during steady playback, so a per-frame property
     * like slot occupancy cannot be measured by it -- every comparison so far has
     * been against a snapshot taken at some other moment. Before this line is
     * changed again, the probe needs a periodic sample while audio is running;
     * otherwise any result here is unfalsifiable. */
    /* Keep the chain gdma_init() built -- see the note in gdma_init() on why
     * auto-writeback and next = 0 are the one combination that cannot work. */
    desc[i].next   = (uint32_t)(unsigned long)&desc[(i + 1) % XT_TX_RING];
    /* len 0 keeps the slot owned by the DMA with nothing to move: the engine
     * takes it, transfers nothing, and moves on. That is what makes an empty
     * slot safe rather than fatal. */
    desc[i].dw0 = XT_TX_SLOT | (len << 12) | DESC_SUC_EOF | DESC_OWNER;
}

/* Place one frame into slot i and hand it to the DMA. len 0 leaves the slot
 * owned with nothing to move, which is what keeps the ring fully owned between
 * hand-offs. */
/* TEMPORARY: sample-width capture. fill() runs inside gdma_tx_isr and may not
 * print, so it stores the frame head here and i2s_probe_tick() prints it from
 * thread context. ready is set last and cleared by the printer, so a frame
 * captured mid-print is simply reported next time round. */
static struct {
    volatile int ready;
    volatile int n;
    volatile int nonzero;
    volatile unsigned long len;
    volatile unsigned long filln;
    unsigned short data[128];
} xt_width;

/* Baseline A/B counters. fill() runs in ISR context, so this only accumulates;
 * the print lives in i2s_probe_tick() (thread context). Sampling every 16th
 * call keeps the ISR cost bounded -- a full 1024-byte scan on every frame would
 * itself become the dropout we are trying to observe. */
static volatile unsigned long bl_n, bl_zero, bl_sum;

/* Arm a slot with silence. auto-writeback means the engine walks the ring
 * whether or not we have data, so an empty slot still has to be owned and filled;
 * that is the invariant this mode buys and the price it charges. */
static void fill_silence(int i)
{
    unsigned long len = tx_frame_len;

    memset(dma_buf[i], 0, len);
    discard_dcache_range((const void *)&desc[i], sizeof(desc[i]));
    arm(i, len);
    commit_dcache_range(dma_buf[i], len);
    commit_dcache_range((const void *)&desc[i], sizeof(desc[i]));
}

/* Take the walk down, make every slot owned and filled, and start exactly once.
 *
 * This is the storm guard. With a free-running ring a single unowned descriptor
 * is self-sustaining: the engine reaches it, raises out_dscr_err, and the handler
 * answers with link.start -- onto the same unowned descriptor. Measured as an
 * interrupt-watchdog panic on CPU0 in ISR context at first play. Re-arming the
 * WHOLE ring before restarting is what makes the second error impossible rather
 * than unlikely, because there is then no slot the CPU owns anywhere in the
 * cycle. */
static void tx_recover(void)
{
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */

    for (int i = 0; i < XT_TX_RING; i++)
    {
        if (pend_valid)
        {
            fill(i, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
        }
        else
            fill_silence(i);
    }

    done_slot = 0;
    dscr_streak = 0;

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);

    /* Re-arming the ring is right whether or not we are playing; starting the
     * walk is not. See the note in the DSCR_ERR branch. */
    if (playing)
        wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start */
}

static void fill(int i, const void *src, unsigned long len)
{
    if (len)
        tx_frame_len = len;

    memcpy(dma_buf[i], (const void *)src, len);

    {
        static unsigned long bl_ctr;
        if (++bl_ctr >= 16)
        {
            const unsigned char *bb = (const unsigned char *)src;
            unsigned long nz = 0, j;

            bl_ctr = 0;
            for (j = 0; j < len; j++)
                nz += (bb[j] != 0);
            bl_n++; bl_sum += nz;
            if (nz == 0) bl_zero++;
        }
    }

    /* What width is actually on the wire? The mixer frame is
     * MIX_FRAME_SAMPLES*4 = 1024 bytes, which reads as 32-bit samples, but
     * i2s_init() configures I2S_DATA_BIT_WIDTH_16BIT in
     * I2S_SLOT_BIT_WIDTH_16BIT slots. If both are true the DAC is fed the low half
     * of every 32-bit sample as a separate channel: near zero and harmless when
     * quiet, garbage when loud. That is amplitude-dependent distortion with a
     * compressed character, which is what it sounds like.
     *
     * This is in fill(), not sink_play(), and that took four attempts to get
     * right. In Rockbox's buffered model ops.play() fires ONCE per playback to arm
     * the initial buffer; every later frame arrives through the pcm_play_data
     * callback, which lands here. A probe in sink_play() therefore samples one
     * frame per session -- which is exactly why it printed nothing across 18k
     * frames while the counter never reached its threshold.
     *
     * It may not PRINT, though. fill() is reached from refill() inside
     * gdma_tx_isr, and the comment at the top of that handler says outright that
     * nothing may print there: printf takes the newlib stdout lock and aborts.
     * Putting it here froze the device about 1.2 s in, at the sample point, which
     * was the probe and not the audio. So the ISR only copies; i2s_probe_tick()
     * prints it from thread context. */
    {
        const unsigned short *w = (const unsigned short *)src;
        static unsigned long fill_n;
        int i2, nonzero = 0;
        int n = 128;

        if ((unsigned long)n > len / 2)
            n = (int)(len / 2);

        fill_n++;
        if (!xt_width.ready && fill_n >= 200 && n > 0)
        {
            for (i2 = 0; i2 < n; i2++)
            {
                if (w[i2] != 0)
                    nonzero++;
            }

            for (i2 = 0; i2 < n; i2++)
                xt_width.data[i2] = w[i2];

            xt_width.n = n;
            xt_width.nonzero = nonzero;
            xt_width.len = len;
            xt_width.filln = fill_n;
            xt_width.ready = 1;     /* last: the printer reads this first */
        }
    }

    /* Take the line from memory before modifying it, then push it back. The
     * engine wrote the owner bit clear through the bus, and our cached copy of
     * this line still has whatever we put there. */
    discard_dcache_range((const void *)&desc[i], sizeof(desc[i]));
    arm(i, len);
    commit_dcache_range(dma_buf[i], len ? len : XT_TX_SLOT);
    commit_dcache_range((const void *)&desc[i], sizeof(desc[i]));

    /* Arming a descriptor is not enough on its own, and this is the step that
     * was missing.
     *
     * outlink.park is not an error condition -- it is the engine doing what it is
     * designed to do when it reaches a descriptor the CPU still owns: it stops
     * walking the list and waits. But it does not re-examine the list on its own,
     * so a descriptor armed afterwards is invisible unless the walk is restarted.
     * That is what gdma_ll_tx_restart() is for ("restart a new outlink right
     * after the last descriptor") and what IDF's gdma_append() does on every
     * hand-off.
     *
     * Without it the very first underrun latches the transmitter off forever,
     * which is why park read 1 in every capture taken since the ring rewrite.
     *
     * Bits from gdma_reg.h: outlink.stop BIT(20), start BIT(21), restart BIT(22),
     * park BIT(23); start and restart are R/W/SC. */
    unsigned int link = rd(GDMA_TX_LINK);

    if (link & (1u << 23))
        wr(GDMA_TX_LINK, link | (1u << 22) | (1u << 21));   /* restart | start */
}


static void gdma_init(void)
{
    /* GDMA channel 0 -> I2S1 */
    wr(GDMA_TX_PERI_SEL, GDMA_TRIG_I2S1);

    /* conf0: outdscr_burst_en | out_auto_wrback -- a free-running ring.
     *
     * The two GDMA settings are a pair, not a choice, and getting them
     * mismatched is what produced the original crackle. The failure mode that
     * was actually measured: auto-writeback ON together with arm() writing
     * next = 0, on the misunderstanding that "zero terminates the link list".
     * This GDMA has no such convention -- with auto-writeback on the engine
     * follows next unconditionally, so it fetched its next descriptor from
     * address 0x00000000: garbage, owner clear, length zero. err tracked irq
     * exactly 1:1 (360 per 379 interrupts), one descriptor fault per audio
     * frame at 172/s, each one stalling the transport until the handler
     * restarted it. That stall was the crackle.
     *
     * Auto-writeback OFF was the previous design: the engine halted after each
     * descriptor and the DONE handler re-armed desc[0] and did link.start. That
     * is the imx233 model, whose DMA really does treat NULL as a stop. Its cost
     * was the second crackle: one 5.8ms descriptor in flight, and a restart gap
     * opened on the I2S bus inside every interrupt 172 times a second -- OUT_DONE
     * means "transferred to the peripheral", not "played", and the I2S FIFO is
     * only ~256 bytes (1.45ms), so nothing bridges that gap.
     *
     * The shipping combination is auto-writeback ON with next chained through all
     * four slots and every slot owned and filled at all times, guaranteed by
     * start_tx()'s preroll, the ISR's refill-the-completed-slot rule and
     * tx_recover(). Depth is what makes the refill race winnable: the engine
     * takes three more descriptors (17ms) between a slot's DONE and the moment it
     * needs that slot again. next = 0 with either setting is the combination that
     * cannot work. */
    wr(GDMA_TX_CONF0, (1u << 4) | (1u << 2));   /* outdscr_burst_en | out_auto_wrback */
    /* conf1: check the owner bit when mounting buffers */
    wr(GDMA_TX_CONF1, (1u << 12));               /* out_check_owner */

    /* Install the completion handler through ESP-IDF.
     *
     * This used to route the interrupt by hand -- writing the per-source slot
     * register at INTERRUPT_CORE0 and setting INTENABLE directly. That is the
     * bare-metal shape, and it cannot work here: IDF owns the level-1 vector
     * table and dispatches from its own allocation table, so a slot claimed this
     * way has no handler registered with it. Worse, nothing in the image
     * referenced gdma_irq(), so --gc-sections discarded the function outright
     * (verified: it was absent from the linked ELF). The hardware unmasked an
     * interrupt whose service routine had been optimised away, refill() never
     * ran, the descriptor ring starved after two buffers, and playback produced
     * neither sound nor a moving position.
     *
     * esp_intr_alloc() does both halves: it picks a slot IDF will dispatch, and
     * it takes the handler's address, which keeps the symbol alive.
     *
     * The source is ETS_DMA_OUT_CH0_INTR_SOURCE. On ESP32-S3 the GDMA sources
     * carry no "GDMA" in their name -- they are ETS_DMA_OUT_CH0_/ETS_DMA_IN_CH0_
     * -- which is worth stating because the obvious grep finds nothing.
     *
     * No ESP_INTR_FLAG_IRAM: the handler lives in the instruction-cache window
     * via ICODE_ATTR and is not guaranteed IRAM-resident, so it must not be
     * called with the flash cache disabled.
     */
    if (!tx_intr)
    {
        esp_err_t err = esp_intr_alloc(ETS_DMA_OUT_CH0_INTR_SOURCE,
                                       ESP_INTR_FLAG_LEVEL1, gdma_tx_isr,
                                       NULL, &tx_intr);
        if (err != ESP_OK)
            panicf("i2s: cannot allocate the GDMA TX interrupt (%d)", err);
    }

    /* out_eof and out_dscr_err. sink_lock/sink_unlock toggle the out_eof bit
     * from here on. */
    wr(GDMA_TX_INT_ENA, GDMA_INT_DONE | GDMA_INT_DSCR_ERR);
    wr(GDMA_TX_INT_CLR, 0xFF);

    for (int i = 0; i < XT_TX_RING; i++)
    {
        desc[i].buffer = (uint32_t)(unsigned long)dma_buf[i];
        desc[i].next   = (uint32_t)(unsigned long)&desc[(i + 1) % XT_TX_RING];
        desc[i].dw0 = 0;   /* start in the CPU's hands; see sink_play() */
    }
    commit_dcache_range((const void *)desc, sizeof(desc));

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
}

/* The transitional register dump declared above. */
static void i2s_reg_snapshot(const char *tag)
{
    printf("I2SREG %s: conf=%08x conf1=%08x tdm=%08x clkm=%08x div=%08x state=%08x\n",
           tag, (unsigned)rd(I2S_TX_CONF), (unsigned)rd(I2S_TX_CONF1),
           (unsigned)rd(I2S_TX_TDM_CTRL), (unsigned)rd(I2S_TX_CLKM_CONF),
           (unsigned)rd(I2S_TX_CLKM_DIV_CONF), (unsigned)rd(I2S_TX_STATE));
}

#ifdef TX_USE_IDF_DRIVER
/* The IDF transmit channel, created but deliberately NOT yet configured.
 *
 * Stopping at i2s_new_channel() is the whole of Stage 1a, and it is a real
 * boundary rather than an arbitrary one. From IDF source:
 *
 *   i2s_new_channel()            -> i2s_acquire_controller_obj() ->
 *                                   i2s_hal_init(), which is only
 *                                   hal->dev = I2S_LL_GET_HW(port) (i2s_hal.c:64)
 *                                   plus heap, a queue and two semaphores.
 *                                   No GDMA allocation. No register writes.
 *
 *   i2s_channel_init_std_mode()  -> i2s_init_dma_intr() (i2s_common.c:804) ->
 *                                   gdma_new_ahb_channel() ->
 *                                   do_allocate_gdma_channel() ->
 *                                   gdma_acquire_group_handle() ->
 *                                   gdma_ll_reset_register() (gdma.c:696)
 *
 * So the block reset that pcm-xtensa.c:228 documents is real, but it belongs to
 * init_std_mode, not to new_channel. That matters here because our
 * hand-rolled transmitter owns GDMA OUT channel 0 (GDMA_TX_CONF0 =
 * GDMA_BASE + 0x60) and IDF has no idea: the first time init_std_mode runs for
 * a TX channel it will allocate the first free OUT channel and take that one.
 * There is therefore no state in which both transmitters are live, which is why
 * the migration is a switch and an atomic cutover rather than a gradual
 * handover.
 *
 * Creating the channel here is still worth doing on its own: it proves IDF will
 * claim the I2S1 controller while our HAL still drives it, and the probe in
 * sink_init() checks afterwards that the live GDMA configuration survived.
 * That is the claim this stage rests on, so it gets measured rather than
 * trusted. */
static i2s_chan_handle_t tx_chan_idf;

bool i2s_tx_chan_create(void)
{
    if (tx_chan_idf)
        return true;

    /* Sized to match the hand-rolled path for now: one descriptor is
     * dma_frame_num * 2 slots * 2 bytes = 1024 bytes, which is exactly one
     * Rockbox mixer frame. desc_num is the runway-vs-report-accuracy knob and
     * is swept in Stage 3 -- see the table in TX-IDF-MIGRATION.md for why 8
     * made the previous attempt bursty. */
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,
                                                       I2S_ROLE_MASTER);
    /* desc_num MUST be >= 3 on this path, and not for the runway reason.
     *
     * i2s_channel_write() only goes looking for the next descriptor when
     *     uxQueueSpacesAvailable(msg_queue) <= (desc_num > 2 ? 1 : 0)
     * and the queue is created with desc_num - 1 slots. At desc_num = 2 that
     * threshold is 0 against a 1-slot queue, so it never prefetches: it fills
     * one whole descriptor, then blocks in xQueueReceive until THAT descriptor
     * has finished transmitting. The DMA therefore drains a burst, stalls, gets
     * refilled, drains again -- which is exactly the "short burst, silence,
     * next burst" that was heard, with pitch and content correct throughout
     * because the bit clock was never the problem.
     *
     * At 3 the predicate becomes <= 1 and a descriptor is taken while one is
     * still in flight, so the stream is continuous. Sweeping this is Stage 3;
     * 2 was chosen for report accuracy and turned out to remove all pipelining.
     */
    cfg.dma_desc_num  = TX_DMA_DESC_NUM;
    cfg.dma_frame_num = TX_DMA_FRAME_NUM;

    esp_err_t e = i2s_new_channel(&cfg, &tx_chan_idf, NULL);

    if (e != ESP_OK)
    {
        printf("TX1a: i2s_new_channel failed: %d\n", (int)e);
        tx_chan_idf = NULL;
        return false;
    }

    printf("TX1a: IDF TX channel created %p (not configured, not routed)\n",
           (void *)tx_chan_idf);
    return true;
}

/* ---- Stage 1b: the IDF transmit path -------------------------------------
 *
 * Everything below is what replaces gdma_init(), the descriptor ring,
 * arm()/fill() and gdma_irq(). The hand-rolled versions stay in the file under
 * the #else so the revert is a one-line flip of TX_USE_IDF_DRIVER and a
 * rebuild, not a git operation.
 *
 * The shape mirrors the receiver in pcm-xtensa.c deliberately: IDF's API is a
 * blocking call from task context, Rockbox's sink contract is a callback from
 * interrupt context, and a small thread is what converts one into the other.
 * It is the same seam, opposite direction, and the receiver already proves it
 * works on this hardware. */

static i2s_std_config_t tx_std_cfg;
static unsigned long tx_std_rate;          /* rate the channel is set to */
static bool tx_std_configured;
static bool tx_chan_enabled;

/* The frame awaiting transmission, published by sink_play() and consumed by
 * the writer. Written under the pcm core's play lock, so only one producer. */
static const unsigned char *tx_ptr;
static size_t tx_size;
static volatile bool tx_pend_valid;
static volatile bool tx_writing;

/* The writer gate. Replaces masking GDMA_INT_DONE: with IDF owning the
 * channel there is no interrupt for us to mask, so lock() has to stop the
 * writer from asking the core for more data instead. */
static volatile int tx_locked;
static struct semaphore tx_sem;

static unsigned int tx_thread_id;
static bool tx_thread_created;
#define TX_THREAD_STACK_SIZE DEFAULT_STACK_SIZE
static long tx_thread_stack[TX_THREAD_STACK_SIZE / sizeof(long)]
    __attribute__((aligned(8)));
static const char tx_thread_name[] = "i2stx";

/* Counters the probe reports, so this path is measurable in the same terms as
 * the baseline: writes issued, bytes handed over, errors, and how often the
 * writer found nothing to do. */
static volatile unsigned long tx_writes;
static volatile unsigned long tx_bytes;
static volatile unsigned long tx_errs;
static volatile unsigned long tx_timeouts;
/* Where the time goes, per blocking phase.
 *
 * The writer stalls up to 0.8s (max gap 799070us) yet tx_timeouts stays 0 --
 * and that counter IS wired to i2s_channel_write()'s ESP_ERR_TIMEOUT, so the
 * DMA genuinely never blocked us. The remaining suspect is the pull: the
 * semaphore wait at HZ/50 is 20ms, and 0.8s is forty consecutive timeouts with
 * the core offering nothing. These three timers say which it is instead of
 * leaving me to reason about it. */
static volatile unsigned long ph_write_c, ph_write_max;
static volatile unsigned long ph_pull_c,  ph_pull_max;
static volatile unsigned long ph_wait_c,  ph_wait_max;
static volatile unsigned long ph_wait_to;   /* semaphore timeouts, previously uncounted */
static volatile unsigned long tx_core_no_data;  /* complete_callback() returned false */
/* The gap budget does not close: in the slow windows write max is 11.6ms and
 * pull max 104us, yet the inter-write gap reaches 122ms. The only uninstrumented
 * block between two handovers is the tx_locked park, whose sleep(1) is 10ms at
 * 1000Hz -- ten of them is exactly the missing hundred milliseconds. This is a
 * regression I introduced: the hand-rolled sink_lock() only masked OUT_DONE,
 * which costs microseconds and never blocks, whereas the thread parks. */
static volatile unsigned long tx_park_n, tx_park_c, tx_park_max;
/* The writer's own cadence turned out to be the wrong quantity to watch. With
 * 12 descriptors the ring is FULL and write() blocks at exactly the drain rate
 * (mean 5800us unchanged from 4 descriptors), so a 135ms gap between writes is
 * harmless -- the DMA still has 139ms of queued audio and plays right through
 * it. The audio still halts anyway, so the silence is not lateness; something is
 * DISABLING the path. The only things that do that are sink_stop() ->
 * i2s_channel_disable() and the channel-enabled flag itself, and neither has
 * been counted during playback. */
static volatile unsigned long tx_play_n, tx_stop_n;
/* The quantity that actually decides whether a stop->play cycle is audible:
 * how long the core keeps the sink stopped. The writer exits its inner loop on
 * playing=false, so the ring drains from that instant and nothing refills it
 * until sink_play(). If that interval exceeds the ring's audio content the
 * listener hears silence for (interval - ring). Everything so far inferred this
 * from the writer's inter-write gap; measure the interval itself.
 *
 * Ring content on this build is 12 * 512 frames = 139ms, and the inferred
 * interval is ~125ms -- barely covered, which matches "dropping out a lot"
 * rather than "fixed". */
/* Is the payload actually sound?
 *
 * Every TX-side explanation is now eliminated by measurement: the ring is full
 * (write blocks at drain rate), the stop->play interval is 106-125ms against a
 * 139ms ring so it is covered, the channel is never disabled, there are no
 * errors, timeouts or parks, and the writer is paced to within 7us of the
 * hardware. If the frames handed over are silence, every one of those counters
 * reads exactly as it does now. So measure the payload: peak |sample| per frame.
 *
 * A window whose frames are all loud while the listener hears silence means the
 * audio is lost after i2s_channel_write(). A window full of near-zero peaks
 * means the core produced silence and TX has been innocent throughout. */
static volatile unsigned long pk_sum, pk_min, pk_max, pk_quiet;
static volatile unsigned long pk_zero;   /* handovers with len == 0 */
static volatile unsigned long tx_noask;   /* pulls where ops.play already supplied */
static volatile unsigned long tx_gate_waits;  /* waits on the play lock before pulling */
/* What size does the core actually hand back? Never measured, and everything
 * about the ring geometry assumes 1024. If pcm_play_dma_complete_callback()
 * returns a different chunk size, the mixer is being asked at the wrong rate
 * and its output buffer legitimately contains silence between real chunks --
 * which is exactly what the byte census shows. */
static volatile unsigned long sz_min, sz_max, sz_sum, sz_n;
static volatile unsigned long sz_1024, sz_other;
static const void *last_ptr;
static volatile unsigned long same_ptr;   /* consecutive handovers, same address */
static unsigned long pk_n;

static unsigned long sp_t0;                    /* cycles at sink_stop() */
static volatile unsigned long sp_n, sp_c, sp_max;

static volatile unsigned long tx_starve;   /* handovers that found the DMA FIFO empty */
static volatile unsigned long tx_idle;     /* handovers that found I2S TX idle */

void i2s_tx_probe(void)
{
    printf("TX idf: chan=%p cfg=%d en=%d rate=%lu writes=%lu bytes=%lu "
           "errs=%lu tmo=%lu pend=%d writing=%d locked=%d\n",
           (void *)tx_chan_idf, tx_std_configured, tx_chan_enabled,
           tx_std_rate, tx_writes, tx_bytes, tx_errs, tx_timeouts,
           tx_pend_valid, tx_writing, tx_locked);
}

/* Build the std config to match, field for field, what the HAL path programs
 * today (i2s_set_frequency): 16-bit data in 16-bit slots, stereo, ws_width
 * SLOT_BITS, bit_shift on, left aligned, WS idle low.
 *
 * Deliberately NOT I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG: that leaves ws_width at
 * the slot width times two, which is not what the ES8311 is currently being
 * fed. If audio comes back wrong-pitched, wrong-channel or as noise, this is
 * the first place to look. */
static void tx_std_config(i2s_std_config_t *c, unsigned long rate)
{
    *c = (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode      = I2S_SLOT_MODE_STEREO,
            .slot_mask      = I2S_STD_SLOT_BOTH,
            .ws_width       = SLOT_BITS,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = true,
            .big_endian     = false,
            .bit_order_lsb  = false,
        },
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false,
                              .ws_inv = false },
        },
    };

    c->clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    /* Explicit about the source rather than leaving it to I2S_CLK_SRC_DEFAULT.
     *
     * This is NOT the burst cause. An earlier reading of I2S_TX_CLKM_CONF
     * claimed tx_clk_sel had moved from 2 (PLL160M) to 3 (I2S_MCLK_in); that
     * was a wrong bit decode. Per i2s_struct.h:153-157 the layout is
     * tx_clkm_div_num[7:0], tx_clk_active[26], tx_clk_sel[28:27], clk_en[29],
     * and decoding the two observed values correctly gives sel=2 in BOTH:
     * baseline 0x1400000e and IDF 0x3400000e differ only in clk_en, the clock
     * gate. Pinning the source here is just so the intent is visible and cannot
     * drift if IDF's default ever changes. */
    c->clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
}

/* Configure the channel. This is the call that grabs GDMA OUT channel 0 --
 * i2s_channel_init_std_mode() -> i2s_init_dma_intr() -> gdma_new_ahb_channel()
 * -- so it must not run while the hand-rolled transmitter is live. That is why
 * the cutover is atomic and cannot be staged more finely. */
bool i2s_tx_chan_configure(unsigned long rate)
{
    if (!tx_chan_idf)
        return false;

    tx_std_config(&tx_std_cfg, rate);

    esp_err_t e = i2s_channel_init_std_mode(tx_chan_idf, &tx_std_cfg);

    if (e != ESP_OK)
    {
        printf("TX idf: init_std_mode failed: %d\n", (int)e);
        return false;
    }

    tx_std_configured = true;
    tx_std_rate = rate;
    printf("TX idf: configured at %lu Hz (desc=%u frame=%u)\n", rate,
           TX_DMA_DESC_NUM, TX_DMA_FRAME_NUM);
    return true;
}

/* Rate change. The channel cannot be re-initialised (init_std_mode asserts
 * state REGISTER), so this is disable -> reconfig clock -> enable, the same
 * route pcm-xtensa.c uses for the receiver. Rebuilding the channel instead
 * would reset the whole GDMA block. */
static bool tx_set_rate(unsigned long rate)
{
    if (!tx_std_configured)
        return i2s_tx_chan_configure(rate);

    if (rate == tx_std_rate)
        return true;

    if (tx_chan_enabled)
    {
        i2s_channel_disable(tx_chan_idf);
        tx_chan_enabled = false;
    }

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);

    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    clk.clk_src = I2S_CLK_SRC_PLL_160M;   /* see tx_std_config() */

    esp_err_t e = i2s_channel_reconfig_std_clock(tx_chan_idf, &clk);

    if (e != ESP_OK)
    {
        printf("TX idf: reconfig_std_clock(%lu) failed: %d\n", rate, (int)e);
        return false;
    }

    tx_std_rate = rate;
    return true;
}

/* The writer. It exists because i2s_channel_write() returns when the bytes
 * have been COPIED into a free descriptor, not when they have been played, and
 * the only thing that paces it is the descriptor queue that the TX-done ISR
 * refills. So the report to the pcm core has to happen after write() returns,
 * and the ring depth decides how honest that report is -- see the table in
 * TX-IDF-MIGRATION.md for why desc_num 8 made the previous attempt bursty.
 *
 * The loop is bounded rather than portMAX_DELAY: a task parked inside
 * i2s_channel_write() cannot be cancelled, and the pcm core can reach
 * sink_stop() from another thread, so a stop has to be able to land. Same
 * reasoning the receiver documents. */
static void i2s_tx_thread(void)
{
    /* TEMPORARY trace: the symptom is "about a second of audio then silence, app
     * still alive", which could be the writer parked on the lock, blocked inside
     * i2s_channel_write(), starved of buffers, or spinning on errors. Those look
     * identical from outside and need opposite fixes, so each transition prints a
     * few times and then goes quiet. */
    static unsigned tr_iter, tr_write, tr_err, tr_lock;

    while (true)
    {
        if (!playing)
        {
            tx_writing = false;
            semaphore_wait(&tx_sem, HZ / 10);
            continue;
        }

        tx_writing = true;

        while (playing)
        {
            /* The play lock means "do not ask the core for more data". Sitting
             * out the iteration is the thread-context equivalent of masking
             * OUT_DONE. */
            if (tx_locked)
            {
                unsigned long t0 = esp_cpu_get_cycle_count();

                tx_park_n++;
                sleep(1);
                {
                    unsigned long d = esp_cpu_get_cycle_count() - t0;
                    tx_park_c += d;
                    if (d > tx_park_max) tx_park_max = d;
                }
                continue;
            }

            if (!tx_pend_valid)
            {
                unsigned long t0 = esp_cpu_get_cycle_count();
                if (semaphore_wait(&tx_sem, HZ / 50) == 0)
                    ph_wait_to++;
                ph_wait_c += esp_cpu_get_cycle_count() - t0;

                if (!playing || tx_locked)
                    continue;
                if (!tx_pend_valid)
                {
                    if (tr_iter++ < 4)
                        printf("TXtr: no buffer offered (underrun)\n");
                    continue;
                }
            }

            const unsigned char *ptr = tx_ptr;
            size_t len = tx_size;

            tx_pend_valid = false;

            size_t off = 0;

            if (tr_write++ < 4)
                printf("TXtr: write #%u ptr=%p len=%lu en=%d\n",
                       tr_write, (const void *)ptr, (unsigned long)len,
                       tx_chan_enabled);

            {
                /* Scale-free census of the frame: how many bytes are non-zero.
                 *
                 * The previous version computed a peak |int16| and reported
                 * maxima of 97193, which is IMPOSSIBLE for a 16-bit sample --
                 * MIX_FRAME_SAMPLES*4 is 256 * (2 channels * 2 bytes) = 1024, so
                 * the mixer output really is 16-bit stereo and no sample can
                 * exceed 32767. The probe was lying, and I was about to conclude
                 * from it that the core emits silence. Counting bytes has no
                 * scale, no sign and no overflow to get wrong: an all-zero frame
                 * is silence whatever the sample width, and a frame with 1024
                 * non-zero bytes is definitely sound. */
                const unsigned char *bb = (const unsigned char *)ptr;
                unsigned long nz = 0, i;

                if (sz_n == 0 || len < sz_min) sz_min = len;
                if (len > sz_max) sz_max = len;
                sz_sum += len; sz_n++;
                if (len == 1024) sz_1024++; else sz_other++;
                if (ptr == last_ptr) same_ptr++;
                last_ptr = ptr;


                for (i = 0; i < len; i++)
                    nz += (bb[i] != 0);

                /* Distinguish "the core handed us an empty buffer" from "the
                 * core handed us silence". A zero-length handover writes
                 * nothing and returns immediately, so counting it as a silent
                 * audio frame would inflate the quiet ratio and send me looking
                 * at the mixer when the fault is my handover size. */
                if (len == 0)
                    pk_zero++;
                else if (nz == 0)
                    pk_quiet++;

                pk_n++;
                pk_sum += nz;
                if (nz > pk_max) pk_max = nz;
                if (pk_min == 0 || nz < pk_min) pk_min = nz;
            }

            unsigned long tw0 = esp_cpu_get_cycle_count();

            while (off < len && playing)
            {
                size_t got = 0;
                /* timeout is in milliseconds, not ticks */
                esp_err_t e = i2s_channel_write(tx_chan_idf, ptr + off,
                                                len - off, &got, 200);

                if (e != ESP_OK || got == 0)
                {
                    if (e == ESP_ERR_TIMEOUT)
                        tx_timeouts++;
                    else
                        tx_errs++;

                    if (tr_err++ < 6)
                        printf("TXtr: write err=%d got=%lu off=%lu/%lu\n",
                               (int)e, (unsigned long)got,
                               (unsigned long)off, (unsigned long)len);

                    if (!playing)
                        break;
                    /* Do not spin: this thread outranks the UI, and a hot retry
                     * loop here starves the button and main threads outright. */
                    sleep(1);
                    continue;
                }

                off += got;
            }

            {
                unsigned long d = esp_cpu_get_cycle_count() - tw0;
                ph_write_c += d;
                if (d > ph_write_max) ph_write_max = d;
            }

            if (!playing)
                break;

            tx_writes++;
            tx_bytes += off;

            /* Measure the actual handover rate instead of inferring it. At
             * 44100 Hz stereo 16-bit, one 1024-byte frame is 5.8ms, so real
             * time is 172 writes/second; anything much above that means the
             * writer is running ahead of the DAC rather than in step with it.
             * The tick-based probe cannot answer this -- during playback the
             * tick task is starved and printed nothing at all -- so the writer
             * has to time itself. */
            {
                /* Inter-write interval, in microseconds.
                 *
                 * The rate measurement came out at exactly 172/s with zero
                 * timeouts, zero errors, zero parks and zero underruns -- so the
                 * writer is not late on average. But "gappy with correct pitch
                 * and nothing lost" is not an average problem, it is a
                 * distribution problem, and an average cannot see it. If these
                 * intervals are tight around the 5804us nominal frame time then
                 * the writer is feeding the ring on time and the stall is in the
                 * DMA/I2S side -- which would mean the re-arm, not the
                 * scheduling. If they cluster and spike, the writer is being
                 * descheduled and it is a priority problem.
                 *
                 * current_tick is 10ms, coarser than the 5.8ms frame it has to
                 * measure, so this uses the core cycle counter directly. */
                static unsigned long w_mark, t_mark, last_cyc;
                static unsigned long gap_max, gap_sum, gap_big;
                unsigned long now_tick = current_tick;   /* 10ms ticks */
                unsigned long cyc = esp_cpu_get_cycle_count();

                if (last_cyc != 0)
                {
                    unsigned long gap = cyc - last_cyc;

                    if (gap > gap_max)
                        gap_max = gap;
                    gap_sum += gap;

                    /* more than two frame-times of nothing handed over */
                    if (gap > 2800000UL)   /* ~11.7ms at 240MHz */
                        gap_big++;
                }
                last_cyc = cyc;

                /* Starvation sampling, done on every handover rather than once a
                 * second. The writer has now been measured delivering one frame
                 * every ~5.8ms with no gap over 11.7ms, zero timeouts and zero
                 * errors -- so if audio is still gappy, the silence is downstream
                 * of us and the only way to see it is to ask the hardware whether
                 * its FIFO ran dry. A once-a-second sample cannot catch a
                 * transient; this catches every frame.
                 *
                 * GDMA_OUTFIFO_EMPTY_Ln are bits 1, 3 and 5; I2S_TX_STATE bit 0
                 * is idle. Any of them set at the moment we hand a frame over
                 * means the transmitter had already drained what we gave it. */
                {
                    unsigned fs = rd(GDMA_TX_FIFO_STATUS);
                    unsigned st = rd(I2S_TX_STATE);

                    if (fs & 0x2Au)      /* bits 1|3|5 */
                        tx_starve++;
                    if (st & TX_STATE_IDLE)
                        tx_idle++;
                }

                if (t_mark == 0)
                {
                    t_mark = now_tick;
                    w_mark = tx_writes;
                }
                else if (now_tick - t_mark >= 100)      /* one second */
                {
                    unsigned long n = tx_writes - w_mark;

                    printf("TXtr: %lu writes/s  gap mean=%luus max=%luus "
                           ">2frame=%lu  tmo=%lu errs=%lu "
                           "starve=%lu idle=%lu\n"
                           "      phases us: write mean=%lu max=%lu | pull "
                           "mean=%lu max=%lu | wait mean=%lu max=%lu | "
                           "timeouts=%lu nodata=%lu | park n=%lu mean=%luus "
                           "max=%luus\n"
                           "      session: play=%lu stop=%lu en=%d pend=%d | "
                           "stopped n=%lu mean=%luus max=%luus\n"
                           "      payload: nonzero-bytes min=%lu mean=%lu "
                           "max=%lu of %lu | size min=%lu max=%lu mean=%lu "
                           "(1024=%lu other=%lu) sameptr=%lu noask=%lu gate=%lu\n",
                           n,
                           n ? gap_sum / n / 240UL : 0,   /* cycles->us at 240MHz.
                                                            * NOT *1000000/240000000:
                                                            * that overflows 32 bits and
                                                            * printed 6us for a 5800us
                                                            * gap. */
                           gap_max / 240UL,
                           gap_big, tx_timeouts, tx_errs, tx_starve,
                           tx_idle,
                           ph_write_c / n / 240UL, ph_write_max / 240UL,
                           ph_pull_c  / (n ? n : 1) / 240UL,
                           ph_pull_max / 240UL,
                           ph_wait_c  / (n ? n : 1) / 240UL,
                           ph_wait_max / 240UL,
                           ph_wait_to, tx_core_no_data,
                           tx_park_n,
                           tx_park_n ? tx_park_c / tx_park_n / 240UL : 0,
                           tx_park_max / 240UL,
                           tx_play_n, tx_stop_n, tx_chan_enabled,
                           tx_pend_valid,
                           sp_n,
                           sp_n ? sp_c / sp_n / 240UL : 0,
                           sp_max / 240UL,
                           pk_min, pk_n ? pk_sum / pk_n : 0, pk_max,
                           pk_quiet,
                           sz_min, sz_max, sz_n ? sz_sum / sz_n : 0,
                           sz_1024, sz_other, same_ptr, tx_noask,
                           tx_gate_waits);
                    tx_noask = 0; tx_gate_waits = 0;
                    sz_min = 0; sz_max = 0; sz_sum = 0; sz_n = 0;
                    sz_1024 = 0; sz_other = 0; same_ptr = 0;
                    sp_n = 0; sp_c = 0; sp_max = 0;
                    pk_sum = 0; pk_min = 0; pk_max = 0; pk_quiet = 0;
                    pk_n = 0; pk_zero = 0;
                    tx_play_n = 0; tx_stop_n = 0;
                    tx_starve = 0; tx_idle = 0;
                    tx_park_n = 0; tx_park_c = 0; tx_park_max = 0;
                    ph_write_c = 0; ph_write_max = 0;
                    ph_pull_c  = 0; ph_pull_max  = 0;
                    ph_wait_c  = 0; ph_wait_max  = 0;
                    ph_wait_to = 0; tx_core_no_data = 0;
                    w_mark = tx_writes;
                    t_mark = now_tick;
                    gap_max = 0; gap_sum = 0; gap_big = 0;
                }
            }

            /* The frame is in the DMA ring. Tell the core it has been consumed
             * and take the next one -- same order refill() uses, and for the
             * same reason: reporting consumption is what makes the mixer
             * produce. */
            {
                unsigned long tp0 = esp_cpu_get_cycle_count();

                /* The play-lock gate has to sit HERE, immediately before the
                 * pull -- not at the top of the loop.
                 *
                 * Rockbox's play lock is not mutual exclusion. pcm_play_lock()
                 * is just sinks[cur_sink]->ops.lock(), a counter; the exclusion
                 * is supplied by the DRIVER masking its own completion interrupt,
                 * which is why the hand-rolled refill() physically cannot run
                 * while the core holds the lock (its CHAIN line reads locked=1
                 * throughout playback). My writer checked tx_locked at the top of
                 * the loop and then pulled 5.8ms later, after a blocking
                 * i2s_channel_write(). park n=0 proved that check never engaged,
                 * so the writer routinely called
                 * pcm_play_dma_complete_callback() inside a locked section.
                 *
                 * With WANT_SWVOL active that callback hands back
                 * pcm_dbl_buf[pcm_dbl_buf_num] (pcm_sw_volume.c:230) -- the half
                 * the mixer is in the middle of filling. That is a silent frame,
                 * with no audio lost and no error anywhere, which is exactly the
                 * signature the baseline A/B produced: 1% all-zero frames on the
                 * hand-rolled path against 50-75% on this one.
                 *
                 * Waiting here narrows the window from a full frame to the few
                 * microseconds between the check and the callback. It is not
                 * airtight -- only reclaiming the completion interrupt would be
                 * -- but it reproduces the baseline's ordering in practice. */
                while (tx_locked && playing)
                {
                    tx_gate_waits++;
                    sleep(1);
                }

                pcm_play_dma_status_callback(PCM_DMAST_STARTED);

                {
                    const void *addr;
                    size_t size;

                    /* THIS is the missing signal-handling, and it is copied from
                     * refill() verbatim in intent:
                     *
                     *     pcm_play_dma_status_callback(PCM_DMAST_STARTED);
                     *     if (pend_valid) return;   // ops.play gave us one
                     *     if (!pcm_play_dma_complete_callback(...)) return;
                     *
                     * The known-good path only asks the mixer for a buffer when
                     * one has NOT already been published through ops.play(). My
                     * writer asked unconditionally, so every sink_play() that
                     * landed during a write made the mixer advance an extra time
                     * for a frame the hardware had not consumed. Combined with a
                     * deep ring -- which lets us ask up to 139ms ahead of the
                     * hardware against a single mixer output buffer the codec
                     * fills in real time -- the mixer legitimately hands back its
                     * zero-filled buffer for chunks it has no data for. That is
                     * the all-zero seconds the byte census caught, with no audio
                     * lost, because the zeros are EXTRA requests rather than
                     * replacements. */
                    /* refill()'s `if (pend_valid) return;`, in thread form: if
                     * ops.play() published a buffer while we were writing, that
                     * IS the next one and the mixer must not be advanced again
                     * for a frame the hardware has not consumed. */
                    if (tx_pend_valid)
                        tx_noask++;
                    else if (pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr,
                                                            &size))
                    {
                        tx_ptr = addr;
                        tx_size = size;
                        tx_pend_valid = true;
                    }
                    else
                    {
                        tx_core_no_data++;

                        if (tr_iter++ < 8)
                            printf("TXtr: core offered nothing (writes so far=%lu)\n",
                                   tx_writes);
                    }
                }

                {
                    unsigned long d = esp_cpu_get_cycle_count() - tp0;
                    ph_pull_c += d;
                    if (d > ph_pull_max) ph_pull_max = d;
                }
            }
        }
    }
}

void i2s_tx_thread_start(void)
{
    if (tx_thread_created)
        return;

    tx_thread_created = true;
    semaphore_init(&tx_sem, 1, 0);
    tx_thread_id = create_thread(i2s_tx_thread, &tx_thread_stack,
                                 sizeof(tx_thread_stack), 0, tx_thread_name
                                 /* PRIORITY_REALTIME, not USER_INTERFACE.
                                  *
                                  * The hook this thread replaces was an ISR, and
                                  * an ISR preempts every thread. As a thread at
                                  * USER_INTERFACE (16) it sat BELOW
                                  * PRIORITY_PLAYBACK (16) and PRIORITY_SYSTEM
                                  * (18), so any long codec or SD-read burst ran
                                  * it off the CPU: measured writer deschedules of
                                  * 824ms with write max 11.6ms, pull max 1us,
                                  * wait max 0 and park n=0 -- no phase inside the
                                  * loop accounted for any of it. A 139ms ring
                                  * cannot cover that.
                                  *
                                  * Safe to raise because the thread is blocked in
                                  * i2s_channel_write() on dma.msg_queue for
                                  * essentially the whole frame, doing ~1ms of
                                  * memcpy per 5.8ms. It cannot starve the codec;
                                  * both of its yield paths (tx_locked park and
                                  * the write-error retry) sleep(1) rather than
                                  * spin. */
                                 IF_PRIO(, PRIORITY_REALTIME));
}

bool i2s_tx_chan_enable(bool en)
{
    if (!tx_chan_idf || !tx_std_configured)
        return false;

    if (en == tx_chan_enabled)
        return true;

    esp_err_t e = en ? i2s_channel_enable(tx_chan_idf)
                     : i2s_channel_disable(tx_chan_idf);

    if (e != ESP_OK)
    {
        printf("TX idf: %s failed: %d\n", en ? "enable" : "disable", (int)e);
        return false;
    }

    tx_chan_enabled = en;
    return true;
}
#endif /* TX_USE_IDF_DRIVER */


void i2s_init(void)
{
    /* Peripheral clock and reset. IDF does this in periph_ctrl; entering from the
     * ROM loader means doing it here. Both halves are required -- with the reset
     * bit still asserted the block discards every write and reads back zero. */
    wr(SYSTEM_PERIP_CLK_EN0, rd(SYSTEM_PERIP_CLK_EN0) | CLK_EN_I2S1);
    wr(SYSTEM_PERIP_CLK_EN1, rd(SYSTEM_PERIP_CLK_EN1) | CLK_EN_DMA);
    wr(SYSTEM_PERIP_RST_EN0, rd(SYSTEM_PERIP_RST_EN0) & ~CLK_EN_I2S1);
    wr(SYSTEM_PERIP_RST_EN1, rd(SYSTEM_PERIP_RST_EN1) & ~CLK_EN_DMA);

    i2s_hal_init(&tx_hal, XT_I2S_TX_PORT);

    /* Route the signals through the GPIO matrix using the same ROM helper the
     * IDF driver uses, so the pad configuration matches what IDF expects. */
    esp_rom_gpio_connect_out_signal(PIN_I2S_MCLK, SIG_I2S1_MCLK_OUT, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_BCLK, SIG_I2S1O_BCK_OUT, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_WS,   SIG_I2S1O_WS_OUT, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_DOUT, SIG_I2S1O_SD_OUT, false, false);

    gpio_set_direction(PIN_I2S_MCLK, 1);
    gpio_set_direction(PIN_I2S_BCLK, 1);
    gpio_set_direction(PIN_I2S_WS,   1);
    gpio_set_direction(PIN_I2S_DOUT, 1);

#ifndef TX_USE_IDF_DRIVER
    gdma_init();
#else
    /* The hand-rolled GDMA channel, its descriptor ring and its interrupt are
     * all skipped: i2s_channel_init_std_mode() will allocate GDMA OUT channel 0
     * itself, and doing both would have IDF take the channel from under a live
     * transmitter. */
    printf("TX idf: hand-rolled gdma_init() skipped by design\n");
#endif
}

#ifdef HAVE_RECORDING

/* The recording session's IDF channel has to be created at the rate the bus is
 * running: the codecs share one MCLK, so a receiver configured for a different
 * one would be clocked against a frame the ADC was not expecting. */
unsigned long i2s_rx_sample_rate(void)
{
    return rx_rate ? rx_rate : hw_freq_sampr[builtin_pcm_sink.caps.default_freq];
}

/* Hand the three shared clock pads to whichever port is master right now.
 * esp_rom_gpio_connect_out_signal() just rewrites the pad's OUT_SEL, so this is
 * cheap and can be done on every mode switch. See the note in i2s_set_frequency()
 * for why the two ports cannot both drive these pads. */
static void i2s_route_clock_pads(int mclk_sig, int bck_sig, int ws_sig)
{
    esp_rom_gpio_connect_out_signal(PIN_I2S_MCLK, mclk_sig, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_BCLK, bck_sig, false, false);
    esp_rom_gpio_connect_out_signal(PIN_I2S_WS,   ws_sig,   false, false);
}

void i2s_rx_pads_take(void)
{
    i2s_route_clock_pads(SIG_I2S0_MCLK_OUT, SIG_I2S0I_BCK_OUT, SIG_I2S0I_WS_OUT);
}

void i2s_rx_pads_release(void)
{
    i2s_route_clock_pads(SIG_I2S1_MCLK_OUT, SIG_I2S1O_BCK_OUT, SIG_I2S1O_WS_OUT);
}
#endif /* HAVE_RECORDING */

static void start_tx(void)
{
#ifdef TX_USE_IDF_DRIVER
    i2s_tx_chan_enable(true);
    /* Wake the writer so a fresh session starts filling immediately rather than
     * after its bounded wait expires. */
    semaphore_release(&tx_sem);
#else
    /* Preroll every slot before the walk begins, so the engine never reaches a
     * descriptor the CPU owns. Ask the core at most once per slot and stop
     * asking at the first decline -- driving pcm_play_dma_complete_callback()
     * repeatedly at start makes the swvol path call pcm_play_stop_int() once it
     * runs out, which re-enters sink_stop() mid-preroll. Silence for the rest is
     * correct regardless: the invariant is "owned and filled", not "real audio". */
    {
        bool core_exhausted = false;

        for (int i = 0; i < XT_TX_RING; i++)
        {
            if (!pend_valid && !core_exhausted)
            {
                refill();
                if (!pend_valid)
                    core_exhausted = true;
            }

            if (pend_valid)
            {
                fill(i, (const void *)pend_addr, (unsigned long)pend_size);
                pend_valid = 0;
            }
            else
                fill_silence(i);
        }

        done_slot = 0;
        dscr_streak = 0;
    }

    i2s_ll_tx_start(tx_hal.dev);
    xt_tx_sessions++;
    irq_at_start = xt_sink_irq_n;

    wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));    /* link.start -- once */
#endif
}

static void stop_tx(void)
{
    /* Stopping leaves every descriptor CPU-owned with length zero while
     * GDMA_INT_DSCR_ERR is still armed, so this is the moment the ring is most
     * fragile. Stamped so the trail says whether a crash follows a stop. */
    xtensa_breadcrumb("txstop", xt_sink_stop_n, xt_sink_play_n);

#ifdef TX_USE_IDF_DRIVER
    /* The channel is left configured and merely disabled: deleting it would
     * drop IDF's GDMA group refcount to zero and gate the block's bus clock
     * outright, which takes the receiver with it. */
    i2s_tx_chan_enable(false);
#else
    wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 20));    /* link.stop */
    i2s_ll_tx_stop(tx_hal.dev);
#endif
}

/* Nothing is started or stopped for the receiver in this file any more: it is an
 * ESP-IDF I2S channel, enabled and disabled by pcm_rec_dma_start() /
 * pcm_rec_dma_stop(). Two hand-rolled attempts lived here and both are worth
 * remembering for what they taught:
 *
 *   i2s_tx_clock_run() borrowed the transmitter's clock domain for recording. It
 *   asserted tx_start, which runs the DAC's port against an empty FIFO, and still
 *   produced no usable clock -- an I2S transmitter with no GDMA feeding it hangs
 *   and stops clocking, which is what the tx_hung interrupt exists to report.
 *
 *   i2s_rx_start() set rx_start directly and never pulsed rx_update, so the
 *   receiver's configuration (master role, clock source, slot width) stayed in the
 *   APB domain and was never latched into the sampling domain. Measured: rx_conf
 *   read 00009404 with slave=0 and rx_update clear, and GDMA never took a
 *   descriptor. That is the same double-buffered-configuration trap as LEDC's
 *   conf0.low_speed_update and I2C's CONF_UPGATE.
 *
 * What the capture path still needs from here is only the shared clock pads. */

/* cur is the half that is free to write. The DMA mounts a descriptor only when the
 * owner bit says it owns it, so the half it is about to need must already be armed
 * when the current one finishes - which is why every write advances cur. */
/* GDMA's size field is 12 bits, so one descriptor carries at most 4095 bytes.
 * The mixer's largest frame is MIX_FRAME_SAMPLES samples times 4 bytes times the
 * high-rate multiplier, and the multiplier only reaches 2 here because
 * HW_SAMPR_CAPS caps at 96 kHz. That is 2048 bytes, comfortably under the limit.
 * If the cap ever rises, the ring needs chained descriptors rather than one per
 * half, not a silent truncation. */
_Static_assert(MIX_FRAME_SAMPLES * 4 * 2 <= GDMA_MAX_DESC_LEN,
               "the mixer's largest frame exceeds one GDMA descriptor");


static void refill(void)
{
    const void *addr;
    size_t size;

    xt_refill_n++;

    /* Report the consumed buffer before anything else. This is the order every
     * working sink driver uses -- see firmware/target/arm/imx233/pcm-imx233.c,
     * whose ISR ends with pcm_play_dma_status_callback(PCM_DMAST_STARTED).
     * Telling the layer above that a buffer was consumed is what makes the mixer
     * produce the next one and hand it down through builtin_pcm_sink.ops.play,
     * which lands in the pending slot. */
    pcm_play_dma_status_callback(PCM_DMAST_STARTED);

    if (pend_valid)
    {
        xt_refill_branch = 1;
        return;     /* ops.play already handed us the next buffer */
    }

    /* Nothing arrived, so ask explicitly. This is the fallback, not the normal
     * path, and it is also the underrun path: the caller re-arms the released
     * slot empty and the engine walks past it. */
    if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &addr, &size))
    {
        xt_refill_branch = 2;
        return;
    }

    xt_refill_branch = 3;
    pend_addr = addr;
    pend_size = size;
    pend_valid = 1;
}

/* Runs on the interrupted thread's stack, so it has to stay small. The handler in
 * vectors.S has already saved a0 and a12-a15; this is the C part of the frame. */
/* TEMPORARY: how long the transmit path is torn down for.
 *
 * The design stops the outlink on every completion, runs the mixer through
 * pcm_play_dma_status_callback(), memcpy's the next frame in, and restarts the
 * walk -- 172 times a second at 1024-byte frames. The only thing between a
 * stopped outlink and silence at the DAC is the I2S FIFO, so the question that
 * decides whether this is the click is simply how long that window is, and how
 * much of it belongs to the mixer rather than the copy.
 *
 * Measured in core cycles because that needs no assumptions; i2s_probe_tick()
 * converts. Nothing prints here -- see the note on gdma_tx_isr(). */
static volatile unsigned long xt_isr_n;
static volatile unsigned long xt_isr_cyc_max;      /* entry -> exit */
static volatile unsigned long xt_isr_refill_max;   /* entry -> after refill() */
static volatile unsigned long xt_isr_cyc_sum;
/* Never reset. The per-window max is read by the tick thread as four separate
 * volatile reads while the ISR keeps updating them, so a long sample landing
 * between the read and the reset is lost and max under-reports -- which is how
 * it came to print max=10us against avg=16us. A monotonic worst case cannot be
 * lost that way, so the number that actually matters ("is it ever large?") is
 * race-free. The windowed max only biases low, never high. */
static volatile unsigned long xt_isr_cyc_worst;

void ICODE_ATTR gdma_irq(void)
{
    unsigned int st = rd(GDMA_TX_INT_ST);
    unsigned long t0 = esp_cpu_get_cycle_count();
    unsigned long trefill = t0;

    /* DSCR_ERR is handled BEFORE DONE, and the order is the bug this fixes.
     *
     * Both branches act on the same st, captured once at entry. When DONE and
     * DSCR_ERR are pending together -- which is exactly what "the engine ran off
     * the end of the list while the refill was still in here" looks like -- the
     * DONE path used to restart the walk and the DSCR_ERR path then stopped it
     * again in the same handler. A stopped outlink raises no completions, so
     * nothing restarted it: one transient became a silent transport until some
     * later event kicked it. Measured rate was about one DSCR_ERR every two
     * frames (err climbing ~173 per 345 interrupts).
     *
     * Handling it first means DONE's restart is the last word on the link, which
     * is what we want: DONE has just armed a real buffer. */
    if (st & GDMA_INT_DSCR_ERR)
    {
        /* Owner error or a bad descriptor word.

           This handler used to reset cur to 0 as well as re-pointing the link.
           That is what turned a single transient into a permanent fault: cur is
           the driver's idea of which slot is free, and forcing it to 0 while the
           engine was somewhere else in the ring meant every later arm() targeted
           a slot the engine had already passed. Measured directly -- write_desc
           armed slot 0 on both of its calls and slot 1 was never armed.

           With every slot owned from init onward this should not fire at all, so
           leave the ring alone and only restart the walk -- which is what this
           comment has always said, and what the code did not do: it wrote
           link.stop, guaranteeing the deadlock described above. Now it starts. */
        xt_dscr_err_n++;

        /* Before or after this session's first completion? */
        if (xt_sink_irq_n == irq_at_start)
            err_startup++;

        /* The storm guard. One DSCR_ERR is a transient and restarting is right.
         * Two in a row with no DONE between means the engine is sitting on a slot
         * the CPU owns, so restarting onto it just errors again -- and the handler
         * re-enters itself forever. Recover properly instead: stop, re-arm the
         * whole ring so no slot can be unowned, start once. */
        /* Only restart if the core actually wants a transport running.
         *
         * This was the burst. err steps to +52, +10 and +16 landed on exactly
         * the samples where (refill - irq) steps by 4 -- one start_tx() preroll,
         * i.e. a playback session starting -- and never on samples with heavy
         * volume or touch traffic and no session start. At a track switch
         * stop_tx() halts the walk, sink_stop() then clears every descriptor to
         * CPU-owned with zero length, and DSCR_ERR stays armed. Any error
         * pending across that teardown fired this branch, which started a
         * transport the core had just stopped -- straight onto the cleared
         * descriptors, producing more errors, which tripped tx_recover(), which
         * also started. Self-sustaining, anchored at the transition. */
        if (!playing)
        {
            /* Stopped: nothing to restart. Clear the pending error and stay down. */
            dscr_streak = 0;
        }
        else if (++dscr_streak >= 2)
            tx_recover();
        else
            wr(GDMA_TX_LINK, rd(GDMA_TX_LINK) | (1u << 21));  /* link.start */
    }

    if (st & GDMA_INT_DONE)
    {
        /* Take the engine off the list before it can fetch anything else.
         *
         * This is the imx233 model -- that driver's DMA command carries
         * next = NULL, so the transfer ends and the ISR restarts it. Our GDMA is
         * chained and free-running, which means it advances to the next
         * descriptor about a microsecond after retiring the current one, while
         * the refill that would arm it is still inside the interrupt. Measured on
         * this part: reaching a descriptor the CPU owns raises out_dscr_err, it is
         * not a benign park, so "let it park and restart later" is not a strategy
         * here. Stopping the walk removes the race instead of trying to win it.
         *
         * The I2S FIFO bridges the gap; it was measured running full, so there is
         * real capacity between a stopped outlink and the DAC. */
        int slot = done_slot;

        done_slot = (slot + 1) % XT_TX_RING;
        dscr_streak = 0;      /* a DONE proves the walk is healthy again */

        /* Free-running: the engine walks the chained ring by itself, so this
         * handler does not stop, restart, or clear next. done_slot is the
         * position to arm next; the slot that just retired has to be rearmed
         * before the engine wraps back onto it, which is what the runway
         * measured above buys. */

        /* A completion can only mean audio was played: a slot the CPU owns is not
         * taken by the engine, so it never completes. */
        xt_sink_irq_n++;
        if (queued > 0)
            queued--;

        /* Deferred is the CORE PULL, not the descriptor arming.
         *
         * This was the steady-state err mechanism, and it was my porting error.
         * The old design's answer to the play lock was to mask GDMA_INT_DONE, so
         * nothing at all happened while it was held -- which is safe with a
         * stop-and-restart transport and actively harmful with a free-running
         * one. The version above inherited that: on a locked completion it set
         * cb_pending and skipped everything, including arming the slot that just
         * retired. That slot stayed CPU-owned, the engine walked onto it under
         * auto-writeback, and out_dscr_err fired. Every locked window longer
         * than the ring's runway produced a wrap, and a wrap produces one error
         * per slot -- which is also why deepening the ring made err look WORSE
         * (257 at seven slots against 19 at four) without changing harm: err
         * counts wraps, and wraps are priced by depth.
         *
         * Arming a descriptor with silence involves the core not at all, so it
         * runs unconditionally. Only pcm_play_dma_complete_callback() is
         * deferred, via cb_pending, and replayed on unlock as as3525 does.
         */
        if (locked)
        {
            cb_pending = 1;

            if (pend_valid)
            {
                /* A buffer already published by ops.play() is ours to transmit;
                 * handing it over is not asking the core for anything. */
                fill(slot, (const void *)pend_addr, (unsigned long)pend_size);
                pend_valid = 0;
            }
            else
                fill_silence(slot);

            trefill = esp_cpu_get_cycle_count();
            goto done_exit;
        }

        refill();
        trefill = esp_cpu_get_cycle_count();

        if (pend_valid)
        {
            fill(slot, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
        }
        else
            fill_silence(slot);

        /* No link.stop and no link.start: the engine is already walking on. The
         * restart that used to sit here is the gap that clicked. */
done_exit:
        ;
        /* Nothing to play: stay stopped. The core's stop path handles the end of
         * stream; a genuine underrun simply pauses the transport rather than
         * feeding the DAC a descriptor the engine is not allowed to read. */
    }

    /* Clear only what fired. int_clr is write-to-clear. */
    wr(GDMA_TX_INT_CLR, st);

    {
        unsigned long d = esp_cpu_get_cycle_count() - t0;

        xt_isr_n++;
        xt_isr_cyc_sum += d;
        if (d > xt_isr_cyc_max)
            xt_isr_cyc_max = d;
        if (d > xt_isr_cyc_worst)
            xt_isr_cyc_worst = d;
        d = trefill - t0;
        if (d > xt_isr_refill_max)
            xt_isr_refill_max = d;
    }
}

/* Mask the event rather than the slot, so a completion that happens while the mixer
 * is busy is not lost: int_raw stays set and fires as soon as the mask is lifted. */
static void sink_lock(void)
{
#ifdef TX_USE_IDF_DRIVER
    /* No interrupt of ours to mask any more -- IDF owns the GDMA interrupt.
     * What the lock has to mean here is "writer, stop asking the core for
     * data", which the thread checks between frames. */
    tx_locked++;
#endif
    if (locked++ == 0)
        wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) & ~GDMA_INT_DONE);
}

static void sink_unlock(void)
{
    /* Clamped, and the clamp is the fix for a drift that was easy to miss.
     *
     * sink_stop() sets locked = 0 deliberately (see the note there: a stop that
     * arrives while the mixer still holds the play lock used to leave the count
     * nonzero and GDMA_INT_DONE masked for good). But the mixer's own outstanding
     * pcm_play_unlock() still runs afterwards, and with a bare --locked that took
     * the count to -1 -- where it then stayed: measured locked=-1 in 500 of 548
     * samples during ordinary playback.
     *
     * Once negative, neither transition ever equals zero again. sink_lock()'s
     * "if (locked++ == 0)" never masks and this function's "if (--locked == 0)"
     * never unmasks, so the pair silently stops doing anything and the completion
     * interrupt is live at all times -- including inside the start and stop
     * sequences the play lock exists to protect. That is a race window, not a
     * cosmetic counter bug.
     *
     * Clamping at zero keeps the invariant the two guards depend on: the count is
     * always the number of locks genuinely held, the 0<->1 edges still mean
     * something, and an unlock that arrives after a stop simply re-asserts the
     * unmask, which is what we want anyway. */
    if (locked > 0)
        locked--;

#ifdef TX_USE_IDF_DRIVER
    /* Clamped for the same reason as `locked` above: a negative gate counter
     * would never reach zero again and the writer would stay parked forever. */
    if (tx_locked > 0)
        tx_locked--;
#endif

    if (locked == 0 && cb_pending)
    {
        int slot = done_slot;

        cb_pending = 0;
        done_slot = (slot + 1) % XT_TX_RING;
        refill();

        if (pend_valid)
        {
            fill(slot, (const void *)pend_addr, (unsigned long)pend_size);
            pend_valid = 0;
        }
        else
            fill_silence(slot);
    }

    if (locked == 0)
    {
        /* TEMPORARY: event-driven, so it cannot sample the wrong moment the way
           a dump taken inside sink_play() did -- the mixer holds this lock across
           mixer_start_pcm(), so any snapshot in that call is inside the locked
           region by construction. If "SINK: first play" prints and this never
           does, the play lock is never released: GDMA_INT_DONE stays masked for
           the rest of the session and the codec blocks waiting for pcmbuf to
           drain while the mixer blocks waiting for the codec. */
        static bool unlocked_once = false;
        if (!unlocked_once)
        {
            unlocked_once = true;
            printf("SINK: play lock released (first unlock to zero)\n");
        }
        wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DONE);
    }
}

static void sink_play(const void *addr, size_t size)
{
    xt_sink_play_n++;

    /* Coarse trail of codec activity. Every 32nd frame, because at 172/s a
     * per-frame stamp would turn a 16-slot ring into the last 93ms and wash out
     * the image-placement entries that identified the last crash. What this
     * answers is the question the "placed" trail could not: did the codec ever
     * hand down a frame before it died, or did it fault on entry? */
    if ((xt_sink_play_n & 31) == 0)
        xtensa_breadcrumb("play", xt_sink_play_n, (unsigned long)size);

#ifdef TX_USE_IDF_DRIVER
    tx_play_n++;
#endif
    xt_sink_last_size = size;

#ifdef TX_USE_IDF_DRIVER
    /* The whole hand-rolled path below -- fill(), the descriptor ring, the
     * link re-latch -- is replaced by publishing the buffer and waking the
     * writer. Note this is called ONCE per playback: ops.play() arms the first
     * frame and every later one arrives through pcm_play_data on the writer's
     * own stack. */
    {
        /* Only meaningful if a stop actually preceded this play. */
        if (sp_t0 != 0)
        {
            unsigned long d = esp_cpu_get_cycle_count() - sp_t0;
            sp_n++; sp_c += d;
            if (d > sp_max) sp_max = d;
            sp_t0 = 0;
        }
    }

    tx_ptr = addr;
    tx_size = size;
    tx_pend_valid = true;

    if (!playing)
    {
        playing = true;
        start_tx();
    }

    semaphore_release(&tx_sem);
    return;
#endif

    /* TEMPORARY: the first frame of a playback. The chain dump is taken after
       start_tx() below, not here: the pcm core holds the play lock across the
       whole start sequence, so at this point GDMA_INT_DONE is legitimately
       masked and the transmitter legitimately not started. An early dump cannot
       tell a muted consumer from a normal snapshot. */
    if (xt_sink_play_n == 1)
    {
        printf("SINK: first play addr=%p size=%lu\n", addr,
               (unsigned long)size);

        /* TEMPORARY: playback allocates its file buffer around here, and this is
           where the EOM panic fires. Dumping at the first frame shows what is
           left at the moment the decision gets made, rather than after it has
           already failed. */
        xt_arena_dump("at playback start");
    }

    /* The sample-width probe lives in fill(), not here. sink_play() is
     * ops.play(), which the pcm core calls once per playback to arm the initial
     * buffer; every later frame comes through pcm_play_data into refill() and
     * then fill(). Sampling here sees one frame per session, and it is the silent
     * head of the track. */

    if (!playing)
    {
        /* First frame of a playback. Fill slot 0 with it and every other slot
         * with an owned nothing, so from the instant the engine starts there is
         * no descriptor in the ring it is not allowed to take. Starting with one
         * armed slot and three the engine must not touch is what made the very
         * first hand-off fatal. */
        fill(0, addr, (unsigned long)size);
        pend_valid = 0;
        queued = 1;

        playing = true;
        start_tx();
        wr(GDMA_TX_LINK, (uint32_t)(unsigned long)&desc[0] & 0xFFFFF);

    }
    else
    {
        /* Steady state: the core hands us the next frame while the engine is
         * still playing the current one. The completion interrupt takes it from
         * here into whichever slot it has just released. */
        pend_addr = addr;
        pend_size = size;
        pend_valid = 1;
        queued++;
    }

    /* TEMPORARY: dump from thread context once the engine has had many
       completions, so dma irq, gdma out and the descriptor lengths are live.
       Not on the first play: the mixer still holds the play lock there, so a
       masked OUT_DONE and started=0 are both correct and say nothing. And never
       from the ISR -- a printf there takes the newlib stdout lock and aborts. */
#ifdef TX_PROBE_FULL
    if (xt_sink_play_n == 16)
    {
        printf("CHAIN: at 16th play --\n");
        i2s_audio_probe();
    }
#endif
}

static void sink_stop(void)
{
    xt_sink_stop_n++;

    /* Session boundaries are rare and meaningful: a stop in the trail means the
     * core ran out of data rather than the transport dying mid-stream. */
    xtensa_breadcrumb("stop", xt_sink_stop_n, xt_sink_play_n);

#ifdef TX_USE_IDF_DRIVER
    tx_stop_n++;
#endif

#ifdef TX_USE_IDF_DRIVER
    playing = false;
    tx_pend_valid = false;
    sp_t0 = esp_cpu_get_cycle_count();

#ifdef TX_STOP_KEEPS_CHANNEL
    /* Do NOT tear the channel down here.
     *
     * The lifecycle counters settled the diagnosis: during ordinary continuous
     * playback the core calls sink_play() and sink_stop() as a pair about once a
     * second (7 of 32 windows showed play=1 stop=1). pcm_play_data() reaches
     * ops.stop() whenever the codec has nothing queued at that instant, so this
     * is normal traffic, not an error path.
     *
     * stop_tx() calls i2s_channel_disable(), which halts the transmitter and
     * discards whatever the ring held; the matching sink_play() then pays
     * i2s_channel_enable() to start from silence again. With the hand-rolled
     * one-frame ring that cycle cost ~5.8ms, which is very plausibly the "minor
     * crackle" the baseline always had. Deepen the ring to 139ms and make the
     * teardown expensive, and the same cycle becomes the burst-silence-burst this
     * whole investigation has been about -- correct pitch, nothing lost, no
     * errors anywhere, because the clock and the DMA were never the fault.
     *
     * Leaving the channel enabled means a stop simply stops feeding it: the ring
     * drains and IDF's auto_clear then holds silence, and a resume is immediate
     * because the transmitter is still armed. */
    tx_locked = 0;
    locked = 0;
    semaphore_release(&tx_sem);
    return;
#endif

    tx_ptr = NULL;
    tx_size = 0;

    stop_tx();

    /* A stop ends the session, so any lock still held is moot. Reset both
     * counters so the next session starts from a known gate -- leaving them
     * nonzero parks the writer for the rest of the boot. */
    locked = 0;
    tx_locked = 0;
    semaphore_release(&tx_sem);
    return;
#endif

    /* TEMPORARY: read the transmit path out as the session ends, before stop_tx()
       and the descriptor handback below erase the evidence. A playback that never
       completed shows up here as sink play>0 with dma irq=0 and gdma out=0, which
       is the engine refusing to take a descriptor -- a different fault entirely
       from a codec or a data problem. Thread context, so printf is safe. */
#ifdef TX_STOP_PROBE
    /* Off by default. The breadcrumb trail localised a crash to the window
     * between sink_stop()'s "stop" stamp and stop_tx()'s "txstop" stamp, and
     * i2s_audio_probe() is the only non-trivial thing in it: register reads,
     * two esp_clk_tree_src_get_freq_hz() calls and a 40-argument printf. The
     * pcmbuf helpers it calls are pure integer arithmetic, so the suspect is the
     * probe itself -- temporary instrumentation that had no business being in
     * the shipping teardown path. Define TX_STOP_PROBE to put it back while
     * debugging; it is still reachable from sink_play() and the tick probe. */
    i2s_audio_probe();
#endif

    playing = false;
    queued = 0;
    stop_tx();

    /* Hand every descriptor back to the CPU. If the DMA left one owned by
     * itself, the engine would suspend on it at the next start and nothing would
     * ever come out.
     *
     * Take the lines from memory before writing them, as fill() does: the engine
     * has been clearing owner bits through the bus, and a blind write here would
     * publish whatever our cache last held rather than what is actually there. */
    discard_dcache_range((const void *)desc, sizeof(desc));
    for (int i = 0; i < XT_TX_RING; i++)
        desc[i].dw0 = 0;
    commit_dcache_range((const void *)desc, sizeof(desc));
    pend_valid = 0;

    /* Restore the interrupt invariant. sink_lock() masks GDMA_INT_DONE on the
     * first lock and sink_unlock() only unmasks it when the count returns to
     * zero, so a stop that arrives while the mixer still holds the play lock
     * leaves the count nonzero and the completion masked for good: the next
     * session's sink_unlock() decrements to a nonzero count and skips the
     * re-enable, no OUT_DONE ever fires, and playback hangs with the buffer
     * full. Measured that way -- locked=2, int_ena 0x4 with the DONE bit clear,
     * dma irq=0, pcmbuf 181504/524288 and rising.
     *
     * A stop ends the session, so any lock still held is moot: drop the
     * accounting and unmask unconditionally. */
    locked = 0;
    wr(GDMA_TX_INT_ENA, rd(GDMA_TX_INT_ENA) | GDMA_INT_DONE);
}

static void sink_set_freq(uint16_t freq)
{
    i2s_set_frequency(hw_freq_sampr[freq]);
}
/* TEMPORARY: periodic CHAIN sample.
 *
 * i2s_audio_probe() was only ever called from event sites -- sink_play, sink_stop,
 * pcm close -- so every snapshot in the logs describes the transmitter at some
 * moment other than steady playback. That made a per-frame property like slot
 * occupancy unmeasurable: the ring experiment compared one event snapshot against
 * another and called the difference a verdict, twice, and neither was one.
 *
 * This samples while audio is actually running. It runs from tick_task_thread
 * (the tick tasks are NOT run in the interrupt on this core -- see the note in
 * apps/main.c), so printf is legal here; the ISR must never reach this path.
 * Every 200 ticks at HZ=100 is two seconds, which is often enough to see drift
 * and rare enough not to drown the console. */
/* Compact transmit status: register reads and one short printf, nothing else.
 *
 * This exists because TX_PROBE_QUIET silenced the whole probe, which threw away
 * the one number the DSCR_ERR storm guard is there to watch -- err, i.e. how many
 * times the engine found a descriptor the CPU still owns. The guard makes those
 * survivable (24 over ~11400 frames), so without err we cannot tell whether that
 * rate is stable or climbing.
 *
 * Deliberately NOT the full CHAIN dump: that holds the newlib stdout lock for
 * ~100ms pushing ~900 bytes over USB-serial, and it calls
 * esp_clk_tree_src_get_freq_hz() and the pcmbuf helpers. A heavy probe on a hot
 * path has already proved to be the fault being observed twice -- once as ~117ms
 * stalls in the writer, once as the crash localised by the breadcrumb trail. This
 * line is ~90 bytes and touches no locks the audio path takes.
 */
static void i2s_dma_line(void)
{
    char ow[XT_TX_RING + 2];
    char ln[XT_TX_RING * 6 + 2];
    int i, o = 0, l = 0;

    /* The DMA writes descriptor results back through the bus, not through our
     * D-cache, so drop the cached line first or the owner bits describe the
     * CPU's memory rather than the hardware's. */
    discard_dcache_range((const void *)desc, sizeof(desc));

    for (i = 0; i < XT_TX_RING; i++)
    {
        ow[o++] = (char)('0' + (int)((desc[i].dw0 >> 31) & 1));
        l += snprintf(ln + l, sizeof(ln) - l, "%d,",
                      (int)((desc[i].dw0 >> 12) & 0xfff));
    }
    ow[o] = '\0';

    printf("DMA: err=%d startup=%d sess=%d err/sess=%d | irq=%lu "
           "refill=%lu br=%d q=%d lk=%d play=%d | "
           "ena=%08x link=%08x own=%s len=%s | fifo f%d/e%d\n",
           xt_dscr_err_n, err_startup, xt_tx_sessions,
           xt_tx_sessions ? xt_dscr_err_n / xt_tx_sessions : 0,
           xt_sink_irq_n, xt_refill_n,
           xt_refill_branch,
           queued, locked, playing,
           (unsigned)rd(GDMA_TX_INT_ENA), (unsigned)rd(GDMA_TX_LINK),
           ow, ln,
           (int)((rd(GDMA_TX_FIFO_STATUS) >> 0) & 1),
           (int)((rd(GDMA_TX_FIFO_STATUS) >> 1) & 1));
}

static void i2s_probe_tick(void)
{
    static int div;

#ifdef TX_USE_IDF_DRIVER
    /* Printed whether or not pcm_playing is set. The failure this is chasing is
     * "audio stops but the app lives", and if the core has already cleared
     * pcm_playing then a probe gated on it says nothing about the moment things
     * went wrong -- which is exactly what happened on the first Stage 1b run:
     * zero probe lines in the whole log. */
    {
        static int tdiv;

        if (++tdiv >= 100)
        {
            tdiv = 0;
            i2s_tx_probe();
        }
    }
#endif

    /* Baseline payload census readout (thread context). Compare against the
     * IDF writer's "payload:" line: if the hand-rolled path shows the same
     * ~all-zero fraction, the mixer's silence is normal and TX was never the
     * fault; if it shows ~0%, the IDF path is causing it. */
    if (bl_n)
    {
        static unsigned long last_n;
        static int cdiv;

        /* Throttled to every 10s. As written it printed whenever bl_n moved --
         * roughly ten times a second, because the census samples every 16th
         * frame at 172/s. Each line is ~70 bytes, which at 115200 baud is ~6ms of
         * blocking UART per line inside the tick task: 60ms of every second spent
         * printing a diagnostic about audio timing. A trend over 10s says the same
         * thing as a trend over 100ms. */
        if (++cdiv >= 1000 && bl_n != last_n)
        {
            cdiv = 0;
            printf("BLcensus: %lu frames sampled, mean-nonzero=%lu, all-zero=%lu (%lu%%)\n",
                   bl_n, bl_sum / bl_n, bl_zero, bl_zero * 100 / bl_n);
            last_n = bl_n;
        }
    }

#ifdef TX_PROBE_QUIET
    /* Instrumentation is the suspect, so it has to be switchable without
     * deleting it.
     *
     * The writer's own measurement came back with gap mean=5804us (exactly the
     * nominal frame time) but recurring ~117ms stalls every few seconds, and
     * 117ms is what this function's CHAIN dump costs to push over USB-serial.
     * The dump holds the newlib stdout lock for that whole time; the writer's
     * pull path goes through pcm_play_dma_complete_callback() into the mixer,
     * which logs, so the writer blocks behind the probe that is supposed to be
     * watching it. starve=0 during those stalls is consistent, not reassuring:
     * IDF's auto_clear_after_cb zeroes each completed descriptor, so a stalled
     * writer shows up as the DMA faithfully clocking silence, not an empty FIFO.
     *
     * If the halting disappears with this off, the bug was never in the
     * transmit path.
     *
     * It turned out the halting was never the writer at all, and the dump itself
     * was a hazard -- but quiet must not mean blind. err is the metric the
     * DSCR_ERR storm guard exists to watch, so the compact register-only line
     * stays on every 2s while playing, and only the full dump is silenced. */
    if (!pcm_playing)
    {
        div = 0;
        return;
    }

    if (++div >= 200)            /* 200 ticks at 100 Hz = every 2s */
    {
        div = 0;
        i2s_dma_line();
    }
    return;
#endif

    if (!pcm_playing)
    {
        div = 0;
        return;
    }

#ifdef XT_PANIC_SELFTEST
    /* Diagnostic bisection, not a feature. The open question is why IDF's panic
     * handler re-enters and aborts on every crash here, printing nothing but
     * "Panic handler entered multiple times". Two causes fit and they need
     * opposite fixes:
     *
     *   - the handler or its console path is broken in general. Then hooking the
     *     exception vector buys nothing, because a hand-written recorder still has
     *     to get its output somewhere across a reboot.
     *   - our crashes corrupt something the handler depends on (stack, or a frame
     *     that ended up in PSRAM). Then a trap from a clean thread context dumps
     *     properly, and a vector hook is exactly the right move.
     *
     * __builtin_trap() emits an illegal instruction from ordinary thread context
     * with a valid stack -- the same exception path, minus whatever a real crash
     * may have already broken. Build with -DXT_PANIC_SELFTEST, play something for
     * ~6 s, and read the dump. Compile-time guarded so it cannot ship.
     *
     * Separate counter on purpose: div resets every 200 ticks to drive the probe,
     * so a threshold above 200 on div would never fire.
     */
    {
        static int selftest_ticks;
        if (++selftest_ticks == 600)
        {
            printf("SELFTEST: trapping now (clean thread context)\n");
            __builtin_trap();
        }
    }
#endif

    if (++div < 200)
        return;

    div = 0;

    /* How long the transport is down, in microseconds. The frame itself is 1024
     * bytes = 5.8 ms at 44.1k stereo 16-bit, and the outlink is stopped for this
     * whole window, so the FIFO has to cover it. Compare the two and the click
     * either is explained or is not. Counters reset each interval so the report is
     * a fresh worst case, not a monotonic mark that only ever shows one old sample. */
    if (xt_isr_n > 0)
    {
        int hz = esp_clk_cpu_freq();
        unsigned long n = xt_isr_n, mx = xt_isr_cyc_max;
        unsigned long rm = xt_isr_refill_max, avg = xt_isr_cyc_sum / n;

        xt_isr_n = 0; xt_isr_cyc_max = 0; xt_isr_refill_max = 0;
        xt_isr_cyc_sum = 0;

        printf("ISR: %lu in window  avg=%luus  max=%luus  worst=%luus"
               "  (refill part max=%luus)  [%d MHz]\n",
               n,
               hz ? avg * 1000000UL / (unsigned long)hz : 0,
               hz ? mx * 1000000UL / (unsigned long)hz : 0,
               hz ? xt_isr_cyc_worst * 1000000UL / (unsigned long)hz : 0,
               hz ? rm * 1000000UL / (unsigned long)hz : 0,
               hz / 1000000);
    }

    /* Sample-width verdict, captured in gdma_tx_isr context by fill() and printed
     * here where printing is legal. */
    if (xt_width.ready)
    {
        int k, nz_even = 0, nz_odd = 0;

        printf("SINK bytes: fill #%lu, len=%lu, first %d half-words"
               " (%d nonzero):\n  ", xt_width.filln, xt_width.len,
               xt_width.n, xt_width.nonzero);
        for (k = 0; k < xt_width.n; k++)
        {
            printf("%04x ", xt_width.data[k]);
            if (xt_width.data[k] != 0x0000 && xt_width.data[k] != 0xffff)
            {
                if (k & 1)
                    nz_odd++;
                else
                    nz_even++;
            }
            if ((k & 7) == 7 && k + 1 < xt_width.n)
                printf("\n  ");
        }
        printf("\nSINK width test: active even-half=%d odd-half=%d of %d"
               "  => %s\n",
               nz_even, nz_odd, xt_width.n / 2,
               xt_width.nonzero == 0
                   ? "HEAD IS SILENCE"
                   : (nz_even > 3 * nz_odd || nz_odd > 3 * nz_even)
                       ? "32-BIT samples: one half of each pair is quiet"
                       : "16-bit PCM: both halves carry signal");
        xt_width.ready = 0;
    }

    i2s_audio_probe();

    /* xt_stack_report() is stood down. It has produced two artefacts in a row:
     * first scanning for DEADBEEF, a fill xTaskCreateStatic overwrites; then with
     * the loop inverted, so it counted the run of fill from the top and reported
     * that as usage -- which is why all eleven threads showed an identical
     * used=320, a property of the buffer layout and not of any thread.
     *
     * Nothing now indicates a stack problem. The reports that motivated this were
     * the canary check reading FreeRTOS's own initial context frame at the bottom
     * of the buffer and concluding the canary had been consumed. With that
     * debunked there is no evidence left to size stacks against, and I have made
     * the measurement wrong twice, so it is not worth a third attempt until a raw
     * word dump establishes where the frame and the fill actually are. */
}

static void sink_init(void)
{
    xt_sink_init_n++;

#ifdef HAVE_RECORDING
    /* Bring up IDF's receive channel BEFORE the transmitter is configured.
     *
     * i2s_new_channel() acquires IDF's GDMA group and, on first acquisition,
     * calls gdma_ll_reset_register(group) -- which resets every channel in the
     * block, including the OUT channel that i2s_init() then configures by hand.
     * Going first meant that reset landed on top of our transmitter and nothing
     * ever put it back: GDMA_TX_INT_ENA 0x06 -> 0x00 and the descriptor address
     * 0x0cf700 -> 0, with playback dead for the rest of the boot from the moment
     * recording was first armed. See i2s_rx_chan_create() in pcm-xtensa.c. */
    if (!i2s_rx_chan_create())
        printf("AUDHW: RX channel create failed\n");
#endif

    i2s_init();

#ifdef TX_USE_IDF_DRIVER
    /* Deliberately AFTER i2s_init(): if creating an IDF channel did disturb the
     * GDMA block, we want to see it happen to a transmitter that is already
     * configured, because that is the ordering the real cutover has to survive.
     * The two registers printed are exactly the ones pcm-xtensa.c:240 records as
     * the casualties of a block reset -- INT_ENA 0x06 -> 0x00 and LINK losing
     * the descriptor address -- so this says directly whether the claim in
     * i2s_tx_chan_create() holds on hardware and not just in source. */
    {
        uint32_t ena_before = rd(GDMA_TX_INT_ENA);
        uint32_t link_before = rd(GDMA_TX_LINK);

        bool ok = i2s_tx_chan_create();

        printf("TX1a: GDMA INT_ENA %08x -> %08x   LINK %08x -> %08x  %s\n",
               (unsigned)ena_before, (unsigned)rd(GDMA_TX_INT_ENA),
               (unsigned)link_before, (unsigned)rd(GDMA_TX_LINK),
               (ok && ena_before == rd(GDMA_TX_INT_ENA) &&
                link_before == rd(GDMA_TX_LINK))
                   ? "UNCHANGED (claim holds)" : "DISTURBED (revert stage 1a)");

        /* Stage 1b: the hand-rolled transmitter was never started above, so
         * this is the point where IDF may take GDMA OUT channel 0. */
        if (ok)
        {
            if (!i2s_tx_chan_configure(
                    hw_freq_sampr[builtin_pcm_sink.caps.default_freq]))
                printf("AUDHW: TX idf configure failed\n");

            i2s_tx_thread_start();
            printf("TX1b: writer thread created, configured=%d\n",
                   tx_std_configured);
        }
    }
#endif

    audiohw_preinit();

    /* The capture ADC is deliberately not brought up here. This is the playback
     * sink: resetting the ES7210 and programming its dividers on every boot --
     * and on every playback rate change -- and holding the I2C bus across two
     * 10ms delays to do it, belongs to the recording session that needs it and
     * nowhere else. It now happens in pcm_rec_dma_start(). */
    sink_set_freq(builtin_pcm_sink.caps.default_freq);

    /* TEMPORARY: the boot baseline. Every later arena dump is read as a delta
       from this picture of who holds what. */
    xt_arena_dump("at boot");

    /* Snapshot the transmit path the instant initialisation completes. Compared
     * against the same dump later, this says whether the configuration was ever
     * there at all or was taken away afterwards. */
    printf("AUDHW -- immediately after sink_init --\n");
    i2s_audio_probe();

    /* Periodic playback sample; see i2s_probe_tick(). */
    tick_add_task(i2s_probe_tick);
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
        .volume_type  = PCM_NATIVE_VOLUME_TYPE,
    },
    .ops = {
        .init     = sink_init,
        .postinit = audiohw_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_play,
        .stop     = sink_stop,
    },
};

/* TEMPORARY: two registers, on demand, from the recording path. i2s_audio_probe()
 * prints a whole wall of state and is only reachable at boot and at a playback
 * stop that this bug never lets happen, so narrowing it to the two numbers that
 * actually changed -- GDMA_TX_INT_ENA going 0x06 -> 0x00 and GDMA_TX_LINK's
 * address field going 0x0cf700 -> 0x000000 -- and calling it either side of each
 * IDF call in the session lifecycle is what names the offender. */
void i2s_tx_gdma_peek(const char *where)
{
    printf("GDMA %-22s ena=%08x link=%08x\n", where,
           (unsigned)rd(GDMA_TX_INT_ENA), (unsigned)rd(GDMA_TX_LINK));
}
