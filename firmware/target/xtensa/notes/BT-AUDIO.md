# Bluetooth audio out for MeowKit — feasibility and plan


> Internal working note. This file and the others in this directory are the
> port's own records — plans, logs and research threads. The reviewer-facing
> documents are ../README.md and ../TESTING.md, and the user-facing ones are the
> repository README and the manual chapter for this target.
**Status: closed.** The goal is hardware-blocked, not deferred — ESP32-S3 has no BR/EDR
radio, so A2DP and AVRCP cannot be built on it (evidence in §1) and LE Audio is unavailable
for the same class of reason. Recorded as permanent in `PENDING.md` §4. Kept because §2 and
§4 document what Rockbox's core already provides for a non-DAC PCM sink, which stands
regardless of this chip and would transfer to a classic-capable co-processor unchanged.

Goal as stated: make the player an **A2DP source** with **AVRCP** so Bluetooth
headphones and speakers can be paired, have the mixer's output switch to the headset on
connect and back to the ES8311 DAC on disconnect, keep USB audio working independently of
it, and add UI for discovery/pairing. Reference library:
[pschatzmann/ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP), which wraps ESP-IDF's
Bluedroid A2DP/AVRC APIs.

Everything below is measured against this checkout: Rockbox branch `esp32-s3-idf` at
`5da7fe90f1`/`1b64056f31`, ESP-IDF **v5.5.5** at `/home/jhohertz/esp/esp-idf`.

---

## 1. The blocker is the radio, not Rockbox

A2DP and AVRCP are **Bluetooth Classic (BR/EDR)** profiles. ESP32-S3 has a
Bluetooth **5.0 LE** radio and no BR/EDR baseband, and ESP-IDF encodes that as a
capability:

| Evidence | Location |
|---|---|
| `SOC_BT_CLASSIC_SUPPORTED` is defined for **one** SoC in the tree: `esp32` | `components/soc/esp32/include/soc/soc_caps.h:455`; nothing under `components/soc/esp32s3/` |
| ESP32-S3 declares only BLE capability | `components/soc/esp32s3/include/soc/soc_caps.h:45` `SOC_BT_SUPPORTED 1`, `:599-602` `SOC_BLE_SUPPORTED`, `SOC_BLE_MESH_SUPPORTED`, `SOC_BLE_50_SUPPORTED`, `SOC_BLE_DEVICE_PRIVACY_SUPPORTED` |
| Classic is therefore unselectable | `components/bt/host/bluedroid/Kconfig.in:44-46` — `config BT_CLASSIC_ENABLED` `depends on BT_BLUEDROID_ENABLED && ((BT_CONTROLLER_ENABLED && SOC_BT_CLASSIC_SUPPORTED) || BT_CONTROLLER_DISABLED)` |
| Every classic profile hangs off that symbol | `BT_A2DP_ENABLE` (`Kconfig.in:92`) and the AVRCP options sit under `BT_CLASSIC_ENABLED` |
| Espressif's own classic example tests run on one target | `examples/bluetooth/bluedroid/classic_bt/pytest_classic_bt_test.py:20,55` — matrix entries are `'esp32|esp32'` |

Two things that look like counter-evidence and are not:

- **The S3 controller binary is named `libbtdm_app.a`.** `components/bt/controller/CMakeLists.txt:24-33`
  links `lib_esp32c3_family/esp32s3/libbtdm_app.a`. "BTDM" is the shared build for the
  C3-family controller; the C3 is BLE-only too, so the name says nothing about BR/EDR.
  Note that `CONFIG_BT_BLUEDROID_ENABLED` is selectable on S3 ("Bluedroid - Dual-mode",
  `components/bt/Kconfig:16-20`) — but with `BT_CLASSIC_ENABLED` impossible, a Bluedroid
  build on S3 is BLE-only Bluedroid.
