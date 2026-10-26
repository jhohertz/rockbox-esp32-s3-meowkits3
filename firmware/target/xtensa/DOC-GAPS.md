# Documentation-driven gap log

Running list of implementation gaps surfaced while writing the target manual.
The manual is effectively a spec of what the player promises, so every `\opt{}`
the body expects and every action macro it renders is a claim we either honour
or don't. Each entry says what the manual promises, what the code actually does,
and where the evidence is. Nothing here is a build failure; these are product
gaps, ordered roughly by how much a user would notice.

Format: **claim / reality / evidence / what fixing it means / status.**

The implementation plan for these entries — gesture budget, precedents, spike
order — is in `UX-PLAN.md` alongside this file. When an entry is closed here,
the corresponding spike is done and the manual's keymap file should stop saying
*not available*.

---

## 1. A-B repeat is enabled but cannot be set

- **Claim:** `ab_repeat_buttons` is in our feature list, so
  `manual/configure_rockbox/playback_options.tex` documents the A-B repeat
  setting.
- **Reality:** no key on this target produced `ACTION_WPS_ABSETA_PREVDIR` or
  `ACTION_WPS_ABSETB_NEXTDIR`. The user could turn A-B repeat on in the
  settings menu but could not mark the two points. **Fixed** — see Closed.
- **Evidence:** the action list extracted from both keymap files had no `AB*`
  entries.
- **What fixing it means:** mappings in the WPS context using a held modifier,
  following the as3525 convention. See `UX-PLAN.md` S4.
- **Status:** closed.

## 2. Shuffle and repeat-mode toggles have no key

- **Claim:** playback options describe shuffle and repeat mode.
- **Reality:** no `ACTION_WPS_SHUFFLE` / `ACTION_WPS_REPEAT_MODE` mapping.
  These are reachable from a skin through touch regions — the skin engine has
  `ACTION_TOUCH_SHUFFLE` and `ACTION_TOUCH_REPMODE` (`skin_parser.c:1625`) —
  so they work only if the theme defines a `%T` region with that action. The
  default cabbiev2 defines none. Measured on the theme pack: two of the ten
  themes define `%T` regions at all, so most catalogue themes give only the
  automatic bar zones (`skin_parser.c:1261`). **Pen&Paper Plus has 18**, and its
  set includes `shuffle` and `repmode` — so on that theme the two actions this
  section is about are reachable on screen with no firmware change at all.
  `tools/rbthemes.py check` reports the count per theme.
- **What fixing it means:** `%T` regions in a theme, not firmware. Deferred: the
  builtin cabbiev2 stays untouched, so these arrive with the third-party theme
  packs that define them (`tools/rbthemes.py` should report which ones do).
  Shuffle and repeat remain settable from the settings menu meanwhile, which is
  what the manual's *not available* note reflects.
- **Status:** open, deferred to theme packs.

## 3. Bookmarks: delete works, create and list do not

- **Claim:** `manual/rockbox_interface/bookmarking.tex` documents creating a
  bookmark, listing them, and deleting them.
- **Reality:** `ACTION_BMS_DELETE` was produced (bottom-right hold, via
  `keymap-touchscreen.c`), but `ACTION_WPS_CREATE_BOOKMARK` and
  `ACTION_WPS_LIST_BOOKMARKS` were produced nowhere, so bookmarks could be
  deleted but never created. **Fixed** — see Closed.
- **Evidence:** the action list extracted from both keymap files. The handlers
  are at `apps/gui/wps.c:1053` (list) and `:1062` (create).
- **Note on resume, which looks like it does nothing.** Bookmarks are keyed to
  the **current playlist name** (`generate_bookmark_file_name()` in
  `apps/bookmark.c`), and `bookmark_load_menu()` splashes *"empty"* when
  `playlist_dynamic_only()` is true. A bookmark created while playing a single
  file directly therefore has no playlist to resume into, and resuming is only
  meaningful when a playlist was in use. That is upstream behaviour, not a port
  bug; the manual should say so.
- **What fixing it means:** two WPS-context mappings using a held modifier.
  Note the delete action's spelling is `ACTION_BMS_DELETE`, not
  `ACTION_BM_DELETE`; the manual's macro is `\ActionBmDelete`.
- **Status:** closed.

## 4. Key lock is documented with an empty key column

