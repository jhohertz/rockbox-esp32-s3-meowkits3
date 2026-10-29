# CDC terminal UI for the ESP32-S3 port — speculative plan

**Status: exploratory. No code, no ABI change, nothing committed to a build.** This is
an idea-space document: what a host-side terminal interface over the USB CDC class
could be, what the tree already gives us, and where it breaks. Related:
`USB-AUDIO-RESEARCH.md` (the audio endpoints on the same controller),
`UX-PLAN.md` (on-device UX), `PENDING.md` §4 (why remote flashing is closed).

---

## 0. The idea

A host attached over USB can *operate* the player: see what the screen is doing, and
drive menus, settings, playback and the debug surfaces from a terminal. Two shapes,
same protocol:

- **TUI** — a full-screen terminal UI that mirrors the on-screen function: menu lists,
  settings rows with values, now-playing state, debug rows. Terminal keys map to
  Rockbox buttons.
- **CLI** — line-oriented, prompt-as-context (`rb:settings/display> `), commands
  instead of arrow keys, scriptable when stdout isn't a tty.

And one canonical gesture in both: **Ctrl-L re-syncs** — the device sends a complete
state snapshot, the host repaints, and any drift (a plugin ran, the cable bounced, the
terminal was resized, someone pressed the physical buttons) is erased.

Non-goals, stated up front so they don't creep in: not a framebuffer mirror in real
time, not a file-transfer channel (that is MSC), not a firmware-update channel, not a
replacement for the on-device UI.

---

## 1. Ground facts, measured from this tree

| Fact | Where | Why it matters |
|---|---|---|
| CDC TX is one exported function, `usb_serial_send(data,len)` | `firmware/usbstack/usb_serial.h:25` | any firmware code can already write to it |
| 512-byte circular send ring, 32-byte per-transfer chunks, dropped when not enumerated | `usb_serial.c:190-192`, `:367` (`if (!active) return;`), `:353-359` | the bandwidth and "logs vanish before enumeration" behaviour, both |
| RX endpoint allocated and primed, then discarded | `usb_serial.c:210`, `:325`, `:412-417` — `logf("serial: %s", receive_buffer); /* TODO: Do something with it ? */` | a consumer must be added; nothing else in the tree has one |
| RX ignores the transfer length (`(void)length` at `:409`), no NUL guarantee, `RECV_BUFFER_SIZE` 32 vs a 512-byte buffer | `usb_serial.c:192/196/409` | four defects to fix before any input channel is trustworthy |
| `SET_CONTROL_LINE_STATE` is received and explicitly ignored | `usb_serial.c:295-303` ("simply ignored here") | a command channel is better as vendor control requests than as modem lines |
| logf → CDC coupling is compile-time `#ifdef USB_ENABLE_SERIAL` inside `_logf` | `firmware/logf.c:246-256` | the eager push is one gate away, not an architectural problem |
| The log ring is exported: `logfbuffer[16384]`, `logfindex`, `logfwrap`, `logfenabled`, and `logf.c` already walks it to draw it on screen | `firmware/export/logf.h:31-36`, `firmware/logf.c:63-66`, `:95-168` | "capture and fetch on request" needs no new storage |
| `button_queue_post(long id, intptr_t data)` is the input seam, and is used by non-hardware sources | `firmware/export/button.h:66`, `firmware/target/hosted/button-devinput.c:306`, `firmware/target/arm/tms320dm320/sansa-connect/avr-sansaconnect.c:197-206` | host keypresses become ordinary button events; the whole UI then works untouched |
| The same entry point is in the plugin API | `apps/plugin.h:302` | a plugin could inject keys — relevant to §7 |
| CDC asks for neither exclusive storage nor a CPU boost | `usb_serial.c:437-438` | CDC does not force the USB UI park |
| MSC does, and the app parks on the USB screen while connected | `usb_core.c:937`, `apps/main.c:668-674` and `:729-730`, `firmware/usb.c:64-66` (`USB_FULL_INIT`), `:775-785` | the mode constraint, §2 |
| Charge-only mode breaks out of the USB loop, so the UI keeps running with the cable attached | `apps/main.c:679-681` (`usb_powered_only()`), `usb.c:232` (audio classes in charge-only mode) | the habitat for remote control already ships |
| Menu engine is data-driven and walkable | `apps/menu.h:75-115` (`struct menu_item_ex`, `MT_*`, `menu_get_name_and_icon`, `list_get_name`), `do_menu()` at `:121`/`:136` | a second renderer can traverse the same structures |
| Settings are addressable by cfg name with string conversion in both directions | `apps/settings.h:301-305,333` (`find_setting_by_cfgname`, `cfg_to_string`, `cfg_string_to_int`, `setting_get_cfgvals`) | the CLI's `set <name> <value>` is already implementable, language-independent |
| UI labels are lang ids with a voice backend | `settings_list.c:252-303` (`get_talk_id`, formatter), `apps/menus/main_menu.c:235-310` (`talk_id(LANG_*)`) | the voice UI is the existing precedent for mapping screen function to a non-visual medium |
| Playback/system state is exposed | `firmware/export/audio.h:57` (`audio_status()`), `apps/settings.h:952` (`global_status`) | now-playing, recording and track position sources |
| Our debug rows are already a "fetch a named value" namespace | `firmware/target/xtensa/debug-xtensa.c:120-215` (`INFO_CPU`…`INFO_CC`, `INFO_RXSTAT`, trail rows) | free read side for diagnostics |
| LCD flush hooks exist, and our driver **drops draws when the panel is off** | `firmware/export/lcd.h:220/256`, `firmware/target/xtensa/lcd-xtensa.c:118-147`, `:228-244` | any framebuffer-derived mirror would stop when the screen sleeps — a hook must read the buffer, not the flush path |
| Endpoint budget on the DWC core | `firmware/drivers/usb-designware.c:160` (`USB_NUM_ENDPOINTS`), FS device | room for a dedicated endpoint if one is wanted |