- **Our `sdkconfig` used to enable Bluetooth.** `idf-app/sdkconfig.defaults` carried
  `CONFIG_BT_ENABLED=y` and the NimBLE roles. That was configuration for a capability this
  chip does not have, and it has since been removed — see §7, which also corrects an
  incorrect cost estimate made earlier in this document.

There is no LE Audio escape hatch either. LE Audio needs isochronous channels (BLE 5.2+)
and an LC3/Audio stack. ESP32-S3 declares BLE 5.0 with **no ISO/CIS capability** anywhere
in its `soc_caps.h`, there is no `le_audio` component in this IDF checkout
(`components/bt/host/` contains only `bluedroid` and `nimble`), and `esp_ble_iso_api.h`
in the Bluedroid API directory has no content backing it. So: **no wireless stereo audio
output is possible from this chip, in any profile, with this toolchain.**

That is the honest starting point. The rest of this document is (a) the Rockbox-side
design, which is real, already supported by the core, and reusable whatever carries the
audio; and (b) the transport options that could actually put sound in headphones.

---

## 2. What Rockbox's core already gives us

Established by reading the tree, not assumed:

- **A sink registry exists in core.** `firmware/export/pcm_sink.h` defines
  `struct pcm_sink { struct pcm_sink_caps caps; const struct pcm_sink_ops ops;
  unsigned long pending_freq, configured_freq; }` with ops
  `init/postinit/set_freq/lock/unlock/play/stop`, and
  `enum pcm_sink_ids { PCM_SINK_BUILTIN, PCM_SINK_IAP /* under USB_ENABLE_IAP */,
  PCM_SINK_NUM }`.
- **There is already a non-DAC sink in-tree.** `firmware/usbstack/iap/audio.c:189`
  defines `iap_pcm_sink`; `firmware/pcm.c:87-91` registers both sinks in
  `sinks[PCM_SINK_NUM]`. IAP is exactly the shape we want: the mixed PCM frame goes to a
  consumer that is not a DAC.
- **Handoff point.** `firmware/pcm.c:121` `sinks[cur_sink]->ops.play(addr, size)`, or
  `firmware/pcm_sw_volume.c:327` when the sink declares `volume_type == PCM_SINK_SWVOL`.
- **Switching exists and continues playback.** `mixer_switch_sink()`
  (`firmware/pcm_mixer.c:431`) → `pcm_switch_sink()` (`firmware/pcm.c:318`): bounds-check,
  swap `cur_sink`, re-derive the rate from the old sink's caps, `pcm_apply_settings()`,
  then if playing `old_sink->ops.stop()` → `pcm_get_more_int()` →
  `pcm_play_dma_start_int()` on the new sink; `mixer_switch_sink` follows with
  `mixer_handle_sampr_change(SAMPR_NUM(pcm_get_frequency()))`. The IAP stack drives it from
  the transport: `usbstack/iap/platform.c:414` switches to IAP, `usbstack/usb_iap.c:452`
  switches back on disconnect. That is precisely "switch to it on connect, back to the DAC
  on disconnect", already implemented as a pattern.
- **Rate semantics are per-sink.** `pcm_set_frequency()` maps the requested rate onto
  `sink->caps.samprs` with `round_value_to_list32(..., false)` and falls back to
  `default_freq` if the rate is absent; `pcm_get_frequency()` returns the *current sink's*
  pending rate, which is what the mixer is then retargeted to. So a sink declaring
  {44100, 48000} makes 96 kHz playback resample down through the mixer rather than fail.
- **One active sink only.** `cur_sink` is a single value and the mixer produces one frame
  per callback (`pcm_mixer.c:122-125`, handing out `downmix_buf[downmix_index]`). Nineteen
  `pcm-*.c` drivers in the tree, all single-sink. **Fan-out is new design** — see §6.
