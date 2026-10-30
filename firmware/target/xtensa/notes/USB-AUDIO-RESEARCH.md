# USB DAC audio corruption -- overnight research notes


> Internal working note. This file and the others in this directory are the
> port's own records — plans, logs and research threads. The reviewer-facing
> documents are ../README.md and ../TESTING.md, and the user-facing ones are the
> repository README and the manual chapter for this target.
Branch `esp32-s3-idf`, target `meowkits3`. Written after a session in which every
measurement said the transport is healthy and the sound was still wrong. This is
research, not a changelog: nothing here is applied except where section 1 says so.

---

## 1. State of the investigation

Symptom: USB DAC (CDC+HID+Audio, UAC1 async, 48 kHz stereo 16-bit) plays with
correct tempo and a steady level, plus a sharp distortion over the top and a fast
gating/pulsing. Local playback on the same mixer/I2S/DAC path is clean.

Measured healthy, on the device's own USB Audio debug page, after tonight's fixes:

| Reading | Value | Meaning |
|---|---|---|
| Playback Frequency | 48000, steady | negotiation correct |
| iface / alt | 3 / 1, steady | no stream teardown storm |
| Samples used per frame | 48.000000, locked | feedback computes and is sent |
| Samples received per frame | 192.0 (x4 display scale) | arrival = 192 B/frame = exactly nominal |
| Frames dropped | ~0.5 / s | wire healthy |
| Min / Max | mostly 1/23, sometimes 15/23 | ring empties; 15/23 is the predicted geometry |
| UNDERFLOW | flashes | channel is being stopped |
| 20 s tone vs 20 s wall clock | ends exactly together | no net loss, no net insertion |

Fixes landed tonight (all committed to the working tree, not pushed):

1. `usb_dw_target_frame_number()` existed for a reason and turned out to be the bug. SOFFN
   at full speed is a plain frame number at `DWC_DSTS >> 8`, masked to 14 bits, which is what
   the driver's generic reading already does; the override's `>> 11` made the frame advance
   ~125 times a second instead of 1000. Deleted -- see section 8 for how that was found.
2. `usb_audio_target_fb_latch()` was overridden as a tolerant elapsed-time test, on the claim
   that the generic exact-frame window could be missed and leave feedback unarmed for a whole
   session. The proof quoted for it -- `Samples used per frame` reading 48.000000 on one build
   and 0.000000 on the next -- was taken while the frame number was wrong by a factor of eight,
   so it proves nothing. The override has since been removed and the generic latch used; the
   check is that `used` settles near 48.87 rather than reading 0.000000.
3. `usb_audio_target_fb_buffers_filled()` was clamped to non-negative. The seam reserves
   negative for the "-1 = I measured nothing" sentinel, so a target that returns
   `queued - target` has its correction silently discarded in exactly the case that
   needs it: the controller could only ever decelerate.
4. `fb_due` / `fb_latch` era's leftovers cleaned up; cadence is Rockbox's own.
5. `NR_BUFFERS` 32 -> 48 and `MINIMUM_BUFFERS_QUEUED` 16 -> 24 behind new `#ifndef`
   seams in `usb_audio.c` (defaults unchanged for every other target).
6. TX: `fill()` and `fill_silence()` now always arm `XT_TX_SLOT` bytes, padding a short
   mixer frame with silence, so a completion always means "the DAC consumed one frame".
7. `USB_AUDIO_BYPASS_DSP` experiment (core seam + target define). Not a fix; it is
   inert for every other target and must be removed before publishing.

---

## 2. Exonerated by measurement -- do not re-open without new data