---

## 2. The mode constraint, in one paragraph

While USB is connected Rockbox normally replaces the UI with `gui_usb_screen_run()` and
waits for disconnect. That is driven by storage exclusivity, not by USB: mass storage
asks for it, the serial class does not. So remote operation lives in **charge-only (or
charge-only + audio)** mode, where the cable is attached, CDC is enumerated, playback
keeps running, and the menus stay operable — which is also our USB DAC mode. A session
ends when the user (or a protocol command) switches to mass storage, because at that
point storage is handed over and the UI parks. The reverse ordering is the useful one:
operate first, transfer files last.

Open question worth measuring before anything else: whether `usb_mode` can be switched
*by* the protocol mid-session without deadlocking on the handover.

---

## 3. Transport design

Three decisions to make, each with a cheap default:

**Where commands arrive.** Options:

- *Bulk OUT + a hook* (`usb_serial_target_rx(buf,len)` weak symbol, or a registered
  callback in `usb_serial.c`). Natural for typed input in a terminal, since the host
  sends a stream. Needs the four RX fixes, plus a queue so the USB thread doesn't
  format replies.
- *Vendor control requests* (`bRequest` 0xC0-0xFF on the CDC control interface, handled
  in `usb_serial_control_request()`). Length-delimited, cannot be re-delivered like bulk
  data, already routed to the driver. Ideal for the handshake and for
  fire-and-forget commands (`ping`, `snapshot`, `log-fetch`, `mode`), wrong for a typed
  stream.
- **Both**: control requests for command/query, bulk OUT for keystrokes. That is the
  shape I'd pick, and it means the dangerous thing (an old keystroke honoured as a new
  command — the exact failure that killed the flash handover, `usb-xtensa.c:617-623`)
  only ever applies to the input channel, where a nonce makes it harmless.

**Framing.** Text lines (UTF-8, `\n`-terminated) or binary frames (magic + length +
crc). Text is debuggable with `cat /dev/ttyACM0`, survives a hostile terminal, and is
what a TUI wants anyway; binary is smaller and unambiguous. Proposal: text lines with a
tiny verb namespace, and a `\0`-free rule so `logf`'s existing string handling stays
safe. A session nonce (random at boot) plus a monotonically increasing sequence number
per command kills stale-command replay.

