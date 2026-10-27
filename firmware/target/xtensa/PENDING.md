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

- **Runtime numbers — parked by you, procedure settled.** `battery_bench` samples every
  60 s (`battery_bench.c:457`): elapsed seconds, level %, estimated remaining time, voltage,
  current, charge flags → appended to `/.rockbox/battery_bench.txt`, flushed on storage
  idle. **A** starts, **B** stops (`ESP32S3_PAD` mapping at `:273-279`), and it keeps running
  as a TSR after you leave the plugin — that, not display control, is what lets the backlight
  timeout blank the panel during an idle run. My earlier note saying it samples "with the
  display off" was wrong about the mechanism. Two caveats: `flush_buffer()` skips writes
  while USB is attached, and plugging in starts charging, which invalidates discharge
  readings — run unplugged and connect only after stopping. Want: playback screen-on,
  playback screen-off, dark idle, same track and volume, 20+ minutes each, and read the
  slope across samples rather than any single line.
- **Coulometer → real battery percent: the readout now exists, the scale does not.**
  `Debug → View HW info` gained a `CC raw: charge N discharge N` row
  (`debug-xtensa.c`, `INFO_CC`), one 8-byte burst read of `0xb0-0xb7`. Nothing else
  displays those counters — `Debug → View battery` shows only `battery_status` fields —
  which is why the scale had stayed unmeasured. Two places, and both are ours.
  *Scale*: the LSB depends on the calibration base in CC_CTRL bit 6 (`AXP_CC_PIECE_BASE`),
  undocumented in this tree, so it is empirical: **reboot** (`axp173_init()` clears the
  counters, so a fresh boot is a clean zero with no clear UI needed), confirm the row reads
  0/0, start battery_bench, stay unplugged, and leave it overnight. Dark idle is the best
  window — `battery_current()` is instantaneous and sampled every 60 s, so integrating it
  is only trustworthy when the current is steady. In the morning read the row again, stop
  the bench, and take the log off the card: `mAh per LSB = ∫current·dt ÷ discharge delta`.
  Free sanity check: 8 h of dark idle at ~42 mA is about 336 mAh, so the delta should make
  that arithmetic land near 336, and against the known 1400 mAh cell you can see at once
  whether the implied percentage is credible. A failed I2C read also returns 0/0, which is
  indistinguishable from a genuine post-boot zero — hence reading the row twice.
  *Wiring*: `CONFIG_BATTERY_MEASURE` is currently `(VOLTAGE_MEASURE | CURRENT_MEASURE)` in
  `config/meowkits3.h:354`; adding `PERCENTAGE_MEASURE` makes `firmware/powermgmt.c:393`
  and `:467` call `_battery_level()` in `power-xtensa.c` instead of mapping voltage through
  `percent_to_volt_discharge[]`. That is the whole hook — no core change. Voltage stays as
  a cross-check (the two can disagree under load sag, which is the reason to prefer the
  counters).
- **Upstream submissions.** §5 lists 17 items including the `keymaps.h` touch-fallback bug
  and the unfinished-puzzles gate. None sent; kept for a future split.
- **Plugin code executes from PSRAM — and the frame rate may not be ours to fix.**
  mpegplayer presents ~11 fps, and a differently encoded file measured 3–4 fps higher,
  which says the cost tracks the content (coded size, bitrate, GOP) rather than a single
  fixed overhead. Plugin text executes from `XT_PLUGIN_TEXT_ADDR` in the PSRAM cache
  window — the one performance property of this port that had never been tested — but the
  variation means libmpeg2's software IDCT and `stretch_image_plane()` are at least part
  of the picture, so moving plugin text into internal RAM is not obviously the lever.
  DIRAM has 1,105 bytes free anyway, so that path would displace something. Audio is
  unaffected. Two things to settle, in order: find the encode settings that suit the panel
  (yours to play with), and only then decide whether placement is worth touching.
- **Frame-rate tuning of the encode recipe.** Deferred until you want it. The chapter's
  recipe caps coded size at 320×240 for memory reasons, not speed reasons; a smaller coded
  frame should cost less in libmpeg2's buffers and in the scaling pass.
## 2. Verified on hardware — closed

