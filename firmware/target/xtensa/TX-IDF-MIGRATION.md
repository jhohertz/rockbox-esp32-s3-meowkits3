# TX migration to `esp_driver_i2s` — staged plan and log

Goal: replace the hand-rolled GDMA transmit path in `i2s-xtensa.c` with IDF's
`esp_driver_i2s`, keeping the `builtin_pcm_sink` contract and the existing
threading model. Rockbox stays in control; IDF is used where it does the work.

This document is the spine of a multi-stage change. **Every stage records its
result here before the next one starts**, because the previous attempt was
abandoned with no record of what actually went wrong.

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

### Stage 0 — freeze the baseline
Done. Recorded above.

### Stage 1 — add the IDF TX channel alongside; route nothing
Create the channel in `i2s_init()` but leave the hand-rolled path carrying all
audio. Prove:
- boots to `root_menu`, no new faults
- RX's GDMA is not reset (channel ordering)
- the shared clock pads are still driven
- sample rate still measured correct

**Revert if** any of those fail, or if `rockbox.elf` behaviour changes at all.

### Stage 2 — route audio through the writer thread, `desc_num = 2`
Judge against the baseline table above.
**Revert if** `pcmbuf` shows the mixer stalling, `err`-equivalent or underrun
counters move, or the crackle is worse than Stage 0 by ear.

### Stage 3 — sweep `desc_num` 2 → 3 → 4
Pick on measurement, not on reasoning.

### Stage 4 — delete the hand-rolled ring, ISR and descriptor code
Only once Stages 2–3 are stable.

---

## Stage log

| stage | date | artifact | result |
|---|---|---|---|
| 0 | baseline frozen | `f5128bc2c87e` | see table above |
