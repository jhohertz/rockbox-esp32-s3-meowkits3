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

