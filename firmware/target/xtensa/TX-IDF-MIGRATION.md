# TX migration to `esp_driver_i2s` — staged plan and log

Goal: replace the hand-rolled GDMA transmit path in `i2s-xtensa.c` with IDF's
`esp_driver_i2s`, keeping the `builtin_pcm_sink` contract and the existing
threading model. Rockbox stays in control; IDF is used where it does the work.

This document is the spine of a multi-stage change. **Every stage records its
result here before the next one starts**, because the previous attempt was
abandoned with no record of what actually went wrong.

**Status: not started, for transmit.** `i2s-xtensa.c` still hand-rolls the TX GDMA layer
it describes below; the I2S side of TX goes through IDF's HAL (`i2s_hal_init`,
`i2s_hal_set_tx_clock`, `i2s_hal_std_set_tx_slot`, `i2s_hal_std_enable_tx_channel`) but not
through `esp_driver_i2s`. The receiver is a different case and already uses
`esp_driver_i2s`, so this plan is about the transmit half only. `i2s-xtensa.c` points at
this file from its own header comment, so the two stay together.

---

## Scope, measured rather than assumed

TX already goes through IDF's **HAL** for the I2S side
(`i2s_hal_std_set_tx_slot`, `i2s_hal_std_enable_tx_channel`). Only the GDMA
layer is hand-rolled — 19 raw writes over six registers:

| register | writes |
|---|---|
| `GDMA_TX_LINK` | 10 |
| `GDMA_TX_INT_ENA` | 4 |
| `GDMA_TX_INT_CLR` | 2 |
| `GDMA_TX_CONF0` / `CONF1` / `PERI_SEL` | 1 each |

plus the descriptor ring, `gdma_irq()`, `arm()`/`fill()` and start/stop.
Roughly 250 lines. **Not** the whole file.

The abandoned attempt (`wip/tx-on-idf-i2s`, a stash) deleted 772 lines and added
435 — far more than the GDMA layer. That was its first mistake.

---

## Why the previous attempt went "bursty" — proven from IDF source

From `components/esp_driver_i2s/i2s_common.c`:

- `i2s_channel_write()` **`memcpy`s into a free DMA buffer and returns**
  (line 1382). It does not wait for transmission.
- The only blocking is `xQueueReceive(handle->msg_queue, …)`, and that queue is
  refilled by the **TX-done ISR** (line 693,
  `xQueueSendFromISR(msg_queue, finish_desc->buf)`).
- Line 1369: when `desc_num > 2` IDF deliberately **keeps one descriptor in
  reserve**, so the writer runs `desc_num - 1` descriptors ahead of the DAC.
- Line 559: `dma_desc_buf_size = dma_frame_num * slot_num * data_bit_width`, so
  for 16-bit stereo one descriptor is `dma_frame_num * 4` bytes.

The stash used `dma_desc_num = 8`, `dma_frame_num = 256`. One descriptor is
therefore 1024 bytes = **exactly one Rockbox mixer frame**, and the writer could
sit **7 frames ≈ 40 ms ahead of transmission** while reporting every frame to the
pcm core as consumed. The mixer believes its output has drained, stops, the ring
empties, `write()` blocks, then it refills in a burst.

**The thread design was correct.** The bug was reporting consumption against
*copy* instead of *transmit*, and that is a sizing decision, not an
architectural one.

### The sizing knob

`dma_frame_num = 256` is right: one descriptor = one mixer frame, so report
granularity matches Rockbox's.

| `dma_desc_num` | writer ahead | report error | runway |
|---|---|---|---|
| 2 | ≤1 frame | ≈ accurate | 5.8 ms |
| 3 | 2 frames | 1 frame early | 11.6 ms |
| 4 | 3 frames | 2 early | 17.4 ms |
| 8 | 7 frames | **40 ms early** | 46 ms ← what broke |

`desc_num = 2` also changes the reserve predicate (`<= 0` rather than `<= 1`), so
the relationship is not linear. **Measure; do not reason.**

---

## Baseline (Stage 0) — frozen

| | |
|---|---|
| HEAD | `84f5b57b52` |
| `rockbox.elf` | `f5128bc2c87e` |
| `rockbox.esp32s3` | `5b4d2fef1b5d`, 1,204,416 bytes |
| device | running `f5128bc2c87e` |
| plugins | 160 `.rock` built, no `MEOWKIT_ESP32S3` gates left |