- **Feature-flag batch — verified on hardware** (`2d550135b2`). Four enablements, all
  target-side, flashed as part of `292debd757186fc632531293fe67c6c2`:
  - *The status bar's disk icon is now driven.* `CONFIG_LED` defaults to `LED_VIRTUAL`, so
    `apps/gui/statusbar.c:123-129` reserved 12 px and drew `bitmap_icon_disk` from
    `led_read(HZ/2)` — but nothing on xtensa ever called `led()`, so the icon was dead space.
    `sd-idf.c` now brackets both sector calls with `led(true)`/`led(false)`, which is the
    shape every storage driver in the tree uses (`sd-as3525.c:694/854`,
    `sdmmc-imx233.c:702/801`, `nand-nano2g.c`). **Confirmed on hardware**: the activity icon
    shows during access, including inside themes.
  - *Backlight fading.* `CONFIG_BACKLIGHT_FADING = BACKLIGHT_FADING_SW_SETTING` plus
    `BRIGHTNESS_STEP 8`, and a target fix: `backlight_hw_on()` used to write the setting's
    brightness, which is precisely what `backlight-e200v2-fuze.c:40-44` guards against
    ("fading up is glitchy"), so that line is now conditional. `BACKLIGHT_FADING_PWM` was
    rejected — it is not a ramp, it is software PWM toggling the lamp fully on/off at a 5 ms
    cycle (`backlight.c:257-300`), which would fight LEDC's own 5 kHz PWM. `SW_HW_REG` was
    rejected too: it writes `MIN_BRIGHTNESS_SETTING` *after* `backlight_hw_off()`
    (`backlight.c:471-474`), and our `hw_off` also drives the panel enable, so that would
    re-light the LED on a sleeping panel and hand back part of the idle saving. **Confirmed on
    hardware**: fade in and fade out both look right and boot brightness is correct — the first
    light-up is instant by design, since the brightness event lands before the panel wakes.
    Settings → Display gained "Backlight fade in"/"fade out".
  - *Software tone controls.* `HAVE_SW_TONE_CONTROLS` pulls `dsp/tone_controls.c` in
    (`lib/rbcodec/SOURCES:23`) and makes Bass/Treble appear in Settings → Tone Controls
    (`audiohw.h:413` → `settings_list.c:1095`). `tone_set_bass`/`tone_set_treble` are in the
    image. **Confirmed on hardware**: no audio concerns at either rate, so it stays. The ES8311
    has no hardware tone controls at all, so this DSP path is the only one there is, and it sits
    upstream of the DAC — bass and treble affect the jack and the speaker identically.
  - *LCD sleep timeout becomes a user setting.* `HAVE_LCD_SLEEP_SETTING` makes the core use
    its own variable (default **10 s**) instead of our `LCD_SLEEP_TIMEOUT (2*HZ)`, so dark idle
    waits 8 s longer for the panel to sleep unless "LCD Sleep After Backlight Off" is lowered.
    That is the intended trade: it is a user setting now rather than a compile-time constant.
- **Speaker mode really does control the amplifier** (`635959d93a`). Found while answering "is
  the DAC right for headphones": the jack is taken straight off the ES8311 HP output and the
  schematic gives the SoC **no jack-detect signal** (confirmed against the schematic, so
  `HAVE_HEADPHONE_DETECTION` is correctly absent), which leaves Settings → Speaker as the only
  lever a headphone user has. It had two problems:
  - the idle policy re-enabled the amp on every playback start whatever the user had set, so
    "Off" was display-only. `audiohw-xtensa.c` now keeps `pa_wanted` (the request) separate
    from `pa_on` (the expander), and never mutes the codec when only the amp is off —
    `es8311_mute()` stops the DAC, which would take the jack down with the speaker.
  - upstream's default for `speaker_mode` is `0` = Off, which on a board whose speaker is the
    primary output means a silent first boot. `apps/settings_list.c` gained a weak
    `#ifndef DEFAULT_SPEAKER_MODE` seam (0 everywhere else) and our config sets 1.
  **Confirmed on hardware**: the option is present in the UI and drives the amp. That also
  settles the doubt I raised about reachability — nothing in `apps/` enumerates `speaker_mode`
  by ID and the manual has no `HAVE_SPEAKER` option anywhere, yet the entry appears.
  The control is `PCA9557 IO3` only; no AXP173 rail is involved, and none can be: DCDC1 feeds
  the SoC, LDO2/LDO3 feed the board including the card slot, and LDO4 feeds the expander that
  carries the enable.
  Checked by preprocessing as well as on hardware: `sim.h:86` `#undef`s `HAVE_SPEAKER`, so no
  simulator compiles that line, and `DEFAULT_SPEAKER_MODE` expands to 1 under
  `-DMEOWKIT_ESP32S3`.