- **Claim:** `manual/rockbox_interface/wps.tex:83` renders a "Key lock (software
  hold switch) on/off" row for every target without a `hold_button` option.
- **Reality:** no `ACTION_STD_KEYLOCK` mapping existed, and that row selects its
  key from an explicit pad list (`SANSA_CLIP_PAD`, `SANSA_FUZEPLUS_PAD`,
  `AIGO_EROSQ_PAD`, `RG_NANO_PAD`), so for us the cell rendered blank. **Fixed**
  — see Closed.
- **What key lock actually is here.** There is no `firmware/keylock.c` and no
  `CONFIG_KEYLOCK` in this tree; the whole mechanism is the *soft lock* in
  `apps/action.c`, active because we do not define `HAS_BUTTON_HOLD`.
  `do_key_lock()` sets `keys_locked` and, for touchscreen targets, calls
  `button_enable_touch(false)` unless the selective-softlock setting is enabled
  with touch not excluded. `do_softlock()` then filters every action while
  locked, except the one matching `unlock_combo`, which is remembered from the
  gesture that locked the panel (`action.c:885-895`).
- **Why a key mapping was required, not optional.** Two reasons, both measured:
  - Touch alone cannot be the whole story. With default settings a locked panel
    has the touch device switched off at the driver level, so a lock produced by
    touch would leave only the *stick* to unlock it — and `unlock_combo` would
    hold the touch code that no stick press matches. That is an unrecoverable
    state until reboot.
  - Autolock was dead code for us. `action_autosoftlock_init()` arms
    `SEL_ACTION_ALOCK_OK` only when `unlock_combo != BUTTON_NONE`, and that combo
    is found by searching the WPS and STD contexts for `ACTION_STD_KEYLOCK`
    (`action.c:1485-1504`). With nothing producing the action, the "Advanced Key
    Lock" submenu in Settings (present for us, since it is gated on
    `!HAS_BUTTON_HOLD`, not on `hold_button`) could never engage.
- **Distinct mechanism, worth not confusing:** the skin engine's `%T(...,lock)`
  is *not* soft lock. `ACTION_TOUCH_SOFTLOCK` only toggles
  `data->touchscreen_locked`, which hides that skin's other regions while
  leaving the lock region itself tappable (`skin_touchsupport.c:73-76, 226`).
  It is a per-theme panel lock, reversible by construction, and does not touch
  the mixer, backlight or autolock state.
- **Status:** closed.

## 5. Pitch screen is reachable only by touch

- **Claim:** `pitchscreen` is in our feature list; the manual documents
  entering it from the WPS.
- **Reality:** `ACTION_WPS_PITCHSCREEN` comes only from holding the bottom-left
  grid cell (`keymap-touchscreen.c:100`). No stick gesture produces it. Once
  inside, the stick works — `ACTION_PS_*` are all available.
- **What fixing it means:** one WPS-context mapping. Free stick slots are
  scarce: `Up|REPEAT` and `Down|REPEAT` are volume on the WPS, `A|REPEAT` is
  context there and stop, `B|REPEAT` is quick screen.
- **Status:** open, low severity. The manual now shows the touch gesture — the
  pitch row in `wps.tex` was gated on an explicit pad list that excluded us, and
  `ESP32S3_PAD` has been added, so the entry reads *Long Bottom-Left* instead of
  being missing. The stick slot remains deliberately unclaimed.

## 6. No hotkeys

- **Claim:** none — `manual/rockbox_interface/hotkeys.tex` is gated on
  `\opt{hotkey}` and our feature list has no `hotkey`, so the manual correctly
  omits it. The shared touchscreen keymap therefore routes its bottom-right tap
  to the playlist instead (`keymap-touchscreen.c:94-98`).
- **Reality:** consistent. Deferred deliberately: `HAVE_HOTKEY` needs a modifier
  button, and this board has six inputs with no natural modifier.
- **Status:** deferred by choice, documented as such.

## 7. No wake-up alarm — driver implemented, flag deliberately still off

- **Claim:** none — the alarm section is gated on `\opt{alarm}`, and we do not
  advertise it. That is currently correct.