Measured over the long session on this exact build (`/tmp/f1.log`, ~34 min of
playback, 353,393 frames served):

| metric | baseline value |
|---|---|
| descriptor faults (`err`) | **flat at 1** — one boot-time event, none per frame |
| sink play-lock (`locked`) | `0` and `1` only — **never negative** |
| ISR teardown window | avg 0–17 µs, **worst 16 µs** (against 5.8 ms per frame) |
| refill branch | `3` (explicit completion path), `queued=0` |
| faults | 1 (see below) |
| subjective | "minor crackle, getting easy to ignore"; user notes low-bitrate
  sources have their own artefacts and reserves judgement |

### Known defect in the baseline — do not confuse it with this work

One crash occurred, and **the TX path was healthy at the moment of it**:
`err=1` flat, `locked=0`, `link=002d05c0` (start bit set), `fifo f1/e0`. What was
actually true: `codec run=0 in=0`, `pcmbuf used=0/524288`, `insert=0`, yet
`core playing=1` and `irq` still climbing. That is the end-of-track /
track-transition shape — the same family as the old EOM panic and the fade-out
crash, and unrelated to the transmit path.

Consequence: **this migration can neither be blamed for that crash nor credited
with fixing it.** If it disappears or changes character, that is coincidence and
must be re-tested against `f5128bc2c87e` before drawing any conclusion.

---

## Contract mapping

| sink op | today (ISR-driven) | IDF (thread-driven) |
|---|---|---|
| `play(addr,size)` | arm `desc[0]`, `link.start` | publish buffer, release writer semaphore |
| completion | `gdma_irq` on `OUT_DONE` | `write()` unblocking when a descriptor frees |
| `lock()` / `unlock()` | mask/unmask `GDMA_INT_DONE` | gate the writer thread — no interrupt to mask |
| `stop()` | `link.stop`, clear ring | clear `playing`, release semaphore, `i2s_channel_disable` |

`lock`/`unlock` is the subtle one. It must stay a **re-entrant counter that
cannot go negative** — see the `locked=-1` bug fixed in `196a531002`: once
negative, neither `locked++ == 0` nor `--locked == 0` ever fires again and the
guard silently stops doing anything. A thread gate has the identical failure
mode.

---

## Landmines already known — do not re-learn them

- **`i2s_new_channel()` resets the whole GDMA block.** RX hit this; the fix is
  ordering (`pcm-xtensa.c:231`). Creating a TX channel while RX is live is the
  same hazard.
- **TX is the I2S bus master**, driving MCLK/BCLK/WS to *both* codecs, and
  `i2s_rx_pads_take()` / `i2s_rx_pads_release()` swap those pads on mode change.
  IDF does not do this; it stays ours.
- **The write path is serialised by a binary semaphore** — one `write()` at a
  time, and a long one holds it.
- **Nothing may print in ISR context** (`gdma_tx_isr` comment; it aborts on the
  newlib stdout lock). Learned twice more today.
- Writer thread stack must live in **DRAM**, not PSRAM.
- Keep the `builtin_pcm_sink` contract; do not upend the threading model.
- Recording and playback are exclusive on this target.

---

## Stages

Each stage has a pre-declared revert criterion. Honour it rather than salvaging
a result after the fact.

### Correction to the original staging, found by reading IDF source

Stage 1 was originally "create the IDF channel alongside and route nothing".
**That state does not exist.** Chased through IDF:

- `i2s_new_channel()` → `i2s_acquire_controller_obj()` → `i2s_hal_init()`, which
  is *only* `hal->dev = I2S_LL_GET_HW(port_id)` (`i2s_hal.c:64-68`). Memory,
  semaphores, a queue. **No GDMA allocation and no hardware writes.** Safe.
- The GDMA grab is `i2s_init_dma_intr()` (`i2s_common.c:804`), which calls
  `gdma_new_ahb_channel()` — and that is reached from
  **`i2s_channel_init_std_mode()`** (`i2s_std.c:338`), not from
  `i2s_new_channel()`.
- `gdma_new_ahb_channel()` → `do_allocate_gdma_channel()` →
  `gdma_acquire_group_handle()` → `gdma_ll_reset_register()` (`gdma.c:696`).

So the block reset the RX comment describes is real, but it is attributed to the
wrong call: it lands at `init_std_mode`, not `new_channel`. `i2s_rx_chan_create()`
calls both (pcm-xtensa.c:266 and :294), which is why the measurement looked like
`new_channel` was the culprit.

