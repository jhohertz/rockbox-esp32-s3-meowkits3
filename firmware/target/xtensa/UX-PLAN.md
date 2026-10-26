# Input & UX plan — MeowKit ESP32-S3

Plan for closing the mapping and usability gaps in `DOC-GAPS.md`. Written after
the manual was built, because the manual is what made the gaps legible: every
`\Action*` the body renders under an option we advertise is a promise.

Hardware reality: **6 physical inputs** (A, B, and a 5-way stick = Up/Down/
Left/Right), a **3x3 touch grid** with tap and hold on each cell, and the
**AXP173 PEK** input (currently only its long press is used, for shutdown).
That is a lot of gesture capacity if combos and holds are used deliberately,
and very little if every action wants a dedicated key.

## Gesture budget, measured

Per input, the dispatcher distinguishes *press*, *hold* (`BUTTON_REPEAT`) and
*release* (`BUTTON_REL`), and a mapping may require a prerequisite key — which
is how other targets get extra capacity. Current usage:

| Gesture | std | wps | settings | tree | recs | yesno | kbd |
|---|---|---|---|---|---|---|---|
| Up / Down | prev/next | vol up/down | inc/dec | prev/next | — | — | *(none)* |
| Up/Down hold | prev/next repeat | vol repeat | inc/dec repeat | repeat | — | — | *(none)* |
| Left tap | cancel | skip prev | prev | — | val dec | — | *(none)* |
| Right tap | ok | skip next | next | — | val inc | — | *(none)* |
| Left/Right hold | — | seek back/fwd | — | — | val repeat | — | *(none)* |
| A tap | ok | play/pause | enter | enter | rec pause | accept | *(none)* |
| A hold | context | stop | — | — | — | — | *(none)* |
| B tap | menu | menu | cancel | — | cancel | reject | *(none)* |
| B hold | quickscreen | context | — | — | new file | — | *(none)* |

Touch cells: all nine taps and all nine holds are assigned by
`apps/keymaps/keymap-touchscreen.c`. So **tap and hold are exhausted**; the
remaining capacity is (a) key **combos** with a prerequisite, (b) the **short
PEK** press, and (c) **skin touch regions**, which cost no keys at all.

### Combos are established practice, not a hack

- `apps/keymaps/keymap-ipod.c:61` — `BUTTON_SELECT|BUTTON_PLAY` →
  `ACTION_TREE_HOTKEY`.
- `apps/keymaps/keymap-clip.c:91` — `BUTTON_HOME|BUTTON_SELECT` →
  `ACTION_STD_KEYLOCK`; `keymap-3ds.c:56` uses `BUTTON_USER|BUTTON_POWER`;
  `keymap-agptekrocker.c:85` uses plain `BUTTON_POWER`.
- `apps/keymaps/keymap-ondio.c` — `BUTTON_POWER|BUTTON_DOWN` →
  `ACTION_WPS_CREATE_BOOKMARK`, `BUTTON_POWER|BUTTON_SELECT` →
  `ACTION_WPS_LIST_BOOKMARKS`.
- `keymap-c200.c:67`, `keymap-gigabeat.c:81`, `keymap-fuze.c:67` —
  `BUTTON_A|BUTTON_LEFT` / `BUTTON_BACK|BUTTON_LEFT` →
  `ACTION_WPS_ABSETA_PREVDIR`. The convention is: hold one key, press another.
- `keymap-fiiom3k.c:75` — `BUTTON_DOWN|BUTTON_REPEAT` →
  `ACTION_WPS_ABSETB_NEXTDIR`.

On our panel the natural modifier is **B held while a stick direction or A is
pressed**: B's solo tap/hold are menu and quick screen, which does not conflict
with combos because the prerequisite makes the combo a distinct event.

### Skin touch regions are the cheapest capacity

`apps/gui/skin_engine/skin_parser.c:1625` lists the actions a `%T(...)` region
can produce: `lock`, `mute`, `shuffle`, `repmode`, `createbookmark`,
`listbookmarks`, `playlist`, `trackinfo`, `browse`, `play`, `stop`, `pitch`,
`setting_set`, and more. Three consequences:

1. `ACTION_TOUCH_SHUFFLE` and `ACTION_TOUCH_REPMODE` exist **only** as touch
   actions — there is no `ACTION_WPS_SHUFFLE` in `apps/action.h`, and no target
   maps one. Shuffle/repeat toggles are a skin feature upstream, not a key
   feature. Gap #2 is therefore a theme task, not a keymap task.
