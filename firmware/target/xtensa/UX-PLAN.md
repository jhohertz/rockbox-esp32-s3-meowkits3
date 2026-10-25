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

**Status: mapping half landed** (`button_context_keyboard` in
`apps/keymaps/keymap-meowkits3.c`, wired for `CONTEXT_KEYBOARD` and
`CONTEXT_MORSE_INPUT`). The hint half is still open and needs the seam.

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

Fix needs a seam in a core file, and it must default to upstream behaviour:
an `#ifndef HAVE_KBD_HINT` (or a weak no-op `kbd_draw_hint()`) that our target
implements to draw one line under the edit field naming Done / Abort / Backspace.
Keep it additive: no change to layout math for targets that do not define it.
Worth checking first whether the existing title line can carry the hint instead,
which would need no seam at all.

Files: `apps/keymaps/keymap-meowkits3.c`, `apps/recorder/keyboard.c` (seam),
`firmware/target/xtensa/*` if the hint is target-drawn.
Doc: replace the five `*kbd*` "not available" entries.

### S2 — Short PEK as a real key, then screen/backlight toggle

There is no upstream action for a backlight toggle: `apps/action.h` has no
`ACTION_STD_BACKLIGHT`/`SHOWSCREEN`, and `backlight_hw_off()` is only called
from `firmware/backlight.c`'s own timeout paths. So this needs a target-level
handler, which is the shape the user asked for anyway.

Prerequisites, in order:

1. **Make PEK short press a button code.** `button-target.h` defines no power
   code and its comment says the PMU owns power. Add `BUTTON_POWER` at
   `0x00008000` — verified free: our keys use bits `0x00000001` through
   `0x00004000`, and the modifiers live far above them (`BUTTON_REL` is
   `0x02000000`, `BUTTON_REPEAT` is `0x04000000`, `firmware/export/button.h:96`).
   Do **not** add it to `BUTTON_MAIN`: the panic path and the raw-press helper
   should keep seeing only GPIO keys.
2. **Read the short-press latch.** `button-xtensa.c` already reads REG `0x46`
   for the long-press bit (bit 0 / IRQ23). The short press is **bit 1 of the
   same register**, so this costs no extra I2C traffic — one read, two bits.
   The IRQ *enable* side needs attention: `axp173_init()` currently enables only
   IRQ23, so the IRQ pin stays high for a short press and the cheap pin gate
   would skip the read. Either enable the short-press IRQ bit too, or rely on
   the existing "every twentieth pass ignores the pin" fallback — the latch
   survives release, so a missed short press is still seen within 2 s. Enabling
   the bit is cleaner but makes the pin low more often, which is fine because
   the pin is only a gate.
3. **Debounce and distinguish from shutdown.** Long press already means
   `sys_poweroff()`. A short press must not fire on the way to a long press:
   emit it on release only, after the long-press threshold check.
4. **Implement the toggle.** Call the backlight API from target code. The usual
   trick is `backlight_set_timeout(1)` to force it off promptly, then restore
   `global_settings.backlight_timeout` on the next key. `backlight_set_timeout()`
   takes seconds and multiplies by HZ (`firmware/backlight.c:872`), so 0 means
   always on and 1 means off within a second. Waking is already handled: the
   button thread calls `backlight_on()` on key activity
   (`firmware/drivers/button.c:374-397`), and `is_backlight_on()` exists to
   decide which way to toggle. What still needs checking on hardware is whether
   the restore of the user's timeout happens on wake or on the next PEK press —
   the flag has to live in target code either way.

Files: `firmware/target/xtensa/button-target.h`, `button-xtensa.c`,
`drivers/axp173.c` (IRQ enable), maybe `power-xtensa.c`.
Doc: add `\ActionStdShowScreen` (or whatever we name it) with the short-PEK
gesture, and mention it in the Quick Start controls section.

**Deliberately reserved:** short-PEK goes to the screen toggle and nothing
else. It is also the only remaining single-input capacity, so spending it here
closes off other ideas — noted so later decisions are made with that in mind.

### S3 — Bookmarks via B-modifier combos

Currently deletable, not creatable. Proposed, following the ondio precedent
(`POWER+DOWN` / `POWER+SELECT`):

- WPS: `BUTTON_B|BUTTON_UP` (prereq B) → `ACTION_WPS_CREATE_BOOKMARK`
- WPS: `BUTTON_B|BUTTON_DOWN` (prereq B) → `ACTION_WPS_LIST_BOOKMARKS`
- Std/tree: same pair, so bookmarks can be listed from the file browser too.

Also add the skin regions (`%T(...,createbookmark)`, `%T(...,listbookmarks)`)
to our theme so the gesture is visible on screen, not just memorised.

Files: `apps/keymaps/keymap-meowkits3.c`, theme file.
Doc: replace `ActionBmView`/`ActionBmJump` "not available", document create and
list.

### S4 — A-B repeat via A-modifier combos

`ab_repeat_buttons` is enabled but unreachable. Following the c200/gigabeat
precedent (`BUTTON_A|BUTTON_LEFT` with prereq `BUTTON_A`):

- WPS: `BUTTON_A|BUTTON_LEFT` (prereq A) → `ACTION_WPS_ABSETA_PREVDIR`
- WPS: `BUTTON_A|BUTTON_RIGHT` (prereq A) → `ACTION_WPS_ABSETB_NEXTDIR`
- WPS: `BUTTON_A|BUTTON_B` (prereq A) → `ACTION_WPS_ABRESET`