- **The nearest tap precedent is input-side.** `mixer_channel_set_buffer_hook()`
  (`pcm_mixer.h:147`, `next_buffer(start,size)`) exists and its only user in the tree is
  `apps/plugins/oscilloscope.c`. It observes a *channel*, pre-mix, so it cannot yield the
  mixed output. `audio_set_output_source()` / `audiohw_set_monitor()` are analog mux
  inside one codec and mostly input routing — neither is a sink mechanism.
- **Synthetic button events have a public API.** `firmware/export/button.h:66-68` exports
  `button_queue_post(long id, intptr_t data)`,
  `button_queue_post_remove_head(...)` and `button_queue_try_post(long button, int data)`
  (implemented in `firmware/drivers/button_queue.c:129-165`). That is the door for remote
  control events; no new core surface needed for it.

---

## 3. Transport options that could actually reach headphones

| Option | What it is | Cost / risk | Verdict |
|---|---|---|---|
| **A. BR/EDR co-processor** | An ESP32 (the original, which *is* classic-capable) or a purpose-built BT-audio RX module, fed PCM over I2S or UART, running A2DP source + AVRCP. AVRCP events come back over the same link as bytes. | One more chip on the board, plus a link layer. Rockbox side is a `pcm_sink` plus a button-injection shim. No IDF BT work in Rockbox at all. | **The only realistic path on this hardware.** |
| **B. Move the port to an ESP32** | The original ESP32 has BR/EDR and Bluedroid A2DP source. | Loses PSRAM (4 MB max, no octal), 16 MB flash, native USB OTG, and re-does the whole port. | Non-starter. |
| **C. LE Audio / ISO** | BLE 5.2+ isochronous streaming with LC3. | S3 declares no ISO capability; IDF ships no LE Audio stack in this checkout. Needs both to change. | Not available. |
| **D. Wire instead** | USB DAC class already works, plus the 3.5 mm-less board reality. | None. | Already shipped; not wireless. |

Option A deserves a hardware note before any software: it implies a board change (a second
SoC, a link, and power for it). If the intent is "the current MeowKit hardware does BT
headphones", the answer is no, and the plan stops here. If the intent is "Rockbox on
ESP32-family hardware should support BT headphones", option A is a clean design and the
Rockbox work below transfers unchanged.

---

## 4. The Rockbox-side design (valid for any transport)

Guarded so nothing changes for other targets — matching exactly how IAP is wired
(`USB_ENABLE_IAP` set in `firmware/export/config.h:1461` and used to gate
`pcm_sink.h:62`, `pcm.c:89-91`, `usb.h:181`, `usb_core.c`).

New knob, defined only in our target config header: `HAVE_BT_AUDIO_SOURCE`.

Core seams it needs, each an `#ifdef` that defaults to today's behaviour:

1. `firmware/export/pcm_sink.h` — add `PCM_SINK_BT` to `enum pcm_sink_ids` under
   `#ifdef HAVE_BT_AUDIO_SOURCE`, mirroring the `#ifdef USB_ENABLE_IAP` line above it.
2. `firmware/pcm.c` — `[PCM_SINK_BT] = &bt_pcm_sink` in `sinks[]` under the same guard,
   and the `extern struct pcm_sink bt_pcm_sink;` declaration likewise.
3. That is the whole core delta. `pcm_init()`/`pcm_postinit()` already loop over
   `PCM_SINK_NUM`, per-sink `pcm_is_ready[]` and `pending_freq`/`configured_freq` are
   managed for us, and `pcm_switch_sink()` already continues playback across the switch.

Target-side, new file `firmware/target/xtensa/bt-audio.c`:

```c
static unsigned long bt_samprs[] = { 44100, 48000 };

struct pcm_sink bt_pcm_sink = {
    .caps = {
        .samprs       = bt_samprs,
        .num_samprs   = ARRAYLEN(bt_samprs),
        .default_freq = 0,
        /* The sink has no hardware volume of its own; Rockbox keeps software
         * volume and pcm_sw_volume.c becomes the caller of ops.play(). */
        .volume_type  = PCM_SINK_SWVOL,
    },
    .ops = {
        .init     = bt_sink_init,
        .postinit = bt_sink_postinit,
        .set_freq = bt_sink_set_freq,
        .lock     = bt_sink_lock,
        .unlock   = bt_sink_unlock,
        .play     = bt_sink_play,
        .stop     = bt_sink_stop,
    },
};
```

The rules `play()` has to obey, all learned the hard way on this board:

- **Copy, don't retain.** `play()` is handed `downmix_buf[]` — a double buffer the mixer
  refills on the next callback (`pcm_mixer.c:65-67`). Anything that holds the pointer
  past that reads the next frame. So `play()` memcpy's into our own ring. IAP gets away
  with latching the pointer because its USB pull happens immediately in the same flow
  (`usbstack/iap/audio.c:155-160`).
- **Non-blocking, no allocation, no printing, no I2C.** The play path runs under
  `pcm_play_lock()` with our `sink_lock()`/`sink_unlock()` masking GDMA `out_eof`
  (`i2s-esp32s3.c:1210-1246`), and the USB DAC path calls into the mixer inside
  `disable_irq_save()` (`usb_audio.c:1380-1396`, noted at `i2s-esp32s3.c:1250`). A ring
  write with a dropped-oldest policy is the correct shape; blocking is fatal.
- **Rate changes arrive as `set_freq(index)`,** an index into our caps, and the mixer has
  already been retargeted by `mixer_switch_sink()`. No renegotiation needed beyond
  telling the transport its new rate.
- **`lock`/`unlock` can be no-ops** if the sink owns no DMA — that's what IAP does
  (`sink_nop`). Decide deliberately rather than inheriting our I2S driver's masking, which
  exists because that sink owns GDMA.

Lifecycle: whoever owns the link calls `mixer_switch_sink(PCM_SINK_BT)` when audio is
established and `mixer_switch_sink(PCM_SINK_BUILTIN)` on teardown, from thread context —
never from `play()`.

---

## 5. Remote control, pairing and UI

**AVRCP events → Rockbox button events.** As an A2DP source we are the AVRCP *controller*;
the headset's own buttons arrive as *target* passthrough commands
(`ESP_AVRC_PASSPLAY_*`: play, pause, stop, next, prev, vol up/down, seek). Map them in the
transport shim to `button_queue_post(BUTTON_..., 0)` using codes our keymap already
produces, so no new actions are invented. A press needs a matching `BUTTON_REL` post, and
volume events should map to the existing WPS volume codes rather than to a new concept.
This is the same trick used for hardware that has no physical buttons, and it means the
theme/WPS/menu behave identically whether the input came from the stick or a headset.

Worth noting what AVRCP gives us that Rockbox has no home for: absolute volume
(`avrcp_absolute_volume` exists as an IDF example) maps to Rockbox's volume setting rather
than a new knob, and metadata/cover-art *requesting* (AVRCP CT) has no Rockbox consumer
that would want it while we are the source.

**Pairing and discovery.** Three stages, cheapest first:

1. *Fixed identity*: device name from a config define (`PRODUCT_NAME`/`MODEL_NAME` are
   already per-target), discoverable on request, "Just Works" secure simple pairing — no
   keyboard or display pairing needed for headsets. The reference library's
   `a2dp_source.start("MyMusic")` is this shape.
2. *Card-sourced target*: read a preferred device name or MAC from the card before
   initiating a connection — e.g. `/.rockbox/bt/target.txt`. Read it in a thread that owns
   storage, never from the sink or an ISR, and cache the parsed value. This is the
   "read file w/ name or mac off card" idea, and it is the right interim because storage
   already has a thread model we respect.