Why that kills the coexistence plan: our hand-rolled transmitter owns **GDMA OUT
channel 0** (`GDMA_TX_CONF0 = GDMA_BASE + 0x60`). The moment
`i2s_channel_init_std_mode()` runs for a TX channel, IDF allocates the first free
OUT channel — and it has no knowledge that we are using channel 0, so it takes
it, mid-playback. **The cutover is atomic by construction.**

### Stage 1a — scaffolding, and `i2s_new_channel()` only
Add a compile-time switch `TX_USE_IDF_DRIVER` (default **off**) so the two
implementations are mutually exclusive and the A/B is a rebuild, not a race.
With the switch on, call `i2s_new_channel()` for a TX channel on `I2S_NUM_1` and
nothing further — no `init_std_mode`, no routing.

Proves: IDF can claim the I2S1 controller while our HAL still drives it; the
hand-rolled path still carries audio unchanged; RX is undisturbed; boots clean.

**Revert if** any baseline metric moves, or the device fails to boot.

### Stage 1b — the cutover (atomic)
With the switch on: skip `gdma_init()` and the `gdma_irq` install entirely, and
instead do `i2s_channel_init_std_mode()` + the writer thread. There is no
in-between state to stage, so this is where all the risk lives and it cannot be
split further.

**Revert if** audio does not play, or the baseline metrics move.

### Stage 2 — measure at `desc_num = 2`
Judge against the baseline table above: descriptor-fault equivalent, mixer
stall, `pcmbuf` fill band, and ears.
**Revert if** the crackle is worse than Stage 0.

### Stage 3 — sweep `desc_num` 2 → 3 → 4
Pick on measurement, not on reasoning. Remember `desc_num = 2` also changes
IDF's reserve predicate (`<= 0` rather than `<= 1`), so it is not linear.

### Stage 4 — delete the hand-rolled ring, ISR and descriptor code
Only once Stages 1b–3 are stable.

---

## Why bother, given the cutover is atomic and risky

One concrete gain beyond "use IDF drivers": **the migration removes the single
most dangerous interaction in this file.** The GDMA block reset only threatens us
because the transmitter is invisible to IDF's GDMA bookkeeping — IDF resets the
block and has no idea channel 0 is live. Once both directions are IDF's, IDF
knows about both and the ordering hazard that killed playback the first time
recording was armed (`pcm-xtensa.c:228-254`) stops existing.

---

## Stage log

| stage | date | artifact | result |
|---|---|---|---|
| 0 | baseline frozen | `f5128bc2c87e` (see correction below) | see table above |
| 1a | done | image `f838a7ae3570`, switch ON | **PASS** |

### Stage 1a result

```
TX1a: IDF TX channel created 0x3c116990 (not configured, not routed)
TX1a: GDMA INT_ENA 00000006 -> 00000006   LINK 008d05c0 -> 008d05c0  UNCHANGED
root_menu: 1   faults: 0
```

Playback then ran on the hand-rolled path with the IDF channel existing
alongside: 1399 frames, `refill` 1:1 with `irq`, `branch=3`, `queued=0`,
`err` flat at 0–1, `locked` never negative, ISR worst 10 µs, no faults.

**The claim is confirmed on hardware, not just in source:** `i2s_new_channel()`
does not disturb a live GDMA OUT channel. The switch itself was verified by
comparing `i2s-xtensa.o` built from the pre-scaffolding file against it built
with the scaffolding present and the switch off — identical symbol sets,
identical non-debug section sizes, and identical concatenated section content
(`dc78a6799715651f3b39ff166453959a`).

### Correction to the Stage 0 baseline record

The `rockbox.elf` / `rockbox.esp32s3` hashes in the table above are **not valid
comparators across commits**, for two reasons found while verifying Stage 1a:

1. **There are two `rockbox.elf` files.** `build-meowkits3/rockbox.elf` is
   Rockbox's own pre-link (`root.make:396`) and for ESP targets is *not in the
   dependency chain of what gets flashed*. IDF links `idf-app/build/rockbox.elf`
   from the archives; that becomes `rockbox.bin` and then `rockbox.esp32s3`.
   The stale copy sat at `f5128bc2c87e` while the real artifact moved.