2. `lock` maps to `ACTION_TOUCH_SOFTLOCK`, so key lock can come from a skin
   region as well as from a key.
3. Progress, volume and setting bars already get automatic regions
   (`skin_parser.c:1261`), so bars work without theme edits.

---

## Spikes

Each is independently flashable and independently revertable. "Doc" means the
matching change to `manual/platform/keymap-meowkits3.tex`, which currently says
*not available* for anything unimplemented.

### S1 — Keyboard: stick context and discoverability  *(highest value)*

**Status: done.** Mapping landed (`button_context_keyboard` in
`apps/keymaps/keymap-meowkits3.c`, wired for `CONTEXT_KEYBOARD` and
`CONTEXT_MORSE_INPUT`). The hint half is **not being done** — see below.

Two separate problems.

**Mapping.** `apps/keymaps/keymap-meowkits3.c` has no keyboard context, so in
line-mode the stick does nothing and only the touch grid (which maps
`ACTION_KBD_*` at `keymap-touchscreen.c:255`) works — and tapping a
character-list keyboard navigated by touch is not possible at this resolution,
because the grid cells drive *actions*, not characters. Proposed stick
mapping, mirroring the touch table's semantics:

| Gesture | Action |
|---|---|
| Up / Down | `ACTION_KBD_UP` / `ACTION_KBD_DOWN` (move between rows) |
| Left / Right | `ACTION_KBD_LEFT` / `ACTION_KBD_RIGHT` (move along the row) |
| Left / Right hold | `ACTION_KBD_CURSOR_LEFT` / `ACTION_KBD_CURSOR_RIGHT` |
| A tap | `ACTION_KBD_SELECT` (commit the highlighted character) |
| A hold | `ACTION_KBD_DONE` (accept the string) |
| B tap | `ACTION_KBD_ABORT` |
| B hold | `ACTION_KBD_BACKSPACE` |
| Up/Down hold | `ACTION_KBD_UP` / `DOWN` repeat |
| A+B | `ACTION_KBD_PAGE_FLIP` |

Note the touch table's distinction, which this copy must keep: **select is a
tap and done is a hold on the same cell** (`BUTTON_CENTER` / `BUTTON_CENTER|
BUTTON_REPEAT` at lines 265-266). `keyboard.c:722-725` also rewrites
`ACTION_KBD_LEFT/RIGHT` into cursor movement in certain states, so the mapping
is safe to provide both ways.

**Discoverability.** The user entered the keyboard and could not tell how to
finish. Confirmed cause: `kbd_draw_buttons()` (`keyboard.c:1442`) is called only
for the point-mode layout (`keyboard.c:688`); line mode draws a picker and an
edit line and **never names the keys**. That is not a target bug — every
stick-only target has the same problem.

**Decision: do not fix — there is nothing to integrate.** `gui_dynbutton` and
`apps/gui/dynamic_menu.c` are absent from this version entirely, so there is no
soft-key label facility to call. The `LANG_KBD_OK` / `LANG_KBD_DELETE` /
`LANG_KBD_CANCEL` labels exist but are drawn only as touchable regions on the
point-mode path, and point mode is itself gated behind the 480x640 requirement
we do not meet. `keyboard.c` includes `talk.h` but never calls the voice UI, so
there is no spoken-hint feature either. Adding a hint line would mean inventing
a core feature for one target. The manual documents the gestures, and they
deliberately mirror the touch grid's tap-select / hold-done convention so the
two input paths agree.

Files: `apps/keymaps/keymap-meowkits3.c` only.

Files: `apps/keymaps/keymap-meowkits3.c`, `apps/recorder/keyboard.c` (seam),
`firmware/target/xtensa/*` if the hint is target-drawn.
Doc: replace the five `*kbd*` "not available" entries.

### S2 — Short PEK: parked

**Not doing.** The intended use was a screen/backlight toggle, and there is no
upstream action for one: `apps/action.h` has no `ACTION_STD_BACKLIGHT` or
`ACTION_STD_SHOWSCREEN`, and `backlight_hw_off()` is only reached from
`firmware/backlight.c`'s own timeout paths. Making the gesture work would mean
inventing a new action and target-side handling for it, which is out of scope
while there are unimplemented gestures that map to functions Rockbox already
has. Short PEK stays unused; long PEK continues to mean shutdown.

