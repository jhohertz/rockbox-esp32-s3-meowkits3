# Pending — MeowKit ESP32-S3

The single status list. `MEMORY.md` is evidence, `TX-IDF-MIGRATION.md` /
`USB-SPRINT.md` / `USB-AUDIO-RESEARCH.md` are investigation records, `DOC-GAPS.md` and
`UX-PLAN.md` are the documentation and input logs. None of them is a to-do list.
Every entry here was checked against the code before being written.

Board access: **hold BOOT, tap RESET** for download mode, then
`make flash ESPPORT=/dev/ttyACM0`. `flash` and `espverify` both depend on
`make esplayer`, so a layout overrun fails the build before the board is touched.

---

## 1. Open

- **Runtime numbers, using the existing tool.** `battery_bench`
  (`rocks/apps/battery_bench.rock`) samples `battery_current()` with the display off, which
  is the right instrument here — the debug views need the screen on, and the screen is the
  largest variable. Worth recording: playback screen-on, playback screen-off, dark idle.
  **Unplug USB first**: `xt_light_sleep_block(true)` is held while the USB device is armed
  (`usb-xtensa.c:348`), so a plugged board neither light-sleeps nor gives a representative
  idle current. The recorded progression 57 → 48 → 46 → 42 mA predates several changes and
  should be re-measured, not reused.
- **Card re-extract outstanding.** `rockbox.zip` carries the puzzle exclusion, the
  `keymaps.h` A/B fix and the mpegplayer revert. Firmware on the board is current, so this
  is a card action only.
- **Arena cleanup — needs your go-ahead.** Halving `XT_CODEC_TEXT_SIZE` (512 KiB for a
  largest codec of 200,232 B) returns 256 KiB to the 3,875,072-byte arena. It moves where
  codec code executes, so it touches the audio path; `make esplayer` and `apps/codecs.c`'s
  exact-`codecbuf` check would catch a mistake loudly. `PLUGIN_BUFFER_SIZE` cannot shrink
  (`clock.rock` is 482,560 B against a 524,288 B buffer). `CODEC_SIZE` 1 MiB vs the tree's
  512 KiB convention is another half-megabyte but needs runtime measurement of codec
  working memory first.
- **Coulometer → real battery percent.** Counters are enabled and readable raw. Converting
  them needs the scale established against the known 1400 mAh cell — a measurement, not a
  datasheet lookup — and then a decision on moving `CONFIG_BATTERY_MEASURE` to
  `PERCENTAGE_MEASURE` with a target-supplied percentage.
- **Charge target voltage — the battery-lifetime lever we deliberately did not pull.**
  Neither our firmware nor the reference sets it, so the cell charges to the 4.2 V default;
  ~4.1 V is the meaningful cycle-life improvement, but confirming the register on this die
  is the probing you ruled out.
- **Upstream submissions.** §5 lists 16 items including the `keymaps.h` touch-fallback bug
  and the unfinished-puzzles gate. None sent; kept for a future split.

## 2. Verified on hardware — closed

Each of these was checked on the board, not inferred.

- **MSPI/MMU table survives light sleep.** Debug → View HW info's "External cache MMU: N
  of 512 pages mapped" read the same after a long dark idle and wake, with playback working
  afterwards, so `mmu_init()`'s audiobuffer and framebuffer mappings are intact across the
  sleep boundary. Why it was ever in doubt: `PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y` and
  `SOC_PM_SUPPORT_RTC_PERIPH_PD`/`VDDSDIO_PD` are supported, but
  `ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`, `ESP_SLEEP_MSPI_NEED_ALL_IO_PU` and
  `ESP_SLEEP_RTC_BUS_ISO_WORKAROUND` are all set in `idf-app/sdkconfig`.
- **GDMA TX row stays clean across sleep/wake.** The TX row (`sess / err / rec / sil`) did
  not move across a long dark idle with a track loaded, so descriptor ownership survives the
  boundary.