- **The contract.** `firmware/export/rtc.h` requires five functions under
  `HAVE_RTC_ALARM`, not the `rtc_setalarm()` this log used to name:
  `rtc_set_alarm(h, m)`, `rtc_get_alarm(*h, *m)`, `rtc_enable_alarm(bool)`,
  `rtc_check_alarm_started(bool release)`, `rtc_check_alarm_flag()`.
  All five are now implemented in `firmware/drivers/rtc/rtc_pcf8563.c` and
  verified to build (alarm menu compiles, `apps/features` gains `alarm`, DIRAM
  free 1089 B). Register map and CTRL2 bit values are copied from
  `rtc_e8564.c` — same chip family, and the stock MeowKit `PCF8563.hpp` only
  claims alarm support in a comment while implementing none.
  One upstream bug not carried over: `rtc_e8564.c:158` reads the *hours* out of
  `buf[0]`, which is the minute alarm register. Ours reads `buf[1]`.
- **Why the flag is still off.** Rockbox's alarm means "power on at time X",
  not "make a noise at time X". `apps/root_menu.c:896` calls
  `rtc_check_alarm_started(true)` **once at boot**, and `firmware/powermgmt.c:519`
  merely clears the flag if it is seen while running. A tree-wide grep for
  `SYS_ALARM`/`SYS_WAKEUP` finds no runtime alarm event anywhere. So without a
  power-on wake, an armed alarm does nothing at the alarm time.
- **Wake-from-off is not possible on this board.** Two independent confirmations:
  - The reference BSP wires only `HAL_PIN_PMU_IRQ = 43` (`HAL_IRQ_PIN` is that
    same pin). The PCF8563 `/INT` output is not connected to the SoC, so there
    is no wake source. The AXP173 can be re-enabled only by a PEK press.
  - Our `power_off()` issues AXP173 REG 0x32 bit 7, which drops DCDC1 — the
    SoC's own supply. Nothing runs afterwards to be woken.
  Targets that do wake own the wake in the PMU or SoC: `power-as3525.c:33` calls
  `rtc_alarm_poweroff()` from the RTC driver, and imx233 has
  `RTC_PERSISTENT0.ALARM_WAKE_EN`/`ALARM_WAKE` bits. We have neither.
- **A route that would work without touching core.** In `power_off()`, if an
  alarm is armed, skip the AXP shutdown and take **ESP32 deep sleep** with
  `esp_sleep_enable_timer_wakeup()` set to the seconds until the alarm
  (computed from the PCF8563, which keeps time on its own battery-backed rail).
  The image then restarts from the mask ROM, our `crt0`/PSRAM/MMU init runs
  again, `clean_shutdown()` has already flushed, and `rtc_check_alarm_started()`
  sees AF+AIE because we also arm the chip's own alarm — so `root_menu` jumps to
  the WPS exactly as upstream intends. Target-only, no core edit. Needs a power
  measurement: rails held on with the SoC in deep sleep, which should be well
  under today's 42 mA dark idle.
- **Meanwhile the device can already be a clock.** Both `clock` (subdir, in the
  zip) and `alarmclock` (`apps/plugins/SOURCES:73`) are built, and
  `pluginlib_actions.c` has an `ESP32S3_PAD` branch supplying four directions
  plus `PLA_SELECT`/`PLA_CANCEL`/`PLA_EXIT` (A, B, held B). Neither needs
  `HAVE_RTC_ALARM`; `alarmclock` counts down and rings while the player is on,
  which is the practical alarm until the wake path exists. `PLA_SCROLL_FWD/BACK`
  are used only as alternates there, so their absence does not matter.
- **Closed as not doing — power-on from the RTC is not possible here.** The
  ESP32-S3 does have its own RTC and can wake itself from *deep sleep*, but that
  is not the state our shutdown reaches: `power_off()` sets AXP173 REG 0x32 bit 7,
  which drops DCDC1 and removes the SoC's supply entirely, so no ESP RTC timer
  survives to fire. The AXP173 offers no alarm or timeout output of its own — its
  only re-enable is a PEK press — and the PCF8563 `/INT` is not wired to a GPIO,
  so there is no external wake source either. Making an alarm boot the player
  would mean never truly powering off, i.e. substituting a always-on low-power
  state for shutdown, which is a power-semantics change this port should not
  invent.
- **What we keep.** The five driver functions stay in `rtc_pcf8563.c`, compiled
  out while `HAVE_RTC_ALARM` is undefined — they cost nothing, and they document
  the contract and the register map for whoever revisits this if the hardware
  ever gains a wake path. The practical clock and alarm are the `clock` and
  `alarmclock` plugins, both already shipped and working while the player is on.