If it is revisited, the groundwork is worth keeping in mind: the short press is
bit 1 of REG `0x46`, the register `button-xtensa.c` already reads for the long
press, so it would cost no extra I2C; `BUTTON_POWER` at `0x00008000` is free
(our keys use `0x00000001`–`0x00004000`, `BUTTON_REL` is `0x02000000`,
`BUTTON_REPEAT` is `0x04000000`); and `firmware/drivers/button.c:374-397`
already calls `backlight_on()` on key activity, so waking would come free.


### S3 — Bookmarks via B-modifier combos  *(done)*

Was deletable, not creatable. Following `keymap-hm801.c:79-80`
(`POWER|SELECT` to list, `POWER|DOWN` to create), with B as the modifier:

- WPS: `BUTTON_B|BUTTON_UP` (prereq B) → `ACTION_WPS_CREATE_BOOKMARK`
- WPS: `BUTTON_B|BUTTON_DOWN` (prereq B) → `ACTION_WPS_LIST_BOOKMARKS`

WPS context only, deliberately: the handlers are `apps/gui/wps.c:1053` and
`:1062`, so a standard- or tree-context mapping would emit an action nothing
consumes. The original plan to also cover std/tree was wrong.

No manual change was needed — the body has no `\ActionWpsCreateBookmark` or
`\ActionWpsListBookmarks` macro to fill.

Still worth doing later: add the skin regions (`%T(...,createbookmark)`,
`%T(...,listbookmarks)`) to our theme so the gesture is visible on screen rather
than only memorised. That belongs with S6.

Files: `apps/keymaps/keymap-meowkits3.c`.
Doc: `ActionBmView`/`ActionBmJump` remain *not available* — those are bookmark
screen actions (`ACTION_BM_*`) and no target maps them to keys.

### S4 — A-B repeat via A-modifier combos  *(done)*

`ab_repeat_buttons` was enabled but unreachable. Following the c200/gigabeat
precedent (`BUTTON_A|BUTTON_LEFT` with prereq `BUTTON_A`):

- WPS: `BUTTON_A|BUTTON_LEFT` (prereq A) → `ACTION_WPS_ABSETA_PREVDIR`
- WPS: `BUTTON_A|BUTTON_RIGHT` (prereq A) → `ACTION_WPS_ABSETB_NEXTDIR`
- WPS: `BUTTON_A|BUTTON_B` (prereq A) → `ACTION_WPS_ABRESET`

Conflict check: A's solo tap and hold are play/pause and stop, and A+hold is
not a combo the dispatcher would confuse with A-then-Left, because the combo
fires while A is still held and the prereq is A. This is exactly how the as3525
targets do it. Verify on hardware that a seek gesture (`Left|REPEAT`, no A)
still reaches seek and not A-B.

**Observed on hardware, and correct:** when A-B repeat is *not* active, A+Left
and A+Right jump to the previous and next directory. That is upstream's
semantics — the `*_PREVDIR` / `*_NEXTDIR` action names say so — and it is why
the manual lists the same gesture twice, once in the A-B rows and once in the
directory-skip rows. Recorded here so the dual purpose is not mistaken for a
mis-mapping.

Doc: replace the five `Ab*` "not available" entries — that also fixes the
"not available or Short Right + Long Right" line on manual p.35.

### S5 — Key lock: landed as a held two-button combo

Your framing was the useful one: the old players had a physical rocker that
blocked everything but the unlock, and here that has to be a gesture which is
hard to hit by accident. `BUTTON_A|BUTTON_B|BUTTON_REPEAT` with pre-button
`BUTTON_A|BUTTON_B`, in both the standard and WPS contexts.

What the measurement settled:

- **There is no keylock subsystem in this tree.** No `firmware/keylock.c`, no
  `CONFIG_KEYLOCK`, nothing in `settings_list.c` under that name. It is the soft
  lock in `apps/action.c`, live because we do not define `HAS_BUTTON_HOLD`.
  While locked, `do_softlock()` filters everything except the gesture matching
  `unlock_combo` — exactly "prevents anything but unlock".
- **Touch alone was unsafe.** Under default settings `do_key_lock()` calls
  `button_enable_touch(false)`, so a lock produced by a skin region would switch
  the panel off and leave `unlock_combo` holding a touch code no stick press can
  match. Unrecoverable until reboot. That is why a button gesture is not a
  nicety here.