- **USB serial console survives idle/wake — and it is CDC, not USB-JTAG.** Rockbox's CDC
  class stays up while the player idles and still emits after a wake. The IDF console
  (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`) only writes during early boot, which is why a `cat`
  on the port during idle showed nothing; the post-boot output worth documenting is
  Rockbox's own CDC path.
- **Core rationalization watch-items, all four passed** (`439b7866a0`): single-press
  buttons, serial console reachable, a codec loads and plays, settings survive a reboot and
  the UI renders. That commit was meant to change nothing on this target, and it didn't.
- **SGT and xworld A/B fix works** (`6cf0d20da7`): stick moves, A acts, B opens the pause
  menu, Long B leaves. The shared touch-grid fallback had been redefining `BTN_FIRE` and
  `BTN_PAUSE` away from our physical keys — GCC warned about the redefinition and nobody had
  read it.
- **The three unfinished sgt puzzles are no longer shipped** (`06096f15c6`): group, slide
  and sokoban all crashed on launch and come from `src/unfinished/`, whose own README calls
  its contents unready to ship. Gated behind `PUZZLES_SKIP_UNFINISHED`; zip went 43 sgt
  binaries to 40.
- **Plugin documentation pass complete** (`4e39ddcee9`, `b53d7719ec`, `a24ee27ee7`):
  sgt-puzzles key table (verified against the hardware), chess_clock's nine blank rows
  filled from its own keymap branch, rockpaint described as a cursor painter, lua gesture
  set documented with `print_buttons.lua` as the live reference, imageviewer's file-browser
  launch path stated, and stopwatch/nim/midiplay/rockboy recorded as not built for this
  target rather than described with invented keys.
- **`battery_update()` was a phantom** — the symbol does not exist here. The power thread
  polls `_battery_voltage()` every tenth of a second and calls `battery_status_update()`,
  which maps voltage through the target's `percent_to_volt_*` tables.
- **Core probe removal and layout guard behave** (`ed9805f1e8`, `727a3efaf7`): playback
  through the restored `open_pathfmt()` path, and `make esplayer` runs as a prerequisite of
  `flash`/`espverify`.

## 3. Deferred — might return

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
  1. **Coded frame larger than ~352×288.** Now the leading hypothesis, and my earlier
     documentation was wrong about why: `video_out_rockbox.c:416-428` calls
     `stretch_image_plane()` to scale the decoded picture to the panel, so mpegplayer
     does *not* reject a big coded frame — contrary to the "no scaling, ≤ 320×240" claim
     in the chapter and in earlier notes here. libmpeg2 sizes its buffers from the coded
     resolution, the plugin's fixed pool is 2 MiB, and `libmpeg2/header.c` checks none of
     the allocations. So a 640×480 or DVD-sized file exhausts the pool and writes through
     NULL: the plugin appears to start, then the OS bails. The mpegplayer chapter now
     carries an encode recipe that caps the coded size at 320×240.
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

  Zero-code tests to run when the board is back: **re-encode at 320×240 with the recipe in
  the mpegplayer chapter** (that alone separates hypothesis 1 from everything else); keep
  USB attached; use the failsafe theme (`rockbox_failsafe.wps`, one font) to cut the font
  claim; read **Debug → View device info**
  "Prev panic" and "Reset reason / rtc trail" after a crash — reason 1 with a non-zero rtc
  count means the chip was lost mid-run rather than a clean shutdown.


- **mpegplayer now reaches playback but shows audio only** (failsafe theme + dircache off
  → 2.33 MB fit). So `stream_init`, the disk buffer and the parser all succeeded, and the
  video stream is being dropped somewhere between parsing and blitting. Two cheap
  discriminators, both using facilities that already exist:
  1. **Debug → View log file** after a run. `ROCKBOX_HAS_LOGF` is on for this build, the
     debug menu includes `logfdisp.h`, and mpegplayer's `DEBUGF` lines are exactly the
     ones that answer this: `Getting movie size` then `failed` means `VIDEO_GET_SIZE`
     found no sequence header (an encoding problem), and `stream not found:0xe0` means the
     parser never saw a video stream id. No instrumentation needed.
  2. **Encode video-only** (`-an`). If the picture appears, the video path is fine and the
     MP3-in-Program-Stream combination is the variable; if it stays black, it is our
     blit/stretch path (`video_out_rockbox.c:416-428`, `rb->lcd_blit_yuv`).
  Encoding notes worth testing alongside: MPEG-2 video (not MPEG-1) because
  mpegplayer needs PTS timestamps, which MPEG-1 video in a PS does not carry;
  `-bf 0`; `-g` smaller than 12 is also fine.


- **chip8.** No CHIP-8 ROMs on hand to test with. The 16-key pad on
  stick + A/B + 3x3 grid is a design call, not a doc fix.



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
| `6cf0d20da7` | `apps/plugins/lib/keymaps.h` | Add `CONFIG_KEYPAD != ESP32S3_PAD` to the touch-grid fallback's exclusion list | A real upstream bug, not a target quirk: the `HAVE_TOUCHSCREEN` block redefines `BTN_FIRE`/`BTN_PAUSE` unconditionally, so any touchscreen target that also has physical fire/pause keys silently gets plugins that ignore them. GCC warns about the redefinition; nobody reads the warning. Eight targets are already excluded — the list just needed another entry. |
| — | `apps/plugins/puzzles/SOURCES.games` | Stop building the three games under `src/unfinished/` | Upstream's own README calls them half-written or unready, and its CMake build excludes them from install, yet Rockbox ships `sgt-group`, `sgt-slide` and `sgt-sokoban`. Sokoban crashes on launch here. Removing the three lines is the whole fix. |

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
| `rockbox.esp32s3` | 1,293,696 B | md5 `dc393b48c30369046139f4ca2eea460a`, **flashed 2026-10-27** — carries the PMU parity change (coulometer enabled, VBUS-current and TS ADC channels on). Card content unchanged, so no re-extract needed. |
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
