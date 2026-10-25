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
  default cabbiev2 defines none.
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
- **Reality:** no `ACTION_STD_KEYLOCK` mapping exists. That row selects its key
  from an explicit pad list (`SANSA_CLIP_PAD`, `SANSA_FUZEPLUS_PAD`,
  `AIGO_EROSQ_PAD`, `RG_NANO_PAD`), so for us the cell renders blank.
- **What fixing it means:** either implement key lock (it is cheap —
  `ACTION_STD_KEYLOCK` just gates non-power keys), or accept a blank row.
  Upstream could gate the row properly, but that edits other targets' manuals.
- **Status:** open, cosmetic until key lock is implemented.

## 5. Pitch screen is reachable only by touch

- **Claim:** `pitchscreen` is in our feature list; the manual documents
  entering it from the WPS.
- **Reality:** `ACTION_WPS_PITCHSCREEN` comes only from holding the bottom-left
  grid cell (`keymap-touchscreen.c:100`). No stick gesture produces it. Once
  inside, the stick works — `ACTION_PS_*` are all available.
- **What fixing it means:** one WPS-context mapping. Free stick slots are
  scarce: `Up|REPEAT` and `Down|REPEAT` are volume on the WPS, `A|REPEAT` is
  context there and stop, `B|REPEAT` is quick screen.
- **Status:** open, low severity.

## 6. No hotkeys

- **Claim:** none — `manual/rockbox_interface/hotkeys.tex` is gated on
  `\opt{hotkey}` and our feature list has no `hotkey`, so the manual correctly
  omits it. The shared touchscreen keymap therefore routes its bottom-right tap
  to the playlist instead (`keymap-touchscreen.c:94-98`).
- **Reality:** consistent. Deferred deliberately: `HAVE_HOTKEY` needs a modifier
  button, and this board has six inputs with no natural modifier.
- **Status:** deferred by choice, documented as such.

## 7. No wake-up alarm

- **Claim:** none — the alarm section is gated on `\opt{alarm}`, and we do not
  advertise it.
- **Reality:** `rtc_setalarm()` is not implemented in
  `firmware/drivers/rtc/rtc_pcf8563.c`, so `HAVE_RTC_ALARM` cannot be enabled.
  The PCF8563 has a countdown timer register that would support it.
- **Status:** deferred by choice. Enabling it is a target-driver task, not a
  config flag.

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
| `ACTION_STD_REC` | start recording from any screen | WPS only (`ACTION_WPS_REC`) |
| `ACTION_STD_SHOWSCREEN` | screen/backlight toggle | none |
| `ACTION_SETTINGS_RESET` | reset a setting to default | none |
| `ACTION_STD_KEYLOCK` | key lock | none (see §4) |
| `ACTION_STD_HOTKEY` | hotkey | none, and correctly undocumented |

- **Status:** open, low severity. Each is one mapping if we decide the gesture.

## 10. Plugins: 53 sources reference buttons we don't have

- **Claim:** the manual documents many plugins with their own key tables
  (`ButtonHome`, `ButtonPlus`, `ButtonMinus`, `ButtonMode`, `ButtonFn`,
  `ButtonRec`, `ButtonOff` appear throughout `manual/plugins/`).
- **Reality:** this board has six inputs plus the touch grid. 53 plugin sources
  under `apps/plugins/` reference at least one button we cannot produce.
  `apps/plugins/lib/pluginlib_actions.c` has an `ESP32S3_PAD` branch, but it
  defines only the four directions and their repeats; select/cancel/exit come
  from the shared `HAVE_TOUCHSCREEN` block.
- **Why this matters:** that shared block is the reason a comment in our branch
  warns about a silent failure mode — with `HAVE_TOUCHSCREEN` defined, the
  `#else` branch compiles a pluginlib list with no `PLA_*` entries at all, so a
  plugin loads, runs, draws and ignores every key. `2048` was exactly that
  before the `ESP32S3_PAD` branch existed.
- **What fixing it means:** a per-plugin audit against our inputs, not a
  wholesale fix. Worth ranking: which plugins ship in our zip and are
  unplayable, and whether the touch grid can stand in for the missing keys.
- **Status:** open, needs an audit pass.

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
- **Status:** verify the touch keypad on hardware (mode switch repaint, usages
  reaching the host). Not a code gap; the host-side descriptor claim is the one
  thing still unverified.

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