- **Without a mapping, autolock was dead code.** `action_autosoftlock_init()`
  arms it only if `unlock_combo != BUTTON_NONE`, and finds that combo by
  searching the WPS and STD contexts for `ACTION_STD_KEYLOCK`. So the "Advanced
  Key Lock" submenu — which we do build, since it is gated on `!HAS_BUTTON_HOLD`
  rather than on `hold_button` — could never engage. Mapping the gesture is what
  makes locking-on-backlight-time-out usable, which is the pocket case.
- **Held, not tapped.** In the WPS the tap is already A-B reset, and a lock
  should need intent. Unlock is the same gesture; the press that precedes it
  does whatever the context uses the tap for, which is harmless (in the WPS it
  resets A-B markers).
- **`%T(...,lock)` is a different thing.** `ACTION_TOUCH_SOFTLOCK` only toggles
  the skin's own `touchscreen_locked` flag, hiding that theme's other regions
  while keeping the lock region tappable. It is a per-theme panel lock,
  reversible by construction, unrelated to soft lock. It stays with the theme
  packs (S6).

Manual: `\ActionStdKeylock` now reads *Long (A + B)*, and the WPS key-lock row —
which hard-codes keys per pad rather than using that macro — gained
`ESP32S3_PAD`. The pitch row gained the same option, since our touch grid does
provide that gesture.

Files: `apps/keymaps/keymap-meowkits3.c`, `manual/platform/keymap-meowkits3.tex`,
`manual/rockbox_interface/wps.tex`. Status: **landed, needs hardware test** —
lock in the WPS, confirm stick and touch are both ignored and the screen says
locked, unlock with the same gesture, then try autolock via Settings ->
Advanced Key Lock.

### S6 — Shuffle, repeat mode and the visible affordances: deferred to theme packs

**Deferred by decision: do not touch cabbiev2.** The builtin theme is the
default set and stays untouched. Shuffle and repeat mode have no upstream key
action (`apps/action.h` has neither), so the only route is `%T` regions:

```
%T(x,y,w,h,shuffle)   %T(x,y,w,h,repmode)   %T(x,y,w,h,mute)
%T(x,y,w,h,createbookmark)   %T(x,y,w,h,listbookmarks)   %T(x,y,w,h,lock)
```

That belongs with the third-party theme packs rather than our default: a theme
that ships these regions gets them on our panel with no firmware change at all,
which is the point of the skin engine's touch actions. When theme packs are
evaluated, `tools/rbthemes.py` should flag which themes define `%T` regions —
that is a compatibility signal worth showing at `list` time, since a theme with
no regions on a touch panel gives the user only the automatic bar zones
(`skin_parser.c:1261`).

Until then: shuffle and repeat are reachable from the settings menu, and the
keymap continues to say *not available* for them, which is accurate.

### S7 — USB HID keypad: touch-only, stick layer declined

**Decision: leave HID on the touch grid, no stick mapping, no core edit.**

The path is real and compiled:

- `firmware/export/config.h:1450` defines `USB_ENABLE_HID` from
  `USB_HAS_INTERRUPT`, so HID is enabled for us; `firmware/SOURCES:1033` builds
  `usbstack/usb_hid.c`; `firmware/usb.c:251` enables `USB_DRIVER_HID`.
- Descriptors are assembled in `firmware/usbstack/usb_core.c` (device and config
  descriptors at lines 94 and 117), **not** in the designware driver — that was
  the open question in the earlier draft of this plan and it is now answered.
- `apps/usb_keymaps.c` maps `ACTION_USB_HID_*` to consumer and keyboard usages,
  per mode, each with its own context.
- `apps/keymaps/keymap-touchscreen.c` supplies complete tables for
  `CONTEXT_USB_HID`, `_MODE_MULTIMEDIA`, `_MODE_PRESENTATION` and
  `_MODE_BROWSER`, and its dispatcher answers those contexts.

**Where input can happen.** `get_hid_usb_action()` (`apps/usb_keymaps.c`) is the
only pump, and its only caller is `usb_screen.c:handle_usb_events()` under
`global_settings.usb_hid`. So the keypad is live only while the USB screen runs.
That is upstream design, not a port gap, and it is not a limitation in practice:
our profile is always `CDC+HID+MSC`, MSC is what makes the USB screen appear,
and the screen is up whenever the cable is attached. The `remote_control` plugin
calls `usb_hid_send()` directly but plugins cannot run while USB is active, so
it is not an alternate route either.