| Suspect | How it was excluded |
|---|---|
| Host scheduling quantum | played at quantum 1024/512/256/128: no meaningful change |
| RX FIFO too small for ISO | `RX_FIFOSZ` = 136 words = 544 B vs documented worst case ~448 B, `_Static_assert` at `usb-xtensa.c:99` passes; the real failure mode (`ISOODPR` set, `XFERSIZE` frozen) is absent |
| Received length arithmetic | `EPTSIZ_XFERSZ` for non-EP0 is 19 bits and the SVD puts `PKTCNT2` at [21:19]: `size - bytes_left` is correct, 192 in, 192 reported |
| Error-status discards | `dw_ep->status` is set to 0 at the end of every completion and `-2` only if `transferred > sizeleft`: the `status != 0` discard path is unreachable |
| RX buffer placement / alignment | `CONFIG_CPU == ESP32S3` is in the `USB_STATIC_ALLOC` list, so the ISO buffer is the internal-SRAM `usb_storage_transfer_buffer`, `USB_DEVBSS_ATTR aligned(32)` |
| Cache-line straddle at 48 kHz | 192 bytes = exactly 6 x 32-byte lines from a 32-byte-aligned base |
| Mixer input width | `asm/pcm-mixer.c` consumes `const int16_t *`, matching the class's `p16out` |
| Mixer discarding partial slots | lines 182-183 advance `chan->start`/`size` by `last_size`; loops are `do{}while` so exact multiples are handled |
| DAC / codec setup asymmetry | cold boot straight to USB vs after local playback: identical |
| The entire DSP library | `USB_AUDIO_BYPASS_DSP` removed `dsp_process()` from the path (verified: zero references in `usb_audio.o`) and the sound did not change |
| Sample-rate / clock error | arrival is 192 B/frame and tempo is correct; a 16-20 % consumption excess would be ~3 semitones on local playback, which is not heard |

---

## 3. Are we mixing UAC1 and UAC2? -- no, and here is the audit

