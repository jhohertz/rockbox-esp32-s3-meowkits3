# Pending — MeowKit ESP32-S3

The single status list. `MEMORY.md` is evidence, `TX-IDF-MIGRATION.md` /
`USB-SPRINT.md` / `USB-AUDIO-RESEARCH.md` are investigation records, `DOC-GAPS.md` and
`UX-PLAN.md` are the documentation and input logs. None of them is a to-do list, and each
has drifted. Anything that says "needs hardware test" here was checked against the code
first; the "corrected" section at the end lists claims that turned out to be stale.

Board access, for every item below: **hold BOOT, tap RESET** to enter download mode
while firmware is running, then `make flash ESPPORT=/dev/ttyACM0`. The flash goal now
runs `make esplayer` first, so a layout overrun fails the build before the board is
touched. There is no UART bridge — the host reaches the SoC through its own USB.

---

## A. Needs the board (blocked on hardware, not on code)

1. ~~Flash the pending build~~ **done 2026-10-26.** `make flash ESPPORT=/dev/ttyACM0`
   wrote bootloader 22,288 B at `0x000000`, partition table 3,072 B at `0x00008000` and
   Rockbox 1,293,648 B at `0x00010000`, each "Hash of data verified", then hard-reset into
   the new firmware. The layout guard ran first as a prerequisite and reported the arena
   ending 53,620 B clear of the plugin/codec reservations. This image carries the core
   probe removal (`ed9805f1e8`). Card content still needs re-extraction from
   `rockbox.zip` for pictureflow's keymap, the uniform quit gesture and metronome —
   plugins are files on the card, not part of the flash.