**Why the stick is not feasible without touching core.** The HID tables end with
`LAST_ITEM_IN_LIST` or `LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_USB_HID)`; unlike
the keyboard table, which chains with
`NEXTLIST(CONTEXT_CUSTOM2|CONTEXT_KEYBOARD)` and is therefore how our stick
keyboard context is reached, the HID tables never call
`target_get_context_mapping()`. So `keymap-meowkits3.c` cannot contribute HID
gestures at all. Adding the chain would mean editing `keymap-touchscreen.c`,
which is shared — and unlike the `CONTEXT_RECSCREEN` chain this port already
added (that fixed a screen no touchscreen target could use at all), this one is
a preference and would make every touchscreen target's HID contexts fall through
to their own standard table.

Revisit only as an upstream change, or if a target ever gains a HID screen of
its own.

**Verify on hardware (touch only).** With the board plugged in as MSC:

- Top-Left tap / hold switches keypad mode, and the mode name in the title
  repaints (`usb_screen.c:draw_usb_keypad_mode`).
- Multimedia: Top-Middle / Bottom-Middle volume (tap and hold behave the same —
  those entries have no release variants on purpose), Centre tap = mute,
  Centre hold = play/pause, Top-Right tap = stop… read the table rather than
  trusting this line: `keymap-touchscreen.c:300-311` is the authority.
- Presentation and browser modes map onto Top-Right / MidLeft / MidRight /
  Bottom-Right with tap and hold distinct.
- The host must see a HID interface alongside MSC and CDC. That is the one claim
  still unverified, and it is a host-side observation (`lsusb -d 1058:`, or the
  OS input device list), not something the source can settle.

Files: none. Doc: the manual already documents the modes via
`configure_rockbox/system_options.tex`; the gesture table for our target lives
in the shared touchscreen mapping, so nothing in `keymap-meowkits3.tex` changes.

### S8 — Recording screen entry from anywhere: **passed**

`ACTION_STD_REC` is real — `apps/menu.c:495` maps it to `GO_TO_RECSCREEN` — so
the mapping would have worked. Passed on purpose: recording is already reachable
from the WPS (touch Top-Left hold, and our recscreen table) and from the
settings/menu route, and a from-anywhere record key is not something the panel
needs. Revisit if short PEK is ever implemented, which is driver work rather
than a keymap change: `0x00008000` is still free for a PEK button code.

### S9 — Settings reset: **not doing; nothing to integrate**

`ACTION_SETTINGS_RESET` does not exist anywhere in this tree — not in
`apps/action.h`, not in any keymap. Same class as the short-PEK screen toggle
and the keyboard hint line: mapping it would mean inventing a core action for
one target. Reset is available by editing the value, or by deleting
`/.rockbox/config.cfg` off the card.

### S10 — Plugin button audit  *(its own effort)*

53 plugin sources reference `BUTTON_HOME`, `PLUS`, `MINUS`, `MODE`, `FN`, `REC`,
`OFF` — none of which exist on this board. `pluginlib_actions.c` has our
`ESP32S3_PAD` branch but only for the four directions; select/cancel come from
the `HAVE_TOUCHSCREEN` block. This is not a single fix:

1. Enumerate the plugins actually shipped in our zip.
2. For each, determine whether pluginlib covers it, or whether it reads raw
   buttons.
3. For raw-button plugins, decide: alias in the plugin (some have per-target
   button headers, e.g. `apps/plugins/imageviewer/imageviewer_button.h`), or
   exclude from our build, or map to a combo.
4. Re-check the failure mode that bit 2048: with `HAVE_TOUCHSCREEN` defined,
   pluginlib's `#else` branch compiles **no `PLA_*` entries at all**, so a
   plugin loads, draws and ignores every key silently. Any new pad-specific
   branch must be verified by running a plugin, not by compiling.

Worth doing as several spikes, grouped by plugin family (games needing SELECT/
START, viewers needing PLUS/MINUS, etc.).

### S11 — Tooling and release stubs

Canonical short name is **`meowkits3`** — it matches `MODELNAME`, `builds.pm`,
`config/meowkits3.h`, the manual platform file and the front-image filename.
Keep it everywhere.