3. *Real UX*: a settings submenu — enable, discoverable, scan results, connect/disconnect,
   status. Rockbox has **no Bluetooth UI precedent anywhere** (a full-tree search for
   `bluetooth|a2dp|avrcp|CONFIG_BT|HAVE_BT` in `firmware/`, `apps/`, `docs/` returns only
   IAP spec enums, SonyNWZ database fields, an ALSA enum and erosq GPIO comments), so this
   is new menu surface. There is no `target_menu` seam in this tree either, so menu entries
   mean `apps/main.c` / `apps/settings_list.c` touched under `#ifdef HAVE_BT_AUDIO_SOURCE`.
   That is the adoption-hygiene cost of the feature and it should be raised with upstream
   rather than hidden in a target.

Status display should live in a place users already look — the WPS can show it via a theme
token only if a token exists, so a settings screen plus a splash on connect/disconnect is
the honest minimum, and it is another reason the theme work (`DOC-GAPS` item 2) and this
feature interact.

---

## 6. Coexistence questions the goal raises

- **USB audio in, BT out: works.** Our USB DAC mode is on the *source* side —
  `usb_audio.c` calls `mixer_channel_play_data()` (`i2s-esp32s3.c:1250`,
  `usb_audio.c:1380-1396`), so host audio becomes a mixer channel and the BT sink plays
  whatever the mixer outputs. "BT indifferent to USB audio" is satisfied by the existing
  model, and the USB feedback/timing machinery is untouched by a sink switch.
- **DAC and BT at the same time: not supported.** `cur_sink` is one value. Local speaker
  *and* headphones simultaneously requires new design, and the shapes are: a **broadcast
  sink** whose `play()` fans the same frame out to two consumers (each must copy, and the
  slower one dictates drop policy), or an **output-side observer hook** — the
  `mixer_channel_set_buffer_hook` pattern lifted to the mixer's output, with a documented
  interrupt-context contract. A broadcast sink is target-local and cheap; the observer hook
  is a core patch that would be worth proposing upstream because it is genuinely general.
- **Sample rates.** BT sinks realistically offer 44.1/48 kHz. `pcm_set_frequency()`
  silently falls back to the sink's default when a rate isn't in its caps, so 88.2/96 kHz
  playback becomes 44.1 through the mixer's resampler. That is correct behaviour but it is
  *invisible* — worth a settings note or a debug row, since a user with "44.1 kHz forced
  off" will otherwise wonder why hi-res files sound different on headphones.
- **Latency and pause/seek.** A BT sink needs a ring sized for radio jitter (order
  tens of ms, i.e. ~8-32 KB at 44.1k stereo 16-bit). That latency means the audible
  position trails `pcm_playing`'s position, so pause/seek must drain or reset the ring
  inside `stop()` — the same class of bug as the audiobuffer flush ordering we already
  fixed on this target. Budget for it explicitly.
- **Internal RAM.** DIRAM is `340,655 / 341,760` bytes — **1,105 B free**, and IRAM is at
  100% (`MEMORY.md:147-149`). The internal *heap* has roughly 149 KB
  (`MEMORY.md:408`). Any ring, link buffers and task stacks for a BT path must come from
  that heap or from PSRAM with cache maintenance, and DMA-facing buffers must be internal.
  The NimBLE configuration was a small donor — it accounted for 4,048 bytes of linked code,
  and it is gone (§7) — but the link buffers and task stacks for a BT path must come from
  that heap or from PSRAM with cache maintenance, and DMA-facing buffers must be internal.
  There is nothing further there to reclaim.
- **Sleep.** Light sleep must be blocked while a link is active, exactly as
  `xt_light_sleep_block(true)` does while USB is armed (`usb-esp32s3.c:348`). A BT link that
  isn't blocked means the core powers down mid-stream (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y`).
- **Power.** Wireless audio adds real current on top of the ~42 mA dark-idle floor, which
  the parked battery-bench run should measure with BT active rather than assumed.