- **Status:** closed as not doing. `HAVE_RTC_ALARM` stays off permanently unless
  the board changes.

## 8. The on-screen keyboard is line-mode, not touch-mode

- **Claim:** `manual/rockbox_interface/browsing_and_playing.tex` documents the
  virtual keyboard and shows `ss-virtual-keyboard-320x240x16.png`.
- **Reality:** `apps/recorder/keyboard.c:506` requires `LCD_WIDTH >= 480 &&
  LCD_HEIGHT >= 640` for `HAVE_KBD_POINT_MODE`, and `#error`s otherwise (line
  539). We have a touchscreen but not the resolution, so the keyboard is the
  character-list keyboard. **The stick half of this is now fixed** —
  `keymap-meowkits3.c` had no keyboard context at all, so the stick did nothing
  in that screen and the panel could only drive actions, not characters; see the
  Closed section. What remains is the discoverability problem: `kbd_draw_buttons()`
  is called only for point mode (`keyboard.c:688`), so line mode never names the
  keys and a user cannot tell how to finish or cancel without being told.
- **What fixing it means:** a point-mode layout for 320x240 in `keyboard.c`.
  That is a core file, so it would need the same kind of seam we use elsewhere
  rather than a resolution-specific hack.
- **Status:** part 1 closed (stick mapping). Part 2 closed as "not doing" —
  see Closed. Text entry is now possible with the stick; the screen still does
  not name the keys, and that is upstream behaviour shared by every stick-only
  target rather than a gap we should paper over.

## 9. Actions the manual names that we never produce

Minor, collected because the macros have to say something:

| Action | Manual context | Ours |
|---|---|---|
| `ACTION_STD_REC` | start recording from any screen | WPS only (`ACTION_WPS_REC`) — deliberately passed; the std handler exists at `apps/menu.c:495` |
| `ACTION_STD_SHOWSCREEN` | screen/backlight toggle | **no such action in this tree** |
| `ACTION_SETTINGS_RESET` | reset a setting to default | **no such action in this tree** — not in `apps/action.h` or any keymap |
| `ACTION_STD_KEYLOCK` | key lock | both face buttons held (see §4) |
| `ACTION_STD_HOTKEY` | hotkey | none, and correctly undocumented |

- **Status:** open, low severity. Each is one mapping if we decide the gesture.
- **SHOWSCREEN was never real.** `ACTION_STD_SHOWSCREEN` appears nowhere in this tree:
  not in `apps/action.h`, not in any handler, not in any target's keymap. The only
  occurrence anywhere was `\newcommand{\ActionStdShowScreen}{\NotAvail}` in our own
  platform file, and no chapter referenced it. Removed. It belongs in the same column
  as `ACTION_SETTINGS_RESET`: a name this log carried forward, not a gap in the port.

## 10. Plugins: the input gap is touch-only, not missing keys

- **Claim:** the manual documents many plugins with their own key tables
  (`ButtonHome`, `ButtonPlus`, `ButtonMinus`, `ButtonMode`, `ButtonFn`,
  `ButtonRec`, `ButtonOff` appear throughout `manual/plugins/`).
- **The earlier claim in this log — "53 plugin sources reference buttons we
  cannot produce" — was wrong**, and the reason is instructive: grepping plugin
  sources finds those names inside `#if CONFIG_KEYPAD == <other pad>` chains for
  ~50 keypads that never compile here. Name matching measures the file, not the
  build.
- **Method that actually works.** Take each shipped plugin's real compile line
  from `make -n --always-make`, swap `-c/-o` for `-E -P -x c -include config.h`,
  and scan the *preprocessed* text for the padded button literals our
  `button-target.h` defines (`0x00000004` for `BUTTON_UP` etc.) — macros expand
  to numbers, so matching names in that output finds nothing. `PPCFLAGS` must
  carry `-DHAVE_SYSTEM_LIBC` and `-DMEOWKIT_ESP32S3`, otherwise `rbendian.h`
  and `config.h` both fail. All 116 shipped plugin sources preprocess cleanly.
- **Result: zero plugins reference a button we cannot produce.** The one hit was
  `keyremap`, whose `"BUTTON_UNKNOWN"` is a string in its own lookup table
  (`keyremap.c:54, 292`), not a macro.