2. **The ESP app descriptor embeds `git describe`** in its `version` field
   (`f-174-g0f193de9` on this build), so the image hash changes on every commit
   regardless of code. Within one commit it is deterministic (verified: two
   rebuilds produced the same image).

So the correct way to A/B this migration is **switch-vs-switch at the same
commit**, and the correct object-level check is section content of the
translated `.o`, as used above. Image hashes are only meaningful as "did
something change", never as "is this the baseline".


## Stage 1b — FAILED, reverted by switch (kept in tree, `TX_USE_IDF_DRIVER` off)

Symptom: audio plays in short bursts separated by silence, correct pitch, nothing
lost. Reproduced across twelve instrumented builds; every candidate fix measured
no change.

### The controlled A/B that localises it

Same tree, same track, same mixer; only `TX_USE_IDF_DRIVER` differs. The probe is
a byte census (count of non-zero bytes per 1024-byte handover) — scale-free, so it
cannot be fooled by sample width the way a peak-amplitude probe was.

| | hand-rolled path | IDF writer thread |
|---|---|---|
| mean non-zero bytes / frame | **992** | 0–430 |
| all-zero frames | **1%** (43 / 2256) | **~50%** (82 / 171 windows entirely zero) |
| audio | continuous | halting |

Shape on the IDF path: a burst of 2–44 real frames, then 110–150 consecutive
all-zero frames (0.6–0.9 s). The zeros are *extra* material, not replacements —
which is why nothing sounds missing.

### Everything on the TX side is identical and healthy on both paths

`size min=1024 max=1024 mean=1024 (other=0) sameptr=0` · 172 writes/s ·
`gap mean=5804us max=5811us` · `tmo=0 errs=0` · `park n=0` · `gate=0` ·
`noask=0` · `nodata=0` · `en=1` throughout · `starve=0`.

So the core hands the IDF writer silence and the hand-rolled path sound, from the
same `pcm_play_dma_complete_callback()`.

### Hypotheses tested and refuted by measurement

| hypothesis | how it was refuted |
|---|---|
| `desc_num` too small | 2 → 4 → 12 → 24, no change |
| IDF prefetch predicate (`<= desc_num>2 ? 1 : 0`) | same |
| writer descheduled by priority | `PRIORITY_USER_INTERFACE` → `PRIORITY_REALTIME`, no change |
| `sink_stop()` tearing the channel down | `TX_STOP_KEEPS_CHANNEL`, no change |
| descriptor ≠ mixer frame (512 vs 256) | fixed to 256, no change |
| lookahead over the mixer (deep ring) | ring back to 2, no change |
| missing `refill()`'s `if (pend_valid) return` | added; `noask=0` — never applicable |
| play-lock race on the swvol double buffer | gate moved to just before the pull; `gate=0` — lock never held at pull time |
| probe lying about silence | replaced peak-amplitude (which reported impossible >32767 values) with a byte census; result held |

### The one difference left

The baseline calls `pcm_play_dma_status_callback()` /
`pcm_play_dma_complete_callback()` from the GDMA `OUT_DONE` **ISR**; the IDF path
calls them from a **thread**. With `WANT_SWVOL` the callback returns
`pcm_dbl_buf[pcm_dbl_buf_num]` (`pcm_sw_volume.c:230`), and something in that
path appears to be context-sensitive. `gate=0` and `noask=0` say the play lock and
the `ops.play()` delivery path are never involved on the IDF side, which is itself
part of the signature: on the baseline the same window shows `locked=1`.

### If this is picked up again

Do not start from ring sizes, priorities or descriptor geometry — all measured
inert. Start by taking the completion interrupt back: register a done callback on
IDF's GDMA channel (`gdma_register_done_callback()`) and run the pull in ISR
context exactly as `refill()` does. If the zeros survive *that*, the fault is in
the swvol/mixer path's assumption about caller context, not in this driver.

### Instrument traps hit during this investigation (all since fixed in-tree)

- `gap_sum / n * 1000000 / 240000000` overflows 32 bits — a 5804 µs gap printed
  as 6 µs. Cycles→µs at 240 MHz is `/ 240`.
- A `printf` format string gained a duplicated line from a bad edit; four `%lu`
  then read garbage off the stack and printed `en=1024 stop=4294967295`.
- `starve`/`idle` sampled at handover cannot see starvation: IDF's
  `auto_clear_after_cb` zeroes each completed descriptor, so a starved writer
  looks identical to a healthy one.