Each of these was checked on the board, not inferred.

- **Bluetooth config lines removed — and my earlier cost claim was wrong.**
  `idf-app/sdkconfig.defaults` carried `CONFIG_BT_ENABLED=y` plus the NimBLE roles and a
  4 KiB host-task allocation, left over from the A2DP feasibility work. Nothing called them
  and no Bluetooth path exists on this silicon (`BT-AUDIO.md`), so they were misleading
  rather than useful — but they were **not** free: deleting them shrank the image
  1,295,584 → **1,291,536 B**, i.e. **4,048 bytes** of linked code. My earlier conclusion
  that they contributed nothing came from grepping `rockbox.map` for `libbtdm_app.a(` and
  `libbt.a(`, which returned nothing — correct for those two archives, wrong as a
  conclusion, because the NimBLE host objects come from elsewhere in the `bt` component
  build. Measured now: `grep -c nimble rockbox.map` is **0**, and the only Bluetooth archive
  left is `libbtbb.a`, the baseband PHY library the WiFi coex pulls in. Re-seeding works
  automatically (`tools/root.make:485-495` removes `sdkconfig` when the defaults are newer);
  verified `# CONFIG_BT_ENABLED is not set` in the regenerated file.
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
- **Arena change is verified on hardware.** With `XT_CODEC_TEXT_SIZE` halved and
  `XT_AUDIOBUF_SIZE` grown (image `0x3c155800..0x3c575800`, arena 4,325,376 B), plugins
  load and run — mpegplayer and clock both work, which is the case a wrong text reservation
  breaks silently — and music codecs play correctly, which exercises the codec half. The
  `.rock`/`.codec` images were relinked against the new origins and verified with
  `readelf -lSW` (`0x425c0000` plugin text, `0x42640000` codec text) because the preprocessed
  `.link` files cached the old value; the makefile rules now declare `memory-xtensa.h` as a
  dependency. Card extraction is routine on your side, so it no longer tracks as an item.
- **mpegplayer plays video, correctly coloured, at ~11 fps.** With the OSD hidden, the video
  band animates and the picture is right in both geometry and colour; the FPS overlay reads
  11 on one file, and a differently encoded file ran 3–4 fps higher, so the rate is bound by
  the encoded content rather than being a fixed ceiling. Tuning encode settings to find the
  best combination is yours to do later. This needed one thing from the target: `lcd_blit_yuv`
  that pushes. Rockbox's generic version in `drivers/lcd-memframe.c` only fills the shadow
  framebuffer, and mpegplayer never issues an update for the video rectangle itself — it
  blits, then pushes its bottom OSD strip (`mpegplayer.c:1665`) and, when enabled, the FPS
  rect. On targets where video works the LCD has CPU-writable display memory or a hardware
  YUV mode, so the blit lands on the glass by itself. `LCD_OPTIMIZED_BLIT_YUV` in
  `lcd-target.h` hands the function to the target (the same seam `config/3ds.h` uses) and
  `lcd-xtensa.c` walks the planes exactly as memframe does, calls the shared line helper, and
  pushes once per frame. Two things worth remembering: the panel's rect updates were never
  the problem (the OSD updates through the identical `lcd_update_rect` path and draws fine),
  and the conversion must stay shared — writing it locally and packing with
  `LCD_RGBPACK_LCD` (which takes 5/6/5 values, `lcd.h:310`) instead of `LCD_RGBPACK` (8-bit,
  `lcd.h:344`) produced the right picture in the wrong colours.