Answer to the question: **we are UAC1, and consistently so.** `usb_audio.h` says so
explicitly ("This is USBAudio 1.0. USBAudio 2.0 is notably _not_ backwards
compatible"). Verified in the descriptors:

* `ac_header.bcdADC = 0x0100` -- UAC 1.0
* `USB_AS_GENERAL` with `wFormatTag = FORMAT_TYPE_I_PCM`, `bDelay`, `bTerminalLink`
* `USB_AS_FORMAT_TYPE` / `FORMAT_TYPE_I` with `bNrChannels 2`, `bSubframeSize 2`,
  `bBitResolution 16`, `bSamFreqType` + 3-byte `tSamFreq[]` -- UAC1 shapes
* Endpoint CS `SAMPLING_FREQ_CONTROL` with **3-byte** `SET_CUR`/`GET_CUR` -- UAC1
* UAC2 would instead need clock source/selector/multiplier entities and **4-byte**
  `SET_CUR` on the clock source (`wValue = 0x0100`, `wLength = 4`). We emit none of that
  and the host's requests are 3 bytes and parse correctly, so the negotiation is UAC1
  end to end. Nothing to "pick" -- it already picked, and correctly.

Cross-checked the feedback encoding against TinyUSB `audio_device.c`:

```c
// TinyUSB, UAC1:  For FS format is 10.14
*(fb++) = (audio->feedback.value >> 2) & 0xFF;
*(fb++) = (audio->feedback.value >> 10) & 0xFF;
*(fb++) = (audio->feedback.value >> 18) & 0xFF;
...
usbd_edpt_xfer(..., uac_version == 1 ? 3 : 4);
```

That is byte-for-byte what `encodeFBfixedpt()` does on the FS path
(`fixedpt = value / 4`, then three bytes), and it confirms the `<< 4` line left
"for posterity" in Rockbox's encoder really would be wrong. `usb_drv_port_speed()` is
also right for this core (`(DWC_DSTS & 0x6) == 0`, Espressif puts `enumspd` at bits
2:1), and `lsusb` showing `bInterval = 1` proves it reads false on a full-speed bus.

Four genuine deviations worth fixing on their own merits -- none of them a spec mix:

1. `as_iso_synch_in_ep.wMaxPacketSize` is hardcoded **4** while on FS we send **3**
   bytes. Tolerated by Linux; Windows descriptors checkers complain.
2. `as_playback_cs_interface.bDelay = 1` claims 1 ms of latency. Real latency is the
   24-slot gate plus the GDMA ring plus the mixer frame: roughly 35-45 ms. This is what
   hosts use for A/V sync, so video will be out of sync by that much.
3. The Feature Unit advertises `MUTE | VOLUME` but there is **no AudioControl
   interrupt endpoint** (the code has a TODO for it), so the device cannot report
   changes it made on its own -- and `feature_unit_set_volume()` writes
   `global_status.volume`, i.e. a host volume change retunes the *player's* volume,
   from the USB interrupt.
4. **We advertise 96000 and 88200, which the declared packet cannot carry.** 48 kHz
   stereo 16-bit is 192 B per 1 ms frame and fits in `USB_AUDIO_BUFFER_SIZE 320`, but
   96 kHz is 384 B and 88.2 kHz is 352.8 B -- both larger than the endpoint's
   `wMaxPacketSize` and larger than the armed buffer. A host that selects those rates
   is guaranteed to be truncated. The advertised rate list and the packet size are two
   independent constants that must be derived from one another.

### The finding that explains "nothing I change matters"

At full speed, feedback is **samples per 1 ms frame, integer**. TinyUSB's own limits:

```c
uint32_t const frame_div = (TUSB_SPEED_FULL == tud_speed_get()) ? 1000 : 8000;
audio->feedback.min_value = ((sample_freq - 1) / frame_div) << 16;
audio->feedback.max_value = (sample_freq / frame_div + 1) << 16;
```

For 48 kHz FS that is **47 or 49** around a nominal 48: the whole authority of the
feedback endpoint is **one sample per frame, i.e. 2.08 %**, and the host is entitled to
clamp to that band. Consequences:

* No amount of target-side gain, limit or setpoint can command more than +/- 2 %.
  Our `fb_limit_samples()` of 8 and setpoint of 36 are theatre on an FS bus.
* The P-term divides `buffers_filled` by 4, so a queue error of fewer than 4 buffers
  produces **zero** correction. A dead band, not a bug, but it means the controller is
  silent in the regime where we are operating (queue ~1-23, error < 4 half the time).
* A real UAC1 device **dithers** between 47/48/49 to hold the queue. Ours emits
  48.000000 forever, which is what "the feedback is healthy and changes nothing" looks
  like from the outside.

So the queue cushion cannot be regulated at FS to better than about 2 %. That is not a
bug to fix in the controller; it is a constraint the *buffer geometry* has to absorb --
which is what the 48/24 seam buys, and why 15/23 behaved and 1/23 does not.

---

## 4. Prior art, and what each one offers

| Implementation | Relevant design point | Use here |
|---|---|---|
| **TinyUSB `audio_device.c`** (upstream, indexed) | per-EP `tu_fifo` + linear buffer, re-armed per packet; feedback from measured SOF vs cycle count with min/max clamp; ISO IN must return ZLP when it has nothing | the model for a dithering, measured feedback value; the ZLP rule explains our feedback EP behaviour |
| **`espressif/usb_device_uac`** (what `esp-box/examples/usb_headset` actually uses -- the link) | managed component over TinyUSB; app gets `output_cb(buf, len)` per ISO packet plus `set_volume_cb`/`set_mute_cb`; **UAC device on this silicon via TinyUSB, not DWC2-direct** | proves clean UAC1 audio is achievable on ESP32-S3; its volume/mute callbacks are separate from the player's own, which is the right shape |
| **Rockbox `ingenic_x1000/usb-x1000.c`** (the only other DWC2 target in the tree) | `rx_fifosz = 816` of 3576 words; `ahb_burst_len = HBSTLEN_INCR16`; `ahb_threshold = 0` with the note *"Disable Rx FIFO thresholding... Synopsys recommends disabling it because it can cause issues during certain error conditions"* | we already match the threshold (0) but **not** the burst length: ours is `HBSTLEN_SINGLE`. Candidate one-liner |
| **Rockbox `hosted/hiby/usb-dac-hiby.c`** | a real USB DAC into the same `PCM_MIXER_CHAN_USBAUDIO`: pump thread + frame-indexed int16 ring, hands the mixer `DAC_CHUNK_FRAMES` whole frames | the mixer-facing half is directly applicable; the thread half is not, because the class's `dsp` handle is file-static in `usb_audio.c` |
| **Asymmetry found in the class** | `usb_audio.c` registers `mixer_play_cbs { .get_more = ... }` with **no `sampr_changed`**, and never sets a sampling frequency on the borrowed `dsp_get_config(CODEC_IDX_AUDIO)` instance | a real gap (the codec path always sets it), but exonerated for *this* symptom by the bypass experiment |
| **Espressif esp-usb docs** | FS-only, EP0 + 6 endpoints, max 5 IN including EP0 (our profile uses **all five**); and `CONFIG_TINYUSB_DWC2_PTI_ENABLE`: *"enable Periodic Transfer Interrupt (`DCTL.IgnrFrmNum`)... isochronous even/odd frame matching is ignored so a transfer can be armed in the current or next (micro)frame"* | see hypothesis 1 |

---

## 5. Data flow, buffers, and views

| Stage | Size / unit | Producer | Consumer | Views | Notes |
|---|---|---|---|---|---|
| DWC2 global RX FIFO | 136 words = 544 B | USB bus | core DMA | none | holds ISO(192) + CDC(64) + HID(64) + EP0: adequate, asserted |
| ISO OUT endpoint | armed 320 B, PKTCNT 1 | core DMA | `fast_transfer_complete` | **one single buffer, armed per packet** | the structural weak point, see below |
| `rx_buffer` | 320 B, internal SRAM, aligned 32 | core DMA | `dsp_process()` in ISR | 1 | borrowed from the MSC transfer buffer (profiles are exclusive) |
| `dsp_buf` ring | 48 slots x 1280 B stride, 192 B useful = 60 KB | ISR | mixer (GDMA ISR) | 1 | `dsp_buf_size[i]` is the DSP's *output* byte count |
| mixer frame | `mix_frame_size` = 1024 B = 256 frames | mixer | pcm sink | 1 | 5.33 slots per pass: the unit mismatch that sets the sawtooth |
| GDMA TX ring | 4 x 1024 B = 21 ms | `fill()` | I2S | 1 | now always armed full-length |
| I2S FIFO + DAC | small | I2S | ES8311 | none | proven by local playback |

Two structural observations:

* **The ISO OUT endpoint has exactly one armed buffer and no descriptor chain.** DWC2
  ISO needs the driver to re-arm between packets; Rockbox does that inside the
  completion ISR, after `dsp_process()`. Any latency between completion and re-arm
  costs a whole 1 ms frame, and ISO has no retry. Everything we measured says that is
  not happening tonight (drops ~0.5/s), but it is the reason the class runs the DSP in
  the ISR at all -- the note in `usb_audio_transfer_complete()` says the normal handler
  is "too slow to deal with ISO data at the rate required".
* **Depth is denominated in slots, appetite in frames, and neither derives from the
  other.** `NR_BUFFERS`, `MINIMUM_BUFFERS_QUEUED`, `MIX_FRAME_SAMPLES` and
  `USB_AUDIO_BUFFER_SIZE` are four independent constants; the cushion the device
  actually has is a derived quantity that appears nowhere in the source. The two new
  seams let the target state the cushion, but the *agreement on units* the boss asked
  about is still not there: a slot is a packet, which is 48 frames at 48 kHz and 96 at
  96 kHz, so samples are only a stable unit at a fixed rate. hiby can declare frames
  because it hard-codes one rate; we advertise five.

---

## 6. Ranked hypotheses for the corruption, cheapest test first

1. **`DCTL.IGNRFRMNUM` is never set -- verified on this silicon.** Espressif documents
   that isochronous transfers do even/odd frame matching unless the bit is set: "enable
   Periodic Transfer Interrupt (PTI) in the DWC2 device controller (`DCTL.IgnrFrmNum`)
   ... isochronous even/odd frame matching is **ignored** so a transfer can be armed in
   the current or next (micro)frame." Checked rather than assumed:

   | Fact | Evidence |
   |---|---|
   | the field exists on esp32s3 | `soc/usb_reg.h:1689` `USB_IGNRFRMNUM` = `BIT(15)`, `usb_dwc_struct.h:571` `ignrfrmnum:1` |
   | reset value | 0, i.e. frame matching **enabled** |
   | Rockbox never defines or writes it | `usb-designware.h` DCTL bits are only `CGONAK` 10, `SGONAK` 9, `SGINAK` 7, `SDIS` 1; grep for `ignr\|PTI\|frnummatch` across driver, target and class returns nothing |

   So our ISO OUT arm is coupled to frame parity, and we re-arm inside the completion
   ISR *after* running the DSP: a re-arm that lands after the frame boundary has moved
   is a candidate for being taken in the wrong frame. That is a frame-coupled fault that
   would corrupt payload while leaving lengths and the drop counter nearly clean -- the
   profile we have. Caveat: Espressif's text motivates it for alternating-interval
   *IN* scheduling (Bluetooth HCI), so whether it changes an ISO OUT at bInterval=1 is
   unproven. Cheap either way.
   *Test:* `DWC_DCTL |= (1<<15)` in the target's USB enable path, one line, one flash.
2. **The distortion is level-dependent**, i.e. a gain stage, not a data fault.
   `feature_unit_set_volume()` maps the host's UAC1 8.8 dB value into
   `global_status.volume`, which reaches `audiohw_set_volume()` and the ES8311 -- and it
   is applied downstream of the bypassed DSP, so the bypass would not have removed it.
   *Test, no flash:* read the **`Volume:` line** on the USB Audio debug page (never
   once read this entire session) and compare the sound at laptop volume 100 % vs 30 %.
   If the distortion tracks level, this is it.
3. **Advertised rates the packet cannot carry** (section 3.4). If the host ever picks
   88.2/96 kHz we truncate by ~50 %. *Test:* confirm `Playback Frequency` is 48000
   during the bad audio, then trim the advertised list to what 320 B can carry
   (or raise `USB_AUDIO_BUFFER_SIZE` to >= 384 and re-check the FIFO assertion).
4. **`ahb_burst_len = HBSTLEN_SINGLE`** vs the x1000's `HBSTLEN_INCR16`. Single-beat
   AHB drains the RX FIFO slower; with three live OUT endpoints that is the difference
   between keeping up and not. *Test:* one line, one flash, low risk.
5. **Single armed ISO buffer** (section 5). *Test:* check the IDF interrupt allocator's
   `calls` vs `enable_cnt` for the USB handler to see whether we are servicing at ~1 kHz
   and keeping up, then decide whether the ISR path is the limit.
6. **Not a data fault at all but an analogue/DAC-domain artefact** -- amp enable, MCLK
   integrity, ES8311 clock config. Only the spectrum can separate this, and it would
   finally explain why every digital measurement is clean.

**The arbiter for all six is the same measurement: record the output and look at the
waveform.** Odd harmonics = clipping/gain; flat garbage floor = byte corruption; image
at 48 kHz minus tone = dropped or duplicated samples; sidebands at the gate rate = the
mutes. Recording writes a WAV, so **no SD card codec or plugin changes are involved.**

---

## 7. Morning checklist, cheapest first

1. Read **`Volume:`** on the USB Audio page while playing. Then play the tone at
   laptop volume ~30 % and ~100 %. Level-dependent = hypothesis 2.
2. Record the tone and 10 s of host silence; bring me the WAVs (pull the card or
   switch to the CDC+HID+MSC profile). Classify, then stop guessing.
3. If level-invariant: hypothesis 1 (`IgnrFrmNum`), one flash. Then 4 (burst length).
4. Trim the advertised rate list to what the packet can carry (real bug: 88.2 kHz needs
   353 B and 96 kHz needs 384 B, both over the 320 B packet; the CDC log showed Linux
   requesting 96 kHz). Done via usb_audio.c's USB_AUDIO_MAX_PACKET_BYTES, which filters
   tSamFreq and recomputes bSamFreqType and bLength so the descriptor cannot promise a rate
   the endpoint cannot receive. Raising the packet size instead is impossible on this core:
   512 B needs 176 RX words and the split leaves 136, which is what usb-xtensa.c asserts.
5. Decide the UAC1 reality check: FS feedback is +/- 2 % only, so the design has to be
   "deep buffer + dithered feedback", not "tight controller". Revisit `fb_limit` and the
   setpoint with that in mind, and consider dropping the ±8 limit back toward 1.
   Done: both target overrides on the feedback path were deleted, so the generic signed
   accumulator measures the queue and the limit is the upstream 1 sample. The setpoint is
   MINIMUM_BUFFERS_QUEUED, lowered to 22 because that is where the queue actually sits.
6. **Remove `USB_AUDIO_BYPASS_DSP` afterwards.** It is not a fix, it changed nothing,
   and it should not survive into a published tree. Done -- the define and the bypass path
   are both gone, and the restored chain cost nothing measurable (~1000 dsp_process() calls
   a second inside the USB interrupt did not move the drop rate).

---

## 8. Calls I got wrong tonight, so they are not re-litigated

* `SOFFN` field position: claimed bits 17:4, then implemented `>> 4`; both wrong. The
  frame is at `>> 11`, masked to 11 bits. Two flashes lost.
  -- **This line is itself wrong, and a later session proved it.** At full speed `SOFFN`
  is a plain frame number, not a microframe counter: the `>> 11` in the target override
  made the frame advance ~125 times per second instead of 1000, and because
  `usb_dw_epstart()` derives the isochronous RX data-PID expectation from the frame, parity
  flipped every 8 ms and roughly half the host's packets were rejected silently. Deleting
  the override and using the driver's generic `(DWC_DSTS >> 8) & 0x3fff` is what made the
  transport work. The `>> 11` claim above is kept only as a record of the error.
* Claimed 48 kHz stereo needs 384 B per frame. It needs 192 B; 384 is 96 kHz.
* Treated `Frames dropped` as a discard: it is a counter only, data still queues.
* Believed `received per frame` as an absolute: the `/8` is wrong for 4-byte frames and
  the accumulator double-counts one window, so it is a scale factor, not a measurement.
* Called `refill()`'s "re-arms the released slot empty" comment fact; every re-arm site
  already calls `fill_silence()`. The comment is stale.
* Suspected a power-of-two assumption in the ring: every wrap is `% NR_BUFFERS`.
* My first version of the TX pad ratcheted its own reference down to the first short
  frame, i.e. reproduced the bug inside the guard for it.
* Reached for a `status != 0` counter before reading how `status` is set: it is always
  0 on this path, so the counter would have counted nothing.

Process note worth keeping: eleven of tonight's measurements were bookkeeping --
levels, counts, rates, lengths, indices -- and all eleven were healthy. The one datum
that was never gathered is the waveform itself.

## 9. What the follow-up sessions measured, so these doors stay closed

Transport, in order of what moved the number:

* **Frame read fix** -- the defect that mattered. ~530 accepted packets/s became ~980.
* **Interrupt level 1 -> 2 for USB** -- the audio GDMA runs the mixer pass at level 1, so
  at the same level the two are serialised by IDF's dispatcher and a packet completion
  cannot preempt a mixer pass.
* **Deleting both feedback overrides** -- the target hooks could only ever report
  "at or above target" (a negative return means "no measurement"), which pinned the
  controller at nominal and left the ring at one slot. The generic accumulator is signed
  and measures real queue depth in both directions.
* **MINIMUM_BUFFERS_QUEUED 24 -> 22** -- discrete >=2 ms timing events went 10.4/s to
  6.6/s, and the burst test was audibly quieter. Partial, not a fix.

Tried and rejected, each on one flash:

* **Arming more than one packet per transfer.** Inert. The first attempt was unfair -- it
  armed four packets *and* cut NR_BUFFERS to 12 with a gate of 6, so the 110 -> 553 underflow
  cycles it produced measured the shrunken ring rather than the arm. The fair re-test armed two
  packets with the gate halved to 11 so the cushion in wall clock was unchanged, and Frames
  dropped stayed at ~10/s while the queue sat exactly on the new gate (Min/Max 6/11). That is
  the measurement: had the core delivered two packets per completion, the frame number would
  have advanced by 2 at each one and the class's adjacency test would have counted ~500 drops a
  second. So this core retires every isochronous packet as its own transfer whatever PKTCNT
  holds, and a wider arm cannot hold the endpoint open across a frame boundary. It also costs
  cushion: a trough of 6 two-millisecond slots is 12 ms where one-packet geometry gives 17 ms,
  so the arm is back at its default of one.
* **PTX_FIFOSZ 8 -> 4 to buy RX words.** No measurable effect either way; the RX FIFO is
  not the limiter.
* **Ping-pong receive buffers, re-arming before the payload copy.** No effect. TinyUSB's
  RX path does exactly this (audio_device.c:470-478), and it only helps if the core will
  accept the armed transfer at all.
* **DCTL.IGNRFRMNUM (TinyUSB's PTI).** Clearly worse: Samples used per Frame pinned at
  49.0000 -- the controller pressed against its +1 clamp -- and playback halted more. The
  reason is that frame matching is not free-standing here: usb_dw_epstart() programs the
  expected data PID from the frame parity on every arm (usb-designware.c:849-857, upstream's
  block), and with matching ignored the core tracks the toggle itself, so the per-arm PID
  writes fight it. TinyUSB never programs a PID from the frame number. Reverted.
* **Feedback cadence 32 -> 2 frames** (FEEDBACK_UPDATE_RATE_P 9, bRefresh 1 -- verified on
  the wire, and Linux's `u->urb->interval = 1 << syncinterval` at endpoint.c:760 confirms
  2 ms is the floor, since bRefresh outside 1..9 falls back to 1 at full speed). No audible
  change, and it costs ~500 IN transfers a second inside the completion handler. Reverted.
* **Restoring the DSP chain.** No measurable cost, no audible change; kept because it is
  what every other target does.
* **AHB burst Single -> INCR4.** No measurable change in drop rate or in what was heard, and
  no regression on audio, I2C or the flash cache path. Kept anyway: `HBSTLEN_SINGLE` was the
  odd one out among Rockbox's five other DWC2 targets (INCR4/INCR8/INCR16), and INCR4 is what
  TinyUSB's dwc2 init programs on this core with DMA enabled. The original justification --
  "no burst on the AHB side: the DMA is fed from the FIFO" -- was invented, not sourced.
  ESP-IDF's bare HAL does leave esp32s3 at Single, and only raises it to INCR for esp32-s2
  below rev 1.00 to dodge an AHB arbiter errata that does not apply here, so this is an
  alignment change rather than a hardware requirement.
* **Interrupt level 3 for USB.** Not attempted, and the reason changed mid-course: nothing
  runs above level 2 on this build, so there would have been nothing to preempt. The tick is at
  level 1, not level 3 -- sdkconfig selects `CONFIG_FREERTOS_CORETIMER_SYSTIMER_LVL1`, and the
  `ESP_INTR_FLAG_LEVEL3` line in `port_systick.c:64` sits inside an `#ifdef` for the choice that
  is not set. I read that line as unconditional and built a theory on it.
* **CONFIG_FREERTOS_HZ 1000 -> 500.** No change to the drop rate or to what was heard, which is
  what the level map predicts: the tick chains with the audio GDMA pass at level 1 and never
  blocks a level-2 USB entry. Reverted to 1000. The one thing it bought is the corrected level
  map in MEMORY.md.

**Resolved (2026-10-24): the loss was an interrupt-masked region, and it was ours.**
`disable_irq_save()` on this port is `set_irq_level(DISABLE_INTERRUPTS)`, which writes
`PS.INTLEVEL = 15`. Nothing on the core can be taken while that is set, level 2
included, so priority is simply not a factor against a mask -- which is why raising
USB to level 2 improved throughput (530 -> 980 packets/s) and never removed a single
dropped frame. `i2c_async_queue()` held that mask across `__i2c_async_submit()`, and
this target's submit *is* the transaction: it programs the command list and polls
`TRANS_COMPLETE`, about 1.6 ms for the FT6336's 14-byte read and up to its 10 ms
bound on a NACK. At the 20 Hz touch throttle that was 20 everything-off windows a
second. `Filled` sat at 18-19 against a gate of 22, i.e. `L = (gate - Filled)/4 =
0.875` frames per frame, ~875 frames/s, ~1.8%.

Turning the touch poll off brought the loss to almost nothing -- that is what located
it, and it is also why the residual looked periodic and looked like the display: any
bus user does this, and a redraw that triggers a bus read shows the same effect.
Deferring the submit until the mask is released (`I2C_ASYNC_SUBMIT_UNMASKED` in
`firmware/drivers/i2c-async.c`, target opt-in, upstream path unchanged) with the poll
restored gives `Filled` at the gate, no frames dropped under any UI activity tested,
and no pops or flutter. Exclusivity is unaffected: only the idle-bus path starts a
transaction and enqueueing the node has already made the bus non-idle.

The attribution this replaces -- IDF queue and ready-list critical sections, or the
flash/cache-disable window during a settings save -- was never ruled out and never
needed: every masked region on the UI path goes through the same queue.

For completeness, the host side was and remains innocent. `hw_ptr` advanced ~48,960/s
while the DAC consumed ~48,012/s (a 1 kHz tone measured 1000.25 Hz against the host's
clock, +253 ppm), so the host honours the feedback and was delivering everything. The
class's drop accounting is also innocent: `usb_audio_fast_transfer_complete()` only
counts, never discards. Read that counter with one caveat: it over-reads by about
0.5/s, because usb_audio.c wraps its adjacency test at `USB_FRAME_MAX 0x7ff` while the
driver returns a 14-bit frame, so every 2048 frames it counts a drop that never
happened.

The data PID asserted at arm time turned out to be load-bearing, not the leak. `usb_dw_epstart()`
programs `SETD0PIDEF` or `SETD1PIDOF` from the parity of the frame number read in the completion
handler, and those are *setters*, so the hypothesis was that an arm running in a frame that had
already advanced past the packet about to arrive forced the wrong expectation and made the core
discard that packet silently. Measured on this board, arming isochronous OUT without those
writes -- TinyUSB's shape, which programs the PID once at `edpt_open` (`dcd_dwc2.c:325-327`) and
never per transfer -- took `Frames dropped` from ~10/s to ~100/s and pinned the queue floor at
`Min 1`. So this core carries no toggle expectation across transfers and each isochronous OUT
arm must state one. The hypothesis is dead, upstream's block is correct here, and the seam that
let it be tested has been removed.

Reading any of this on the device has one constraint worth remembering: the display is the
only console in the audio profile. With the cable attached, OTG is handed to our firmware about
a second after boot, so the USB-Serial-JTAG console that would carry the breadcrumb dump is
gone before anything can listen; with the cable unplugged there is no console at all, because
GPIO 43/44 are the PMU interrupt rather than UART0. The RTC breadcrumb ring is therefore
unreachable while the DAC is in use, which is why instrumentation there was removed rather than
extended.

The acoustic rig is not an instrument. Recording the speaker with the laptop mic and
coherent-detecting a 100 Hz marker train gave 0.9 slips/s and 16.4/s on two runs of the
identical build minutes apart -- an 18x spread, because the 1 ms envelope is riding on the
speaker's 3-4 kHz resonance and room response. Use it for a gross A/B at most; never for a
before/after claim. Two practical notes: PipeWire tears down a capture stream started
before the sink is awake, so start playback first and record second; and `pw-record` takes
no `--format` flag, writing 48 kHz stereo 16-bit natively.

Descriptor and control items that are NOT our deviations. `bDelay = 1`, the absent
AudioControl interrupt endpoint, and `as_iso_synch_in_ep.wMaxPacketSize = 4` are all upstream
values that every target ships: a declared maximum of 4 while sending 3 bytes at full speed is
legal, and UAC1 gives the mute selector only a `CUR` request, so stalling `GET_RES` on mute is
conformant while `GET_RES` on volume is handled (`usb_audio.c:1243`). Fixing any of those in a
target would be a core-behaviour change for one board. What is genuinely ours and deliberately
kept: `as_iso_audio_out_ep.wMaxPacketSize = BUFFER_SIZE` (upstream's 1023 cannot be armed on a
320-byte full-speed packet) and the filtered rate list, whose aligned alternative is described
against `HW_SAMPR_CAPS`.
