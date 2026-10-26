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

- **Watch-items after the 2026-10-27 flash** (`439b7866a0`, the core rationalization).
  The point of that commit is that our behaviour must *not* move, so a failure would be a
  seam misfiring rather than a feature missing. Normal use will show it: buttons respond
  to a single press (`HAVE_TARGET_TICK_THREAD` is what keeps the polling thread, and the
  stick lives on it); the serial console is still reachable (`USB_SERIAL_DEFAULT_ON`);
  a codec loads and plays (`apps/codecs.c` reverted to upstream); settings survive a
  reboot and the UI renders (`logfbuffer` and `language_buffer` are now placed by the
  ldfragment instead of a section attribute — a wrong placement would hang or read
  `0x0bad0bad`, not fail quietly).
- **Plugin doc rows that give us nothing — 40 rows in 38 chapters.** Measured by
  walking every `btnmap` row and asking whether any key in it is reachable for us. Five
  chapters name keys from pads we lack (`\ButtonHome`/`\ButtonOption` in `chessbox`
  and `fractals`, `\ButtonBack` in `rockpaint`, `\ButtonDisplay` in `rockblox`):
  those need either an ESP32S3 row or an explicit "not available". Everything else
  already renders correctly through the per-target macros — an earlier claim that 30
  chapters had no column was wrong and has been corrected in `DOC-GAPS` §10.
- **`sgt-puzzles` (40 shipped binaries) has no key table at all**, and lua scripts (63
  shipped) get raw buttons with nothing documenting the gesture set. Both are genuine
  doc gaps, unlike the item above.
- **SGT puzzles: A and B did nothing — root cause found and fixed.** `lib/keymaps.h`
  defines `BTN_FIRE = BUTTON_A` and `BTN_PAUSE = BUTTON_B` in our branch, then the shared
  `HAVE_TOUCHSCREEN` block further down **redefines** them to `BUTTON_BOTTOMLEFT` and
  `BUTTON_TOPLEFT`, because that fallback is meant for pads with no fire/pause key of
  their own. GCC said so out loud (`keymaps.h:336: warning: "BTN_FIRE" redefined`) and
  every `BTN_*` plugin — all 40 sgt puzzles and `xworld` — silently took the touch cells.
  Fixed by adding `CONFIG_KEYPAD != ESP32S3_PAD` to the block's own exclusion list, which
  is the idiom it already uses for COWON_D2, DX50, ONDAVX777, ZENXFI2, SHANLING_Q1,
  HIBY_R3PROII, HIDIZS_AP80MAX and N3DS. Warning gone; plugins rebuilt into the zip; the
  chapter now documents the real mapping. **Needs a card re-extract, then a test** that A
  acts and B opens the pause menu in a puzzle and in xworld.
- **SGT sokoban crashes on launch.** One of 40 shipped puzzle binaries. Needs the panic
  log over serial; not diagnosable from source. Other puzzles launch and play, so it is
  per-game rather than the family.