**Bandwidth.** Today: 32 bytes per transfer from a 512-byte ring. Rough numbers,
*to be measured*, not assumed:

| Payload | Size | At today's caps | With TRANSIT 512 / BUFFER 4096 |
|---|---|---|---|
| One debug row or status line | ~60 B | ~instant | instant |
| A settings screen (24 rows) | ~1.5 KB | ~0.5-1 s | ~0.1-0.2 s |
| Full text-grid repaint (80×24 + ANSI) | ~2-3 KB | ~1-2 s | ~0.2-0.4 s |
| LCD screenshot, RGB565 raw | 153,600 B | ~1-2 min | ~10-30 s |
| LCD screenshot, 8-colour RLE | ~5-15 KB | ~5-15 s | ~1-3 s |

Conclusion the table forces: text is comfortable, bitmaps are on-demand-only, and live
mirroring of pixels is out.

**Endpoint choice.** Reuse the CDC bulk IN (contends with logf if streaming is left on),
or take the optional interrupt IN already allocated for the ACM notification descriptor
(`usb_serial.c:211`), or add a second interface. Reuse is simplest; the interrupt IN is
there if framing needs to be independent of log traffic.

---

## 4. Ctrl-L, precisely

Ctrl-L is the recovery gesture, and it is only worth having if it is *complete*:

1. Host sends `snapshot` (vendor request).
2. Device replies with one structured block: context path, screen kind, list contents
   (bounded window), selection, values with their cfg names, now-playing state, battery,
   storage, USB mode, terminal hints (rows/cols it was told), session nonce, build id.
3. Host repaints from scratch, clears scrollback in TUI mode, and re-issues any resize.

That single primitive covers every drift case: the user pressed physical buttons, a
plugin ran and exited, the cable dropped and re-enumerated, the terminal resized, the
host crashed and restarted, or the protocol version changed. It also makes the device
state *authoritative* — which is what stops a TUI from becoming a lie.

Design consequence: the device must be able to answer `snapshot` cheaply and from one
place. That is the real engineering work of the whole idea (§6), and everything else is
incidental.

---

## 5. What "mirror the on-screen function" can mean

Three fidelity levels, and it is worth being explicit about the cost of each:

- **L1 semantic mirror (recommended).** Device sends *meaning*: which menu, which
  entries, which value. Host renders with its own widgets. Cheap, robust, terminal
  native, and it can be *better* than the screen (real text entry, scrolling, search,
  copy of a value, diffs between settings profiles).
- **L2 text-grid mirror.** Device renders into an internal 80×24 character grid (reusing
  the same lang strings and the same list code the LCD uses) and sends the grid +
  attributes. Faithful in layout, terminal-portable, ~2-3 KB per repaint. Nice middle
  ground; needs a "font metrics → cell" translation and would ignore skins.
- **L3 pixel mirror.** Framebuffer dump, quantised and RLE'd. On-demand screenshot only
  (§3), never live. Note the panel-off gotcha from §1: hook the buffer, not
  `lcd_update_rect()`, or the mirror dies when the backlight times out.

A TUI built on L1 with L2 as a fallback ("show me the screen as text") is the shape I
would aim at. L3 stays a command (`shot`), not a mode.

---

## 6. Mapping screen function to terminal elements — the "theme"

The mapping is a lookup table over things the tree already names:

| Rockbox surface | Source | Terminal element |
|---|---|---|
| Menu lists | `apps/menu.h` `struct menu_item_ex`, `list_get_name` callbacks, `menu_get_name_and_icon` | selectable list, one line per item, icons as `[+]`/`[>]` |
| Settings | `struct settings_list` + `find_setting_by_cfgname` + `cfg_to_string`/`cfg_string_to_int` + `setting_get_cfgvals` | rows `name = value`, with min/max/step → arrow keys and `[0-9]` typing; cfg name in the CLI |
| Labels | lang ids (`Lang[]`/`lang_gets()` from the generated `apps/lang/lang.h`), same ids the voice UI uses (`get_talk_id`) | text; a `lang` command could switch the terminal's language independently |
| Now playing | `audio_status()`, `global_status`, playlist position | header block: track, artist, elapsed/total, repeat/shuffle, volume |
| Recorder | `audio_status() & AUDIO_STATUS_RECORD`, `INFO_RXSTAT` | status row + level meter from the RX counters |
| Debug state | `INFO_*` rows in `debug-xtensa.c` | a `stat <name>` verb, and a live tail pane for CPU/MMU/CC/RXSTAT |
| Battery | `battery_level()`, `battery_voltage()`, `battery_current()`, `axp173_coulomb_*` | gauge row; the CC columns land here for free |
| Storage | `storage_status()`, free space, card state | rows, plus `mount` warnings when MSC is active |
| Text entry | `on_screen_keyboard` equivalent | **terminal is strictly better**: a `/`-prefixed line replaces the on-screen keyboard for paths, filenames and search |
| Touch | our FT6336 axes | either coordinate injection (needs a target seam) or semantic equivalents (list selection *is* touch's meaning) |
| Skins, bitmaps, plugin graphics | skin engine, `mpegplayer`, plugins | **do not map.** A theme shows "plugin running, keys forwarded" and stops |

The theme file lives host-side and is declarative: which contexts exist, how each
renders, which keys are legal per context. That is what keeps the device side small: the
device sends facts, the theme decides presentation.

---

## 7. Plugins, and why Ctrl-L is canonical *there*

Today a plugin owns both the LCD and its button loop, and plugins cooperate with USB by
acknowledging `SYS_USB_CONNECTED` through the default event handler
(`apps/plugins/battery_bench.c:556-558`, `alarmclock.c:195`). Consequences:

- While a plugin runs, L1 mirroring is meaningless — the device cannot describe a
  Tetris board. The TUI must **degrade to a console**: keys forwarded verbatim as
  `button_queue_post`, no state claims, a status line saying "plugin: <name>, raw keys".
- On plugin exit, Ctrl-L is exactly the fix. The gesture is canonical precisely because
  the plugin case is the common way the host's model goes stale.
- Two plugin-specific ideas worth writing down even though they're speculative:
  - a plugin could *host* the interface itself (it can already post buttons via
    `apps/plugin.h:302`) — but it cannot reach CDC, so this needs the plugin API to gain
    a serial entry, which means `PLUGIN_API_VERSION` 285 → 286 and every plugin on every
    card rebuilt. Cheap for us, expensive for third parties, and upstream would resist.
  - conversely, a *firmware-side* terminal makes every existing plugin remotely
    controllable with no plugin changes at all, which is the better trade.

---

## 8. CLI sketch (prompt-as-context, iOS-ish in spirit)

The prompt *is* the UI: it says where you are, and the commands are always the same
small set, so navigation is just "go somewhere".

```
rb> stat                       # build, uptime, battery, storage, mode, audio state
rb> menu                       # top-level list, numbered
rb> menu 3                     # descend; prompt becomes rb:menu/recorder>
rb> ls [path]                  # directory listing, sizes, playable marker
rb> play <path|n>              # queue and start
rb> now                        # track/artist/elapsed/total + volume + repeat
rb> keys                       # what the current context accepts, as key names
rb> set                        # all non-default settings as cfgname = value
rb> set volume 62              # via find_setting_by_cfgname + cfg_string_to_int
rb> get theme.cfgname
rb> save                       # settings_save()
rb> diff profile.cfg           # compare current against a cfg file
rb> stat mmu iram cc rxstat     # INFO_* rows, one verb, names as arguments
rb> log [since <seq>]          # drain the logf ring from a snapshot of logfindex
rb> log off | log on           # the stream gate (§9)
rb> shot [--rle]               # screenshot, on demand, slow
rb> mode charge|storage|audio  # usb_mode; switching to storage ends this session
rb> watch cc 5                 # periodic rows, like tail -f
rb> keys ^l                    # explicit re-sync, same as Ctrl-L in TUI mode
rb> exit
```

- Non-tty stdout ⇒ CLI mode, no ANSI, one record per line, `key=value` pairs. That is
  the scripting surface, and it is what makes a regression harness possible
  (`rb> set volume 62` then `rb> get volume` in a shell loop).
- Tab completion from the menu and cfg-name tables; `?` prints the legal keys for the
  current context (this is `keys`, and it is the answer to "what can I do here").
- Output is always bounded: lists come as a window with an index range, so the protocol
  never needs to stream a 10,000-entry playlist in one frame.

---

## 9. Device-side architecture, if it were ever built

| Piece | Location | Notes |
|---|---|---|
| RX hook + RX fixes (honour `length`, size buffer to `RECV_BUFFER_SIZE`, NUL-terminate before logging, re-arm after dispatch) | `firmware/usbstack/usb_serial.c` | shared file; small, and the defects are real upstream bugs independent of this feature |
| Vendor-request dispatch | `usb_serial_control_request()` + a weak target hook | shared file, additive, target-guarded |
| Log-stream gate | `firmware/logf.c` checks a runtime flag next to `logfenabled`; flag setter next to `usb_set_serial()` (`firmware/usb.c:945-953`) | ~5 lines; makes "CDC is now a protocol" and "CDC is the log" mutually exclusive by choice rather than by `#ifdef` |
| Terminal server | `apps/` (e.g. `apps/remote_ui.c`), guarded by a target define | must be apps-side: menus, settings, lang ids and `global_status` are apps state. Precedent for apps-side USB UI: `apps/gui/usb_screen.c`; precedent for reaching everything from apps: `apps/debug_menu.c` |
| Key injection | `button_queue_post()` from the dispatch path (`firmware/export/button.h:66`) | same call the hosted and coprocessor drivers use |
| Snapshot builder | one function, one lock-free discipline | the crux; every surface in §6 contributes a bounded block |
| Buffer sizes | `TRANSIT_BUFFER_SIZE`, `BUFFER_SIZE` in `usb_serial.c` | bump only after measuring; it costs static RAM, and the arena is already planned (`MEMORY.md`) |

Threading: RX arrives on the USB thread (`usb.c:301` posts transfer completions onto
`usb_queue`). Injecting a key from there is fine. *Building replies* is not — the server
should take a message and format on its own thread, blocking with a timeout like
`debug_menu` does, because this port's threading is deliberately cooperative
(`thread-idf.c:121-127`) and a busy loop that never yields never gets service. The logf
ring has no lock: snapshot `logfindex` before draining, as the on-screen view implicitly
does.

---

## 10. Host side

One binary, three modes, same protocol:

- `rockctl` (default): CLI, prompt-as-context, works piped.
- `rockctl tui`: ncurses full screen — header block (now playing, battery, mode), main
  pane (list or grid), key hints footer, Ctrl-L bound, `:` for a command line.
- `rockctl console`: raw CDC passthrough with the log stream on — i.e. today's
  behaviour, kept, so nothing that works now stops working.

Terminal requirements: VT100/ANSI basics only (`clear`, cursor position, SGR bold/color,
`SMCS/RMCS` optional), 80×24 assumed and resize reported, UTF-8 accepted (Rockbox lang
strings are UTF-8) with an ASCII fallback flag. Terminals that can't do full-screen fall
back to CLI rather than degrading the TUI.

Theme: a small declarative file mapping context → layout, so a new build's menus show up
without a client release. The client's source of truth is always the snapshot, never the
theme.

Extras the protocol should allow cheaply: settings diff/apply, log tail with a level
filter, screenshot, `watch <row> <interval>` for bench measurements, and a
`bench` mode that drives repeated operations and timestamps them (playlist switch,
seek, card scan) — that last one is where this idea earns its keep, because today those
measurements need a screen and a thumb.

---

## 11. Alternatives considered

| Idea | Why not |
|---|---|
| LCD framebuffer mirroring over CDC | bandwidth (§3), and the panel-off drop (`lcd-xtensa.c:228-244`) |
| HID as the control channel | HID is already used for remote-control *out*; making it bidirectional means a new report model and it is less inspectable than a serial stream |
| A plugin hosting the server | cannot reach CDC without a plugin API bump (§7) |
| Bluetooth / Wi-Fi | no classic BT on S3 (`BT-AUDIO.md`), no Wi-Fi path in the port |
| Network (adb-like) | no networking on this target |
| Card-file protocol (write commands to a file, poll it) | works with no transport changes, but latency and storage contention make it a bad UI substrate; good as a fallback for headless scripted tests |
| Doing nothing: debug menu + boot console | what we have today, and adequate for bring-up; the proposal is about *operating* the device, not debugging it |

---

## 12. Risks and open questions

1. **RX re-delivery.** The bulk OUT endpoint has been observed re-delivering packets it
   already consumed. Nonce + sequence numbers, and commands that are idempotent or
   explicitly acked. Anything as dangerous as a reboot must not be a bare byte.
2. **MSC exclusivity.** No remote operation while the card is exported (§2). Ordering
   discipline and a clear "session ends now" response.
3. **Cooperative threading.** Server must yield, must not spin, must not block on
   storage; playback must survive the server being busy.
4. **Snapshot consistency.** Reading UI state while the UI thread is mid-scroll. Needs
   either a bounded lock-free snapshot convention (versioned, best-effort) or a
   request-to-the-UI-thread-and-wait model with a timeout. This is the hardest correctness
   problem in the whole idea.
5. **logf races** and the ring's wrap flag during a drain.
6. **Detach semantics.** `SYS_USB_DISCONNECTED` broadcast must abort the server cleanly
   and leave the device operable.
7. **Bandwidth assumptions are estimates** — everything in §3's table needs measuring
   before it is quoted as a design fact.
8. **Lang strings in a terminal.** Widths, UTF-8, truncation, RTL-irrelevant. Also
   some labels only make sense with icons.
9. **Key collisions.** Injecting physical key codes means the host's keys go through the
   target's keymap and context rules; `keys` must report what's legal, or the host will
   press something meaningless.
10. **Power.** Charge-only mode keeps the panel and amp alive for the mirrored UI's
    benefit; a `screen off` command should exist so remote operation doesn't burn the
    battery we are trying to measure.
11. **Upstream surface.** Three shared files touched (`usb_serial.c`, `logf.c`, `usb.c`)
    plus one apps-side file. The RX defects are independently submittable; the rest is
    port-scoped and would need a `#define` gate and a real argument for inclusion.
12. **This does not solve flashing.** The loader-exit limitation in `PENDING.md` §4 is
    unrelated and unaffected: no protocol running in the app can reset the chip out of
    the ROM loader.

---

## 13. Milestones, if it were ever pursued

| Step | Deliverable | Unlocks | Device effort | Host effort |
|---|---|---|---|---|
| M0 | RX hardened, vendor-request dispatch, log-stream gate, `console` mode | a trustworthy two-way channel; logs become pull-based | small (~60 lines shared, target-guarded) | trivial (`cat`, `ioctl`) |
| M1 | CLI, read-only: `stat`, `menu`, `ls`, `now`, `get`, `log`, Ctrl-L equivalent | remote *inspection*, including bench harnesses | medium (snapshot builder for read-only surfaces) | medium (REPL + table rendering) |
| M2 | Key injection + `set` + navigation | remote *operation* of menus and settings | small, given M1 | small |
| M3 | Full-screen TUI, theme file, L1 mirror + L2 text grid fallback | the reasonable UI the idea started with | medium | large (ncurses is the bulk of the work) |
| M4 | `shot`, `watch`, profiles diff/apply, recording control, plugin raw-key mode | day-to-day usefulness | medium | medium |
| M5 | scripted `bench` mode, CI-ish regression runs | measurements that need no hands | small | small |

Each milestone is independently useful; stopping after M2 leaves something worth having,
which is the property a speculative plan should have.

---

## 14. What would make this not worth doing

- If snapshot consistency (risk 4) turns out to need real locking in the UI thread, the
  change stops being additive and becomes a core-touch. That is the make-or-break.
- If M1's read-only set doesn't cover the things we actually want to measure remotely,
  the CLI is a toy and the TUI is decoration.
- If MSC exclusivity means most sessions want the card exported, the operating window
  shrinks to charge-only, which may be too narrow to justify a client.
- If the log-stream gate plus the existing debug menu already covers bring-up, then the
  only remaining argument is *operation without hands* — which is a real but narrower
  benefit than "a TUI for Rockbox".