- Peak amplitude computed as `int16` is meaningless if the width assumption is
  wrong; it reported maxima above 32767, which is impossible. Count non-zero
  bytes instead.

## CONCLUSION — Stage 1b cannot be completed: IDF's esp_driver_i2s TX cannot satisfy Rockbox's driver contract

Proven from IDF source, not inferred:

    components/esp_hw_support/dma/gdma.c:900
        if ((intr_status & GDMA_LL_EVENT_TX_EOF) && tx_chan->cbs.on_trans_eof)
            need_yield |= tx_chan->cbs.on_trans_eof(...);

`tx_chan->cbs` is a single stored struct, not a list. IDF's I2S driver occupies
`on_trans_eof` with `i2s_dma_tx_callback` (i2s_common.c:843), and *that callback is
what refills `dma.msg_queue`*. Registering our own overwrites it, after which
`i2s_channel_write()` blocks forever. The two are mutually exclusive by
construction:

| who owns the GDMA completion callback | consequence |
|---|---|
| IDF | the driver's only completion observable is `msg_queue`, reachable only by calling `i2s_channel_write()`. There is no way to invoke `pcm_play_dma_complete_callback()` at the hardware moment. |
| us | IDF's write path breaks, so it is no longer "using IDF drivers". |

Rockbox requires the driver to call the core's completion callback when the
hardware has actually finished transmitting — that call is what makes the mixer
produce, and what wakes the codec via pcmbuf space. IDF offers no hook for it
(`grep` for `register_tx_callback` / `i2s_done_cb_t` across `esp_driver_i2s`:
nothing). The MeowKit reference firmware never needs one because it has no such
contract: `Speaker_Class::_i2s_write()` is a bare `i2s_write(..., portMAX_DELAY)`
producer loop with no consumption reporting at all.

**Therefore the hand-rolled GDMA ring is not incidental complexity to be removed;
it is the integration point.** The parts of the migration that were always sound
stay: TX uses IDF's *HAL* (`i2s_hal_std_set_tx_slot`), and RX uses full IDF
`esp_driver_i2s` end to end. Only the TX GDMA layer is DIY, and it has to be.

Do not re-attempt Stage 1b. If IDF ever exposes a per-descriptor TX completion
hook, revisit.

## Where the remaining defects actually live

`arm()` runs with `out_auto_wrback` cleared and `next = 0`, so the engine STOPS
after each descriptor and software restarts it. Despite `XT_TX_RING 4`, only one
frame — 5.8 ms — is ever in flight. Any hiccup longer than that is audible, which
is the single-frame dropout and the micro-stutter. The fix is the other mode our
own AGENTS.md records: `out_auto_wrback` ON with every slot DMA-owned and filled,
so 3-4 frames (17-23 ms) are queued continuously and there is no per-frame
restart at all.

## Follow-up: does >1 frame in flight break the mixer? NO — measured on the DIY path

Asked because the IDF failure looked like it might be "Rockbox's contract cannot
tolerate a driver that keeps more than one buffer live". It is not. Converting the
hand-rolled path to a free-running double buffer (`out_auto_wrback` on, `next`
chained, ring prerolled, ISR refilling the completed slot with no
`link.stop`/`link.start`) gave **continuous play** with `all-zero` frames at 4%
against the 1-frame baseline's 1%. So multi-frame lookahead is safe, and the IDF
silence was specific to IDF's `i2s_channel_write()` path.

Two facts that came out of it, both load-bearing for any future attempt:

1. **`OUT_DONE` means "transferred to the peripheral", not "played by the DAC".**
   The I2S FIFO is ~256 bytes — about 1.45 ms — so it bridges nothing. With two
   slots the DONE that tells us to refill slot 0 arrives while the engine is
   already heading back to it, so we arm a descriptor the engine may be about to
   fetch. Measured symptom: `fifo f0/e1` (empty) and stutter slightly *worse* than
   the 1-frame design.
2. **Four slots hung**: interrupt-watchdog panic on CPU0 in ISR context, right at
   `SINK: first play`. Making the preroll conservative (ask the core at most once
   per slot, stop at the first decline, arm silence for the rest) did **not** fix
   it, so the storm is in the free-running ring itself, not in over-asking the
   core. The prime suspect is the `GDMA_INT_DSCR_ERR` branch, which does
   `link.start` unconditionally: if any slot ends up unowned, that re-triggers
   forever. Not yet proven.