Conflict check: A's solo tap and hold are play/pause and stop, and A+hold is
not a combo the dispatcher would confuse with A-then-Left, because the combo
fires while A is still held and the prereq is A. This is exactly how the as3525
targets do it. Verify on hardware that a seek gesture (`Left|REPEAT`, no A)
still reaches seek and not A-B.

Doc: replace the five `Ab*` "not available" entries — that also fixes the
"not available or Short Right + Long Right" line on manual p.35.

### S5 — Key lock

Two options, decide once S2 lands:

- Combo: `BUTTON_A|BUTTON_B` tap in std context → `ACTION_STD_KEYLOCK`
  (precedents: `BUTTON_HOME|BUTTON_SELECT` in `keymap-clip.c:91`,
  `BUTTON_USER|BUTTON_POWER` in `keymap-3ds.c:56`). Note the collision with
  S4's reset in WPS — different contexts, so acceptable, but check the ordering.
- Skin region: `%T(...,lock)` → `ACTION_TOUCH_SOFTLOCK`, visible and tappable.

Doing both is reasonable: combo when the keys are handy, region when the screen
is. Key lock also matters for pocket operation on a touch panel, since a locked
panel is the difference between accidental track skips and nothing.

### S6 — Shuffle and repeat mode: theme, not keys

No upstream key action exists. Implement as `%T` regions in our theme:

```
%T(x,y,w,h,shuffle)   %T(x,y,w,h,repmode)   %T(x,y,w,h,mute)
```

This is also the fix for gap #2 and it improves third-party themes' usability on
our panel. It needs a theme file of our own (cabbiev2 variant) rather than
`keymap-*.c` edits. Keep cabbiev2 in the core zip as the default set; the
variant is theme content.

### S7 — USB HID keypad: add the stick, then verify on hardware

Better news than expected. The whole path exists and is compiled:

- `firmware/SOURCES:1033` builds `usbstack/usb_hid.c`; `firmware/usb.c:251`
  enables `USB_DRIVER_HID`; `apps/settings_list.c:2470` offers the
  multimedia / presentation / browser modes (no mouse, we lack
  `HAVE_USB_HID_MOUSE`).
- `apps/usb_keymaps.c` maps `ACTION_USB_HID_*` to consumer and keyboard usages.
- `apps/gui/usb_screen.c` draws the current mode and switches on
  `ACTION_USB_HID_MODE_SWITCH_NEXT/PREV`.
- `apps/keymaps/keymap-touchscreen.c:293-330` already maps all three modes onto
  the 3x3 grid: mode switch on Top-Left tap/hold, multimedia on the four
  edges plus Centre for mute, presentation on Top-Right/MidLeft/MidRight/
  Bottom-Right, browser likewise.

So HID is usable by touch today. What is missing is **the stick**, and
verification. Proposed stick layer per mode (`keymap-meowkits3.c`), mirroring
`button_context_usb_hid_mode_*`:

- `CONTEXT_USB_HID` base: B tap = next mode, B hold = previous mode.
- Multimedia: Up/Down = volume up/down, A tap = play/pause, A hold = stop,
  Left/Right = track prev/next, B tap = mute.
- Presentation: Left/Right = slide prev/next, Up = slideshow start, Down =
  leave, A = black screen, B = white, holds for first/last slide.
- Browser: Up/Down = scroll, Left/Right = tab prev/next, A = zoom in,
  B = zoom out, holds for full-screen and history back/forward.

Then on-device: confirm the composite descriptor enumerates the HID interface
alongside MSC and CDC, that mode switching repaints, and that each mode's usage
reaches the host. The endpoint/descriptor question is the part I have not
verified — `firmware/usb.c` does not reference `USB_INTERFACE_HID` in a grep,
so the descriptor assembly lives elsewhere (`usb_architecture/` or the
designware driver) and needs reading before any keymap work is trusted.

Files: `apps/keymaps/keymap-meowkits3.c`, then driver verification.
Doc: document the keypad gestures per mode.

### S8 — Recording screen entry from anywhere

`ACTION_WPS_REC` exists (touch Top-Left hold). For the stick: `B|UP` is taken by
S3's bookmark create, so use `BUTTON_A|BUTTON_DOWN` (prereq A) →
`ACTION_STD_REC`, or leave recording as a touch/WPS-only gesture and rely on the
settings menu. Decide alongside S3 rather than pre-assigning; the modifier space
is small and one gesture should not do two things.

### S9 — Settings reset

Minor. `ACTION_SETTINGS_RESET` has no upstream precedent in any keymap. Cheapest
correct answer is a skin region (`%T(...,setting_set)`), or `Long B` in the
settings context, which is currently free there. Low priority.

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

---

## Order of work

1. **S1** keyboard stick mapping — pure `keymap-meowkits3.c`, no core edit,
   immediate usability win; then the hint seam as a separate commit.
2. **S4** A-B combos — closes the most visible advertised-but-unusable gap and
   fixes manual p.35.
3. **S3** bookmarks — same modifier pattern, same file.
4. **S7** USB HID stick layer, preceded by reading the descriptor assembly.
5. **S2** short PEK + screen toggle — the one spike that touches the PMU and the
   button thread, so do it alone and measure the poll cost.
6. **S6** theme touch regions (shuffle, repmode, lock, bookmarks).
7. **S5** key lock, **S8** recording, **S9** settings reset.
8. **S10** plugin audit as its own multi-part effort.
9. **S11** tooling and rbutil stubs.

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