- **pluginlib is in better shape than this log said.** Of the 17 `PLA_*` actions
  it defines, our `ESP32S3_PAD` branch supplies 13: four directions plus their
  repeats, `PLA_SELECT`, `PLA_SELECT_REL`, `PLA_SELECT_REPEAT`, `PLA_CANCEL`,
  `PLA_EXIT`. Only `PLA_SCROLL_FWD/BACK` (+ repeats) are missing, and those are
  scroll-wheel actions that appear in plugin code only as alternates. 48 shipped
  plugins go through pluginlib, so they already respond to the stick and A/B.
- **The real gap is different and much more specific.** 38 plugin sources have a
  `#ifdef HAVE_TOUCHSCREEN` fallback block that fills in any action macro the
  keypad chain left undefined — which is why they compile here at all. Net
  effect: they consult **only touch cells**; the stick and A/B are never read.
  Confirmed by the literal scan for `snake`, `snake2`, `spacerocks`, `wormlet`,
  `jewels`, `pong`, `chopper`, `flipit`, `minesweeper`, `sliding_puzzle`,
  `chip8`, `cube`, `oscilloscope`, `vu_meter`, `rockblox`, `pegbox`-adjacent
  titles and others, including ones that are plainly games where a stick is the
  natural input.
- **Already fixed with a real branch (9):** `blackjack`, `brickmania`, `clix`,
  `invadrox`, `pegbox`, `rockpaint`, `solitaire`, `star`, `xobox` each carry an
  `ESP32S3_PAD` mapping.
- **The fix per game, and why it is cheap.** Add an
  `#elif CONFIG_KEYPAD == ESP32S3_PAD` branch to the existing chain. The touch
  fallback uses `#ifndef` per macro, so anything we define wins and anything we
  leave untouched keeps its touch mapping. No core change, no plugin ABI change,
  one file per game.
- **Done (16 branches added, one build):** `snake`, `snake2`, `spacerocks`,
  `wormlet`, `pong`, `chopper`, `jewels`, `cube`, `flipit`, `minesweeper`,
  `sokoban`, `sliding_puzzle`, `rockblox`, `oscilloscope`, `vu_meter`, `chip8`.
  Convention, matching pluginlib: stick = movement, **A** = primary action,
  **B tap** = pause/menu/cancel, **B held** = quit (that is `PLA_EXIT`). Verified
  by re-running the literal scan: all sixteen now reference the stick and A/B in
  their preprocessed output, where before they referenced only touch cells.
- **Three things that bit the naive version, all of them real:**
  - `rockblox`: `ROCKBLOX_OFF` as held B collided with the game's
    `ROCKBLOX_ROTATE_CCW | BUTTON_REPEAT` switch label, so counter-clockwise went
    to a touch cell instead.
  - `sokoban`: several switch labels must stay distinct (`SOKOBAN_QUIT_REPLAY`
    defaults to `SOKOBAN_MENU`), and its help text needs `*_NAME` strings for
    every key the branch defines — so the branch defines `SOKOBAN_MENU_NAME`,
    `SOKOBAN_UNDO_NAME`, `SOKOBAN_PAUSE_NAME`, `SOKOBAN_REDO_NAME` and leaves
    `LEVEL_REPEAT`/`BUTTON_SAVE` to the touch fallback rather than colliding.
  - `vu_meter` and `jewels` likewise need `LABEL_*` / `HK_SELECT` / `HK_CANCEL`
    strings, not just button macros.