- **mpegplayer: still failing, and the failure has three distinct shapes.** Ordered by
  what the code actually does, allocation order is grey → libmpeg2 → audio → pcm → disk
  buffer, and each step has its own message:

  | step | fails when | message |
  |---|---|---|
  | `mpeg_alloc_init()` | grant < grey + 2 MiB | "Out of memory in stream_init" |
  | `pcm_output_init()` | grant < grey + 2 MiB + 176,400 | "Could not initialize PCM!" |
  | `disk_buf_init()` | remainder < `MIN_MEMMARGIN` (4 KiB) | "Cannot create buffering thread!" |

  Arena is **3,875,072 B** (re-measured, `MEMORY.md` §6), so the plugin needs ~2.57 MB of
  it *contiguous and free*, leaving ~1.3 MB for the disk buffer only if nothing else is
  holding the arena. With dircache off it got past step 1 and failed at step 2, which says
  free was between 2.33 MB and 2.50 MB — so theme fonts and skin data are holding roughly
  1.3–1.5 MB. The `LIBMPEG2_ALLOC_SIZE` override I tried was **reverted**: mpegplayer
  never rejects a coded frame larger than the panel (it only clips at blit,
  `mpegplayer.c:818`/`:951`), `alloc.c` returns NULL on exhaustion, and `libmpeg2/header.c`
  assigns those results into `mpeg2dec->yuv_buf[][]` with no NULL check anywhere — so
  under-sizing it is a write through NULL and a reset, which matches "it starts, then the
  OS bails".

  **Hypotheses for the reset, in the order I'd test them, none yet proven:**
  1. Coded frame larger than 352×288 in the test files → 2 MiB genuinely insufficient on
     *any* target. Ask which files were used; a 640×480 or 720×576 stream is the simplest
     explanation and needs no bug in our port.
  2. Thread-stack exhaustion. `video_thread.c:65` gives the decoder `VIDEO_STACKSIZE`
     = **4 KiB**, and `disk_buf`/`stream_mgr` get `DEFAULT_STACK_SIZE*2` = 8 KiB. Xtensa
     call frames are wide, and libmpeg2's IDCT plus our 16-bit blit path are not shallow.
     A stack overflow inside pluginbuf would corrupt adjacent plugin data and fault
     without a useful address — consistent with "hangs, then bails".
  3. Light sleep during a stall. Sleep is entered only from the idle task, so it needs the
     plugin's threads to be blocked — which is what a hang looks like. Note the cheap
     discriminator: `xt_light_sleep_block(true)` is held while USB is armed
     (`usb-xtensa.c:348`), so **with a host attached light sleep cannot be the cause**.
     Testing plugged-in vs unplugged separates this from 1 and 2 for free.

  Zero-code tests to run when the board is back: keep USB attached; use the failsafe theme
  (`rockbox_failsafe.wps`, one font) to cut the font claim; try a known-small stream
  (352×288 or smaller) rather than a DVD-sized one; read **Debug → View device info**
  "Prev panic" and "Reset reason / rtc trail" after a crash — reason 1 with a non-zero rtc
  count means the chip was lost mid-run rather than a clean shutdown.

- **Arena cleanup, ranked by safety.** `XT_CODEC_TEXT_SIZE` 512 KiB covers a largest codec
  of 200 KB, so halving it returns 256 KiB with a measurable margin and the layout guard
  catches any mistake. `CODEC_SIZE` 1 MiB vs tree convention 512 KiB at `MEMORYSIZE 8` is
  the other 512 KiB, but it needs a runtime measurement of codec working memory first —
  the failure mode is a codec refusing to load, which is at least loud. `PLUGIN_BUFFER_SIZE`
  cannot shrink: `clock.rock` is 482,560 B against a 524,288 B buffer.

- **`imageviewer` looks missing because it is not in the Plugins menu.** It is launched
  by the file browser on an image file, via `apps/plugins/viewers.config`. The manual does
  not say that. Documentation gap, not firmware.
- **`rockpaint` has no `ESP32S3_PAD` branch.** It defines `ROCKPAINT_QUIT/DRAW/MENU/
  TOOLBAR/TOOLBAR2` per pad (iriver H300, iPod, iAudio, …) and falls through to the
  generic `#else` for us, which is why it is unusable/confusing. Its drawing model wants
  pixel addressing, which a 3x3 touch grid cannot give; free-touch drawing is the natural
  fit but is what the plugin does not do today.
- **chip8 deferred by you.** No CHIP-8 ROMs on hand to test with. The 16-key pad on
  stick + A/B + 3x3 grid is a design call, not a doc fix.

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
| `7f9d1f98ce` | `firmware/drivers/i2c-async.c` | `I2C_ASYNC_SUBMIT_UNMASKED` seam defers the idle-bus submit until interrupts are restored | Already a proper opt-in seam with unchanged default behaviour. A target whose `__i2c_async_submit()` polls the controller to completion must not hold `disable_irq_save()` across a 100 kHz transfer. Worth showing upstream. |

## 6. Core touches rationalized into the target — done

Each was checked against `upstream/master` hunk by hunk, then changed so the shared code
keeps its default and the target opts in. Verified with an xtensa build **and** a
simulator build, which exercises the other side of every new seam (hosted path has
`HAVE_TARGET_TICK_THREAD` undefined and no `PSRAM_BSS_ATTR` consumer).