Reverted; the shipping path is unchanged. If this is picked up, prove the
DSCR_ERR-storm theory first (count DSCR_ERR entries and refuse to `link.start`
twice in a row without an intervening DONE) rather than varying ring depth again.

## SOLVED — free-running ring + DSCR_ERR storm guard (best audio the port has produced)

The build that hung at four slots was not refuted by depth; it was refuted by a
self-sustaining error. Adding deterministic recovery and re-running:

```
faults: 0
census: all-zero 0%  (11 of 1283), mean-nonzero 985
irq=11431  refill=11435          <- 4 ahead: the preroll, as designed
err=0 -> err=24                   <- 1 DSCR_ERR per 476 frames, survivable
```

Against the 1-frame baseline's 1% all-zero frames and the two-slot attempt's 4%.

**What actually mattered was the guard, not the depth.** `err` climbing to 24
proves unowned slots still occur about 0.2% of frames. Depth alone did not remove
them -- it only made them survivable, because with four slots the engine takes
three more descriptors (17ms) before it needs the slot we just rearmed, so a
transient no longer lands under the engine's feet. The guard handles the rest: a
second `DSCR_ERR` with no intervening `DONE` means the engine is parked on a slot
the CPU owns, so instead of `link.start` onto it again we stop, re-arm **every**
slot so none can be unowned, and start once.

Design now in the shipping path:

- `XT_TX_RING 4`, `out_auto_wrback` **on**, `next` chained (never zero)
- `start_tx()` prerolls all four slots, asking the core at most once per slot and
  stopping at the first decline; remaining slots armed with silence
- ISR `DONE` refills the slot that completed; **no `link.stop`/`link.start`**
- A completion arriving inside the play lock sets `cb_pending` and is replayed on
  unlock (as3525's `play_callback_pending` convention)
- `DSCR_ERR` twice with no `DONE` between calls `tx_recover()`

Still open: why slots go unowned at 0.2%. The guard makes it survivable, not
impossible, and the honest reading is that a refill occasionally loses the race
even with 17ms of margin. Worth a look when something else is not on fire.

## The scaffolding is deleted; the one variant that was never tested

`TX_USE_IDF_DRIVER` and its ~1000 lines are gone from `i2s-xtensa.c`. What remains
is the locked-in design, and it is worth stating in one line so nobody re-reads the
history as an unfinished migration:

**IDF's HAL configures the transmitter (`i2s_hal_set_tx_clock`,
`i2s_hal_std_set_tx_slot`, `i2s_hal_std_enable_tx_channel`), IDF's
`esp_driver_i2s` drives the receiver, IDF builds and boots the app. Rockbox owns
exactly one thing: the TX GDMA descriptor ring, because GDMA exposes a single
completion slot and IDF's own driver occupies it.**

The recipe further up this file ("register a done callback on IDF's GDMA channel")
is the route that *cannot* work, for the reason the CONCLUSION section proves: taking
that slot overwrites `i2s_dma_tx_callback`, which is what refills `dma.msg_queue`,
and `i2s_channel_write()` then blocks forever. Do not start there.

Never tested, and it sidesteps the slot entirely:

**Ride `i2s_channel_write()` without touching GDMA.** Leave `i2s_dma_tx_callback`
installed, call `i2s_channel_write()` from a dedicated thread with a short timeout,
and drive `pcm_play_dma_complete_callback()` from the moment `write()` returns. IDF
returns a buffer once the peripheral has taken it, which is the same instant our
`OUT_DONE` fires, so the consumption report is honest. It never asks for a
per-descriptor signal, so there is no slot to fight over.

The cost is lookahead rather than correctness. IDF keeps `desc_num` internal buffers
in flight, so a report is late by up to `desc_num * dma_buf_len / bytes_per_second`:
`desc_num = 2` at one mixer frame per buffer is ~11.6 ms, which `as3525` and
`imx233` tolerate; `desc_num = 4`, the reference firmware's value, is ~23 ms and
begins to matter for anything latency-sensitive.

If it is attempted, watch the payload census and not `err`. `err` counts ring wraps
and tells you nothing here. `all-zero` frames during *continuous* play means the
mixer is being paced too slowly -- and compare the delta between consecutive census
windows, because the counters are cumulative and a run that includes track switching
will report historical transition silence as if it were current.