2. **mpegplayer transport + PSRAM code execution.** Keys are written and documented (A
   tap pause, A held stop, stick volume/seek, B menu). Blocked on `.mpg` files: MPEG-1/2
   Part 2 video, MP3 audio only (libmad — a VCD's MP2 track will be silent), no scaling
   so ≤ 320×240, container `-f mpeg`. Playing *any* video also answers the long-standing
   question of whether codec/plugin code executing from the PSRAM window behaves —
   mpegplayer allocates ≥ 2 MB there.
3. **Does light sleep survive the MSPI MMU table?** `MEMORY.md` §"light sleep":
   `SOC_PM_SUPPORT_RTC_PERIPH_PD` is enabled and `rtc_init.c:114` clears
   `EXTMEM_CACHE_MMU_MEM_FORCE_ON`, so the 512-entry `DR_REG_MMU_TABLE` may lose power.
   If it does, `mmu_init()`'s audiobuffer and framebuffer mappings are gone on wake and
   the first PSRAM access reads the Ext_RAM sentinel `0x0bad0bad`. S4 (light sleep) is
   on and unverified against this. `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` stays unset.
4. **Dark-idle current with sleeping enabled.** The 42 mA floor was measured without
   S4 active. No new instrumentation: `Debug → View battery` prints discharge current
   off the same moving average `powermgmt.c` uses.
5. **96 kHz local playback.** The TX slot is now sized by `XT_MIX_MULT` (2048 B above
   48 kHz, `dma_buf` 4 × 2048), which fixes the 1 KB overflow the old unconditional slot
   caused. Never confirmed audibly on hardware. If it cannot hold, drop
   `HW_SAMPR_CAPS` to `SAMPR_CAP_ALL_48` rather than advertise it — the USB DAC path is
   already capped at 48 kHz by the FIFO ceiling.
6. **USB DAC and CPU boost.** `usb_audio.c` has `.needs_cpu_boost = false` and `usb.c`
   sets `cpu_idle_mode(true)` on USB entry, so the core runs at the low frequency while
   isochronous frames arrive. Ruled out as a problem by use, not measurement. Watch the
   TX row (`sess / err / rec / sil`) in `Debug → View HW info` if glitches ever appear.
7. **Why TX slots go unowned at ~0.2%.** `TX-IDF-MIGRATION.md` §"Where the remaining
   defects actually live": the play-lock guard makes it survivable and replays a
   completion on unlock, but the cause of an unowned slot is unknown. Only reachable
   through long-running playback.
8. **ES8311 registers that don't read back as written.** REG 0x08 written 255 reads
   0x00; REG 0x0D written 0x01 reads 0x02, over a working I2C master with no failed
   writes. Assumption is that 0x08 is don't-care in slave mode and 0x0D reports status,
   which is not a measurement. `es8311_stat()` is a HW-info row so it stays observable.
   Deliberately not investigated now.

## B. Deferred feature flags (won't set unless hardware changes)

- `HAVE_RTC_ALARM` — driver functions are implemented and compile-verified in
  `rtc_pcf8563.c`, but Rockbox only reads the alarm at boot, there is no runtime
  `SYS_ALARM` handler, the PCF8563 `/INT` is unwired, and the AXP173 removes the SoC's
  supply at power-off. `clock`/`alarmclock` plugins cover the use while on.
- `HAVE_HOTKEY` — needs a modifier we don't have free.
- `HAVE_KBD_POINT_MODE` — `#error`s without a point-mode layout at this screen size.
- `HAVE_GENERAL_PURPOSE_LED`, `HAVE_LCD_INVERT` — no such hardware / no need.
- SMP off permanently. Single-core in practice; the IDF SMP kernel path was rejected.
- USB DAC sample rates above 48 kHz — hardware FIFO ceiling, closed rather than
  worked around.

## C. Deferred by design (UX decisions, not gaps)

- **Theme `%T` regions.** Core already maps `%T<shuffle>`, `%T<repmode>`, `%T<pitch>`,
  `%T<trackinfo>`, `%T<playlist>`, `%T<listbookmarks>`, `%T<createbookmark>`
  (`skin_parser.c:1648`). Built-in `cabbiev2` stays untouched; affordances arrive via
  the theme pack. Shuffle/repeat are settings + status-bar icons on a default install.
- **USB HID stays on the touch grid.** HID contexts in `keymap-touchscreen.c` never
  chain to `target_get_context_mapping()`, so a stick mapping would edit the shared
  touchscreen keymap for a preference.
- **Recording from anywhere: passed.** `ACTION_STD_REC` has a handler
  (`apps/menu.c:495`); recording is already reachable from the WPS and target menu.
- **Short PEK: parked.** The intended use was a screen/backlight toggle, and
  `ACTION_STD_SHOWSCREEN` does not exist in this tree — mapping it would be invention.
- **Pitch screen accepted as touch-only** (Long Bottom-Left). Stick slots are scarce;
  deliberately unclaimed.
- **Six games keep B-tap quit** because their quit macro opens a menu (`blackjack`,
  `brickmania`, `xobox`, `solitaire`, `pegbox`, `doom`). Held-B exits everywhere else,
  matching pluginlib's `PLA_EXIT`.
- **`PLA_SCROLL_*` cannot be mapped.** The enum entries are inside
  `#ifdef HAVE_SCROLLWHEEL`; wheel-less targets get `PLA_UP/DOWN` fallbacks instead.
- **Utilities stay touch-driven** (viewers, converters, viewers/menu-driven plugins).

## D. Tooling and release — deliberately not done

- `utils/rbutilqt/rbutil.ini`, the build-info server entry, themes-server target entry.
  These depend on infrastructure and upstream acceptance we don't control. Third-party
  themes are pulled through a proxy target by `tools/rbthemes.py` instead.
- In-tree adoption hygiene *is* maintained as if adoption were intended: `configure`
  target entry (`133|meowkit-esp32s3`, `target_id=126`), unique `MODEL_NUMBER 133`,
  `tools/builds.pm` (`status => 2`, `sim => 0`, `manualok => 1`), `list_targets.pl`
  derives from `builds.pm`, manual platform/keymap wiring, simulator buttonmap and UI
  bitmap. `utils/` is verified untouched against upstream.

## E. Upstream-submittable, not yet filed

Fixes made in shared code that stand on their own and could be posted:

- `manual/plugins/xworld.tex` — four rows gated on `\opt{HAVE_TOUCHSCREEN}`, an option
  no target ever gets (features.txt spells it `touchscreen`), so those rows rendered for
  nobody. Enabling them exposed a second bug in the same chapter: `\ButtonBottomLeft`
  where `\TouchBottomLeft` was meant.
- `apps/gui/skin_engine/` — `skin_is_loaded()` guard and the `INVALID_OFFSET`
  (not 0) tree check; an unloaded skin used to be rendered anyway.
- `apps/audio_path.c` — guard input-source selection with `#if INPUT_SRC_CAPS != 0` so
  a speaker-only target can build the file at all.
- `firmware/usbstack/usb_core.c` — serial-number index masked to a hex digit.
- `firmware/drivers/ft6x06.c` — clamp parsed point count to the driver's storage.
- `tools/functions.make` — `make_archive` helper that removes the archive first; stale
  `ar` members were the bug. Applied to every codec `.make`.
- `apps/keymaps/keymap-touchscreen.c` — recording context was missing from the shared
  table, so no touchscreen target could produce `ACTION_REC_PAUSE`. This one changes
  behaviour for every touchscreen target and needs flagging in the message.
- Report: HID contexts never consult the target keymap (see C).
- `firmware/usbstack/usb_audio.c:290` — upstream TODO: no AudioControl interrupt
  endpoint, so the device cannot tell the host about on-device changes. The feedback
  endpoint itself does work (paced by `FEEDBACK_UPDATE_RATE_*`).

Changes that affect other targets and must be called out in any submission: the
touchscreen keymap change above, `i2c-async.c`'s descriptor-start semantics,
`rolo.c`'s broadened ICODE placement, `usb.c`'s serial-driver default plus
`usb_host_session()`, and the unguarded hunks in `apps/main.c`.

## F. Artifact state

| File | Size | Notes |
|---|---|---|
| `build-meowkits3/rockbox.esp32s3` | 1,293,648 B | md5 `1c5e23555de13eda481ea78d7519621c`, **flash pending** |
| `build-meowkits3/rockbox.zip` | 9,469,730 B, 414 entries | card content, current |
| `build-meowkits3/rockbox-fonts.zip` | 8,478,936 B | 88 fonts, built from the tree |
| `build-meowkits3/rockbox-themes.zip` | 17,237,059 B | audited clean, additive |
| `build-meowkits3/rockbox-manual.pdf` | 3,007,703 B | 282 pages; three pre-existing upstream LaTeX errors |
| `idf-app/build/rockbox.map` | — | input to `make esplayer` |

Memory: DIRAM 340,655 / 341,760 (1,105 B free), IRAM full, arena ends 53,620 B clear of
the plugin/codec reservations.

Build recipe: `export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1; . $IDF_PATH/export.sh`
then `make`, `make zip`, `make manual-pdf`, `make esplayer`, `make flash ESPPORT=…`.

## G. Corrected — stale claims that are no longer true

- **"Recording's short-grant check doesn't exclude the overflow."** It does now:
  `on_init_recording()` panics naming both numbers when `rec_buffer_size < PCM_BUF_SIZE`
  (`apps/recorder/pcm_record.c:1422`), because `core_alloc_maximum()` grants what is
  *left* rather than what was asked for. `MEMORY.md` still carries the older wording.
- **"games pending a hardware test of the batch"** (`DOC-GAPS.md` §10) — plugin batch,
  theme pack and fonts were checked on the device.
- **"key lock: landed, needs hardware test"** (`UX-PLAN.md` S5) — lock, unlock and
  touch suppression were verified; autolock armed by the same combo.
- **Pitch screen "open, low severity"** (`DOC-GAPS.md` §5) — accepted as touch-only and
  documented in `wps.tex`, including that it is not in any context menu on any target.
- **`HANDOFF.md`** was removed deliberately (`9942277b0c`); this file replaces it as the
  status list.