- **chip8 works on the split keypad.** With ROMs on the card, the `ESP32S3_PAD` branch
  (`chip8.c:1332-1354`) behaves as described: the stick is chip8 keys 2/4/6/8, **A** is 5,
  **B** is E, and the remaining nine keys sit on the touch grid (corners 1/3/7/9, top and
  bottom edge midpoints A/0, side midpoints B/C, centre D). Press and release are both
  handled, so held keys work as games expect. The chapter
  (`manual/plugins/chip8emulator.tex`) now carries the full sixteen-key table plus a note
  saying the grid is **not drawn** — the emulator renders only the 64×32 picture, so the
  touch cells have to be remembered. Verified: the mapping. Not separately confirmed: that
  long **A** (chip8's F key) actually switches a ROM to the 128×64 superchip canvas — the
  chapter states it as the conventional behaviour, which is worth checking against a ROM
  that supports it before it is treated as fact.
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

- **mpegplayer: working, with the memory arithmetic still worth knowing.** It plays now
  (§2), so the earlier failure shapes are closed, but these facts were measured and are not
  obvious from the code:

  | step | fails when | message |
  |---|---|---|
  | `mpeg_alloc_init()` | grant < grey + 2 MiB | "Out of memory in stream_init" |
  | `pcm_output_init()` | grant < grey + 2 MiB + 176,400 | "Could not initialize PCM!" |
  | `disk_buf_init()` | remainder < `MIN_MEMMARGIN` (4 KiB) | "Cannot create buffering thread!" |

  Demand is ~2.57 MB of the **4,325,376 B** arena, contiguous, with theme fonts, skin data
  and album art competing for the same pool — which is why a font-heavy theme reports
  `stream_init` failure and the failsafe theme does not. Two hazards that remain true even
  though the plugin now runs:

  - mpegplayer never rejects a coded frame larger than the panel; it clips only at blit
    (`mpegplayer.c:818`/`:951`) and `video_out_rockbox.c:416-428` scales via
    `stretch_image_plane()`. libmpeg2 sizes its buffers from the *coded* resolution, the
    plugin's pool is a fixed 2 MiB, and `libmpeg2/header.c` checks none of those
    allocations — so a DVD-sized file exhausts the pool and writes through NULL. An
    under-sized `LIBMPEG2_ALLOC_SIZE` is therefore worse than no file at all, which is why
    the override tried here was reverted. The chapter's encode recipe caps coded size at
    320×240 for that reason.
  - `video_thread.c:65` gives the decoder `VIDEO_STACKSIZE` = **4 KiB**, and
    `disk_buf`/`stream_mgr` get 8 KiB. Xtensa call frames are wide; libmpeg2's IDCT plus a
    16-bit blit are not shallow. A stack overflow inside pluginbuf would corrupt adjacent
    plugin data and fault without a useful address.

  Encoding notes the chapter carries: MPEG-2 video rather than MPEG-1 (mpegplayer needs PTS,
  which MPEG-1 video in a Program Stream does not provide), `-bf 0`, and `-g` below 12.


- **Theme packs parked — the one tried renders incorrectly.** The `%T` region work
  (`DOC-GAPS.md` item 2: shuffle, repeat mode, bookmarks, lock) is still deferred to theme
  packs, but it is now blocked on something plainer: the theme tested displays wrong, so
  there is no usable rendering to test regions against. Finding a theme that draws correctly
  on this panel comes first; the `%T` mapping work after that. Not a firmware gap — the
  builtin `cabbiev2` continues to render fine.

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
| Charge target voltage (~4.1 V) | Neither our firmware nor the reference sets it, so the cell charges to the AXP173's 4.2 V default. ~4.1 V is the meaningful cycle-life lever, but confirming the register on this die is the probing you ruled out, and lowering it wrong would undercharge the pack. | You want the lifetime trade and accept one register check. |
| Playback surviving a huge playlist switch | Investigated, not fixed. The scan loops already yield per entry (`apps/playlist.c:2301`, `:2637`) and hold a CPU boost, and Rockbox's priority bands are correct for playback (`codec_thread.c:662-664` `PRIORITY_PLAYBACK` boosted toward 5, `audio_thread.c:168-171` 15, playlist refresher `PRIORITY_BACKGROUND` 20) — but this port is deliberately cooperative (`thread-idf.c:121-127`, all threads at one FreeRTOS priority), so priorities only bite at yield points. What doesn't yield is `sort_playlist_unlocked()` (`playlist.c:1619`): `qsort()` at `:1625`, the linear index scan at `:1636`, and `update_control_unlocked()` at `:1643` writing the control file to a 1-bit 20 MHz card. Existing levers cover it: shuffle off, lower "Max files in playlist", keep dircache on (turning it off made large collections *more* flaky). `PCM_NUM_CHUNKS` was deliberately not raised — 64→128 takes 512 KiB from the arena. | You want it to survive the unreasonable case, or accept a core yield/clamp as its own spike. |
| `playlist.max_playlist_size` can outrun its own allocation | Latent hazard, not a bug we hit by accident. `apps/playlist.c:3652-3655` memcpy's `playlist->max_playlist_size * sizeof(*indices)` into `current_playlist.indices`, whose block was allocated in `playlist_init()` (`:1972-1974`) from `global_settings.max_files_in_playlist` **at boot**. The only clamp in the load path is `:2073-2074`, which bounds `num_indices` to the *setting*, not to that block. So raising Settings → Playback → "Max files in playlist" past the boot-time allocation makes the copy walk past the end of the block into whatever buflib neighbour follows it (playback file buffer, dircache). Raised deliberately while testing huge lists; kept to reasonable values now. Same function is also the allocation-failure hazard already documented at `:1983-1989`. | A bounds clamp in `apps/playlist.c` — a bug fix, not a feature, and worth proposing upstream. |
| IR remote input (board has a 38 kHz RX on GPIO 5, TX on GPIO 7) | Noted as a maybe-later feature, not a flag: **there is no generic IR abstraction in this tree** — `HAVE_IR_REMOTE` appears nowhere, and every remote in Rockbox is a per-target driver feeding button codes. The useful half of the board's IR hardware is the decoder side, and its output shape is exactly `button_queue_post()` (`firmware/export/button.h:66-68`), which is already public. Needs an NEC-protocol decoder on a real input path (RMT or a capture timer), so it is a feature spike, not a checkbox. | You have a remote you want to use with it. |

## 4. Deferred — permanent unless the hardware changes

| Item | Reason |
|---|---|
| `HAVE_RTC_ALARM` wake-from-off | Driver functions exist and compile-verified in `rtc_pcf8563.c`, but Rockbox reads the alarm only at boot, there is no runtime `SYS_ALARM` handler, the PCF8563 `/INT` is unwired, and the AXP173 removes the SoC's supply at power-off. `clock`/`alarmclock` cover it while on. |
| `HAVE_KBD_POINT_MODE` | `#error`s without a point-mode layout for this screen size. |
| The physical WS2812B (GPIO 38) | Not driven, and there is nothing to enable. Rockbox's only LED abstraction is `CONFIG_LED` with `led(bool)` — on or off, no colour, no pattern (`firmware/drivers/led.c`), and `LED_REAL` is Archos-register-specific. `HAVE_GENERAL_PURPOSE_LED` is **not** a core flag in this tree: it appears only in the hosted hiby sim files, so the earlier row naming it was a phantom. The addressable LED would need target code with no Rockbox surface to hang on; if it is ever wanted, the shape you suggested is a *plugin visualisation* using the pin directly, not a target LED driver. |
| `HAVE_LCD_INVERT`, `HAVE_LCD_FLIP` | Deliberately not exposed, **not** because the hardware can't. Both settings exist generically (`settings_list.c:1302-1308`) and the panel supports them — `st7789.c:10-11` define `CMD_INVOFF`/`CMD_INVON` and send `INVON` at `:130`, and `:129` writes `MADCTL`, whose MX/MY bits are flip. Each is a ~10-line target function plus the flag. Judgement: no user need, and inverting a 320×240 theme's colours is more confusing than useful. |
| `HAVE_FAT16SUPPORT` | Exists and is pure config (`fat.c:193-212` dispatches 16- vs 32-bit BPB helpers, no target code), so it is cheap — but you said you don't need it, so FAT32 cards only. Related trap, documented instead: **exFAT is supported nowhere in the tree**, and cards ≥ 64 GB are commonly exFAT by default, so the install page tells users to format FAT32. |
| `HAVE_VOLUME_IN_LIST` | Needs dedicated volume keys, which this board has no equivalent of. The flag only adds `ACTION_LIST_VOLUP`/`VOLDOWN` entries in keymaps for targets that own `BUTTON_VOL_UP`/`VOL_DOWN` hardware (`keymap-zenxfi3.c:114-117`); there is no trick. Our stick is *contextual* — up/down are volume in the WPS (`keymap-meowkits3.c:83-84`) and navigation in lists — so enabling it would steal navigation. If list-context volume is ever wanted, the honest route is unused touch cells in list contexts (keymap work), not this flag. |
| SMP | Off permanently. The IDF SMP kernel path was rejected; single-core in practice. |
| USB DAC above 48 kHz | Full-speed isochronous FIFO ceiling, measured. Not a software knob. |
| Deeper light sleep | `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` is deliberately off: retaining the peripheral domain is what makes our hand-written I2S/GDMA/GPSPI2/LEDC/I2C/USB drivers safe. Going deeper needs save/restore for six subsystems. |
| `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` | Must stay unset: flash keeps power, which the XIP-from-PSRAM design depends on. |
| Bluetooth audio out (A2DP source + AVRCP) | **Closed, not deferred.** ESP32-S3's radio is BLE only: `SOC_BT_CLASSIC_SUPPORTED` exists solely for `esp32` in IDF v5.5.5, so `BT_CLASSIC_ENABLED` and everything beneath it (`BT_A2DP_ENABLE`, the AVRCP options) cannot be selected, and IDF's classic_bt tests run on `esp32` alone. LE Audio is not a fallback — no ISO/CIS capability on S3 and no LE Audio stack in this checkout. Only a co-processor with BR/EDR could carry the audio; the Rockbox design (`PCM_SINK_BT` against the IAP precedent) is written up in `BT-AUDIO.md` and would transfer unchanged. Reopens only with a hardware change. |
| `HANDOFF.md` | Removed deliberately (`9942277b0c`); this file replaces it. |

## 5. Upstream inventory — core fixes that stand on their own

Not being submitted now. Kept so the work can be split for a merge.

| Commit | File | What | Why it belongs in core |
|---|---|---|---|
| `56681eaf5f` | `apps/gui/skin_engine/{skin_engine.c,.h,skin_render.c,skin_display.c}` | `skin_is_loaded()` guard, and "no tree" is `INVALID_OFFSET` (-1) not 0 | An unloaded skin was rendered anyway, walking a stale buffer-relative offset through a NULL-based tree. Fails identically on any target; the offset-0 case is a real allocated tree at the buffer start. |
| `9d9581a612` | `apps/audio_path.c`, `apps/SOURCES` | Guard input-source selection with `#if INPUT_SRC_CAPS != 0` | A target with a speaker but no recording/radio references `audio_enable_speaker()`, which `apps/SOURCES` only builds when `INPUT_SRC_CAPS != 0`. Shape problem, not ours. |
| uncommitted | `apps/settings_list.c` | `#ifndef DEFAULT_SPEAKER_MODE` seam for the Speaker choice default | The hardcoded default is 0 = Off, which is only correct for targets where the speaker is an extra. A weak seam defaulting to today's value; the target that needs On states it in its own config, the way `DEFAULT_TOUCHSCREEN_MODE` already does one block above. |
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
| `931cb55135` | `tools/rockboxdev.sh` | Strip the release tag's `esp-` prefix when composing the Xtensa archive name | Upstream-facing only if the ESP target lands upstream, but the underlying fact is worth recording: Espressif's release *tag* is `esp-14.2.0_20260121` while the published asset is `xtensa-esp-elf-14.2.0_20260121-x86_64-linux-gnu.tar.xz`, so building the file name from the tag verbatim asks for a doubled prefix and the CDN answers 404. Verified against the release API (35 assets; for this triple only `riscv32-esp-elf-14.2.0_...` and `xtensa-esp-elf-14.2.0_...`). |
| `931cb55135` | `tools/rockboxdev.sh` | `getfile_ex()` must exit non-zero when every mirror failed | A bare `exit` takes the status of the last command, and the two `echo`s above it always succeed — so a failed download printed "couldn't download the file!" and returned 0. Any build system driving this script continues with no toolchain. Affects every arch, not just Xtensa. |
| — | `tools/configure` | Include `-` in the `--target=` class when rebuilding `CONFIGURE_OPTIONS` | The five sed patterns that strip the options configure re-sets unconditionally cover `--voice=[-_./a-zA-Z0-9]*` but not `-` for `--target`, so a hyphenated target spelling is only half stripped and `make reconf` re-runs configure with a stray `-esp32s3` and dies on "Option '-esp32s3' unsupported". Only our alias triggers it today, which is exactly why the asymmetry is a bug rather than a rule. |

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
| `rockbox.esp32s3` | 1,291,536 B | md5 `c9e9dfbf76d0b2307ac464e3c62f3b22`, **built but not flashed** — differs from the flashed image only by the Bluetooth config removal (`4,048 B` of never-called NimBLE host code). No functional change, so the board running `292debd757186fc632531293fe67c6c2` / 1,295,584 B is behaviourally current. The digest moves whenever the embedded build metadata does; `make flash` regenerates the image, so build and flash sizes can differ by a few bytes. |
| `rockbox.zip` | 9,318,040 B, 411 files | card content; re-extract after plugin changes |
| `rockbox-fonts.zip` | 8,478,936 B | 88 fonts, built from the tree |
| `rockbox-themes.zip` | 17,237,059 B | audited clean, additive |
| `rockbox-manual.pdf` | 3,047,948 B | 286 pages; four pre-existing upstream LaTeX error lines as baseline. Rebuilt after the flag batch — now renders "Backlight Fade In/Out", Bass and Treble. `build-meowkits3/manual/features.tex` must be deleted when target flags change (see `AGENTS.md` §9) |

Flash layout: bootloader `0x000000`, partition table `0x008000`, Rockbox `0x010000`.
Memory: DIRAM 340,655 / 341,760 (1,105 B free), IRAM full, `.ext_ram.bss` ends `0x3c5b2e9c`,
arena `0x3c155800..0x3c575800` = 4,325,376 B, **53,604 B** clear of the plugin-text reservation
at `0x3c5c0000` (`make esplayer`).

Recipe: `export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1; . $IDF_PATH/export.sh`,
then `make`, `make zip`, `make manual-pdf`, `make esplayer`,
`make flash ESPPORT=/dev/ttyACM0`.

`tools/configure` has to run with that environment sourced. Without
`xtensa-esp-elf-cpp` on PATH it prints "Warning: Could not determine target arch" and
still writes `autoconf.h` — with `#define ARCH arch_none`. The build then fails
far away from the cause, with `field 'context' has incomplete type` in
`thread-internal.h` and `DEFAULT_STACK_SIZE undeclared` in `apps/main.c`. Re-running
configure with the environment sourced is the whole fix; `make reconf` inherits the
same requirement.

Container: `tools/docker/Dockerfile` + `tools/docker/rb-build-artifacts.sh`.

```sh
docker build -f tools/docker/Dockerfile --build-arg RB_REFRESH=$(date +%s) -t rockbox-meowkits3 .
docker run --rm -v "$PWD/out:/out" rockbox-meowkits3
```

Measured, not assumed: `ubuntu:26.04` resolves (codename `resolute`, Python 3.14.4,
cmake 4.2.3 — IDF 5.5.5 builds fine with cmake 4); the toolchain stage installs
xtensa-esp-elf plus ESP-IDF in ~110 s once the download cache is warm (~19 min cold,
168 MB archive + IDF clone + `install.sh esp32s3`); the image is 10.4 GB; the entrypoint
builds the firmware, passes `esplayer` and `espverify`, and reports the app partition
16 % free. Three defects it found are fixed above and in `tools/` — see §5. It
configures `--type=AV` by default (`RB_VOICE=0` for the plain build) so the payload
matches what the device gets: 411 files including `english.voice`, versus 409 and
7.1 MB without it. The run emitted all 12 files (image, bootloader, partition table,
`flash_args`, zip, fonts zip, PDF, info, map, espverify log, manifest, SHA256SUMS)
and exited 0.
`tools/espflash.make` now derives paths from `$(ROOTDIR)` rather than
`dir $(BUILDDIR)`, which is what makes an out-of-tree build (the container builds into
`/work`) work at all; in-tree `make esplayer` and `make reconf` were both re-verified on
the host afterwards.