| File | What it was | What it is now |
|---|---|---|
| `apps/main.c` | Tick-task thread and its `DEFAULT_STACK_SIZE` buffer gated on `!(CONFIG_PLATFORM & PLATFORM_HOSTED)` — every native target got a polling thread whose only reason is that this core cannot program an interrupt level (`wsr.intlevel` is not a valid opcode). | `#ifdef HAVE_TARGET_TICK_THREAD`, defined in `config/meowkits3.h`. Confirmed still present in our image (`.text.tick_task_thread` in the map) and absent-but-compiling in the sim. |
| `firmware/usb.c` | `usb_serial` default flipped to `true` in shared code. | `#ifdef USB_SERIAL_DEFAULT_ON`, defined in `config/meowkits3.h`; core default stays `false`. The reason for opting in is kept in the comment: this build has no debug menu, which is the only switch for the class. |
| `apps/language.c`, `firmware/common/unicode.c`, `firmware/logf.c` | `PSRAM_BSS_ATTR` on core declarations — a PSRAM concept in portable code, and for two of them duplicating the ldfragment mapping that already moved those objects. | Attributes removed; `idf-app/main/rockbox_memory.lf` owns placement, with a new `logf (rb_psram)` entry. Verified `logfbuffer` still lands at `0x3c53aad0`, inside the PSRAM window. |
| `firmware/export/usb-designware.h` | `PHYSEL` (a DWC2 `GUSBCFG` bit whose meaning is specific to the embedded PHYs) added to a shared header. | Deleted. It was dead — every reference in the tree is a comment, and the measurement in `USB-SPRINT.md` says the bit is not this core's to set. |
| `apps/codecs.c` | `ret = entry_point(CODEC_LOAD); return ret;` plus stray blank lines — presented as propagating the return value, but functionally identical to upstream. Pure churn. | Reverted to upstream. The §5 inventory no longer lists it as an upstream candidate. |
| `firmware/asm/memset16.c` | `#if defined(CPU_XTENSA)` **replaced** the `PREFER_SIZE_OVER_SPEED` branch, silently taking the size-optimised path away from any target that sets it; plus an unrelated indentation fix. | Three branches: `CPU_XTENSA` (counted store loop, because zero-overhead-loop state is not preserved across the level-1 tick), then `PREFER_SIZE_OVER_SPEED`/`__OPTIMIZE_SIZE__` as upstream had it, then the generic path. Cosmetic hunk dropped. |
| `firmware/drivers/i2c-async.c` | The submit change itself was already a clean seam (`I2C_ASYNC_SUBMIT_UNMASKED`, default unchanged) — my earlier note that it was a behaviour change was wrong. | Only the stray double blank line removed. Moved into the §5 upstream list. |

Still target-appropriate but left alone: `firmware/drivers/axp173.c` is a new PMU driver
in shared `firmware/drivers/`, which matches how upstream places PMU drivers. Noted so a
reviewer is not surprised.

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
| `rockbox.esp32s3` | 1,293,648 B | md5 `2a48323b15807c369fa7da3ba0acfad5`, **flashed 2026-10-27** (same size as the previous image — expected, the rationalization changes nothing for this target) |
| `rockbox.zip` | 9,469,730 B, 414 entries | card content; re-extract after plugin changes |
| `rockbox-fonts.zip` | 8,478,936 B | 88 fonts, built from the tree |
| `rockbox-themes.zip` | 17,237,059 B | audited clean, additive |
| `rockbox-manual.pdf` | 3,007,703 B | 282 pages; three pre-existing upstream LaTeX errors |

Flash layout: bootloader `0x000000`, partition table `0x008000`, Rockbox `0x010000`.
Memory: DIRAM 340,655 / 341,760 (1,105 B free), IRAM full, arena ends `0x3c572e94`, 53,612 B clear of
the plugin/codec reservations (`make esplayer`).

Recipe: `export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1; . $IDF_PATH/export.sh`,
then `make`, `make zip`, `make manual-pdf`, `make esplayer`,
`make flash ESPPORT=/dev/ttyACM0`.