- **Manual coverage followed the code, not the other way round.** 37 plugin
  chapters now carry an `ESP32S3_PAD` row per key table, 208 rows in total, written
  from the branches rather than from the plugin headers. The code side turned out to
  be much further along than the chapter side: 41 plugin sources already contain an
  `ESP32S3_PAD` branch, including `pacbox`, `sudoku`, `calculator`, `calendar`,
  `chessbox`, `doom`, `fractals`, `imageviewer`, `mpegplayer`'s start-time dialog,
  `text_viewer`, `zxbox` and `chip8` — so the gap was documentation, not bindings.
  Three things came out of writing it down:
  - The quit gesture was then unified: eight branches (`clix`, `invadrox`,
    `rockpaint`, `star`, `calculator`, `calendar`, `text_viewer`, `fractals`) quit on
    **B tap** while pluginlib and the newer branches use **B held**, which is also what
    most upstream keypads do (`SELECT|REPEAT`, `HOME|REPEAT`). All eight now use held,
    their chapters say *Long Button B*, and `star`'s on-screen help label went from
    "B" to "Long B" so the plugin tells the truth. Six games keep B tap because their
    quit macro opens a menu rather than exiting (`blackjack`, `brickmania`, `xobox`,
    `solitaire`, `pegbox`, `doom`) -- tapping it is cancel/menu, which is the other half
    of the convention. `pegbox` was also printing "TOPLEFT" in its help text from an
    `#ifndef` default because our branch never set `QUIT_TEXT`.
  - `sokoban`'s level-change rows had no key on this target, which is why the branch
    now defines `SOKOBAN_LEVEL_UP`/`SOKOBAN_LEVEL_DOWN`. Three other sokoban rows have
    no macro on any target worth copying and read *not available*.
  - `text_viewer`'s one-column-left/right rows are genuinely unavailable: our branch
    does not define `TV_COLUMN_LEFT`/`TV_COLUMN_RIGHT` and the touch fallback does not
    either, so those cases compile out. The chapter says so instead of inventing keys.
  The chapters already had `\opt{touchscreen}` rows, and that option comes from the
  features list rather than from the platform file, so the touch half of every table
  was rendering for us all along — only the stick and A/B column was missing.
- **Residual doc gap, measured**: 3 shipped chapters still have rows with no key for
  us — `pictureflow` (album jump/sort/menu, whose key table is written per-pad with no
  MeowKit branch), `xworld` (a Spectrum emulator whose input goes through an on-screen
  keyboard, not a keypad chain at all), and `metronome` (its tap-tempo, volume and sync
  rows use `PLA_SCROLL_*`, which no keypad without a wheel can supply). Four more
  (`chess_clock`, `midiplay`, `rockboy`, `stopwatch`) have the same shape and are not
  shipped. Documenting those three needs a mechanism read, not a guess, so they are
  left rather than filled with plausible keys.
- **Hardware status**: the plugin batch, the theme pack and the fonts artifact were
  checked on the device. The open plugin item is documentation only (see the residual
  doc gap below), plus `mpegplayer` transport, whose chapter is written but whose keys
  have not been exercised -- it needs video files on the card.
- **Remaining touch-only plugins** are viewers and utilities (`credits`,
  `rb_info`, `logo`, `matrix`, `starfield`, `fire`, `fireworks`, `snow`,
  `mosaique`, `oscilloscope` is now fixed, etc.), where a touch grid is the
  natural input and the upstream mapping is already reasonable. The
  subdirectory titles with no classified mechanism (`chessbox`, `goban`,
  `pacbox`, `sudoku`, `zxbox`, `superdom`, `rocklife`) go through pluginlib or
  the helper libs and need only hardware confirmation, not new keymaps.
- **Status:** open — the games are done pending a hardware test of the batch;
  the utility set is deliberately left on touch.

## 11. USB HID: wired, and the keypad only lives on the USB screen

## 11. USB HID: wired, and the keypad only lives on the USB screen

- **Claim:** `manual/configure_rockbox/system_options.tex` documents USB HID
  with multimedia, presentation and browser keypad modes.
- **Reality:** HID is enabled — `firmware/export/config.h:1450` defines
  `USB_ENABLE_HID` from `USB_HAS_INTERRUPT`, `firmware/usb.c:251` enables
  `USB_DRIVER_HID`, descriptors are assembled in `usbstack/usb_core.c`, and
  `keymap-touchscreen.c` supplies complete tables for all four HID contexts.
  The catch is the pump: `get_hid_usb_action()` (`apps/usb_keymaps.c`) is called
  only from `usb_screen.c:handle_usb_events()`, so the keypad is live only while
  the USB screen runs. Our profile is always `CDC+HID+MSC`, and MSC is what makes
  that screen appear, so the keypad is available whenever the cable is attached.
  The `remote_control` plugin calls `usb_hid_send()` directly, but plugins cannot
  run during USB mode, so it is not a second route.
- **Stick input is not reachable without a core edit.** The HID tables end with
  `LAST_ITEM_IN_LIST` or `NEXTLIST(CONTEXT_USB_HID)` and never call
  `target_get_context_mapping()`, unlike the keyboard table's
  `NEXTLIST(CONTEXT_CUSTOM2|CONTEXT_KEYBOARD)`. Adding the chain would change
  behaviour for every touchscreen target, so it was declined; see `UX-PLAN.md`
  S7.