- **CPU.** Single-core, SMP off permanently. Whatever encodes SBC (or drives a link)
  competes with codecs and the mixer on one core; the mpegplayer result (11 fps of a 25 fps
  source) is the current evidence about headroom, and 44.1 kHz stereo SBC encoding is not
  free.

---

## 7. Housekeeping this investigation turned up

`CONFIG_BT_ENABLED=y` and the NimBLE stack were configured in `idf-app/sdkconfig.defaults`
(`CONFIG_BT_NIMBLE_ENABLED=y`, three roles, `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`, and the
derived `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y` with a 4096-byte host task). Rockbox has
no BLE consumer and no Bluetooth path exists on this silicon, so those lines were removed.

**An earlier version of this section said they cost nothing. That was wrong, and the way it
was wrong is worth recording.** The evidence was `grep 'libbtdm_app.a(' rockbox.map` and
`grep 'libbt.a(' rockbox.map` both returning nothing, and the conclusion drawn was "zero
contributing sections". The greps were accurate and the conclusion was not: they covered two
archives, while the NimBLE host is built from sources inside the `bt` component and lands
elsewhere, so a name-scoped search for the controller archives says nothing about the host.
The measured answer is that removing the configuration **shrank the image by 4,048 bytes**
(1,295,584 → 1,291,536). After the removal `grep -c nimble rockbox.map` is 0, and the only
Bluetooth archive left is `libbtbb.a`, the baseband PHY library the WiFi coexistence layer
pulls in — which stays whatever this file says.

Lesson for anyone else auditing a link footprint here: measure the artifact size across the
change, not the presence of an archive name in the map.

---

## 8. Phases, if the goal survives the radio finding

If option A (co-processor) is chosen, the work splits so that each step is provable
without the next one existing:

| Phase | Deliverable | Proved by |
|---|---|---|
| P0 | Decide the transport. Without a classic-capable device there is nothing to build. | A written decision, not code. |
| P1 | Sink spike (the `idf-spike/` directory that held this was a bare-metal-era leftover and is deleted; a re-spike would live under `idf-app/`): PCM in, link out, no Rockbox. Measure ring size, latency, drop behaviour. | Spike on the bench, audio audible through the headphones. |
| P2 | `PCM_SINK_BT` + `bt_pcm_sink` + switch-on-connect/back-on-disconnect, guarded by `HAVE_BT_AUDIO_SOURCE`. | Playback switches to the link and back mid-track with no gap; `Debug → View HW info` row for link state. |
| P3 | Remote control: link events → `button_queue_post`, press paired with `BUTTON_REL`. | Headset play/pause/skip/volume work in the WPS and menus. |
| P4 | Pairing UX: fixed name, then card-sourced target, then a settings submenu. | Pair from cold, reconnect automatically. |
| P5 | Policy and docs: rate fallback behaviour made visible, sleep blocking, current measurement, manual chapter, `MEMORY.md`/`AGENTS.md` numbers. | Documentation pass, same as the plugin chapters. |

## 9. Open questions needing a decision

1. **Is a second chip acceptable?** This is the whole feature's feasibility question, and
   nothing in Rockbox can answer it.
2. **Do you want simultaneous DAC + link output?** If yes, that is new core design (§6),
   and I'd argue for the output-side observer hook as an upstream proposal rather than a
   target hack.
3. **Volume ownership**: `PCM_SINK_SWVOL` keeps Rockbox in charge (simplest, costs
   software volume in the mixer path), or the headset's absolute volume becomes the
   controller — which needs a mapping onto Rockbox's volume setting and a decision about
   which device wins.
4. **Codec**: SBC only is the safe answer. Anything better has licensing and CPU
   consequences on a single core.
5. **Where the device list lives**: card file (simple, inspectable, matches how we handle
   theme packs) versus settings persistence.
6. **Menu surface**: how much UI before upstream has any Bluetooth framework at all —
   because the answer determines whether this lands as target code or as a core proposal.
