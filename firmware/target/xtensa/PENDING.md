# Pending — MeowKit ESP32-S3

The single status list. `MEMORY.md` is evidence, `TX-IDF-MIGRATION.md` /
`USB-SPRINT.md` / `USB-AUDIO-RESEARCH.md` are investigation records, `DOC-GAPS.md` and
`UX-PLAN.md` are the documentation and input logs. None of them is a to-do list.
Every entry here was checked against the code before being written.

Board access: **hold BOOT, tap RESET** for download mode, then
`make flash ESPPORT=/dev/ttyACM0`. `flash` and `espverify` both depend on
`make esplayer`, so a layout overrun fails the build before the board is touched.

---

## 1. Needs the board

- **mpegplayer transport.** Keys documented (A tap pause, A held stop, stick
  volume/seek, B menu). Waiting on `.mpg` files: MPEG-1/2 Part 2 video, MP3 audio only
  (libmad — a VCD's MP2 track is silent), no scaling so ≤ 320×240, container `-f mpeg`.
  Playing any video also answers whether codec/plugin code executing from the PSRAM
  window behaves — mpegplayer allocates ≥ 2 MB there. **Waiting on you.**
- **MSPI MMU table across light sleep.** `SOC_PM_SUPPORT_RTC_PERIPH_PD` is enabled and
  `rtc_init.c:114` clears `EXTMEM_CACHE_MMU_MEM_FORCE_ON`, so the 512-entry
  `DR_REG_MMU_TABLE` may lose power; if it does, `mmu_init()`'s audiobuffer and
  framebuffer mappings are gone on wake and the first PSRAM access reads `0x0bad0bad`.
  Partially evidenced: playback resumes fine after a dark idle, so the mappings were
  still valid on wake — indirect, and a short idle may never cross a sleep boundary.
  `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` stays unset.
- **Fresh dark-idle reading on the current build.** Recorded progression is 57 → 48
  (frequency seam) → 46 (automatic light sleep on) → **42 mA** (amp/codec off when idle),
  so 42 mA already included S4 — an earlier note claiming otherwise was wrong. The core,
  plugins, USB and power policy have all moved since, so take a new number rather than
  compare against 57. `Debug → View battery` view 3 prints `battery_current()`, the
  instantaneous `current_now` (`apps/debug_menu.c:1244`), so repeated readings while
  waking are meaningful; the 128-sample average (`BATT_CURRENT_AVE_SAMPLES`,
  `firmware/export/powermgmt.h:100`) feeds the runtime estimate only.

**Answered 2026-10-26:** flash and boot (`ed9805f1e8` probes removed, restored
`open_pathfmt()` icon path), playback after the flash, playback after dark idle, and
96 kHz local playback — the `XT_MIX_MULT` slot sizing holds and the old 1 KB overflow at
96 kHz is gone.

## 2. Break out when we come to it

Crossed off the queue; each is its own small job, not a tracked pending item.

- What actually calls `battery_update()` — no caller found, so the current average's
  time span is unknown.
- Whether the GDMA TX row stays clean across sleep/wake boundaries.
- Whether the USB-JTAG boot console survives an idle sleep (it is the boot log's only
  output).

## 3. Deferred — might return

| Item | Why deferred | What would reopen it |
|---|---|---|
| `%T` regions in the built-in theme (`UX-PLAN` S6) | `skin_parser.c:1648` already maps `shuffle`, `repmode`, `pitch`, `trackinfo`, `playlist`, `listbookmarks`, `createbookmark`, so themes get them without firmware. Editing `cabbiev2` would put our 320×240 layout opinion in a file every target shares. | A theme pack proves the regions are unreliable at 320×240, or upstream accepts a touch-first default skin. |
| USB HID stick layer (`UX-PLAN` S7) | HID contexts in `keymap-touchscreen.c` never chain to `target_get_context_mapping()`; a stick mapping means editing the shared touchscreen keymap for a preference. | Upstream accepts the chaining fix, then a target mapping becomes possible. |
| Recording from anywhere (`UX-PLAN` S8) | `ACTION_STD_REC` has a handler (`apps/menu.c:495`); recording is already reachable from the WPS and the target menu. | You want a global record key. |
| Short PEK (`UX-PLAN` S2) | Intended as a screen/backlight toggle, but `ACTION_STD_SHOWSCREEN` does not exist in this tree at all — mapping it would be invention. | Upstream adds the action, or a hardware change gives PEK a distinct short-press event. |
| `HAVE_HOTKEY` | Needs a modifier with no free combination; the combo work in bookmarks/A-B/key-lock consumed the useful ones. | A gesture you are willing to spend, or upstream's hotkey model changing. |
| Pitch screen on the stick (`DOC-GAPS` §5) | Accepted as touch-only (Long Bottom-Left). Free WPS stick slots are none: Up/Down+REPEAT are volume, A+REPEAT is context/stop, B+REPEAT is quick screen. | A stick gesture you prefer over the touch cell. |
| Nine utility chapters left touch-driven (`DOC-GAPS` §10) | The grid is the natural input and upstream's mapping is reasonable; adding stick keys there is invention. | You want specific utilities on the stick. |
| `PLA_SCROLL_*` mapping | The enum entries are inside `#ifdef HAVE_SCROLLWHEEL`; wheel-less targets get `PLA_UP/DOWN` fallbacks, which is why metronome volume works. | Upstream widens the enum, or a touch-as-scroll proposal is accepted. |
| GDMA TX slots going unowned at ~0.2% | The play-lock guard makes it survivable and replays a completion on unlock; the cause is unknown and only reproducible over long playback. | A long-run reproduction. |
| ES8311 REG 0x08 / 0x0D readback | Written 255 reads 0x00; written 0x01 reads 0x02, over a working I2C master. Assumption is 0x08 is don't-care in slave mode and 0x0D reports status — not measured. `es8311_stat()` is a HW-info row so it stays observable. | A datasheet answer or a measurement. |
| USB DAC CPU boost | `.needs_cpu_boost = false` plus `cpu_idle_mode(true)` on USB entry means the core idles at the low clock during isochronous frames. Never showed a problem. | Glitches in the TX row (`sess / err / rec / sil`). |
| rbutil / build-info / themes-server entries (§4) | Depends on infrastructure and upstream acceptance we do not control. | The port goes upstream. |

## 4. Deferred — permanent unless the hardware changes

| Item | Reason |
|---|---|
| `HAVE_RTC_ALARM` wake-from-off | Driver functions exist and compile-verified in `rtc_pcf8563.c`, but Rockbox reads the alarm only at boot, there is no runtime `SYS_ALARM` handler, the PCF8563 `/INT` is unwired, and the AXP173 removes the SoC's supply at power-off. `clock`/`alarmclock` cover it while on. |
| `HAVE_KBD_POINT_MODE` | `#error`s without a point-mode layout for this screen size. |
| `HAVE_GENERAL_PURPOSE_LED`, `HAVE_LCD_INVERT` | No such hardware / no need. |
| SMP | Off permanently. The IDF SMP kernel path was rejected; single-core in practice. |
| USB DAC above 48 kHz | Full-speed isochronous FIFO ceiling, measured. Not a software knob. |
| Deeper light sleep | `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` is deliberately off: retaining the peripheral domain is what makes our hand-written I2S/GDMA/GPSPI2/LEDC/I2C/USB drivers safe. Going deeper needs save/restore for six subsystems. |
| `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` | Must stay unset: flash keeps power, which the XIP-from-PSRAM design depends on. |
| `HANDOFF.md` | Removed deliberately (`9942277b0c`); this file replaces it. |

## 5. Upstream inventory — core fixes that stand on their own

Not being submitted now. Kept so the work can be split for a merge.

| Commit | File | What | Why it belongs in core |
|---|---|---|---|
| `56681eaf5f` | `apps/gui/skin_engine/{skin_engine.c,.h,skin_render.c,skin_display.c}` | `skin_is_loaded()` guard, and "no tree" is `INVALID_OFFSET` (-1) not 0 | An unloaded skin was rendered anyway, walking a stale buffer-relative offset through a NULL-based tree. Fails identically on any target; the offset-0 case is a real allocated tree at the buffer start. |
| `9d9581a612` | `apps/audio_path.c`, `apps/SOURCES` | Guard input-source selection with `#if INPUT_SRC_CAPS != 0` | A target with a speaker but no recording/radio references `audio_enable_speaker()`, which `apps/SOURCES` only builds when `INPUT_SRC_CAPS != 0`. Shape problem, not ours. |
| `9cb4f7325a` | `tools/functions.make` + every codec `.make` | `make_archive` removes the archive before `ar` adds | `ar` only replaces members it is handed, so a stale object survives a rebuild. Generic build bug. |
| `d4092053ed` | `apps/keymaps/keymap-touchscreen.c` | Chain `CONTEXT_RECSCREEN` to the target keymap | The recording screen fell to the default table, so no touchscreen target could produce `ACTION_REC_PAUSE`. **Changes behaviour for every touchscreen target** — must be flagged in the message. |
| `f270c29b11` | `firmware/rolo.c` | Use `ICODE_ATTR` instead of forcing `.section(".icode")` | `ICODE_ATTR` is deliberately null on targets low on IRAM, and the hard-coded section ignored that. |
| `18ca6599c8` | `apps/recorder/pcm_record.c` | Panic naming both sizes when `core_alloc_maximum()` grants less than `PCM_BUF_SIZE` | `core_alloc_maximum()` grants what is left, not what was asked; every index wraps at `PCM_BUF_SIZE`, so a short grant writes past the allocation into buflib's handle table. Core hardening. |
| — | `firmware/usbstack/usb_core.c` | Mask the serial-number index to a hex digit | `USB_NUM_DRIVERS` is not capped at 4, so the bitmask can exceed a single hex digit on any target. |
| `3630b5b65f` | `firmware/drivers/ft6x06.c` | Clamp parsed point count to the driver's storage | The register is a 4-bit field; a misbehaving panel can report more points than storage, and callers iterate `nr_points`. Any touch target. |
| `536e7cf365` | `firmware/include/fs_defines.h` | `#ifndef MAX_OPEN_FILES` / `#ifndef DC_NUM_ENTRIES` | Both were sized from `MEMORYSIZE`, i.e. storage size; a target whose DMA-visible region is tighter than its flash needs to shrink them together. A seam, additive and default-preserving. |
| `c5f40905ac` | `apps/settings_list.c` | `#ifndef DEFAULT_LIST_LINE_PADDING` | Auto row height is derived from LCD DPI; on a dense small panel that gives ~3 visible rows. Seam, default unchanged for everyone else. |
| — | `firmware/usb.h`, `firmware/usb.c` | `usb_host_session()`, overridable MSC default | The USB screen loop ignores everything but `SYS_USB_DISCONNECTED`, so a shutdown request posted while USB is live is dropped. General. |
| — | `firmware/usbstack/usb_audio.c` | Target hooks for feedback pacing and RX ping-pong (`usb_audio_target_*`), `USB_AUDIO_RX_PINGPONG` default off | Hooks let a target own the feedback-band and buffer shape without core edits. Worth showing upstream; the AC interrupt endpoint at line 290 is still an upstream TODO. |
| `1a7ecb4aee` | `firmware/drivers/rtc/rtc_pcf8563.c` | New PCF8563 driver on the generic I2C API, alarm functions under `HAVE_RTC_ALARM` | The existing `rtc_e8564.c` is PP-async-only and reads hours from the wrong buffer index. Reusable driver. |
| — | `apps/codecs.c` | Propagate `CODEC_LOAD`'s return value | Currently the loader discards the entry-point result, so a codec that refuses to load reports success. Arguably core; see §6 for whether we keep it. |

## 6. Core touches that belong in the target — rationalize these

Found by classifying every core hunk against `upstream/master` and asking whether the
change is justified for other targets or only for us.

| File | Commit | Problem | Move |
|---|---|---|---|
| `apps/main.c` | `cb71fd2136` | The tick-task thread is gated only on `!(CONFIG_PLATFORM & PLATFORM_HOSTED)`, so **every native target** gets an extra thread that runs `run_tick_tasks()`. It exists because our core timer cannot take an interrupt (`wsr.intlevel` invalid on this core config) and the tick must be polled. | Gate on `CPU_XTENSA`, or introduce a `HAVE_TARGET_TICK_TASK` hook the target defines. Also costs a `DEFAULT_STACK_SIZE` slice in DRAM we are short of. |
| `firmware/usb.c` | `e8825a8bca` | `usb_serial` default flipped to `true` in shared code. The comment is honest that no other target defines `USB_ENABLE_SERIAL` today, but a default in core is still a core decision. | A target-side default (`DEFAULT_USB_SERIAL` in config, or set it in the target init), keeping the core default `false`. |
| `apps/language.c`, `firmware/common/unicode.c`, `firmware/logf.c` | various | `PSRAM_BSS_ATTR` (ours, empty for other targets via `config.h:1303`) on core declarations — and for `language.o`/`unicode.o` it duplicates the mechanism we already have: `idf-app/main/rockbox_memory.lf` moves those objects to PSRAM by archive mapping. | Drop the attribute from those core files and let the ldfragment own placement; give `logf.o` a mapping entry too. Keeps a PSRAM concept out of portable code. |
| `firmware/export/usb-designware.h` | `4132c502e3` | `PHYSEL` (a DWC2 `GUSBCFG` bit meaning specific to the embedded PHYs) added to the shared header. | Move to the target header; the driver already guards its use with `CPU_XTENSA`. |
| `firmware/drivers/i2c-async.c` | `7f9d1f98ce` | The async core was changed to start the first descriptor when the bus is idle and to run a blocking submit with interrupts enabled — motivated by our `__i2c_async_submit()` completing the transaction before returning. | Express that as a documented target contract (a capability flag or a target hook) rather than a behaviour change in the shared queue. |
| `apps/codecs.c` | `18ca6599c8` | Return propagation is generically correct, but it was added because our ELF codec loader can fail in ways the flat-binary loaders never hit. | Decide: upstream it as a real fix (§5), or guard it so only targets that opt in change behaviour. Currently undecided. |
| `firmware/drivers/axp173.c` | board bring-up | A new PMU driver in the shared `firmware/drivers/` tree, used by exactly one board. Upstream does put PMU drivers there, so this is defensible — noted so a reviewer is not surprised. | Keep, or move under `firmware/target/xtensa/` if upstream prefers board-local. |
| `firmware/asm/memset16.c` | — | The `CPU_XTENSA` counted-store branch is legitimately target-motivated and guarded; the accompanying indentation fix on the generic path is unrelated churn. | Drop the cosmetic hunk from any submission. |

Verified as **not** errant: every change in `firmware/drivers/usb-designware.c` is inside
`#if defined(CPU_XTENSA)`, `firmware/export/config.h` additions are arch dispatch and a
new `RTC_PCF8563` enum entry, and `lib/libsetjmp`, `firmware/asm/xtensa/`,
`firmware/target/hosted/sdl/` and the plugin keymaps are target-scoped by construction.

## 7. Tooling deliberately untouched, and the adoption hygiene that is maintained

Not touched: `utils/rbutilqt/rbutil.ini`, the build-info server entry, the themes-server
target entry (`meowkits3` returns "No themes available"; `tools/rbthemes.py` pulls
through a proxy target instead). Verified `git diff upstream/master -- utils` is empty.

Maintained as if adoption were intended: `tools/configure` entry `133|meowkit-esp32s3`
with `target_id=126`, unique `MODEL_NUMBER 133`, `tools/builds.pm` (`status => 2`,
`sim => 0`, `manualok => 1`), `list_targets.pl` derives from `builds.pm`, manual
platform/keymap wiring, simulator buttonmap and UI bitmap.

## 8. Artifacts and build

| File | Size | Notes |
|---|---|---|
| `rockbox.esp32s3` | 1,293,648 B | md5 `1c5e23555de13eda481ea78d7519621c`, **flashed 2026-10-26** |
| `rockbox.zip` | 9,469,730 B, 414 entries | card content; re-extract after plugin changes |
| `rockbox-fonts.zip` | 8,478,936 B | 88 fonts, built from the tree |
| `rockbox-themes.zip` | 17,237,059 B | audited clean, additive |
| `rockbox-manual.pdf` | 3,007,703 B | 282 pages; three pre-existing upstream LaTeX errors |

Flash layout: bootloader `0x000000`, partition table `0x008000`, Rockbox `0x010000`.
Memory: DIRAM 340,655 / 341,760 (1,105 B free), IRAM full, arena ends 53,620 B clear of
the plugin/codec reservations (`make esplayer`).

Recipe: `export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1; . $IDF_PATH/export.sh`,
then `make`, `make zip`, `make manual-pdf`, `make esplayer`,
`make flash ESPPORT=/dev/ttyACM0`.