- **Status:** closed — verified on hardware, HID enumerates and works with MSC
  enabled. Not a code gap.

## 12. Release and tooling infrastructure has no entry for us

- `https://download.rockbox.org/build-info` lists 100 targets; we are not one,
  so `\download{...}` links in the manual point at paths that will not resolve
  for this board.
- `themes.rockbox.org` answers `code=1 "No themes available for the selected
  target"` for `meowkits3`, so third-party themes have to be pulled through a
  proxy target (`ibassodx90` for 320x240 touch, `ipodvideo` for the wider
  320x240 set).
- Rockbox Utility cannot install this target, which is why
  `meowkits3_install.tex` is a hand-written esptool procedure.
- **Status:** expected for an unreleased port. Worth revisiting when the target
  goes upstream.

## 13. Two fonts the themes server carries are not in the tree

`05-Cabbie-Bold.fnt` and `06-Cabbie-Bold.fnt` are in the server's 90-font pack
but not in `fonts/` here, nor in `upstream/master`. Our tree builds the other 88.
Nothing sampled needs them. **Status:** no action unless a theme asks for one.

---

## 14. Lists were three rows tall — DPI-driven row height on a 2.0" panel

- **Symptom:** themes and cabbiev2 alike looked oversized; the file browser,
  main menu, settings and playlist showed about three rows.
- **Cause:** not theme rendering. Rockbox has no scale/DPI/aspect path for
  skins at all (`ui_scale`, `lcd_scale`, `SCALE`, `zoom` appear nowhere in
  `settings_list.c`, `lcd.c` or `font.c`), and our geometry matches
  `ipodvideo`/`ibassodx90` exactly. The size came from
  `apps/gui/list.c:117`: with `HAVE_TOUCHSCREEN` and the *Line Padding in Lists*
  setting on "auto", row height is `MAX(lcd_get_dpi()*4/12, font_height)` =
  **66 px** at our declared 200 dpi, on a 240 px screen.
- **`LCD_DPI 200` is correct, not a guess:** 400 px diagonal over a measured
  2.0" glass is exactly 200 dpi. Surveying every target that defines `LCD_DPI`
  shows touch targets get 50-83 px rows by design — about 8 mm, a finger
  target. cowond2 (160 dpi, 2.50") gets 4.5 rows, ibassodx90 (166, 2.41") 4.4,
  us 3.6. We are not wrong, we are the smallest touch panel in the tree.
- **Fix:** `DEFAULT_LIST_LINE_PADDING`, a weak `#ifndef` seam in
  `apps/settings_list.c` defaulting to `-1` so every other target is unchanged,
  set to `4` for this target (~20 px rows, ~11 visible). Pinned as a *default*,
  deliberately not via `/.rockbox/fixed.cfg` — `settings.c:212` loads that file
  last on every boot, so it would silently undo a user who preferred bigger
  rows. The setting is at Settings -> Display -> Touchscreen Settings.
- **Same number, two smaller consumers:** `apps/gui/bitmap/list.c:164` adds 2 px
  to the list separator when dpi > 200 (we sit exactly on the boundary), and
  `firmware/drivers/touchscreen.c:199` sets the scroll threshold to
  `dpi*32/160` = 40 px of slop, vs 32 px at 160 dpi.
- **Also enabled: `HAVE_BMP_SCALING`.** Every other 320x240 target has it; it is
  the scaler in `apps/recorder/bmp.c` that fits album art to the box a skin
  asked for (`playback_claim_aa_slot()` hands over a `struct dim`). Without it,
  a `%aa` image is drawn at native size — which is why album art in themes like
  Laserhead sprawls. ABI-safe: `apps/plugin.h` has no `HAVE_BMP_SCALING` gate,
  `read_bmp_file`/`read_bmp_fd` are always present, and `apps/features.txt` has
  no bmp entry so the features list is unchanged.
- **Status:** closed — verified on hardware. `4` px padding was measured as
  "so much better" and pinned as the target default.

## Keeping this log

When a gap is closed, move the entry to the closed section at the bottom and
note the commit. When the manual is regenerated, re-check the action macros in
`manual/platform/keymap-meowkits3.tex`: it deliberately says *not available*
where the code produces nothing, so a mapping added to the keymap should be
followed by a change in that file, or the manual will understate the player.