- `utils/rbutilqt/rbutil.ini` `[platforms]` ends at `platform149=rgnano`; add
  `platform150=meowkits3` so rbutil's device tree can show the target. Note the
  limit: rbutil reads build URLs and `themes_url` from the server's build-info,
  which has no `meowkit` entry, so a stub alone gives a selectable platform with
  nothing behind it. Useful only together with a local build-info source or a
  server-side entry upstream.
- `tools/builds.pm` already has us with `manualok => 1`.
- Our own scripts should become the install path until upstream: extend
  `tools/esp-stage2` / `tools/espflash.make` to also flash *and* populate the
  card, i.e. a single goal that writes the image and unpacks core + fonts +
  themes zips. The theme artifact still needs `tools/rbthemes.py`
  (`list` / `get` / `fonts` / `check` / `merge`, audit against the union of all
  three zips).
- Deferred by choice: `HAVE_HOTKEY`. Note that the combo work in S3/S4/S5
  effectively creates a modifier, which would make hotkeys feasible later —
  revisit after S4.

### S12 — RTC alarm wake: **dropped, hardware says no**

The driver half is done and stays compiled out — see `DOC-GAPS.md` §7 for the
five-function contract and the register map taken from `rtc_e8564.c`.

The integration half is not buildable on this board. The ESP32-S3 can wake
itself from deep sleep with its own RTC timer, but our shutdown does not reach
deep sleep: `power_off()` sets AXP173 REG 0x32 bit 7, dropping DCDC1 and
removing the SoC's supply, so no timer survives to fire. The AXP173 has no alarm
or timeout output — a PEK press is its only re-enable — and the PCF8563 `/INT`
is not wired to a GPIO, so there is no external wake source either. The only way
to boot on an alarm would be to stop truly powering off, which is a power-
semantics change this port should not invent.

What we ship instead: the `clock` and `alarmclock` plugins, both already in the
zip, both driven by pluginlib's `ESP32S3_PAD` branch (four directions, A =
select, B = cancel, held B = exit). `alarmclock` counts down and rings while the
player is on. `HAVE_RTC_ALARM` stays off permanently unless the board gains a
wake path.

---

## Order of work

1. **S1** keyboard stick mapping — **done**, verified on hardware. The hint
   line is dropped as an invention rather than deferred.
2. **S4** A-B combos — **done**, closes the most visible advertised-but-unusable
   gap and fixes manual p.35.
3. **S3** bookmarks — **done**, WPS context only.
4. **S7** USB HID — **touch-only, stick layer declined**: the HID contexts never
   chain to the target, so a stick mapping would require editing the shared
   `keymap-touchscreen.c`. Verify the touch keypad on hardware instead.
5. **S6** theme touch regions — **deferred**: cabbiev2 stays untouched, and
   `%T` regions come with the theme packs instead.
6. **S5** key lock — **landed** as A + B held, std and WPS contexts, verified on
   hardware. **S8** recording — **passed**: the action exists
   (`apps/menu.c:495`) but a from-anywhere record key isn't wanted. **S9**
   settings reset — **not doing**: `ACTION_SETTINGS_RESET` is not in this tree
   at all. S2 is parked; short PEK stays unused rather than becoming a new
   action.
7. **Theme pack** — **built**: `tools/rbthemes.py` plus an 8-theme
   `rockbox-themes.zip`, audited clean against the union of the core and fonts
   artifacts.
8. **S10** plugin audit as its own multi-part effort.
9. **S11** tooling and rbutil stubs.
10. **S12** RTC alarm wake — **dropped**: the AXP removes the SoC's supply at
    power-off, so nothing survives to wake. Driver code stays compiled out;
    `clock`/`alarmclock` plugins are the answer.

Every step updates `manual/platform/keymap-meowkits3.tex` and moves the matching
entry in `DOC-GAPS.md` to *Closed* with the commit id.

## Verification rules carried over

- One variable per flash, one repeatable trigger.
- Measure, don't infer. The PEK work in particular needs the poll cost and the
  wake-after-off behaviour checked on hardware.
- A mapping that compiles is not a mapping that works: 2048 compiled and
  ignored every key. Run the affected screen before calling a gesture done.
- Core files get seams only (`#ifndef` / weak hooks defaulting to upstream
  behaviour), never target-specific conditionals.