The check that produces the list is small: extract `ACTION_*` from
`apps/keymaps/keymap-meowkits3.c` and `apps/keymaps/keymap-touchscreen.c`, and
compare against the `\Action*` macros the manual body uses under options we
actually have.

## Closed

- **§1 — A-B repeat markers.** `button_context_wps` in
  `apps/keymaps/keymap-meowkits3.c` now maps `BUTTON_A|BUTTON_LEFT` →
  `ACTION_WPS_ABSETA_PREVDIR`, `BUTTON_A|BUTTON_RIGHT` →
  `ACTION_WPS_ABSETB_NEXTDIR` and `BUTTON_A|BUTTON_B` → `ACTION_WPS_ABRESET`,
  each with `BUTTON_A` as the prerequisite — the as3525 pattern
  (`keymap-c200.c:66-69`, `keymap-e200.c:66-69`) with A as the modifier instead
  of POWER. `action.c:591-604` requires an exact `button_code` and prefers an
  exact prerequisite, so a lone A is still play/pause and these are separate
  events. This also removed the "not available or Short Right + Long Right"
  line the manual was printing on the directory-skip rows.
- **§3 — bookmark create and list.** `button_context_wps` now maps
  `BUTTON_B|BUTTON_UP` → `ACTION_WPS_CREATE_BOOKMARK` and
  `BUTTON_B|BUTTON_DOWN` → `ACTION_WPS_LIST_BOOKMARKS`, with B as the
  prerequisite, following `keymap-hm801.c:79-80` (`POWER+SELECT` to list,
  `POWER+DOWN` to create). Deliberately WPS-context only: the handlers live in
  `apps/gui/wps.c:1053` and `:1062`, so a standard- or tree-context mapping
  would produce an action nothing consumes. The manual needed no change — its
  body has no `\ActionWpsCreateBookmark` or `\ActionWpsListBookmarks` macro to
  fill.
- **§8, part 1 — the stick had no keyboard context.** `button_context_keyboard`
  now exists in `apps/keymaps/keymap-meowkits3.c` and
  `target_get_context_mapping()` answers `CONTEXT_KEYBOARD` and
  `CONTEXT_MORSE_INPUT` with it, which is where
  `keymap-touchscreen.c`'s keyboard list chains to via `LAST_ITEM_IN_LIST__NEXTLIST`.
  Mapping: Up/Down rows, Left/Right along a row, A tap = select, A hold = done,
  B tap = abort, B hold = backspace, A+B = page flip. No cursor gestures on
  purpose: `keyboard.c:713-725` already turns LEFT/RIGHT into cursor movement in
  line-edit and morse mode, and the dispatcher returns the first match, so a
  hold-based cursor mapping would have shadowed the row move. The manual's
  keyboard macros were updated to match.
- **§8, part 2 — keyboard hint line.** **Not doing; nothing to integrate.**
  `gui_dynbutton` / `dynamic_menu.c` do not exist in this tree at all (zero
  references tree-wide), and `kbd_draw_buttons()` draws
  `LANG_KBD_OK`/`DELETE`/`CANCEL` only as touchable regions on the point-mode
  path (`keyboard.c:688`), which is itself gated behind the 480x640 requirement.
  `keyboard.c` includes `talk.h` but never calls the voice UI, so there is no
  spoken-hint feature to enable either. A hint line would mean inventing a core
  feature for one target. Discoverability is covered by the manual's keyboard
  table instead, and by the stick gestures deliberately mirroring the touch
  grid's tap-select / hold-done convention.

- **§4 — key lock.** `ACTION_STD_KEYLOCK` on both face buttons held, in the
  standard and WPS contexts (`apps/keymaps/keymap-meowkits3.c`). Held rather
  than tapped, because a pocket lock must not be reachable by an accidental
  press, and because in the WPS the tap is already A-B reset. The same gesture
  in both contexts matters: unlocking compares the raw button against the
  `unlock_combo` recorded at lock time. The pad list in
  `manual/rockbox_interface/wps.tex` gained `ESP32S3_PAD` for the key-lock row,
  and for the pitch row, which our touch grid does provide. Autolock becomes
  reachable as a side effect, since arming it requires a stored unlock combo.
