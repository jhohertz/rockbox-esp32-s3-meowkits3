# Pending — MeowKit ESP32-S3


> Internal working note. This file and the others in this directory are the
> port's own records — plans, logs and research threads. The reviewer-facing
> documents are ../README.md and ../TESTING.md, and the user-facing ones are the
> repository README and the manual chapter for this target.
The single status list. `MEMORY.md` is evidence, `TX-IDF-MIGRATION.md` /
`USB-SPRINT.md` / `USB-AUDIO-RESEARCH.md` are investigation records, `DOC-GAPS.md` and
`UX-PLAN.md` are the documentation and input logs, and `MERGE-PLAN.md` is the shape plan
for getting this port upstream (where the port should conform to other ports, and what
to change first). None of them is a to-do list.
Every entry here was checked against the code before being written.

Board access: **hold BOOT, tap RESET** for download mode, then
`make flash ESPPORT=/dev/ttyACM0`. `flash` and `espverify` both depend on
`make esplayer`, so a layout overrun fails the build before the board is touched.

---

## 1. Open

- **A fresh, never-flashed board boots correctly -- nothing in the port depends on
  retained state.** A second MeowKit unit that had never run our firmware was
  flashed with the tag build and reached the screen (it drew the battery warning),
  and a probe build marked each init step into the RTC trail (`xtensa_breadcrumb_rtc`,
  24 slots, survives resets). The trail came back in order:
  `pLcd -> pFon -> pSet -> pLan -> pBl -> pPm`, with `SD card up` after it. So
  LCD, fonts, settings, language, backlight, buttons and power management all run
  on a cold device, and the earlier idea that a working display might be an
  artefact of the PCA9557 keeping CS/reset from previous firmware is disproved for
  this build. Its dark screen was an empty cell: power management measures, and
  shuts down. That unit is now the charge-curve and battery-capacity rig (see the
  capacity item below), and it needs charging before anything else is concluded
  from it.
  Two measurement traps hit while doing this, both already known and worth
  restating: the `.noinit` breadcrumb ring is *not* cleared between boots, so its
  40 slots showed the previous build's `sdWr`/`sdRd` traffic and read as progress
  in this one -- which is exactly why the RTC channel exists; and a host-driven
  reset on a unit whose IO0 strap samples low leaves `RTC_CNTL.FORCE_DOWNLOAD_BOOT`
  set, which a reset does not clear, so the board appeared dead until the cable
  was re-seated and power cycled.

- **Ogg load is slow, and it is not the Ogg codec.** Measured by phase on
  `45549562`/`3c018a80`/`1f595459` (gated counter probes, read through
  `Debug -> View HW info`, since removed from the tree): pressing play to
  `audio_init_codec()` is **2246 ticks = 22.5 s**, that to codec run is 58 ticks,
  and codec run to first PCM is **6 ticks**. Loading `vorbis.codec` itself is
  `read 50 ms, heap 30 ms`, tag parsing is **8 ticks (80 ms)**, and the audio
  thread picks up `Q_AUDIO_PLAY` with **`wait 0`**. So everything is downstream
  of the codec, and the codec, DSP, PCM buffer (`pcm=512KB`), file buffer
  (`file=2017KB`) and arena (`2.8MB` allocatable, mpegplayer still fits) are all
  healthy. The `wait=925` seen in `codec_pcmbuf_insert_callback` is ordinary
  backpressure over a multi-minute session, not a stall.

  What phase A contains is I/O: **42,376 `storage_read_sectors()` calls /
  45,861 sectors before tag parsing even starts**, essentially all of them one
  sector each (`1:76557` against `64+:70` in a longer capture), and
  **38,532 of them above sector 2,000,000**. The dominant caller resolves to
  `.text.transfer$constprop$0` in `fat.o` -- fat.c's data mover, reached one
  level below `storage_read_sectors`. Reads are deterministic (the same
  `42376/45861` in three separate runs, including one with two minutes of
  playback afterwards), and `wr=0` means playback writes nothing, so this is not
  FAT corruption by us.

  **The hard fact: `vol 30445MB` with `farmax 52400MB`.** This card is ~30 GB
  (62.4M sectors) and reads were requested at sector ~107,000,000 -- past the end
  of the volume. The observed offender values (`107376004`, `107374272`,
  `0x666666`-shaped) look like ASCII `fff` bytes interpreted as a cluster
  number, and with `clus 32768B` one bogus cluster index maps ~32 GB out. Vorbis
  walks cluster chains from arbitrary offsets because it seeks to build its seek
  table; FLAC and mp3 read forward from a chain start, which is why only Ogg
  shows it. Card-independent: reproduced on a second card.

  **Ogg is not special: FLAC does the same thing.** On the `esp32-s3-idf-20261029`
  tag with a counters-only probe (`rd` = SD reads since `audio_start_playback()`,
  `re` = reads of a sector seen in the last 256, `span`/`bkt` = sector range and
  count of distinct 1 GB regions touched):

  | | rd | re | span | bkt |
  |---|---|---|---|---|
  | Ogg open | 80,453 | 1,260 | 0-36,203 MB | 16 |
  | FLAC open | 83,944 | 1,560 | same | 18 |
  | FLAC, +1 min later | 84,913 | 1,756 | -- | -- |

  So ~41 MB is read per playback start, scattered across 36 GB of a 64 GB card,
  and **only ~1,000 reads happen during a further minute of playback** -- the
  file is buffered once and then the bus is quiet. Chain walking is measured and
  ruled out: `get_next_cluster32()` is called 1,378 times per open, which is
  under a second. Short-distance re-reads are ruled out the same way (100 of
  78,724).

  What that points at is a **background card-wide scan competing with playback**,
  not the decoder: a scan of every directory on the card reads tens of megabytes
  of directory and FAT sectors, and at ~520 us per one-sector command that is tens
  of seconds of bus time. Ogg is the victim because Vorbis must parse tags and
  open the stream before it can produce sound, so it queues behind that traffic;
  mp3 and flac put samples on the bus first and look unaffected. The database on
  this card was also failing to populate, which is the same suspect.

  Two claims I made and then disproved, recorded so they are not repeated:

  - *Reads past the end of the volume.* Wrong. `fat_size()` returns **KB**
    (`factor = bytes_per_cluster / 1024`, fat.c:3038) and my debug row divided it
    by 2048 and labelled the result MB, so a 64 GB card printed as `vol 30445MB`.
    `farmax 52400MB` was inside the volume all along.
  - *`dircache_wait()` deadlocks when dircache is disabled.* Wrong. It calls
    `thread_wait(dircache_runinfo.thread_id)`, and with no thread that id is 0,
    but thread ids are seeded `THREAD_ID_INIT(slot) = 256 + slot`
    (thread-common.c:90), so thread 0's id is 256, the `thread->id == thread_id`
    guard fails, and `thread_wait(0)` returns without blocking. The apparent
    freeze at "Building database, 0 found" was `get_progress()` returning 0 while
    `dcinfo.status == DIRCACHE_IDLE` and `entry_count == 0` (tagcache.c:5402-5418)
    -- a scan running with no progress to report, not a hang. Enabling dircache
    made the database populate, which is consistent with that.

  `MAX_VIRT_SECTOR_SIZE` is **not** a lever here: `fat_mount()` takes the logical
  sector size from `disk_get_log_sector_size()` (fat.c:2955), so the trick only
  helps devices that present large sectors themselves (ipod6g, ipodnano3g). An SD
  card presents 512 and the multiplier stays 1.

  Hypotheses tested and **retracted**: disk-cache thrash (`rr:7` out of 38,912
  re-reads), whole-cache maintenance added by `59a7794acc` (`full=0`, none ran),
  CPU frequency and MSPI retime (pinned at 240 MHz, unchanged), dircache boot
  scan (`dc idle ent=0`, cache disabled), and my own `logf()` probes (clean HEAD
  reproduced both symptoms, so they were innocent -- though `LOGF_ENABLE` in
  `apps/codec_thread.c` does turn `PLAYBACK_LOGQUEUES` into per-event logging,
  which is worth avoiding in future probes). Also note `DC_CACHE_BUFSIZE` is one
  sector for *every* target (`fs_defines.h:127`), so single-sector misses are
  normal Rockbox behaviour; it is our per-command cost (~520 us) that turns a
  high request count into tens of seconds.

  **The regression test was run and came back negative.** `esp32-s3-idf-20261029`
  was built in a worktree (`/home/jhohertz/co/rb-bisect`), the card populated from
  its own zip, and flashed: Ogg is just as slow there. Note the ABI coupling that
  makes this expensive -- that tag is `target_id=126` / `PLUGIN_API_VERSION 285`
  while HEAD is **134** / **286**, and `apps/codecs.c` and `apps/plugin.c` reject
  images whose embedded values differ, so a build on either side of the rebase
  needs its own card payload, not just its own image. `CODEC_API_VERSION` is 50 on
  both and `CODEC_SIZE` / `PLUGIN_BUFFER_SIZE` are unchanged.

  **RESOLVED -- it was the directory cache size, not a regression.** On a clean
  card with nothing else changed: 256 KB cache gives ~80,000 single-sector reads
  per open (~40 s of this 1-bit bus), 384 KB gives 4,169. The cache cannot hold a
  path's worth of directory entries on a card with thousands of directories, so
  every open re-walks them through `fat.c`'s `transfer()`.

  Raising the cap was blocked by mpegplayer, and that block is gone too: the
  2 MiB `LIBMPEG2_ALLOC_SIZE` was the whole constraint, and libmpeg2's pool demand
  scales with the *coded* frame, which here is 320x240 (~0.65 MB of demand). The
  reservation is now overridden to 1 MiB for this target -- legitimate only
  because libmpeg2 gained the allocation guard it never had (`header.c` was
  assigning `mpeg2_malloc()` results into `yuv_buf[][]` and `fbuf->buf[]` from a
  `void` function with no NULL check, so an exhausted pool was a write through
  NULL; it now sets `STATE_INVALID` and `decode.c` returns it before any slice
  decode). Verified on hardware on `b18c39eb`: Ogg opens quickly, mpegplayer plays,
  Doom plays, recording works. This is the best-performing build of the port so
  far by user assessment.

  **Card payload:** the board runs HEAD (`b18c39eb`, `target_id` 134, API 286) with
  HEAD's zip, so the earlier tag-payload caveat no longer applies. A database
  rebuild is needed after any card re-population.

- **Fetch-class panic, unresolved, and now without a repro.** Two captures on
  the pre-repair card reported `IllegalInstruction` whose captured bytes at the
  faulting address matched the linked image exactly, at two different addresses
  (`switch_thread+0x52`, just past `waiti 0`, and `vbus_sample+0xc`). A legal
  instruction that faults is a fetch that did not return the image's bytes, and
  an address that moves is a window rather than a bug in one function. The only
  window on this build is the MSPI speed-mode switch: `esp_pm_configure()` with a
  changed max/min reaches `pm_impl_update_freq()`, which calls
  `mspi_timing_change_speed_mode_cache_safe()` and freezes the external cache
  while retiming SPI0/SPI1 -- and it stalls the secondary core only under
  `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`, which esp32s3 does not define, while
  everything executable here (Rockbox's whole `.text`, and IDF's tick, timer and
  idle tasks on the other core) fetches from that same XIP window. Both captures
  predate the card repair, so they may have been downstream of the same metadata
  damage. Neither the light-sleep experiment nor the frequency-pinning experiment
  reproduced it after the repair. The `clk` breadcrumb stamp is permanent, so a
  future occurrence says whether a frequency switch was the last thing that ran;
  the candidate fix is IDF stalling the secondary core on parts that execute from
  PSRAM, not deleting the 80/160/240 ladder.

- **MSC is absent from the USB composition on this build.** With the cable in,
  the device exposes three interfaces -- CDC (`02`), audio (`0a`) and HID (`03`)
  -- and no mass storage (`08`), so the host cannot see the card at all and
  `lsblk` shows no disk. `usb.c:249` enables `USB_DRIVER_MASS_STORAGE` on every
  insertion under `USB_ENABLE_STORAGE`, so this is either the card failing to
  mount (it was damaged at the time) or a composition regression. Re-check with
  a healthy card before assuming the former; MSC writes are the only way the
  card's contents can be verified from the host, and AGENTS treats them as the
  reference test for cache coherence.

- **mpegplayer's buffering thread can fail to be created after a long chain of
  plugin and codec switches, until a reboot.** Observed on the repaired card with
  the restored PM configuration; everything else in that session was clean.
  `apps/plugins/mpegplayer/disk_buf.c:966` passes `disk_buf_stack`, a static array
  in the plugin's own bss, to `rb->create_thread`, so the failure is one of two
  claims: the kernel's fixed thread table has no free slot, meaning an earlier
  plugin run left a thread claimed rather than exiting it, or the plugin buffer
  itself has not been reclaimed. Distinguish them by the return value -- no slot
  is `-1`, a missing stack is a NULL pointer from the arena path -- and by the
  arena row on `View HW info` before and after N plugin runs. Not urgent: it is
  recoverable by reboot and does not corrupt anything, but a thread slot that
  survives a plugin unload is a slow leak in a table that cannot grow.

  **Retested 2026-10-31 on `e4dc52a5` and not reproduced, in the ordering that
  makes it hardest:** doom launched first (the largest single arena request the
  port can make, and the one whose shortfall used to panic), then a long chain of
  codec switches, then mpegplayer, which started without a reboot and played to
  the end of the sequence. The claim is therefore "seen once, not reproducible in
  the ordering that should trigger it", which is weaker than the original note
  implied. If it returns, the distinguishing measurement above is still the way
  to settle which of the two claims it is.

- **Power-key shutdown during a USB host session is refused, and the refusal is
  correct but incomplete.** `usb_host_session()` gates it because in MSC mode the
  host owns the card. What is missing is the way to *end* the session cleanly and
  then shut down, which the core already knows how to do on cable removal: the
  USB thread's `USB_EXTRACTED` case runs `usb_stack_enable(false)`
  (`usb_core_exit()` -> soft disconnect -> pads back), sets `usb_state`, sends
  `SYS_EVENT_USB_EXTRACTED`, and `usb_set_host_present(false)` reaches
  `usb_release_exclusive_storage()` -> `usb_slave_mode(false)` -> `disk_mount_all()`
  -> `queue_broadcast(SYS_USB_DISCONNECTED)`, which is what lets
  `handle_usb_events()` return. Two hazards to handle deliberately: `usb_slave_mode(false)`
  calls `panicf("mount: %d")` when `disk_mount_all()` returns <= 0, so a polite
  shutdown would panic on a quirk; and releasing MSC while the host still has the
  drive mounted needs the LUN eject and class notify first, as the reference
  firmware's `usb_msc_disable()` does. The seam should be a core entry point in
  that exact order, gated so no existing port changes behaviour.

- **Battery: charge leg and a discharge run to shutdown.** Capacity measurement
  still gates the next release tag.


- **Narrowing the SDK include path — done, not pending.** Applied as `58a5344523` `IDF_CFLAGS`
  is 64 `-I` directories plus 33 defines and codegen flags, and today the whole
  firmware domain gets it — 766 objects reach the newlib shim through
  `rbendian.h`'s `<endian.h>`, but only **13** reach an IDF component header, and
  those are exactly the target files that name one.
  The narrowed shape gives every firmware-domain object 2 directories
  (`components/newlib/platform_include` — the only `endian.h` in sight, the
  toolchain sysroot has none — and the project's `build/config`, which that shim
  includes) and the full 64 only to the 13, compiled with IDF's path leading.
  Result: clean build, identical warning profile (35 `-Wchar-subscripts` and 28
  `-Wcomment` come from the platform libc and upstream code in both trees; they
  are not new), 1259 objects with **exactly two changed sections**
  (`debug-esp32s3.o`: `.text.core_prid` −8, `.text.pm_cycle_tick` +57, because
  IDF's `esp_cpu.h`/`esp_timer.h` inlines now resolve ahead of Rockbox's), image
  1,306,912 B (−944), same segment layout, layout guard passes, zip entry list
  identical. Needs a board check of `dbg_hw_info` — the two changed functions are
  exactly its core-id and cycle-tick readers — before it is committed.
  Two prerequisites it exposes: the dep-scan hook has to be `DEP_EXCLUDE` applied
  to both `$(SRC)` and `$(OTHER_SRC)` (firmware sources live in `OTHER_SRC`, so
  `DEP_SRCS` alone never excluded them), and `-Wno-undef` retargets from the
  directory to the object list.
  A second variant also measured (`/tmp/rb-v2`): dropping IDF's codegen flags
  globally so Rockbox's own `-Os` and warning policy apply to Rockbox's code.
  Image 1,256,176 B — **4 % smaller**, no new warnings except 2 `-Wsign-compare`
  in `power-meowkits3.c`. It is a bigger claim than the include narrowing and
  wants its own hardware pass; it also showed that IDF's libc feature macros are
  load-bearing (`-D_GNU_SOURCE`, or `firmware/include/string-extra.h` fails on
  `mempcpy`), so the honest form is naming them in the target makefile rather
  than inheriting 97 SDK flags.
  Both experiments are saved for re-running:
  `build-meowkits3/notes-reference/idf.make.narrowed` (the narrowed makefile as
  built, V1 shape) and `notes-reference/root.make.dep-hook.diff` (the
  `DEP_EXCLUDE` change it needs). They sit in the gitignored build directory so
  the tree stays clean; regenerate them if the build directory is removed.
- **Simulator: built and running, not interactively verified.** `--target=meowkits3
  --type=s` compiles from a new directory (7,645,176-byte `rockboxui` after the
  `IMAGE_LINK` polarity fix) and stays alive under
  `SDL_VIDEODRIVER=dummy`, and the buttonmap, touch conversion and skin touch objects are
  in the binary. What no headless run can show is the part a reviewer actually looks at:
  the 320×240 panel on screen, the stick mapping to `BUTTON_UP/DOWN/LEFT/RIGHT`, and the
  3×3 touch grid. Run it on a desktop with
  `./build-sim-meowkits3/rockboxui --root <dir-with-.rockbox>` and check those three.
- **A fresh build directory can lose a race upstream has.** In a new sim build dir,
  `lib/rbcodec/metadata/hes.c` compiled before `lang_enum.h` existed and died on the
  include. `plugin.h` includes `lang_enum.h`; nothing in the dependency graph orders the
  generated header ahead of the codec objects, and `make.dep` does not list it for `hes.o`
  even in builds that succeeded. A fresh `ondavx747 --type=s` won the race, so the
  exposure is timing, not target. Locally: `make $(BUILDDIR)/lang_enum.h` first. The fix
  belongs in `apps/lang/lang.make`; recorded, not patched here.
- **Runtime numbers — parked by you, procedure settled.** `battery_bench` samples every
  60 s (`battery_bench.c:457`): elapsed seconds, level %, estimated remaining time, voltage,
  current, charge flags → appended to `/.rockbox/battery_bench.txt`, flushed on storage
  idle. **A** starts, **B** stops (`ESP32S3_PAD` mapping at `:273-279`), and it keeps running
  as a TSR after you leave the plugin — that, not display control, is what lets the backlight
  timeout blank the panel during an idle run. My earlier note saying it samples "with the
  display off" was wrong about the mechanism. Two caveats: `flush_buffer()` skips writes
  while USB is attached, and plugging in starts charging, which invalidates discharge
  readings — run unplugged and connect only after stopping.
  **Dark idle is measured** (2026-10-27, 582 samples over 9.68 h, ended by a power-key
  press): 43–45 mA, mean 43.9 mA, 425.1 mAh delivered for a 27-point percent drop, so
  ~35.9 h of dark-idle life projected and ~1,574 mAh deliverable at that load. See
  `MEMORY.md` for the full read, including why remaining time runs 25–33 % pessimistic at
  dark idle and why `BATTERY_CAPACITY_DEFAULT` stays at 1400 anyway. Still wanted:
  playback screen-on and playback screen-off on the same track and volume, 20+ minutes
  each, reading the slope across samples rather than any single line.
- **Coulometer → real battery percent: the counters run, the scale is measured, and the
  gauge now uses them.** The row read `0/0` on every visit for one reason, and it was
  ours: `axp173_coulomb_clear()` wrote REG 0xB8 as a bare whole-register value (`0x20`),
  which cleared the enable bit along with the counters, so counting was off from boot
  onward. The header also called bit 6 a "calibration base"; the datasheet calls it a
  pause, and the reference driver's enum agrees (`COULOMETER_RESET/PAUSE/ENABLE` at bits
  5/6/7). Both fixed — enable and clear are now read-modify-writes, and `Debug → View HW
  info` prints the control byte so "nothing happened" is distinguishable from "counting is
  off".
  *Scale*: derived from the datasheet's two stated facts — the battery-current ADC LSB is
  0.5 mA and the rate is 25/50/100/200 Hz — because the datasheet says nothing about the
  counter's own resolution. Each sample adds one LSB per 0.5 mA, so **1 LSB = 0.02 mA.s at
  25 Hz and 180,000 LSB per mAh**. The reference library's `getCoulometerData()` multiplies
  by 65536 instead; that is wrong by four orders of magnitude and the hardware says so —
  minutes of charging already show six- and seven-figure counts. Nothing in the stock
  firmware calls those getters, so its constant has never been exercised. The row prints
  raw counts beside the estimate so the scale can be confirmed against a known current.
  `battery_bench` now measures the other side itself: two extra columns, cumulative **mAh
  charged** and **mAh discharged**, integrated from `battery_current()` in mA.s (so no
  interval is rounded) and attributed to the sample that *begins* each interval — which puts
  the charger-to-battery switch on the row where it happened instead of smearing it across
  one. That is the independent total; the chip's counter is the one under test.
  Procedure, revised after two attempts that both failed the same way — starting from a full
  charge and trying to catch the moment the charger switches to discharge:
  1. **Don't start from full.** The counters are separate cumulative registers (0xb0-0xb3
     charge, 0xb4-0xb7 discharge), so the transition is not part of the measurement; each
     interval only has to sit inside one direction. Starting near full makes the tail
     ambiguous, because charge current tapers as the cell reaches the target voltage and the
     average stops matching the instantaneous reading.
  2. Discharge leg: unplug, run playback with `battery_bench` going, and read the CC row
     whenever it is convenient. Catching the charge-to-discharge switch is not required:
     the two counters are separate and cumulative (`axp173.h:35-51`), so the discharge
     figure at any instant already contains everything the cell has given up since boot,
     and `battery_bench` now writes a `# switched to discharge HH:MM:SS` banner at the row
     where the direction bit flipped. What *is* required is reading before a reboot —
     `axp173_init()` clears both counters, so a session's history dies with the session.
     Consumption does not have to be even. The counters integrate instantaneous current, so
     a varied load still gives correct mAh; what varies is the *usable* total, because a run
     ends at a voltage rather than at a charge. The floor is `power-meowkits3.c:36-37`:
     disksafe 3400 mV, shutoff 3300 mV, which is also the 0 % entry of
     `percent_to_volt_discharge[]`. So the last CC reading that exists is whatever the
     screen shows above 3.3 V, and a run-to-shutdown figure measures the capacity above
     3.3 V. Overflow is not a concern: the counters are 32-bit raw at ~187,786 LSB/mAh,
     i.e. 22,874 mAh before wrap, sixteen full cells.
  3. Charge leg: start near 30 %, plug in, same shape against the `chg` counter.
  4. Compare the CC row's `Δcounts` against the log's `ΔmAh`. Agreement settles 180,000
     LSB/mAh; a clean factor says `coulomb_adc_rate_hz()`'s ADC-rate assumption is wrong, and
     by how much.
  5. Take the log off the card. Run to shutdown as well if you want the bottom of the gauge
     measured rather than assumed: `battery_level_shutoff` / `battery_level_disksafe`
     (3300 / 3400 mV) and the low end of `percent_to_volt_discharge[]` only get exercised by
     a run that actually reaches them.
  **Result, 2026-10-28.** 56 min on battery with playback running, current 76-86 mA (mean
  83.2), voltage 4126 → 4059 mV, gauge 93% → 88%. Discharge counter 617,273 → 15,189,454,
  i.e. **14,572,181 counts = 80.96 mAh** at 180,000 LSB/mAh; the plugin's independent
  integration of `battery_current()` over the same window gives **77.6 mAh** (trapezoid on
  the same rows: 77.7, so the log side is internally consistent). Implied scale **187,786
  LSB/mAh — 4.3% off the derivation, not a factor**: a wrong ADC rate would read 2×, 4× or
  8×, so `coulomb_adc_rate_hz()`'s 25 Hz assumption holds. The residual's *sign* is the
  tell — the chip reads higher because a once-a-minute instantaneous sample misses peaks in
  a varying load, while the chip integrates continuously. Treat the chip counter as the
  better measurement. `ctrl 80` at both ends confirms the enable bit and that CLEAR
  self-clears. Tightening it further needs a steady load (screen off, idle) or a shorter
  sample interval; neither is required to build a percentage on it.
  **Wired on 2026-10-28, pending hardware confirmation.** `CONFIG_BATTERY_MEASURE` gained
  `PERCENTAGE_MEASURE`, and `power-meowkits3.c` now has `_battery_level()`: it anchors to the
  voltage-derived percent on the first reading (the counters are cleared at boot, so they
  cannot know the cell's state), then moves from that anchor by
  `100 × Δ(dis − chg) / get_battery_capacity()`. Voltage stays in the config for the anchor,
  for the fallback (`powermgmt.c:388-392` goes back to `voltage_to_battery_level()` when
  `_battery_level()` returns -1), and because shutoff/disksafe are voltage comparisons either
  way (`powermgmt.c:503`, `:537`) — so a failed read costs accuracy, not protection. Polling
  is throttled to 5 s on a shared bus, and `axp173_coulomb_mAh()` was added so one 8-byte
  transaction answers both counters *and* says whether the chip replied
  (`axp173_coulomb_read()` reports zeroes either way).
  Three things on it still need the bench:
  1. **the full signal.** This chip has no charge-done flag, so "full" is inferred from the
     taper — direction bit still charging while `axp173_charge_current()` sits at the
     termination threshold (10% of the programmed current, so 45 mA at 450 mA; REG 33H bit 4
     left clear). Confirm a charge actually reaches ~100% after unplug and that the taper is
     observable, not just assumed. The level is capped at 99% while charging, as the voltage
     path does.
  2. **the boot anchor's error is inherited, not corrected.** A gauge anchored at 93% by
     voltage stays 93%-based until a taper re-anchor or a counter clear. The run-to-shutdown
     test both bounds that error and measures the bottom of `percent_to_volt_discharge[]`,
     which is still assumed rather than observed.
  3. **`battery_time()` sanity.** It now divides the coulometer percent by the same capacity,
     so ~14 h remaining at 84 mA is the expected reading on the measured run's state.
  The hook is entirely target-side: no core change, and the voltage tables stay in place
  as the anchor and the fallback, which is also why voltage and percent disagree under
  load sag without either being wrong.
- **The 83 % "full" reading was our own charge ceiling, not the gauge table.** REG 0x33
  bits 6:5 set the charge target voltage and reset to `10` = 4.20 V. Our init wrote the
  register as `0x80 | 4` — "enable, 450 mA" — which zeroed those two bits and set a 4.10 V
  ceiling, so an overnight charge stopped at ~4.0 V under load and the bar read 83 %. Fixed
  by naming `AXP_CHG_TARGET_4V20` explicitly; the reference firmware reaches the same place
  by never touching bits 6:5. Verify on hardware that a charge now runs to ~4.2 V and the
  gauge reads near 100 %.
  Two related corrections: **REG 0x01 bit 7 is the chip's over-temperature flag, not
  charge-done** (this chip has no charge-done bit — bit 6 reads 0 both when idle and when
  finished, bit 2 only says the charge current fell below the set value), and the previous
  text here claimed otherwise. Also `Debug → View HW info` now prints the decoded target
  and set current (`INFO_CHG`), so a wrong REG 0x33 is visible instead of inferred.
- **Does this board wire an NTC to TS?** Unverified, and it is not a cosmetic question:
  REG 0x38/0x39 and 0x3C/0x3D are charge and discharge temperature thresholds the chip
  acts on from the TS voltage, and our init used to write REG 0x84 as a bare `0`, which
  turned the TS drive current off (bits 1:0) and dropped it to 20 µA (bits 5:4) while
  setting the rate. Now written as the datasheet default `0x32` — 25 Hz, 80 µA,
  thermistor mode, drive during sampling, matching the reference firmware's 80 µA.
  `INFO_TS` prints the raw TS code: a real thermistor moves with handling, a floating pin
  sits at a rail and does not.
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
  unaffected. The encode half is settled (§3): the chosen profile halves the decode cost by
  dropping to 15 fps. **Placement is still undecided** — whether moving plugin text into
  internal RAM is worth displacing something is now a smaller question, because the recipe
  no longer asks 7.5k macroblocks/s of the core.
- ~~Frame-rate tuning of the encode recipe~~ — **done 2026-10-29.** Nine profiles plus two
  audio variants, measured with ffprobe; the chosen recipe is `-r 15`, 500k video, `-g 8`,
  mono 48k at 44.1 kHz, and it is in the manual chapter. Details and the two independent
  levers are in §3's mpegplayer entry.
- **Doom cannot launch: the arena is the limit, not the plugin buffer.** Observed on
  `b94d2a0bd1`: launching doom failed with a shortfall that varies (76,800 in one run,
  ~18k on a fresh boot) and then *crashed* — the console captured
  `rb panic: LoadProhibited pc=425e3425 excv=00000010 cause=28`, and `pc` decodes inside
  doom's own text (plugin text reserved at `0x3c5c0000`, executed through the instruction
  window at `+0x06000000`; `doom.map` gives `.text 0x425c0000 0x29678`). `excv=0x10` is a
  NULL plus an offset.
  The number 76800 is `LCD_WIDTH × LCD_HEIGHT`, i.e. one doom screen buffer. `V_Init()`
  callocs two (`v_video.c:389`) and the wipe allocates two more (`f_wipe.c:161`, `:168`),
  none of them checked in upstream, and each drawing routine writes straight through the
  pointer — so an allocation failure is a reboot, not an error message. Peak demand is
  ~307 KB of screens plus per-level `Z_Malloc` data (`p_setup.c`), `paldata` and
  `GREYBUFSIZE`. Panels on targets that ship doom are 112×64 to 160×80, where a screen is
  8-12 KB; at 320×240 ours is ~7-9× that.
  Plugin `malloc` comes from the core buflib arena (`firmware/buflib_malloc.c` →
  `core_alloc.c:72`), *not* from `pluginbuf`: doom's `.data`+`.bss` are 62,816 + 113,044
  of the 524,288-byte plugin buffer, so that buffer is not the constraint. The arena is
  ~4 MiB of PSRAM shared with the audiobuffer, pcmbuf, codec buffer, playlist cache and
  albumart (`MEMORY.md` §2), which is why the shortfall varies with what was running.
  **What the memory model actually is.** `z_zone.h:103-106` maps `malloc`, `calloc` and
  `free` onto doom's zone, so every runtime allocation -- V_Init's screens, the wipe's two
  extra screens, the lump cache, the grey table, all preloaded sound effects -- comes out
  of the single block `Z_Init()` obtains from `rb->plugin_get_audio_buffer()`
  (`z_zone.c:244`). There is no second pool. That function is `core_alloc_maximum()`
  (`apps/plugin.c:1170`), which sends shrink hints at *both* positions, and that is
  exactly the condition under which the player surrenders the audiobuffer
  (`apps/playback.c:984-992`: both positions -> `give_up = true`, otherwise
  `BUFLIB_CB_CANNOT_SHRINK`). So the maximum ask is the correct idiom for a plugin that
  needs the pool, and a bounded `rb->malloc()` zone would have been *refused* -- it sends
  `SHRINK_POS_FRONT` only. That was tried and reverted.
  **Fixed:** `I_Error()` no longer returns. Upstream prints, shuts down and hands control
  back to the caller, which carries on with the NULL it just got -- that is the
  `LoadProhibited` at `excv=0x10`. It now shows the reason and calls `exit(EXIT_FAILURE)`;
  `apps/plugins/plugin_crt0.c:59` implements that as a longjmp to `plugin__start`, which
  maps a nonzero status to `PLUGIN_ERROR` and still runs the registered atexit handler.
  One change covers every error path, including the `Z_Malloc` one that was firing
  (`z_zone.c:401`, "Z_Malloc: Failure trying to allocate %d bytes"), which is the message
  the failure now reports deterministically. `printf()` in this plugin is suppressed while
  the game owns the display (`noprintf` guards the grayscale and direct-LCD paths), so the
  message goes through `rb->splash()`.
  Also fixed on the same path: the post-main restoration used to be inline after
  `D_DoomMain()` and `exit()` longjmps past it, which is what produced the
  "Plugin error / #leak-file-handles" yes/no prompt (`apps/plugin.c:163`). It now lives in
  `doom_cleanup()` registered with `rb_atexit()` just after `backlight_ignore_timeout()`,
  covering `backlight_use_settings()`, `M_SaveDefaults()`, `I_Quit()`, the file closes,
  `cpu_boost(false)` and `talk_disable(false)` -- `apps/plugin.c` restores none of those
  itself. Registering after the menu check keeps the two early exits (quit chosen, missing
  base WAD) from running shutdowns for systems they never started. The close loop also
  dropped an off-by-one: `fpoint` is the next free slot, and the old loop closed
  `filearray[fpoint]` as well, i.e. a stale descriptor the core reports as a double close.
  **Reverted as dead code:** allocation checks on V_Init's calloc, the wipe's two mallocs
  and its column array. Those names are macros onto `Z_Malloc`/`Z_Calloc`, which reports
  failure itself before returning, so a NULL check after them can never fire.
  **Net plugin delta:** two files -- `i_system.c` (leave instead of returning, visible
  message) and `rockdoom.c` (the cleanup handler). `apps/plugin.h` untouched.
  **Outcome: it plays.** Four changes, each measured rather than guessed. Instrumented
  builds walked doom's zone at every stage and reported `st/pu/fr/big/blocks` over the
  CDC, which is what turned this from speculation into arithmetic.

  | change | effect, measured |
  |---|---|
  | `getsfx()` reads the lump directly (`W_ReadLump`) instead of caching it and copying | removes ~900 KB of purgable duplicates -- one block per sound instead of two, in an interleaved pattern that was the fragmentation source |
  | `I_InitGraphics()` releases `V_Init()`'s `d_screens[0]` before replacing it, which `v_video.c:387` always promised | the zone went from `fr=37,824 big=18,624` to a run that can hold a screen; net memory unchanged because the replacement is the same size |
  | the wipe's column-major transform scratch moves to `.bss` (`pluginbuf`, not the arena) | drops the fifth full-panel buffer out of the zone; the failing ask was 76,800 against `big=68,384` with 880,320 usable |
  | `DIRCACHE_MIN` and `DIRCACHE_LIMIT` both to 256 KB | doom's zone 1,757,376 -> ~2.5 MB; the 1 MB floor alone was holding the cache at 1 MB regardless of the limit |

  Measured zone sizes for reference: dircache 1 MB gave `doom: zone 1757376`, dircache
  disabled gave `doom: zone 2805984`. Peak doom demand at a wipe is four panels
  (307,200) + ~1.2 MB pre-cached sounds + ~300 KB level data, i.e. ~1.9 MB, which is
  what the 256 KB cache makes possible. `tagcache` was left alone deliberately -- its
  RAM block mirrors the on-disk database (`tagcache.c:4342-4346`) and has no shrink
  callback (`:4319-4322`), so it is on/off only, and there is no size setting in this
  tree; it is the next lever if headroom is ever short again. Also worth knowing:
  `initialize_tagtree()` does its own `core_alloc_maximum` (`tagtree.c:1436`) and panics
  on failure, so opening Browse-by-Tag before a plugin launch leaves the plugin nothing.

  **Still open, in order:**
  - ~~Doom sound and music~~ — closed, see §2. Sound was `m_misc.c`'s default for
    `enable_sound`, not the mixer: the ~1.2 MB of pre-cached effects were being played
    into a channel the plugin had switched off. Music is absent by upstream design —
    `I_InitMusic`, `I_PlaySong` and `I_RegisterSong` are `// UNUSED.` stubs
    (`i_sound.c:546-595`) on every Rockbox port, so the WAD's `mus00` lumps load and go
    nowhere. Not a gap we can close without an ADM/OPL emulator.
  - ~~Doom key mappings~~ — closed, see §2.

  - ~~Re-test the worst case~~ -- **passed 2026-10-29** on the flashed rebased image
    (`960b37067f-261029`): dircache on at its committed 256 KB, a FLAC track mid-playback,
    then launch doom. It starts and plays. The audiobuffer-surrender path
    (`playback.c:984-992` giving up the buffer only on a both-position shrink hint, which
    `core_alloc_maximum` sends) therefore works with the cache live, and the compile-time
    `DIRCACHE_MIN`/`DIRCACHE_LIMIT` pair is exercised rather than the runtime off switch.
  - ~~The dircache change needs a flash~~ -- done: `90224de484` is in the flashed image, and
    the run above used it.

- **Memory audit: two things still unexplained, one still unverified.** The
  placement/size sweep against upstream conventions is done (see MEMORY.md's
  "The fs_defines.h override is load-bearing" and "The placement fragment is a
  private mechanism"), and the inert `buffering` entry is removed. What the audit
  left open:
  - **The ~1.6 MB the arena loses at boot is now accounted for, and it is not a
    leak.** The debug screen aggregates the pool by owner (see MEMORY.md's "What
    actually occupies the arena at boot"): at boot, before playback, fonts hold
    611 KB in 4 blocks, dircache 384 KB, `tree.c` 195 KB, and six smaller owners
    360 KB -- 1,660 KB held, 2,563 KB free of a 4,325,376-byte pool. The
    actionable half of that is fonts being the single largest consumer:
    `firmware/font.c` turns `FONT_HARD_LIMIT` on only for `MEMORYSIZE < 4`, and
    this target's `MEMORYSIZE` is the 8 MB PSRAM window rather than DMA RAM, so
    the cap is off and glyph-subset allocations are unbounded. `meowkits3.h` now
    defines `FONT_HARD_LIMIT`, and **that is verified on hardware**: codecs play
    with the cap on and the UI still draws normally. What has not been
    re-measured is the resulting font footprint -- the cap should pull each
    oversized font to <=60 KB, so a boot-time read of the arena owner rows is
    still worth taking to confirm the 611 KB actually fell.

  - **`BUFLIB_DEBUG_PRINT` silences codec playback on this target, and the
    mechanism is unexplained.** Enabled, playback engages and produces no audio;
    disabled, with every other change held in place, playback is audible. Two
    builds had it on and were silent, and two had it off and played -- the flag
    is the only thing that differs. Its documented runtime effect is one line in
    `core_alloc.c`: `test_alloc = core_alloc(112)` at `core_allocator_init()`,
    which nothing frees. So the candidate is that 112-byte block sitting in the
    pool ahead of everything else, but nothing about it obviously prevents the
    DAC from producing sound, and `pcm_play_dma_start` evidently still runs (the
    TX row counted a session). The arena rows now walk `core_ctx` directly and
    need neither the flag nor the test allocation, so this target has no reason
    to define it -- but if anyone reintroduces `BUFLIB_DEBUG_PRINT` here, codecs
    going silent is the symptom to recognise. Note the shape of the bug: it is
    silent, not an error, and metadata, art and browsing all still work.
  - **First-boot codec silence is unexplained, and the earlier attributions were
    invalid.** 512 KB and 1 MB `DIRCACHE_LIMIT`, and `BUFLIB_DEBUG_PRINT`, were
    each declared guilty of silencing codecs on single-boot comparisons. The same
    firmware played after a later reset, so boot state -- not the variable being
    tested -- is the uncontrolled factor. Measured since: a *live* database
    rebuild does not starve playback at 384 KB; codecs play, track switching is
    slower while the scan runs, which is the tagcache temp buffer
    (`apps/tagcache.c:789`, `core_alloc_maximum` with `buflib_ops_locked`) leaving
    playback a smaller grant rather than none. So why a fresh-boot playback
    attempt failed is still open, and the retest protocol is flash, reset once so
    the database and directory cache settle, then test -- with the owner rows read
    during any failure, since the `buflib_ops_locked` group is that buffer. The
    font cap's ~413 KB therefore remains unspent rather than refused.

  - **`sd-esp32s3.c` asserts IDF handles cache maintenance "when the destination
    is in PSRAM".** Behaviourally that is true -- dircache populates from the
    PSRAM arena and the database builds -- but `SOC_SDMMC_PSRAM_DMA_CAPABLE` is
    undefined for esp32s3 and the SD host's IDMAC does not go through the
    Ext_RAM MMU, so either IDF bounces internally or Rockbox's read destination
    is never actually the PSRAM buffer. The comment states a mechanism nobody has
    read. Settle it from IDF's `sdmmc_transaction.c` allocation caps, not from
    the fact that it works.
  - **Deliberate, not drift, and worth stating in review:** `MEMORYSIZE=8` here is
    the PSRAM window handed to `tools/configure` as `memory=8`, whereas peers get
    8/16/32/64 for RAM. Upstream heuristics that read `MEMORYSIZE` as RAM
    (`fs_defines.h`, `pcm_record.c`) therefore misread this target, which is why
    the overrides exist. `CODEC_SIZE 0x100000` is *not* a deviation -- 11 targets
    use it and `hibyr1native` pairs it with `PLUGIN_BUFFER_SIZE 0x200000`; ours is
    the smaller 0x80000, which the measured largest plugin (220 KB) supports.

## 2. Verified on hardware — closed

- **The recurring shutdown and menu-navigation panics were damaged FAT metadata
  on the card, not code.** What the panic screen actually said was
  `Updating size on empty dir entry 147` -- `firmware/common/fat.c:1943`, which
  fires when `cache_direntry()` returns a slot whose first name byte is `0x00`
  or `0xe5` for a file being written. The same entry number on every boot, and
  three consecutive boots in the console capture. It explains both symptoms
  exactly: `clean_shutdown()` runs `system_flush()`, which writes settings and
  bookmarks and therefore updates a directory entry, so a shutdown hit the damaged
  slot every time; and navigating menus shortly after boot reads that directory
  while waiting past the boot burst does not, which is why "race to the menu
  crashes, wait a moment and it is fine" looked like a timing bug. Running the
  card through a filesystem check cleared it, and the payload was re-extracted
  from a fresh zip as a precaution.

  The damage has a source: the MSC writes performed *before* the cache-coherence
  fix in `45a8669533` corrupted FAT and directory data, and that damage is on the
  card rather than in the image, so reflashing never removes it. A board that
  panics identically on every boot, in a way that survives a reflash, should have
  its card checked before its code. `reset 5` in these reports is
  `ESP_RST_INT_WDT`, not a panic restart: Rockbox's panic path holds with
  interrupts masked to keep the message on the glass, and the interrupt watchdog
  then resets the chip. Read the panic text, not the reset reason.

  The recorder changes that made this legible are kept: the abort-aware panic line
  (IDF's `info->reason` *is* the assert text for an abort, so printing
  `pc=`/`excv=`/`cause=` on that path spends the record's 96 characters on the
  handler's own break and truncates the message), `hnd-ps`/`frm-sp` labelling
  (the old `ps` row read the handler's PS at exception level 4, so it read the
  same value on every crash and looked like fault data), six raw `XtExcFrame`
  word rows, and a capture of the two words resident at the faulting address,
  which is what distinguishes a bad instruction from a failed fetch.

- **The shutdown crash and the debug-menu crash were stale cache reads, not a
  scheduler bug. Fixed by `45a8669533`.** `cache-xtensa.c` had made
  `commit_dcache_range()` and `discard_dcache_range()` no-ops for internal DRAM on
  the claim that "internal SRAM is not cached on esp32s3", inferred from
  `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` — a symbol that exists only for esp32p4 in
  this IDF and describes that part's L1 strictness, not whether this part caches
  its internal SRAM. ESP-IDF contradicts the inference in its own code:
  `components/hal/esp32s3/include/hal/cache_ll.h` maps both IRAM0 and DRAM0 to a
  cache level, and `esp_driver_sdmmc/src/sdmmc_transaction.c` plus
  `esp_driver_i2s/i2s_common.c` msync internal DMA buffers unconditionally.

  Two consumers were hollowed out by that: `usb_dw_target_dma_discard()`, which is
  the override of a weak hook whose entire purpose is making DWC-written endpoint
  buffers visible (and `usb-designware.c` calls `discard_dcache_range()` for every
  CPU but the ARM940T), and the GDMA descriptor ring, whose owner bit the transmit
  path polls after the engine retires a slot. Reading a stale line there is not a
  slowdown — the driver acts on bytes that were never transferred. The corruption
  then surfaced far from its cause: garbage MSC payload -> FAT and directory
  structures parsed from bad bytes -> a write through a bogus pointer -> a
  smashed thread stack -> a window-refill double exception in `switch_thread` /
  `block_thread_` with a garbage `a5` base-save pointer, on the *next* context
  switch. That is why the symptom read as a scheduling bug, why it appeared on
  shutdown (teardown flushes the card) and in the debug menu (walks the directory
  cache), and never in the audio path that was actually mis-bookkeeping.

  What the no-op had genuinely been protecting against was real but was a race,
  not wasted work: cache code in the GDMA handler interleaving with a whole-cache
  operation in task context, reported as `Cache error` at `0x4004e5b7` in the ROM
  cache module. The remedy is the platform's own — every cache call here now runs
  inside `esp_os_enter_critical_safe()` on a static critical-section lock, exactly
  the shape `components/esp_mm/esp_cache_msync.c` uses, so range and whole-cache
  paths are mutually exclusive instead of one being silent. Two contract
  alignments came out of the same reading: `discard_dcache_range()` honours the
  tree-wide rule (`cpucache-arm.h`, `mmu-mips.c`) that a discard does not write
  back except for partial edge lines, by widening to whole lines and writing them
  back before invalidating — a 12-byte GDMA descriptor cannot discard a
  neighbour's dirty line that way; and `commit_discard_dcache()` discards as well
  as commits, as every other port does, which `usb_storage.c:496` depends on.

  Measured on hardware after the change, in one session: boot, repeated codec
  switching, doom, mpegplayer, MSC file write with a host-side SHA256 match,
  eject, reconnect, `Debug -> View HW info`, and a power-key shutdown that powers
  off instead of panicking. The last two both faulted before it.

- **The retained crash record is readable on-device, and `pc` in it is never the
  cause.** IDF's panic path breaks in `panic_abort` (`0x4004cd8c` on this build),
  so `pc` and `epc1` both name that break and neither is a cause. The usable
  fields are `epc2` — the address a doubled fault came from, because the ISA moves
  the original EPC1 down to EPC2 — and `depc`, the address that doubled. Those are
  nowhere in the frame the handler sees, which is why `panic_cdc.c` reads them from
  the CSRs at level 4 and stores them in RTC slow. `View HW info` now shows ps/sp,
  epc1/epc2, epc3/epc4, depc, the window state and the cache-error latch as
  separate rows: `paniclog_frame_regs_text()` is a single ~90-character line, right
  for the console and wrong for a 320-pixel row, where clipping removes the tail —
  and the tail is where `epc2` sits.

- **Reading a console on this board resets it, and the crash replay happens
  once.** The kernel's `cdc-acm` driver asserts DTR/RTS on `open()` even through a
  plain `os.open`, so opening the USB-Serial-JTAG node gives
  `rst:0x15 (USB_UART_CHIP_RESET)` — a USJ reader destroys the state it is
  watching while the app owns that node. It is only safe as a loader-time or
  boot-time channel. `paniclog_crash_report_logf()` replays the record over CDC
  about 3 s after boot, once, and does not buffer: if the cable is out at that
  moment the report is gone until the next boot, and unplugging can drop the rails
  that hold RTC slow and the `.noinit` ring, i.e. destroy the evidence. So: attach
  a CDC-only reader resolved by USB id (`1234:4002`, since the kernel renumbers
  `ttyACMn` on every re-enumeration), leave the cable alone, and tap RESET.
  `esptool` at 460800 on the USJ link fails with `Invalid head of packet`; 115200
  handshakes. A looping app also breaks it — the console bytes land as noise, and
  `303a:1001` is present both in the loader and while the app runs, so the USB id
  alone does not say which state the chip is in.

- **Instrumentation that occupies the thread it is measuring, or divides by
  zero, reports a port bug that is its own.** Two probe faults during this
  investigation, both misread as port crashes before being decoded. A tick task
  that blocked on an SD read starved `button_read_device()`'s PMU cable poll, so
  `usb_enable()` never ran and the device simply never enumerated. And a zero
  divisor on xtensa is not a quiet result: GCC emits `break 1, 15`, which arrives
  as `EXCCAUSE_SYSCALL` with the pc inside the probing function's own literal
  pool, i.e. a panic whose reported location looks like corrupted code. Decode the
  faulting *instruction* before concluding anything about the port.

- **Plugin, codec and decoder images were executed without cache maintenance, and
  `commit_discard_idcache()` was the reason.** Fixed 2026-10-30 by implementing
  `commit_dcache()`, `commit_discard_dcache()` and `commit_discard_idcache()` as
  out-of-line functions in `firmware/target/xtensa/cache-xtensa.c`, backed by the
  ROM's whole-cache operations (`Cache_WriteBack_All`, `Cache_Invalidate_ICache_All`,
  `Cache_Invalidate_DCache_All`). The emitted code was checked, not assumed:
  `commit_discard_idcache` now disassembles to writeback-all, invalidate-I, invalidate-D
  via `callx8` to `0x400016f8`, `0x400016d4` and `0x400016e0`.

  What it was before: three `static inline` functions in `system-target.h` calling
  `xthal_dcache_all_writeback()`, `xthal_dcache_all_writeback_inv()` and
  `xthal_icache_all_invalidate()`. Those entry points exist in the esp32s3 toolchain,
  they link, and they do nothing — each is `entry a1, 16; retw.n` and nothing else,
  confirmed by disassembling a linked image. The core config has no cache instructions,
  so the Tensilica HAL layer has nothing to emit and the stubs are silently empty. The
  contract was satisfied in the source and not in the binary, which is why nothing in a
  build or in the simulator could have found it.

  The symptom was imageviewer's JPEG and PNG decoders faulting deterministically at a
  pc *one byte into* an instruction inside `jpeg.ovl`'s text, with `EXCVADDR` a garbage
  register plus `0x28`, non-zero spill temporaries and registers belonging to
  imageviewer's window while the pc sat in the decoder. Everything that could be checked
  on the host checked out: the segment bytes in memory matched the file, the api pointer
  the caller wrote was `rockbox_api` (size 1740), the api entry the decoder was calling
  was `memset`, the vtable entry was aligned, `LCOUNT` was 0 so no hardware loop was
  live, and the stack had used 752 bytes of 32 KiB, so nothing had run off the end.
  The decisive clue was that *changing the decoder's source moved the fault pc*: with
  six logf lines added, the crash moved from `0x4260032b`/LoadProhibited to
  `0x42600342`/StoreProhibited. Execution was not doing what the file said — the fetch
  side was serving bytes the write side had never pushed out. The old comment in
  `system-target.h` had already recorded exactly that phenomenon on a vorbis load
  ("the word at the faulting PC read 02 78 b9 aa where the linked image says 29 08 81 5f,
  while both neighbouring words matched exactly") and attributed its cure to the
  `xthal_*` calls, which cannot have done anything. That attribution was wrong and is
  corrected.

  Why whole-cache rather than range: `cache_hal_writeback_addr()` and
  `cache_hal_invalidate_addr()` resolve the address through
  `cache_ll_vaddr_to_cache_level_id()`, which on esp32s3 accepts only IRAM0 and DRAM0.
  Plugin, codec and decoder images all live in the PSRAM window, so a range operation on
  them is rejected before it touches a register — `esp_cache_msync()` returns
  `ESP_ERR_INVALID_ARG` with a message about a null pointer. The LL layer also ignores
  the level and type it is handed and calls the same ROM routines, so the ROM whole-cache
  operations are the only reachable maintenance for external memory. The DMA paths keep
  using the per-range functions, which is correct because their buffers are in internal
  SRAM.

  Verified on hardware: JPEG renders, and the oversized test file fails without faulting
  (a buffer limit, not a crash — see below). Because these names are also called from
  `firmware/usbstack/usb_storage.c` and from `rolo.c`/`lc-rock.c`, and because
  `kernel/thread.c:1217` is `#if NUM_CORES > 1` and not compiled here, the codec, plugin
  and USB-MSC paths need a re-run before the fix is called settled.

- **Two loader bugs surfaced on the way there, both fixed.**
  `lc_open()` returned the caller's buffer as the image handle; `firmware/lc-rock.c`
  returns `hdr.load_addr`, and the handle has to be the address of the image's own
  `.header` struct. For a rock or codec the segment base happens to *be* the header,
  which is why every plugin and codec test passed; an imageviewer decoder links its
  first segment from file offset 0, so the ELF headers sit ahead of `.header` and the
  caller was handed `0x7f 'E' 'L' 'F'`. The address now comes from the section table
  (xtensa `shentsize` is 40), which is the only source that knows where the struct
  lives. Separately, `IMGVDECODER_OFFSET` was applied to `PLUGIN_RAM` but not to
  `PLUGIN_TEXT`: on every upstream target those are the same region, while our two
  aliasing cache windows keep them apart, so every decoder was linked on top of
  imageviewer's own text. `apps/plugins/plugin.lds` now reserves a
  `XT_IMGDEC_TEXT_SIZE` slice at the top of the plugin text half, the plugin text is
  capped so `ld` rejects an oversized plugin, and `tools/meowkits3/esp-layout-check`
  reads the built artifacts and fails the build if a decoder would land under a
  plugin's text end or past the codec text — the failure mode is silent corruption,
  so it is guarded from the artifacts rather than from the headers.

- **Recording at 88.2 kHz and 96 kHz works** — the `SPEED_MODE` bit-1 fix
  (`dfff14a08b`) now has signal through it. Verified 2026-10-29 on `960b37067f` by reading
  the WAVs off the card rather than by listening: `Recordings/R_MIC_260510-131716.wav` is
  96,000 Hz / 16-bit / stereo, 0.92 s, peak 1166 (-29.0 dBFS), RMS 140 (-47.4 dBFS), zero
  clipped samples, and **1.3% of its energy above 22 kHz**. That last figure is the whole
  proof: a 44.1 kHz capture cannot contain anything above its 22.05 kHz Nyquist, so the ADC
  was genuinely clocked at the requested rate. The clip was recorded with mic gain at **0
  dB**, which is why the level is modest — it sits between the measured 0 dB reference
  (RMS -56 dBFS) and the 30 dB one (RMS -23.9 dBFS), exactly where a voice at 0 dB belongs.
  The two earlier high-rate clips (88.2 peak -59.9 dBFS, 96 peak -57.4 dBFS) were also
  genuine captures — 40.8% and 8.4% of their energy sits above 22 kHz — they simply had no
  input, and at 0 dB gain the oversampled noise dominates the top end, which is why the
  *fraction* above 22 kHz is larger in the quiet files. Reading "low level" as "no signal"
  was the wrong inference; bytes, rate and spectrum were all fine.
- **DSD64 DSF plays, and album art decodes out of it.** `1-01 - Hello.dsf` (145,283,752 B,
  Oasis) plays on `960b37067f`, and its embedded art renders. Header: `DSD ` magic,
  `sample_type=2`, DSD rate 2,822,400 → `metadata/dsd.c:55` divides by 64 and asks the sink
  for **44.1 kHz**, channels field 1 (so `lsb_first`), block 4096, `sample_count`
  573,511,680 bits per channel — which computes to 143.4 MB of payload, matching the file
  exactly, so **the data is raw DSD** despite the `sample_type` value. The tag is an ID3v2.3
  block at `metadata_pointer` (143,384,668) with an **APIC frame of 1,890,341 B (JPEG)**,
  read through `setid3v2title()` — the first time our art path has been exercised on a
  container other than FLAC or ID3v2 MP3. Its demand is 0.71 MB/s of sustained reading, and
  it plays clean, so nothing here bounds the card.
  Two things I predicted wrongly and should not be repeated: that `sample_type=2` means
  DST-compressed and therefore this file would decode to static (it plays — the field's
  meaning is unconfirmed, and `dsd.c:148` requires exactly 2 whatever it signifies), and
  that a plain DSF is the common case that the gate rejects (untested either way). What is
  still true from reading the code: `decode_dsf()` only ever runs the CIC decimator —
  `dst_decode()` is reached solely from `decode_dff()` (lines 468/487/528) — so a genuinely
  DST-compressed DSF would decode wrongly, and DSF seeking (`dsd.c:197`) uses linear byte
  arithmetic that only holds for uncompressed data. Unverified, because we have no such
  file.
  **Slow startup is a property of those particular files, not of DSF or of this target.** The
  Oasis album's tracks open slowly; the DSD128 Wall album's 26 tracks load their art and start
  quickly, and those files are *larger* individually (up to 539 MB against 205 MB), so size
  and rate are not what costs the seconds. The Oasis file examined here carries a 1,890,341 B
  APIC JPEG at offset 143,384,668, which is read and decoded before the first sample; the Wall
  files' own tags were not inspected (card unmounted by the time it mattered), so the
  differentiator is recorded as per-file embedded art rather than as a proven comparison.
  Closed as an artifact of how the two albums were packaged — there is nothing to fix, and the
  isolation tests listed earlier are not needed.
- **176.4 kHz / 24-bit WAV plays at the correct pitch.** Both 176.4 kHz files in the MA
  Recordings sampler play fine on `960b37067f`, which is what the code says should happen:
  the DSP resampler is unconditional — `lib/rbcodec/dsp/resample.c:322`, "This stage should
  be enabled at all times" — and converts the file's virtual rate to the DSP output
  frequency, so `HW_SAMPR_CAPS SAMPR_CAP_ALL_96` decides only what the sink is *clocked*
  at, not what is playable. There is no `convert_rate` anywhere in this tree, so that
  resampler is the entire mechanism. 24-bit input still lands in a 16-bit slot
  (`SLOT_BITS 16`, ES8311 `REG_SDPIN`/`REG_SDPOUT` = 16-bit), so nothing about resolution
  is gained at the DAC; what the files really exercise is per-sample resampling at 2× the
  usual rate on one core, with codec and DSP code executing from PSRAM.
  I also stated the card bandwidth wrongly: `rate × channels × bits / 1e6` is **megabits**
  per second. The sustained reads are 0.18 MB/s at 44.1/16, 0.58 MB/s at 96/24 and
  1.06 MB/s at 176.4/24, against roughly 2.5 MB/s available — so the transport was never
  the limit, and my objection to testing those files was a units error.


- **Doom sound was the plugin's own default** (`apps/plugins/doom/m_misc.c:294`).
  `doomstat.c:102` initialises `enable_sound` to 1, but the settings table registers the
  default as 0 and `M_LoadDefaults()` applies it, while `rockdoom.c:876` deletes
  `/code/data/config.cfg` before every launch — so the default was the effective state on
  every start, and a menu change could not survive because the file it wrote was removed
  before the next run. The default is now 1, matching the initialiser. Verified on
  hardware: sound effects audible, still no music (upstream stubs, see §1).
- **`battery_bench` marks the direction change** (`apps/plugins/battery_bench.c`). The
  charging column reads `C`/`D` instead of `C`/`-`, because this chip reports the battery
  *current direction* and a clear bit means current flowing out — a `-` was the same glyph
  the charger column uses for "nothing attached", which made the discharge half of a run
  invisible to the eye. A `#` banner line is written at the row where the bit flips, with
  the charge and discharge totals accumulated up to the previous row, so a capacity figure
  starts at a labelled line. Both columns are live on this target
  (`CONFIG_CHARGING == CHARGING_MONITOR`, `CONFIG_BATTERY_MEASURE & CURRENT_MEASURE`).


- **Doom controls, remapped and verified** (`apps/plugins/doom/i_video.c`). Stick walks
  and turns, `A` fires, `B` uses or opens, and the grid carries escape (top-left), map
  (top-middle), next weapon (top-right), menu confirm (mid-left), autorun toggle
  (bottom-left) and strafe (bottom-middle, bottom-right). Reported working with no
  issues on `90224de484` plus this mapping. Strafe uses Doom's dedicated
  `key_strafeleft`/`key_straferight` (defaults `,` and `.`) rather than the
  `key_strafe` modifier, because the modifier would need a held-key gesture and
  `HAS_BUTTON_HOLD` is not implemented for xtensa. All of them are rebindable in Doom's
  own Controls menu.
  The measurement that settled the implementation: a build that drained the button queue
  *and* kept the held-state machine produced double-fired toggles (escape opening and
  closing on one press), while a build whose queue drain happened first — a tracer that
  consumed the events before the handler could — worked for every cell the state machine
  covers and failed only for the three it did not. That is what proved touch cells arrive
  as held state via `button_status()` (`button-meowkits3.c:190-203`, `button.c:414`) and
  that the queue path was redundant at best. The fix was to delete the queue handler and
  add the three missing cases to the state machine.


- **Mic gain fixed and measured** (`c39d932883` plus the follow-up). Four things were
  wrong at once, and the recordings separate them:
  - `audiohw_set_recvol()` passed the dB setting value to `es7210_set_gain()` as a
    register code, so 30 dB asked for code 30 → clamped to **15, which the datasheet
    does not define**. `es7210_set_gain()` now takes dB and maps it through the real
    ladder (0..30 dB in 3 dB steps, then 33/34.5/36/37.5), and `MIC_GAIN` is declared
    0..37 instead of 0..30.
  - **`REG41` 0x70 was killing the PGA.** Written like that because the stock driver
    writes it that way; the datasheet resets to 0x71 and `ADCnBIAS_SWH` belongs to the
    PGA/ADC bias, not to the mic supply (the schematic's external 3 V3 bias through
    0 Ω made this register look irrelevant — it isn't). With 0x70 the 0 dB vs 30 dB
    span measured 3.3 dB; with 0x71 it measures 32 dB, i.e. the ladder's 31.6x.
  - `REG1D`/`REG1E` (`ADCn_DIRECT_DB`, 0.5 dB steps, 0xBF = 0 dB) had never been
    touched by us or the stock driver. Now `ADC12_DIGITAL_GAIN_DB` = +12 dB, which is
    what the board needs against a 2 Vrms full-scale input and a -38 dBV/Pa mic.
  - `REG08` `SPEED_MODE` is **bit 1**, not bit 6 (bits 7:4 are `LRCK_RATE_MODE`). The
    driver used bit 6, so ≥64 kHz recording stayed single-speed.
  **Measured result** at 44.1 kHz, talking into the mic: setting 30 dB gives RMS
  **-23.9 dBFS**, peak 25721 (-6 dBFS), **zero clipped samples**; setting 0 dB gives
  RMS -56 dBFS. L and R differ on 61,315 of 61,440 samples, so the earlier "channels
  identical" reading was RMS happening to agree to 1%, not an RX slot artifact.
  `Debug → View HW info` keeps an `ADC` row (REG43, REG1E, REG1D, REG41, REG01, REG08)
  so this stays observable; note it shows the value applied at the *last session
  start*, not the current setting.
  Still unverified: whether the **max 37 dB** setting clips on loud input, and
  `pitch_detector` at its **default** Volume Threshold — its gate is now
  `(50 × 37)² = 3.4e6`, i.e. RMS 1842, and the measured speech RMS is 2101, so it
  should pass without touching the knob. (Recording at 88.2 and 96 kHz is no longer on
  this list — it is verified, see §2.)
- **Capture path re-isolated, and `pitch_detector` works** (`a781b24b47`). The receiver and
  the transmitter no longer share one rate variable; see `AGENTS.md` ("Bus ownership") and
  `MEMORY.md` for the contract. Verified on hardware with the image built from that commit:
  - `Debug → View HW info` while the tuner had been open read
    `RX 8000Hz en0 bus0 rd267 to0 e0 1096232B`. 267 reads of a full 4096-byte chunk, zero
    timeouts, zero errors — and 1,096,232 B ÷ 4 B per stereo frame = 274,058 frames ≈ 34 s
    at 8 kHz, which is how long the tuner was open. Audio arrived; the `en0 bus0` is correct
    because the row was read after the session ended (the counters are cumulative precisely
    so that a plugin holding the UI can still be diagnosed).
  - Recording a WAV at Settings → Recording → **8 kHz** and playing it back is audible. That
    is the proof the ADC and the reconfigure path run at a rate playback never set, which is
    the case `pcm_rec_dma_start()` now programs.
  - Playing that WAV back worked, which is also the pads-hand-back test: the transmitter
    clocks the codecs again after a session released them.
  - **Why the tuner still looked dead with audio flowing**: its gate is
    `buffer_magnitude() > sqr(volume_threshold × sound_max(SOUND_MIC_GAIN))`
    (`pitch_detector.c:1039`), and `es8311.h:36` declares mic gain as `AUDIOHW_SETTING
    (MIC_GAIN, "dB", 0, 3, 0, 30, 30)` — so the shipped default of 50 demands RMS > 1500 in
    channel L. That is a *line-level* expectation: most targets reach this plugin through
    `INPUT_TYPE = AUDIO_SRC_LINEIN`, and we are mic-only with two MEMS mics behind a 30 dB
    ladder. Lowering the plugin's own **Volume Threshold** to ~10 gives `(10×30)² = 90,000`
    → RMS 300, which the mic actually reaches, and the tuner paints. Nothing in the driver is
    involved; the plugin's threshold is a per-hardware knob and this board sits at the low
    end of it. It persists: `save_settings()` writes `/pluginapps/.pitch_detector_settings`
    on exit when the struct changed.
- **Feature-flag batch — verified on hardware** (`2d550135b2`). Four enablements, all
  target-side, flashed as part of `292debd757186fc632531293fe67c6c2`:
  - *The status bar's disk icon is now driven.* `CONFIG_LED` defaults to `LED_VIRTUAL`, so
    `apps/gui/statusbar.c:123-129` reserved 12 px and drew `bitmap_icon_disk` from
    `led_read(HZ/2)` — but nothing on xtensa ever called `led()`, so the icon was dead space.
    `sd-esp32s3.c` now brackets both sector calls with `led(true)`/`led(false)`, which is the
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
    "Off" was display-only. `audiohw-meowkits3.c` now keeps `pa_wanted` (the request) separate
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
  `lcd-meowkits3.c` walks the planes exactly as memframe does, calls the shared line helper, and
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

- **mpegplayer: the memory floor is one disk_buf page, and crossing it is a panic.** It
  played, then crashed again on 2026-10-29, and the crash corrected two entries in this
  section. `pc=0x425c56bf` resolved to **`disk_buf_probe+0x1f`** — the deployed `.rock` is
  stripped, but `build-meowkits3/apps/plugins/mpegplayer/mpegplayer.elf` carries 333
  symbols and its `.text` is `0x12c80`, matching the shipped rock byte for byte, so the
  address resolves. That offset is `MAP_OFFSET_TO_PAGE()` (`disk_buf.h:54`), which is

  ```c
  #define MAP_OFFSET_TO_PAGE(o)  (MAP_OFFSET_TO_TAG(o) % disk_buf.pgcount)
  ```

  a modulo by a runtime value. With `pgcount == 0` libgcc's `__umodsi3` executes `modui`
  with a zero divisor → IntegerDivideByZero → panic and watchdog reset. No message, no
  splash.

  Failure shapes, as measured (greylib is compiled out on this target — `HAVE_LCD_COLOR`,
  so the older "grey +" figures in this file were wrong):

  | step | fails when | what you get |
  |---|---|---|
  | `mpeg_alloc_init()` | grant < 2,097,152 | "Out of memory in stream_init" |
  | `pcm_output_init()` | grant < 2,273,552 | "Could not initialize PCM!" |
  | `audio_thread_init()` | grant < 2,340,836 | "Cannot create audio thread!" |
  | `disk_buf_init()` | remainder < `MIN_MEMMARGIN` (4 KiB) | "Cannot create buffering thread!" |
  | first `disk_buf_probe()` | `pgcount == 0` | **divide-by-zero panic** |

  The fifth row is the one that bites, because `mpeg_malloc_all()` refuses only below
  4 KiB: between 4 KiB and `DISK_GUARDBUF_SIZE` (65,544) + one 32 KiB page it *succeeds*,
  hands back a pointer, and leaves `pgcount` at 0 (`disk_buf.c:943` divides by the page
  size, then the shrink loop can decrement to zero). So the plugin starts, allocates
  everything it checks, and faults on the first probe.

  **The floor is a grant of ~2,443,244 bytes** (2,097,152 libmpeg2 reservation + 176,400
  PCM + 67,284 audio queue + 4,096 margin + 65,544 disk guard + 32,768 page). Measured on
  the bench: cold boot `core_allocatable()` = **2,456,452** — 13 KB above the floor, one
  cache page — and it crashed; with the directory cache off = **2,718,620**, about nine
  pages, and playback works. `buflib_alloc_maximum()` takes exactly `buflib_allocatable()`
  (`buflib_mempool.c:872`), which is the second number the `INFO_ARENA` row prints, so
  that row *is* the plugin's grant. A cold boot sits barely inside; anything the browser or
  an album image has claimed since boot puts a file below it.

  **The rebase was ruled out by audit, not by assumption.** `git diff 260c70d53f..HEAD --
  apps/plugins/mpegplayer/` is only keymap blocks (`FIIO_M3K_LINUX_PAD` deleted, our
  `ESP32S3_PAD` cases moved). Kernel changes are PSP-scoped (`mutex_unlock` guard,
  `DEFAULT_STACK_SIZE` guard) plus `TMO_NOT_QUEUED` becoming `((struct … *)1)`.
  `apps/codec_thread.c` gained the `size == 0` guard, which mpegplayer cannot reach (it
  never calls `read_filebuf`). `plugin.lds` gained only an X1600 branch; `plugins.make`
  only renamed `ctru-app` to `3ds-homebrew`, so plugin build flags are unchanged.
  `firmware/lc-rock.c`, `lcd.h`, `lcd-meowkits3.c`, `st7789.c`, `cache-xtensa.h` and
  `asm/lcd-as-memframe.c` are byte-identical, including the `lcd_write_yuv420_lines()`
  signature our blit re-declares locally. Plugin API went 285 → 286 with two tail entries
  mpegplayer does not reference. The clip was verified healthy (320×240 MPEG-2 in a
  Program Stream, 24 fps, homogeneous across the file, no B-frames, no `sequence_error`)
  and the card's rock is byte-identical to the rebuilt zip's member (sha `991a74c034…`).

  **The encode sweep answered the backlog item.** Nine profiles at 320×240 plus two
  full-rate audio variants, all derived from one lossless intermediate, measured with
  ffprobe rather than from the command line that made them. libmpeg2's pool demand is
  **0.65 MB at any bitrate** — 76,800 coded pixels — so bitrate and frame rate are two
  independent levers: bitrate buys *buffer seconds* (one 32 KiB page = 0.87 s at 250 kb/s,
  0.47 s at the chosen profile, 0.29 s at 900 kb/s), frame rate buys *CPU* (300
  macroblocks/frame, so 25 fps = 7.5k MB/s against 15 fps = 4.5k). Chosen profile, now the
  manual's recipe: `-r 15 -b:v 500k -maxrate 650k -bufsize 2000k -g 8 -bf 0`,
  `-c:a libmp3lame -ar 44100 -ac 1 -b:a 48k -muxrate 1100k -f mpeg` → 379 MB for 91 min,
  69 kB/s sustained. 44.1 kHz matters twice: it equals the plugin's `CLOCK_RATE` so the DSP
  resamples nothing (`audio_thread.c:622` only calls `DSP_SET_FREQUENCY` when the rates
  differ), and `-ar 22050` would emit MPEG-2 Layer III frames — decodable (libmad's
  `MAD_FLAG_LSF_EXT` gives `MAD_NSBSAMPLES = 18`, i.e. 576 samples/frame, which
  `audio_thread.c:311`'s `32 * MAD_NSBSAMPLES` computes correctly) but resampled.
  Two ffmpeg gotchas found by measuring: `-muxrate` must be ~2× the combined stream rate
  (at combined+100k the muxer underflowed 103,473 times across the file, invisible in a
  90-second low-motion test), and `ffmpeg` without `-nostdin` eats the loop's stdin and
  swallows rows of a batch list.

  Two hazards that remain true:

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

  **Open, and both are upstream bugs rather than target ones:**
  - `disk_buf_init()` should refuse when the shrink loop has emptied the cache —
    `if (disk_buf.pgcount < 1) return false;` — which turns this panic into the
    "Cannot create buffering thread!" splash `stream_mgr.c:1117` already handles.
  - `libmpeg2/header.c:975-996` assigns `yuv_buf[][]` from `mpeg2_malloc()` with no check,
    and `video_thread.c:672` does not check `mpeg2_init()`. That is the reason the
    panel-sized `LIBMPEG2_ALLOC_SIZE` override was reverted (`29acf747cd`): under-sizing
    was a silent NULL write instead of a failure. Once those results are checked, sizing the
    reservation to the panel becomes a safe knob that returns roughly 1 MB of arena.

  The chapter's recipe and its reasoning are in `manual/plugins/mpegplayer.tex`.


- **Theme packs parked — the one tried renders incorrectly.** The `%T` region work
  (`DOC-GAPS.md` item 2: shuffle, repeat mode, bookmarks, lock) is still deferred to theme
  packs, but it is now blocked on something plainer: the theme tested displays wrong, so
  there is no usable rendering to test regions against. Finding a theme that draws correctly
  on this panel comes first; the `%T` mapping work after that. Not a firmware gap — the
  builtin `cabbiev2` continues to render fine.

- **Remote terminal UI over the CDC class — explored and parked, not scheduled.** Written
  up in `CDC-UI-RESEARCH.md`: a host-side TUI that mirrors menuing plus a line CLI with
  the prompt as context, both re-synced by one canonical Ctrl-L snapshot gesture. The tree
  turns out to support the idea better than expected — the serial class asks for neither
  exclusive storage nor a CPU boost (`usb_serial.c:437-438`), `button_queue_post()` is the
  documented input seam used by non-hardware sources (`button-devinput.c:306`), settings are
  addressable by cfg name with string conversion both ways (`settings.h:301-305`), the log
  ring is exported so CDC streaming can become pull-based with a ~5-line gate in `logf.c`,
  and the voice UI is already precedent for mapping screen function to a non-visual medium
  (`get_talk_id`, `talk_id(LANG_*)`). Constraints recorded there: it only works while the
  card is *not* exported (MSC parks the UI on `gui_usb_screen_run()`), pixel mirroring is out
  on bandwidth, plugins can only get raw-key forwarding, and a plugin host would need a
  `PLUGIN_API_VERSION` bump. Make-or-break: snapshot consistency against the UI thread — if
  that needs locking in shared GUI code, the idea stops being additive. Nothing was
  implemented, and the RX defects it depends on (`transfer_complete` ignores `length`, no NUL
  guarantee, `RECV_BUFFER_SIZE` 32 against a 512-byte buffer) are worth fixing on their own
  merits regardless. Reopens if hand-free operation of the device becomes worth a client.

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
| Charge target voltage | **Resolved, not deferred.** The register is REG 0x33 bits 6:5 (4.10 / 4.15 / 4.20 / 4.36 V, reset `10` = 4.20 V) — no probing was needed, the datasheet states it. Our init had been *lowering* it to 4.10 V by accident, so the port was already undercharging the cell; it now writes 4.20 V explicitly. ~4.15 V remains the cycle-life trade if you ever want it, and it is now a one-line change with the field named. | You want the lifetime trade. |
| Playback surviving a huge playlist switch | Investigated, not fixed. The scan loops already yield per entry (`apps/playlist.c:2301`, `:2637`) and hold a CPU boost, and Rockbox's priority bands are correct for playback (`codec_thread.c:662-664` `PRIORITY_PLAYBACK` boosted toward 5, `audio_thread.c:168-171` 15, playlist refresher `PRIORITY_BACKGROUND` 20) — but this port is deliberately cooperative (`thread-idf.c:121-127`, all threads at one FreeRTOS priority), so priorities only bite at yield points. What doesn't yield is `sort_playlist_unlocked()` (`playlist.c:1619`): `qsort()` at `:1625`, the linear index scan at `:1636`, and `update_control_unlocked()` at `:1643` writing the control file to a 1-bit 20 MHz card. Existing levers cover it: shuffle off, lower "Max files in playlist", keep dircache on (turning it off made large collections *more* flaky). `PCM_NUM_CHUNKS` was deliberately not raised — 64→128 takes 512 KiB from the arena. | You want it to survive the unreasonable case, or accept a core yield/clamp as its own spike. |
| `playlist.max_playlist_size` can outrun its own allocation | Latent hazard, not a bug we hit by accident. `apps/playlist.c:3652-3655` memcpy's `playlist->max_playlist_size * sizeof(*indices)` into `current_playlist.indices`, whose block was allocated in `playlist_init()` (`:1972-1974`) from `global_settings.max_files_in_playlist` **at boot**. The only clamp in the load path is `:2073-2074`, which bounds `num_indices` to the *setting*, not to that block. So raising Settings → Playback → "Max files in playlist" past the boot-time allocation makes the copy walk past the end of the block into whatever buflib neighbour follows it (playback file buffer, dircache). Raised deliberately while testing huge lists; kept to reasonable values now. Same function is also the allocation-failure hazard already documented at `:1983-1989`. | A bounds clamp in `apps/playlist.c` — a bug fix, not a feature, and worth proposing upstream. |
| IR remote input (board has a 38 kHz RX on GPIO 5, TX on GPIO 7) | Noted as a maybe-later feature, not a flag: **there is no generic IR abstraction in this tree** — `HAVE_IR_REMOTE` appears nowhere, and every remote in Rockbox is a per-target driver feeding button codes. The useful half of the board's IR hardware is the decoder side, and its output shape is exactly `button_queue_post()` (`firmware/export/button.h:66-68`), which is already public. Needs an NEC-protocol decoder on a real input path (RMT or a capture timer), so it is a feature spike, not a checkbox. | You have a remote you want to use with it. |

## 4. Deferred — permanent unless the hardware changes

- **Very large JPEGs fail to decode rather than displaying.** With the arena as sized,
  imageviewer needs the compressed file *and* the decoded output in its working buffer
  (`buf + filesize` is where the decoded images start, see `jpeg.c`), so a multi-megabyte
  photo can leave `buf_images_size <= 0` and the decoder returns `PLUGIN_OUTOFMEM`. This
  is a memory-topology limit, not a fault: it now fails cleanly and reports on screen.
  A streaming decode or a larger plugin buffer would both change it; neither is planned.

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
| Zero-intervention flashing (host writes the image, device runs it, no button) | **Closed, not deferred.** The entry half is solvable and was already built once: `RTC_CNTL.option1` bit 0 (`RTC_CNTL_FORCE_DOWNLOAD_BOOT`) plus handing the pads back to USB-Serial-JTAG plus `esp_restart()` lands in the ROM loader with no button — the removed `xt_usb_enter_download_mode()` left `rst:0xc (RTC_SW_CPU_RST), boot:0x3 (DOWNLOAD(USB/UART0))` as proof. The exit half has no solution in software at all: a host-driven reset over USB-Serial-JTAG *is* the download request, so the ROM answers `rst:0x15 (USB_UART_CHIP_RESET), boot:0x3 (DOWNLOAD)` and stays put. Measured on esptool 4.7.0 and the system build, board in download mode: `--after hard_reset` re-enters the loader (still `303a:1001` after 24 s, `1234:4002` never appears); `--after soft_reset` is refused as ESP8266-only; `run` with the stub and `run --no-stub` both end `Staying in bootloader`; a DTR/RTS matrix of all four line combinations gives either another `DOWNLOAD` or no reset at all. There is no "force normal boot" the host can ask the ROM for, because nothing of ours runs while the ROM owns the chip, and the only reset that boots normally is one whose source is not USB. Reopening this means not using the ROM loader — a device-side updater that writes its own app partition from the card and then `esp_restart()`s, which needs no buttons because no loader is involved. Rejected as out of scope: you are not going down that road again. Current workflow: `make flash` (or `esptool.py write_flash 0x0`) with the board in download mode, then one RESET or power cycle. |
| `HANDOFF.md` | Removed deliberately (`9942277b0c`); this file replaces it. |

## 5. Upstream inventory — core fixes that stand on their own

Not being submitted now. Kept so the work can be split for a merge.

| Commit | File | What | Why it belongs in core |
|---|---|---|---|
| `56681eaf5f` | `apps/gui/skin_engine/{skin_engine.c,.h,skin_render.c,skin_display.c}` | `skin_is_loaded()` guard, and "no tree" is `INVALID_OFFSET` (-1) not 0 | An unloaded skin was rendered anyway, walking a stale buffer-relative offset through a NULL-based tree. Fails identically on any target; the offset-0 case is a real allocated tree at the buffer start. |
| `9d9581a612` | `apps/audio_path.c`, `apps/SOURCES` | Guard input-source selection with `#if INPUT_SRC_CAPS != 0` | A target with a speaker but no recording/radio references `audio_enable_speaker()`, which `apps/SOURCES` only builds when `INPUT_SRC_CAPS != 0`. Shape problem, not ours. |
| uncommitted | `apps/settings_list.c` | `#ifndef DEFAULT_SPEAKER_MODE` seam for the Speaker choice default | The hardcoded default is 0 = Off, which is only correct for targets where the speaker is an extra. A weak seam defaulting to today's value; the target that needs On states it in its own config, the way `DEFAULT_TOUCHSCREEN_MODE` already does one block above. |
| `9cb4f7325a` | `tools/functions.make` + every codec `.make` | `make_archive` removes the archive before `ar` adds | `ar` only replaces members it is handed, so a stale object survives a rebuild. Generic build bug. |
| ~~`d4092053ed`~~ | `apps/keymaps/keymap-touchscreen.c` | Chain `CONTEXT_RECSCREEN` to the target keymap | **Superseded upstream**: `2b12d568ff` added `button_context_recscreen` to that switch, which supplies `ACTION_REC_PAUSE` and chains to `CONTEXT_CUSTOM2|CONTEXT_RECSCREEN` itself. The patch was dropped during the rebase — keeping it produced a duplicate case value. |
| `f270c29b11` | `firmware/rolo.c` | Use `ICODE_ATTR` instead of forcing `.section(".icode")` | `ICODE_ATTR` is deliberately null on targets low on IRAM, and the hard-coded section ignored that. |
| `18ca6599c8` | `apps/recorder/pcm_record.c` | Panic naming both sizes when `core_alloc_maximum()` grants less than `PCM_BUF_SIZE` | `core_alloc_maximum()` grants what is left, not what was asked; every index wraps at `PCM_BUF_SIZE`, so a short grant writes past the allocation into buflib's handle table. Core hardening. |
| — | `firmware/usbstack/usb_core.c` | Mask the serial-number index to a hex digit | `USB_NUM_DRIVERS` is not capped at 4, so the bitmask can exceed a single hex digit on any target. |
| upstream `51fdc7f856` | `firmware/usbstack/usb_storage.c` | (upstream's own) fail writes with an unexpected transfer length | The precedent for the `usb_serial.c` defect we still carry: `transfer_complete()` ignores `length`, and the RX buffer is 32 bytes against a 512-byte endpoint. Copy upstream's shape rather than inventing one. |
| `3630b5b65f` | `firmware/drivers/ft6x06.c` | Clamp parsed point count to the driver's storage | The register is a 4-bit field; a misbehaving panel can report more points than storage, and callers iterate `nr_points`. Any touch target. |
| `536e7cf365` | `firmware/include/fs_defines.h` | `#ifndef MAX_OPEN_FILES` / `#ifndef DC_NUM_ENTRIES` | Both were sized from `MEMORYSIZE`, i.e. storage size; a target whose DMA-visible region is tighter than its flash needs to shrink them together. A seam, additive and default-preserving. |
| `c5f40905ac` | `apps/settings_list.c` | `#ifndef DEFAULT_LIST_LINE_PADDING` | Auto row height is derived from LCD DPI; on a dense small panel that gives ~3 visible rows. Seam, default unchanged for everyone else. |
| — | `firmware/usb.h`, `firmware/usb.c` | `usb_host_session()`, overridable MSC default | The USB screen loop ignores everything but `SYS_USB_DISCONNECTED`, so a shutdown request posted while USB is live is dropped. General. |
| — | `firmware/usbstack/usb_audio.c` | Target hooks for feedback pacing and RX ping-pong (`usb_audio_target_*`), `USB_AUDIO_RX_PINGPONG` default off | Hooks let a target own the feedback-band and buffer shape without core edits. Worth showing upstream; the AC interrupt endpoint at line 290 is still an upstream TODO. |
| `1a7ecb4aee` | `firmware/drivers/rtc/rtc_pcf8563.c` | New PCF8563 driver on the generic I2C API, alarm functions under `HAVE_RTC_ALARM` | The existing `rtc_e8564.c` is PP-async-only and reads hours from the wrong buffer index. Reusable driver. |
| `7f9d1f98ce` | `firmware/drivers/i2c-async.c` | `I2C_ASYNC_SUBMIT_UNMASKED` seam defers the idle-bus submit until interrupts are restored | Already a proper opt-in seam with unchanged default behaviour. A target whose `__i2c_async_submit()` polls the controller to completion must not hold `disable_irq_save()` across a 100 kHz transfer. Worth showing upstream. |
| `6cf0d20da7` | `apps/plugins/lib/keymaps.h` | Add `CONFIG_KEYPAD != ESP32S3_PAD` to the touch-grid fallback's exclusion list | A real upstream bug, not a target quirk: the `HAVE_TOUCHSCREEN` block redefines `BTN_FIRE`/`BTN_PAUSE` unconditionally, so any touchscreen target that also has physical fire/pause keys silently gets plugins that ignore them. GCC warns about the redefinition; nobody reads the warning. Eight targets are already excluded — the list just needed another entry. |
| uncommitted | `apps/plugins/pitch_detector.c`, `manual/plugins/pitch_detector.tex` | The gate `sqr(volume_threshold * sound_max(SOUND_MIC_GAIN))` is a line-level assumption | On a mic-only target the multiplier is the gain *scale*, not the gain applied, so with a 0-30 dB mic-gain declaration the shipped threshold of 50 demands RMS 1500 (-15 dBFS) from MEMS mics and the tuner paints nothing while receiving a full stream. The plugin already has a 5-95 threshold knob and works once it is lowered, so the proposal is only that the default and the manual say so. Any mic-only target hits this; nothing target-specific about it.
| — | `apps/plugins/puzzles/SOURCES.games` | Stop building the three games under `src/unfinished/` | Upstream's own README calls them half-written or unready, and its CMake build excludes them from install, yet Rockbox ships `sgt-group`, `sgt-slide` and `sgt-sokoban`. Sokoban crashes on launch here. Removing the three lines is the whole fix. |
| `931cb55135` | `tools/rockboxdev.sh` | Strip the release tag's `esp-` prefix when composing the Xtensa archive name | Upstream-facing only if the ESP target lands upstream, but the underlying fact is worth recording: Espressif's release *tag* is `esp-14.2.0_20260121` while the published asset is `xtensa-esp-elf-14.2.0_20260121-x86_64-linux-gnu.tar.xz`, so building the file name from the tag verbatim asks for a doubled prefix and the CDN answers 404. Verified against the release API (35 assets; for this triple only `riscv32-esp-elf-14.2.0_...` and `xtensa-esp-elf-14.2.0_...`). |
| `931cb55135` | `tools/rockboxdev.sh` | `getfile_ex()` must exit non-zero when every mirror failed | A bare `exit` takes the status of the last command, and the two `echo`s above it always succeed — so a failed download printed "couldn't download the file!" and returned 0. Any build system driving this script continues with no toolchain. Affects every arch, not just Xtensa. |
| — | `tools/configure` | Include `-` in the `--target=` class when rebuilding `CONFIGURE_OPTIONS` | The five sed patterns that strip the options configure re-sets unconditionally cover `--voice=[-_./a-zA-Z0-9]*` but not `-` for `--target`, so a hyphenated target spelling is only half stripped and `make reconf` re-runs configure with a stray `-esp32s3` and dies on "Option '-esp32s3' unsupported". Only our alias triggers it today, which is exactly why the asymmetry is a bug rather than a rule. |
| `a552df3b95` | `apps/plugins/doom/i_sound.c` | `getsfx()` reads a lump once instead of twice | Upstream code: it calls `W_GetNumForName`/`S_LoadSound` and then re-reads the same lump through a second path, so a sound is present in the zone twice. On a device that is out of memory rather than out of time this is ~900 KB of duplicated effects. Any target. |
| `5a967cee39` | `apps/plugins/doom/i_video.c` | `I_InitGraphics()` releases `d_screens[0]` before replacing it | `v_video.c:387` documents that the caller owns that buffer; nothing freed it, so a second `I_InitGraphics()` leaks a full-panel buffer. Any target that re-inits graphics. |
| `8b57f077e7` | `apps/plugins/doom/f_wipe.c` | Wipe transform scratch in static storage rather than the zone | A third full-screen buffer allocated out of the game's memory pool for the duration of a wipe. Measured here: it was the difference between a wipe passing and failing at 68,384 bytes free against 76,800 needed. Device targets only care, but the fix is generic. |
| `249f58a5fb` | `apps/plugins/doom/i_system.c`, `rockdoom.c` | `I_Error()` exits through the plugin's `exit()` instead of returning | Returning from `I_Error()` returns into a NULL caller. `rb_atexit()` cleanup then still runs. Applies to every plugin that can hit a fatal Doom error. |
| new | `apps/plugins/doom/m_misc.c` | `enable_sound` default 1, matching `doomstat.c:102`'s initialiser | The table's default was 0 and `rockdoom.c` deletes `config.cfg` before every launch, so Doom was silent on every target on every start and the menu setting could not persist. One-word fix; changes audible behaviour for everyone. |
| new | `apps/plugins/doom/i_video.c` | Keypad block for `ESP32S3_PAD` and the three cells no other keypad defines | Target-specific, listed only so the doom diff is complete: the four rows above are the ones worth proposing. |
| new | `apps/plugins/metronome.c` | `tap()` clamps `bpm` to at least 1 | A tap interval above 60 s divides to 0, `calc_period()` then does `fp_div(..., bpm<<16, 16)` with `bpm == 0`, and the plugin traps out of the timer path. `change_bpm()` already clamps to [1,400]; `tap()` was the only writer that skipped it. Any target. |
| new | `manual/plugins/xworld.tex` | Four rows gated on `\opt{HAVE_TOUCHSCREEN}` retyped to `\opt{touchscreen}`, and `\ButtonBottomLeft` to `\TouchBottomLeft` | `HAVE_TOUCHSCREEN` is not a token in the manual's opt list (`touchscreen` is, and is what every other file uses), so xworld's touch rows silently never appeared; `\ButtonBottomLeft` is defined only by a few non-touch keymaps, so one of those rows would also have failed to build wherever it did render. Documentation bug, any touchscreen target. |
| new | `firmware/usbstack/usb_audio.c` | `as_playback_freq_idx` initialised to `HW_FREQ_DEFAULT` instead of 0 | `hw_freq_sampr` is in descending order, so index 0 is the *highest* rate the target declares, and `usb_audio_start_playback()` calls `mixer_set_frequency()` with it before the host's `SET_CUR` arrives. Audible as stretched playback until the request lands — milliseconds on most targets, seconds on a composite device where mass-storage enumeration shares the control endpoint. Any target. |
| new | `firmware/usbstack/usb_audio.c` | `USB_AUDIO_BUFFER_SIZE` override also sets the descriptor's `wMaxPacketSize` and the endpoint's `mps` | The driver's endpoint default is the USB maximum (1023), which a tight device-FIFO budget cannot pay for; overriding only the buffer left the descriptor promising a packet the endpoint refused to allocate. Generic shape fix, default unchanged for targets that override nothing. |
| `c3c4f0cdcb` | `apps/plugins/mpegplayer/libmpeg2/header.c`, `decode.c` | Stop on a failed `mpeg2_malloc()` instead of storing NULL | `mpeg2_header_picture_finalize()` is `void` and assigns every allocation result into `decoder.convert_id`, `yuv_buf[][]` and `fbuf->buf[]` unchecked, so any target whose arena cannot satisfy `LIBMPEG2_ALLOC_SIZE` gets a write through NULL and a crash several frames later rather than a report. It now sets `STATE_INVALID`, which `decode.c` returns before the slice decode. Core hardening, and what makes a target-tuned reservation safe. |
| new | `apps/plugins/bitmaps/remote_native/SOURCES`, plugin sources testing `CONFIG_LCD` | Two upstream `#if`s that assume a macro always exists | Found by narrowing `-Wno-undef` to the target directory, which puts `-Wundef` back on for the rest of the tree. `#if LCD_REMOTE_DEPTH == 1` appears in a `SOURCES` file that only dual-LCD targets define that for, and `#if CONFIG_LCD == LCD_SSD1815` in cube.c, clock_draw_analog.c, mandelbrot_set.c and doom/i_video.c assumes every target names its LCD controller — agptekrocker, aigoerosq, hibyr1, hidizsap80max and sansae200r define neither, so those targets emit 17 warnings today. Benign (undefined evaluates to 0 and the branch is the right one), but `defined()`-guarding them or defaulting `CONFIG_LCD` to 0 in `config.h` is a clean upstream patch. Not done here: it is upstream text and unrelated to this port. |
| new | `manual/getting_started/installation.tex`, `manual/configure_rockbox/system_options.tex`, `manual/appendix/config_file_options.tex` | Three LaTeX errors in generic manual text when built for this target | `make manual-pdf` reports "Something's wrong--perhaps a missing \item" twice in `system_options.tex` and once in `installation.tex`, plus "Float(s) lost" in `config_file_options.tex`. They survive removing this target's own `meowkits3_install.tex` section, so they are not ours; the PDF builds (3.05 MB) and the MeowKit chapters render correctly. Worth pinning down before the manual chapter is offered upstream, because a reviewer building the manual for this target sees errors. |

| new | `firmware/logf.c` | `logf_panic_dump()` gets the `__PCTOOL__` stub `_logf()` already has | `logf.c` defines `logfbuffer`, `logfindex` and `logfwrap` under `#ifndef __PCTOOL__`, and `logf.h` declares them the same way, but `logf_panic_dump()` dereferences them unguarded — so any target with `ROCKBOX_HAS_LOGF` fails a checkwps or database build on four undeclared names. Found building `--type=d` for this target; the only other targets that enable logf are three iPods, whose pctool builds nobody runs. Generic, four lines, no effect on a real build. |
| new | `apps/plugins/test_touchscreen.c` | `ESP32S3_PAD` keymap | Without it the plugin's `#error "No keymap defined!"` stops any build that enables test plugins — which is every simulator build, since `--type=s` adds `-DHAVE_TEST_PLUGINS`. |

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

### Full-diff audit against upstream `fe5c0874af` (2026-10-29)

284 files differ from upstream: 50 are the target tree, 4 are our markdown, 13 are our
tooling, 57 are manual/simulator, and the rest is core. The core part was classified
hunk by hunk rather than file by file: 1226 added code lines sit in hunks that mention
no xtensa/ESP32S3 token, across 43 files. What that pass turned up, and what happened:

| Found | Verdict |
|---|---|
| `apps/plugins/solitaire.c` — seven lines of MeowKit keymap text had landed **inside the `SONY_NWZ_PAD` branch**, duplicating that branch's five `HK_*` macros and changing its `SOL_QUIT` from `BUTTON_BACK` to `BUTTON_B`. One of the injected lines was `#define HK_REM2STACK "RIGHT"ACK`, which is not a syntax error where it is written but becomes one where the macro is expanded in the help array. | Real damage to another target, invisible to our own build. Branch restored to upstream verbatim; our block is now pure addition. Proved both ways with a synthetic harness that expands the help array for `CONFIG_KEYPAD == SONY_NWZ_PAD` — it failed before, compiles now. |
| 22 plugin files had upstream's `#error No keymap defined!` wrapped in `#if !defined(HAVE_TOUCHSCREEN)`. That `#error` sits in the chain's `#else`, so a keypad with its own branch never reaches it: the wrapper was useless for us and removed the safety net for every other touchscreen target. | Guard restored in all 22. The two plugins that actually relied on it because they had no branch — `mp3_encoder.c` and `midi/midiplay.c` — now have one (`B` cancels / `B` held quits, stick steps, `A` confirms), so nothing needs the quiet error. |
| `apps/main.c` had 11 `CHART()` calls deleted and three blank lines inserted. `CHART()` is a no-op unless bootchart is compiled in, so deleting it removes instrumentation from the targets that use it and adds nothing. | Restored; blank lines dropped, along with a target-war-story comment in `init_dircache()` that belongs in `MEMORY.md`. |
| `firmware/export/config.h` added a `CPU_XTENSA` branch under `#ifdef USE_IRAM` that remaps `ICONST_ATTR`/`IDATA_ATTR`/`IBSS_ATTR`. This target does not set `USE_IRAM` (verified: no `USE_IRAM` in `autoconf.h`), so the branch was dead in our build and reflowed three upstream lines in everyone's. | Reverted to upstream. The hardware fact it recorded (the instruction bus only carries 32-bit accesses) is already in the target notes. |
| `apps/plugins/mpegplayer/mpegplayer.h` had grown `#ifndef LIBMPEG2_ALLOC_SIZE` around the 2 MiB reservation. Nothing defines it — the override was reverted in `29acf747cd` — and the comment claimed a panel-sized target "can claim far less", which the §3 mpegplayer entry now says is wrong. | Seam reverted to upstream's plain define; the reasoning stays in `config/meowkits3.h`. |
| `firmware/usbstack/usb_audio.c` deleted upstream's `#define LOGF_ENABLE`, which changes logf behaviour for every target that has `ROCKBOX_HAS_LOGF`. The measurement behind it is real (~45 kB/s of log text through CDC at full frame rate). | Turned into an opt-out: the define stays for everyone, `USB_AUDIO_LOGF_DISABLE` in `config/meowkits3.h` turns it off here. Comment trimmed to the shipping reason. |
| Whitespace-only deletions in `manual/plugins/chess_clock.tex` (8 lines) and `manual/plugins/goban.tex` (1), and reflowed lines in `codecs.make` and `usb_audio.c`: `git diff` shows them, `git diff -w` does not. | Restored upstream whitespace. `--numstat` vs `-w --numstat` is now the first thing the audit runs. |
| `brickmania.c` defined `ALTLEFT`/`ALTRIGHT`/`ALTSELECT` in its MeowKit branch, and the file's `HAVE_TOUCHSCREEN` block redefines those three **unguarded**, so the touch cells won and the build warned. | Our three defines dropped; the fill-in block already chooses the same row. The build is warning-free now. |
| `firmware/common/unicode.c` converted an upstream `// XXX` comment to `/* */`. | Reverted. |
| `manual/plugins/xworld.tex` had four rows gated on `\opt{HAVE_TOUCHSCREEN}`. That token is not in the manual's opt list — the manual uses `touchscreen`, which is what 20 other files use — so upstream's touch rows in that table never rendered; one of them also referenced `\ButtonBottomLeft`, which is only defined by a handful of non-touch keymaps. | **Kept**: retyped to `\opt{touchscreen}` and `\TouchBottomLeft`, which `keymap-touchscreen.tex` defines. It changes rendered output for every touchscreen target, which is the point. Added to §5. |
| `manual/plugins/chip8emulator.tex` turned upstream's `\\\bottomrule` into `\\\\\bottomrule` while adding a Key-F cell. | Extra line break reverted; the added cells stay. |

Verified afterwards, on the final tree: the xtensa target builds with no errors and no
macro-redefinition warning of our making, and the simulator builds clean, which is the
other side of every seam. One redefinition warning predates this audit and is not ours to
fix: `firmware/common/strnatcmp.c:43` defines `assert` away, and on an IDF-backed build
the toolchain's own `assert.h` is reachable through the firmware include path, so GCC
reports the redefinition. The file uses no `assert()`; the stub is belt-and-braces, and
silencing it would mean editing a shared file to suit one toolchain's include layout.
A synthetic keymap harness compiled **3516 (keypad, touch)** combinations across the 32
touched plugin files that have a keymap chain — 0 cases where upstream compiled and our
tree did not. Payload rebuilt (`rockbox.zip`, 9,894,906 B), image re-verified with
`make espverify`.

What the audit deliberately left alone, because each is a generic fix or a weak seam
whose default is upstream's: `make_archive` in 43 archives, the `BASE_CFLAGS` /
`FREESTAND_CFLAGS` domain split in `tools/root.make`, `mkdepfile_order` in
`tools/functions.make`, `> TEXT_REGION` in `plugin.lds` (the macro maps back to
`PLUGIN_RAM` on every other arch), the explicit per-game rules in `puzzles.make`,
`PUZZLES_SKIP_UNFINISHED`, `DEFAULT_SPEAKER_MODE` / `DEFAULT_LIST_LINE_PADDING`,
the `run_tick_tasks()` split in `tick.h`, the `skin_is_loaded()` guard, the ft6x06
point clamp, the metronome `bpm` clamp, the `usb_core.c` serial-index mask, the
`sdmmc_host.c` `present` guard, `rolo.c`'s `ICODE_ATTR`, and the doom fixes. All are in
§5 as upstream candidates.

## 7. Tooling deliberately untouched, and the adoption hygiene that is maintained

Not touched: `utils/rbutilqt/rbutil.ini`, the build-info server entry, the themes-server
target entry (`meowkits3` returns "No themes available"; `tools/meowkits3/rbthemes.py` pulls
through a proxy target instead). Verified `git diff upstream/master -- utils` is empty.

One obstacle to that entry was on our side and is gone: rbutil's RockboxLoader expects
a port to leave `rockbox.elf` and `rockbox.map` in the build directory, which is what
`make elfzip` and `make mapzip` read. ESP-IDF owns the link and writes both beside its
own build tree, so both goals found nothing and the map guard had to be pointed at
`$(ROOTDIR)/idf-app/build/` by hand — which broke on an out-of-tree build. The image
rule now copies both into `$(BUILDDIR)`, so the goal names behave the same as on every
other port and `ESPLELFMAP` needs no source-root derivation. Release packaging is a
Rockbox goal too (`make espartifacts`: app image, bootloader, partition table, IDF's
`flash_args`, the merged 0x0 image, `image_info` output, map and ELF), so a build farm
or a release script does not have to know IDF's layout. Neither change touches
`utils/`.

Maintained as if adoption were intended: `tools/configure` entry `133|meowkit-esp32s3`
with `target_id=126`, `MODEL_NUMBER 133` (unique then — upstream later assigned 133 to
`agptekrockernative`, so this target moved to 134 during the rebase), `tools/builds.pm` (`status => 2`,
`sim => 0`, `manualok => 1`), `list_targets.pl` derives from `builds.pm`, manual
platform/keymap wiring, simulator buttonmap and UI bitmap.

## 8. Artifacts and build

### Cache, loader and overlay fixes (2026-10-30)

Six commits, each independently reviewable, on top of `07a3f0bdbb`:

| Commit | What | Evidence recorded in the message |
|---|---|---|
| `d3c0344b2c` | `commit_dcache()`, `commit_discard_dcache()`, `commit_discard_idcache()` implemented out-of-line on the ROM whole-cache operations | The `xthal_*` entry points disassemble to `entry a1, 16; retw.n` in a linked image — empty stubs. The new `commit_discard_idcache` disassembles to three `callx8` to `0x400016f8`, `0x400016d4`, `0x400016e0`. |
| `6a3ea0a6a9` | `lc_open()` returns the `.header` section address, found through the section table; `XT_EHDR_READ_SIZE` grows 46 → 52 to reach `e_shnum`/`e_shstrndx`; the placed extent is checked against the caller's buffer | Across 163 built artifacts, and again across all 207 ELF images in the zip, `.header`'s `sh_addr` equals the `load_addr` the image declares and lies inside its segment. Zero mismatches. |
| `9fe12233af` | Decoder overlays get their own text slice (`XT_IMGDEC_TEXT_SIZE`), plugin text capped so `ld` rejects an oversized plugin, and `esp-layout-check` reads the built artifacts and fails the build on overlap | Guard verified discriminating: swapping `imageviewer.rock` in as the decoder makes it exit 1, restoring makes it exit 0. Measured placements: decoder text `0x3c600000..0x3c601eb2`, slice `0x40000`, highest plugin text `0x3c5e9674`, codec text `0x3c640000`. |
| `668c225b50` | Rockbox's own `-Os` restored; libc feature macros and `CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY=0` named in the target makefile | Image 1,307,056 → 1,256,368 B (−3.9%), warning profile unchanged. |
| `f4fb980c0b` | Coulomb anchors become `int`, matching the counters they anchor | The guard asks whether a signed counter went backwards; unsigned cannot express it. |
| `d0e28cbb31` | Seven comment continuations that opened with `/*` instead of ` *` repaired | Claimed zero `-Wcomment` on a clean rebuild; that claim was wrong — the detector only matched continuations at column 0. Corrected by `4386878ef7`. |
| `c6f778af4f` | Image cache maintenance confined to the code pages: write back only the text segment, invalidate only its instruction lines; range ops stop calling cache code for uncached internal SRAM | Bisect: the codec-switching soak clean on `07a3f0bdbb` faulted on whole-cache maintenance. Plugin load `sync` 38k–73k cycles (was 2.5M–6.7M); `wb 1 srv 1` from the ROM's own answers. Recording stutter and the recording→playback abort gone. |
| `4386878ef7` | The three indented comment continuations the first detector missed | Clean rebuild now genuinely reports zero `-Wcomment`. |

Post-series measurements, from a `make clean` build with `CCACHE_DISABLE=1`:
image `1,256,768 B`, `espverify` valid, `esp-layout-check` exit 0, warning classes
`-Wchar-subscripts` 35, `-Wundef` 17, `-Wparentheses` 2, `-Wmisleading-indentation` 1,
`-Wcomment` 0, `-Wsign-compare` 0, errors 0. `make zip` 7,611,422 B.

The simulator was re-checked in a **newly created** build directory rather than an
already-built one (`--target=meowkits3 --type=s`, `make -j8`, zero errors,
`rockboxui` 7,645,520 B) — the Phase C claim had been made in a warm directory and
the `IMAGE_LINK` polarity fix in `0d1f930a53` had shown that a warm directory hides
a link failure. The fresh directory is deleted after the check; `build-sim-meowkits3`
remains the standing one.

Hardware status after `c6f778af4f` (flashed `1,257,232 B`, md5 `4b23cc20…`):
JPEG and PNG render, codecs play and switch, recording improved, and the
recording→playback abort is gone. The sweep this change demanded has been run —
codecs, imageviewer, recording, doom, video. The one remaining fault is the
parked item above.

### Phase D — the SDK dependency becomes an app_type (2026-10-30)

`app_type="esp-idf"`, `idficc()`/`idf_sdk_setup()` in `tools/configure`, a target
makefile at `firmware/target/xtensa/espressif_esp32s3/idf.make`, and `idf-app/`
moved out of the repository root into the SoC directory. `tools/root.make` no
longer contains any xtensa or IDF logic: the diff is a dispatch entry, two
hook variables (`DEP_SRCS ?=`, `DEPSCAN_EXTRA ?=`), the `BASE_CFLAGS` split, and
wording in the two-domain note.

The gate was measured against a worktree at the pre-change commit, not asserted:

| Check | Result |
|---|---|
| object set | 1259 objects both sides, `only-old` empty |
| section sizes | every object's non-string sections byte-identical; the only deltas are strings containing `__FILE__` (the worktree path is shorter) and `rbversion` (different `git describe`) |
| image | 1,307,856 B, same 7 segments, same lengths, layout guard passes, `espverify` checksum and hash valid |
| payload | 420 zip entries, `diff` of the two entry lists empty; `flac.codec` 17,440 B and `pong.rock` 10,384 B unchanged, load addresses unchanged |
| warnings | 17 `-Wundef`, 2 `-Wparentheses`, 1 `-Wmisleading-indentation` — same as before |
| host builds | checkwps 195,016 B, database 949,568 B, `meowkits3 --type=s` `rockboxui` 7,645,176 B, `ondavx747 --type=s` 7,455,912 B; none of them asks for the SDK |
| board | flashed 2026-10-30 12:13 and booted: 1,307,840 B written at `0x10000`, "Hash of data verified", partition table at `0x8000`; after RESET the host enumerated `1234:4002 Brain Actuated Technologies Rockbox media player` with `ttyACM0` on interface `3-1.1:1.0` |

Three real differences the comparison caught, all of them consequences of
`APP_TYPE` no longer being empty, and all fixed by `IMAGE_LINK`:

- `apps/bitmaps/bitmaps.make` and `apps/plugins/bitmaps/pluginbitmaps.make` add
  `-fPIC` under `ifdef APP_TYPE`. On a native image that moves every bitmap struct
  from `.rodata` into `.data.rel.ro` — 81 objects, and the shipped image linked
  with them for a while. The old tree's objects are `.rodata`; so are the fixed
  ones.
- `apps/plugins/pdbox/pdbox.make` adds `-lm` to the plugin link for hosted targets.
- `apps/plugins/imageviewer/imageviewer.make` picks the overlay linker-script set
  only when `APP_TYPE` is empty, and `tools/root.make` built Rockbox's `setjmp`
  archive the same way. The latter failed at parse time, not link time:
  `lib/libsetjmp/SOURCES` is preprocessed before `firmware.make` puts
  `firmware/export` on the path, so it printed `config.h: No such file or
  directory` and silently yielded an empty file list. The only visible symptom was
  a missing `lib/libsetjmp.a`.

**A follow-up fix, and a correction to §Phase C.** Three of those gates —
`imageviewer.make`, `zxbox.make`, `sdl.make` — were written with inverted
polarity (`ifndef IMAGE_LINK` where the upstream condition was `ifndef APP_TYPE`).
The effect was twice wrong: the device lost the per-decoder linker script on its
imageviewer overlays, and a *fresh* simulator build added a `.link` prerequisite
whose rule ran `-T -Wl,--gc-sections` against an undefined `PLUGINLINK_LDS` and
failed with "cannot open linker script file". Phase C's "simulator builds clean"
row below was measured in a directory that had already been built once, so it did
not exercise that path — the claim was wrong as written. `0d1f930a53` fixes the
polarity and re-verifies against a worktree at `3064dce1df`: all 208 plugin,
codec and overlay artifacts match in size, `bmp.ovl`/`jpeg.ovl` have identical
LOAD segments, a fresh `--type=s` build completes, and checkwps and the device
image are unchanged.

Where the plan and the execution diverged:

- **No `PLATFORM_ESP_IDF` bit.** The platform bits describe the runtime OS a
  target runs on (android, hibyos, SDL); this target is native in every sense that
  those bits gate. `HAVE_SYSTEM_LIBC` also stays a makefile-domain define rather
  than moving into `config/meowkits3.h`: a config header is seen by the
  freestanding domain too, and codecs and plugins must not be told they link a C
  library (`firmware/include/file.h` would reach `<unistd.h>` and the plugin link
  has no libc at all).
- **`GCCOPTS` is not rebuilt.** The plan borrowed `simcc()`'s trick of dropping
  `-nostdlib -ffreestanding`. Those are link-time options as far as Rockbox is
  concerned here — IDF does the linking — but the codec and plugin links are real
  freestanding executables, and dropping them makes those links pull the
  toolchain's `crt0.o` and collide with `lib/rbcodec/codecs/codec_crt0.c`.
- **`idf.make` is included before the subdir makefiles**, not in the `app_type`
  dispatch chain. A `SOURCES` file is a C preprocessor program evaluated while the
  makefiles are read, and `apps/rbcodecconfig.h` reaches the platform headers, so
  the SDK flags have to exist before `apps.make` runs. `.SECONDEXPANSION:` is
  declared inside `idf.make` for the rules that need `$(OBJ)`, which is how
  `sonynwz.make` solves the same problem.
- **`espflash.make` stays in `tools/meowkits3/`.** `tools/echoplayer/openocd.make`
  is the precedent for a flashing makefile, and the host scripts (`idf-flags`,
  `idf-seedcheck`, `esp-layout-check`, the theme and Docker tooling) belong with
  it. Note that upstream also has a `utils/` tree (`utils/jztool`, `utils/hwstub`);
  `tools/<target>/` was kept because `tools/` is where upstream puts target host
  tools too.
- **`thread_support` needed no change.** `ASSEMBLER_THREADS` is already correct:
  `firmware/asm/xtensa/thread.c` is the arch context switch, `thread-idf.c` is the
  target's OS binding, and that split is upstream's shape.

`sdkconfig.defaults` gained `CONFIG_ESPTOOLPY_BEFORE="no_reset"` and
`CONFIG_ESPTOOLPY_AFTER="no_reset"`. The serial port is the chip's own USB-CDC,
which has no DTR or RTS wired to the boot straps, so neither reset can be pulsed.
`--before default_reset` could only ever fail, and the first Phase D flash showed
the other half: esptool wrote the image, printed "Hard resetting via RTS pin", and
left the chip in the download bootloader — the host still saw `303a:1001 Espressif
USB JTAG/serial debug unit` until RESET was pressed. Flashing now expects download
mode and leaves the board there for a manual RESET.

### Phase C — simulator (2026-10-30)

`--target=meowkits3 --type=s` builds and runs, and so do `--type=c` and `--type=d`.

| Build | Result |
|---|---|
| native (`--type=AV`) | 1,307,856 B, unchanged; segments 4/5/6/7 byte-lengths identical to pre-Phase-B (`0x04904`, `0x13a9c`, `0x00020`, `0x00058`); layout guard passes; `espverify` hash valid |
| simulator | `rockboxui` 7,645,576 B; runs headless under `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy`, alive until the 8 s timeout killed it; root dir, SDL 2.32.10 and the audio driver list all initialised |
| checkwps | `checkwps.meowkits3` 195,016 B |
| database | `database.meowkits3` 949,576 B (needed the `logf.c` fix below) |
| control | fresh `--target=ondavx747 --type=s` builds clean, which is what proved the sim failures were ours |

The mechanism: `tools/root.make` now computes `XTENSA_IDF` from the artifact — `CPU=xtensa`
**and** a `rockbox.<chip>` output — and every SDK block hangs off it instead of off the CPU
name: `-include idf.make`, the firmware-domain `CFLAGS`/`PPCFLAGS`, `IDF_FIRST_CFLAGS`, the
separate dependency scan, the `idf.make` rebuild trigger, the link fork and the flash
makefile. `firmware/firmware.make`'s include-order split and `XTENSA_IDF_FIRST_*` do the
same. `firmware/SOURCES`'s two xtensa blocks carry
`&& (CONFIG_PLATFORM & PLATFORM_NATIVE)`, which is how upstream already writes the
`CPU_ARM` branch. A host build therefore never sees IDF's response-file flags
(`-mlongcalls`, `-mdisable-hardware-atomics`) or IDF's `endian.h`, and the sim's own
`TARGET_INC` (hosted/sdl, hosted, plus our dirs) resolves normally.

Two fixes the earlier probe could not have surfaced:

- `es8311.h` moved from the board directory to `firmware/export/`, because every codec
  header `audiohw.h` can include lives there upstream (`as3514.h`, `cs43131.h`, `es9218.h`, `pcm1792.h`,
  `eros_qn_codec.h` — all eleven of them) and a tool build's `TARGET_INC` has no board directory — the dependency scan then
  recorded a bare `es8311.h` with no rule. It also gained `#include <stddef.h>` for
  `es8311_stat()`'s `size_t`.
- `firmware/logf.c`: `logf_panic_dump()` used the logf buffer, index and `MAX_LOGF_SIZE`,
  all of which are `#ifndef __PCTOOL__`, so `--type=d` failed. Now it has the same
  `__PCTOOL__` stub pair `_logf()` already uses. This is a core fix — it breaks any target
  with `ROCKBOX_HAS_LOGF`, and this port is the first one to hit it because the only other
  logf targets are three iPods nobody builds tools for.

`tools/builds.pm` says `sim => 1`, replacing the comment that no simulator could exist for
this model — that was true of the gates, not of the port. `tools/list_targets.pl` lists the
target in both tables.

Not done: the interactive checks. See §1.

### Phase B — target tree restructure (2026-10-30)

`TARGET_INC` no longer names two directories that do not exist.

```
firmware/target/xtensa/                         cache-xtensa.[ch]  paniclog.[ch]
                                                thread-idf.c
firmware/target/xtensa/espressif_esp32s3/       system-target.h  system-esp32s3.c
  gpio-esp32s3.h  i2c-esp32s3.c  i2s-esp32s3.[ch]  spi-esp32s3.[ch]
  usb-esp32s3.c   sd-esp32s3.c   timer-esp32s3.c   pcm-esp32s3.c
  adc-esp32s3.c   debug-esp32s3.c lc-esp32s3.c   memory-esp32s3-syms.S
firmware/target/xtensa/espressif_esp32s3/meowkits3/
  button-  lcd-  backlight-  audiohw-  power-meowkits3.c
  button-target.h  lcd-target.h  backlight-target.h  adc-target.h  i2c-target.h
  st7789.[ch]  pca9557.[ch]  es8311.[ch]  es7210.[ch]
```

`tools/configure` sets `t_manufacturer="espressif_esp32s3"`, `t_model="meowkits3"` —
the `ingenic_x1000/agptekrocker` shape, where the manufacturer slot carries the SoC.
34 files moved; `-xtensa` became `-esp32s3` for register-level code and `-meowkits3`
for board code; the SDHOST driver lost `idf` from its name because that name said which
SDK it talks to rather than which peripheral it drives.

Numbers after the move:

- Arena symbols unchanged: `codecbuf` `0x3c680000`, `pluginbuf` `0x3c780000`,
  `loadaddress` `0x3c800000`, `.ext_ram.bss` ends `0x3c5b3244`, plugin text reserved at
  `0x3c5c0000`. `flac.codec` loads its RW half at `0x3c680000` and its text at
  `0x42640000`; `pong.rock` at `0x3c780000` / `0x425c0000` — identical to before.
- IRAM segment still `0x13a9c` at `0x40374000`; RTC segments unchanged.
- Image **byte-for-byte the same size** as before the move: 1,307,856 B. Comparing the
  pre-move objects against the post-move ones section by section — 21 pairs — found **no
  difference in any non-debug section**: no `.text`, no `.rodata`, no `.data`, no `.bss`.
  Only `.debug_*` differs, because debug info carries source paths. The validation hash
  moves regardless, because IDF's `esp_app_desc` at the head of the app partition embeds
  the version string (`v4.0-final-2683-g39d9de21a0-dir`) and the build timestamp.
- Which matters because the first attempt at this gate *did* change code. `firmware/
  firmware.make`'s `XTENSA_IDF_FIRST_SRC` still named the old paths for `sd-` and `spi-`,
  so those two objects silently lost IDF's include order; the build passed anyway and
  `.text.sd_init` grew 21 B with `.text.spi_init` 25. The mistake was in a build input
  that names files rather than a rule — the same class as `TARGET_INC` pointing at
  directories that don't exist — and it surfaced only because the rename was followed by a
  sweep for the old names, not because the build complained. The sweep also corrected three
  archive-member selectors in `idf-app/main/rockbox_memory.lf` (`memory-`, `pcm-`, `i2s-`),
  which had silently stopped matching and were falling through to the `* (rb_dram)`
  wildcard — harmless today because the wildcard gives the same placement, but a trap for
  whoever next tries to pin one of them.
- Build warnings still 17, all upstream. `-Wno-undef`'s target-directory scoping still
  works after the move: `$(BUILDDIR)/firmware/target/xtensa/%.o` matches the new
  subdirectories.
- Simulator build: `build-sim/make.dep` still referenced
  `lib/rbcodec/codecs/libgme/inflate/mallocer.h`, a header upstream deleted during the
  rebase, so the sim died with "No rule to make target". Deleting the dep file fixed it;
  the sim now builds clean. Worth remembering — a stale build directory can report a tree
  problem that isn't one.

**Confirmation flash.** `rockbox-flash.bin` 1,373,392 B written at `0x0` on
2026-10-30 ("Hash of data verified"), and after RESET the host enumerated
`1234:4002 Rockbox media player` with `ttyACM0` on interface `3-1.1:1.0` — the
composite descriptor came up, so the restructured tree runs on the board exactly
as the flat one did. Mass storage is not attached because this build's default
USB mode is charge-only; holding a button while plugging in flips it, as the
manual says.

### Phase A — conformance cleanup (2026-10-30)

Five commits, no behaviour change by design:

| Commit | What |
|---|---|
| `df30667bc2` | `xtensa_breadcrumb*()`/`xtensa_panic_log()` became `dbg_breadcrumb()`/`panic_console_write()` behind `HAVE_DBG_BREADCRUMB`/`HAVE_PANIC_CONSOLE`; target-internal crash-record readers moved to `paniclog.h`; `firmware/include/xtensa.h` → `firmware/export/xtensa.h` with the memory layout absorbed from `memory-xtensa.h`, which is deleted |
| `e3af37a0a9` | `system-target.h` stops including IDF headers; new `cache-xtensa.c` is the only object that includes the platform cache HAL; `-Wno-undef` scoped to `firmware/target/xtensa/` and dropped from `PPCFLAGS` |
| `2727bfb824` | 1745 comment lines rewrapped to 80 columns; `test_touchscreen.c` keymap for `ESP32S3_PAD` |
| `358e488b8a` | board tooling moved to `tools/meowkits3/` |
| `6940c09bd4` | target `README.md` + `TESTING.md`; working notes moved to `notes/`; user facts added to the manual install chapter |

Numbers, all measured on this tree:

- `rockbox.bin` 1,307,856 B (`0x13ec40`), 7 segments, validation hash
  `704284c94a463a7a…0c9a4a50`. The hash is *identical* across the tooling move and
  the comment reflow, which is the check that those two commits changed nothing.
- `rockbox-flash.bin` (bootloader + partition table + app at `0x0`) 1,373,376 B.
- Layout guard passes: `.ext_ram.bss` ends at `0x3c5b3244`, plugin text reserved at
  `0x3c5c0000`, `codecbuf` `0x3c680000`, `pluginbuf` `0x3c780000`, arena top
  `0x3c800000` — the same numbers `plugin.lds` produces, verified from
  `flac.codec`'s and `pong.rock`'s program headers as well.
- Card payload `rockbox.zip` 9,888,811 B, rebuilt after the last commit.
- Manual PDF built from the rebased tree: `build-meowkits3/rockbox-manual.pdf`,
  3,056,519 B. **This closes the "manual PDF has to be rebuilt before a tag" item.**
- Build warnings: 17, all upstream (`LCD_REMOTE_DEPTH` in a bitmaps `SOURCES` file,
  `CONFIG_LCD` in four plugin sources). Before this phase it was 683, muted by a
  global `-Wno-undef`.
- Objects reaching an IDF API header: 16, all under `firmware/target/xtensa/`.

**Gate flash.** `rockbox-flash.bin` (1,373,376 B) written at `0x0` on 2026-10-30:
"Wrote 1373376 bytes (670970 compressed) … Hash of data verified", `--after
no_reset` so the chip stayed in the ROM bootloader for the RESET press.

**Boot confirmed.** After RESET the host sees
`1234:4002 Brain Actuated Technologies Rockbox media player`, and `ttyACM0` is
bound to that device's interface `3-1.1:1.0` — so Rockbox is running, its
composite descriptor (mass storage + CDC + HID) enumerated, and the console is
the CDC port. The console printing nothing when read is normal: it emits on
events, not as a prompt, and the boot trace had already gone by the time the
port was opened.

One environment fact that cost a failed attempt: the **system `esptool` cannot
write to an ESP32-S3 at all**. `/usr/lib/python3/dist-packages/esptool` is 4.7.0
but has no `targets/stub_flasher/stub_flasher_32s3.json`, so chip detection
succeeds (`Detecting chip type... ESP32-S3`, MAC read fine) and then
`run_stub()` dies with `FileNotFoundError` pointing inside dist-packages. The
IDF venv copy works — its stubs live under `targets/stub_flasher/1/` and `2/`:

```sh
~/.espressif/python_env/idf5.5_py3.14_env/bin/python -m esptool \
    --chip esp32s3 --port /dev/ttyACM0 --after no_reset write_flash 0x0 rockbox-flash.bin
```

`export.sh` does not shadow `/usr/bin/esptool`, so `command -v esptool` finds the
broken one either way. `make espverify` is unaffected because `image_info` needs
no stub — which is why the defect only shows up at the moment of writing.

### Rebase onto upstream `master` (2026-10-29)

`esp32-s3-idf` now sits on `fe5c0874af` (upstream tip, 2026-10-28): **651 commits replayed,
257 upstream commits absorbed, branch 651 ahead and 0 behind**. `backup/pre-rebase-20261029`
is the pre-rebase tip, pushed to origin, and the release tag `esp32-s3-idf-20261029` still
points at the pre-rebase tree — deliberately, because that is the tree the flashed image
came from.

The mechanics: `rerere` on, and a helper that auto-resolved only hunks where both sides
*append* to the same base and the appended branch conditions don't collide. That covered the
dominant shape — upstream's psp port, the fiiom3klinux removal, the frotz removal and the
RNG series all edit the same `#elif CONFIG_KEYPAD == …` chains where the `ESP32S3_PAD`
blocks live — and left nine to be resolved by hand: `tools/configure`, `tools/root.make`
(twice: the `MKFIRMWARE_TOOL` dependency and the IDF-backed link), `lib/rbcodec/codecs/codecs.make`
(the uniform `-O2` baseline against `FREESTAND_CFLAGS`), `apps/settings_list.c`
(`F_THEMESETTING` landed on the very line carrying `DEFAULT_LIST_LINE_PADDING` — both kept,
so our pinned value is now a theme setting's initial value rather than a fixed one),
`firmware/export/config.h`, `firmware/export/audiohw.h`, `apps/main.c`, `apps/plugins/lib/keymaps.h`
and `apps/plugins/battery_bench.c`.

**What actually went wrong was invisible in the diffs.** Three identity collisions, each of
which merged cleanly and each reported only by the build or by target identity:
`ESP32S3_PAD 81` collided with upstream's new `PSP_PAD 81` (the build picked `keymap-psp.c`
and failed on buttons the board has never heard of), `RTC_PCF8563 28` collided with the new
`RTC_X1600 28` (`firmware/SOURCES` picked `rtc_ingenic.c` and failed on a generated header
with no rule here), and `MODEL_NUMBER 133` collided with `agptekrockernative`'s 133 — the
one that would not have failed a build, only made Rolo, the boot loader and rbutil see two
targets as one. Fixed to 82, 29 and 134. The lesson, recorded in AGENTS.md: after a rebase,
run the duplicate-value audit over the identity lists *and* build two targets, because a
merge cannot see a number that two names share.

Also: upstream's `2b12d568ff` added `button_context_recscreen` to the touchscreen context
switch, which is exactly the gap our `d4092053ed` patched — our chain is dropped, and §5's
row for it is marked superseded.

**Post-rebase state, measured:** xtensa firmware links through IDF (image 1,307,856 B, was
1,293,296 B), `esp-layout-check` passes with `.ext_ram.bss` ending 55 KB below the plugin-text
reservation, `make rocks` builds **157** rocks (was 158 — upstream removed frotz) plus 63 Lua
plugins, `make zip` produces 420 files with **44** codec entries (upstream added the DSD/DSF
codec, unconditional in `codecs/SOURCES`), and the SDL simulator configures, compiles and links
from the same tree, which is the check that backs the "weak seams only" claim.
`PLUGIN_API_VERSION` went 285 → 286 upstream, so the shipped rocks are rebuilt and any older
`.rock` on a card will be refused.

**Open, in order:**
  1. ~~Soak the rebased build~~ — **passed 2026-10-29** on the flashed image
     (`rockbox-flash.bin` from `build-meowkits3/esp-artifacts/`, checksum and validation hash
     verified, embedded version `960b37067f-261029`). Upstream now builds every codec at a
     uniform `-O2` baseline where they previously had per-codec levels, and codec code
     executes out of PSRAM; the formats that changed level were played and nothing failed or
     stuttered. FLAC and MP3 were already checked at boot.
     Two things the flash surfaced: a card populated from the pre-rebase zip makes **every**
     plugin report "incompatible version", because `plugin.c:986` matches `api_version`
     exactly and upstream bumped 285 → 286; and the rebuilt zip initially shipped 158 rocks
     instead of 157 because `buildzip.pl` globs the build directory and a removed plugin's
     stale `.rock` was still there. Both are in AGENTS.md, and the fix for the payload is
     `rm -rf <builddir>/apps/plugins && make rocks && make zip`. The canonical card zip is
     back at `build-meowkits3/rockbox.zip` — 9,894,915 B, 398 members, version matching the
     flashed image. Counted from the archive rather than asserted: 44 `.codec`, 157 `.rock`,
     63 `.lua`, 48 `.lng`, 2 `.talk` plus one voice pack, 41 WPS/theme/backdrop files. The
     `422 files` figure this entry previously carried was a count of something else and has
     been replaced by the zip's own member list.
  2. **DSD: DSD64 good; DSD128 is CPU-bound and is left as a documented limit.** Observed 2026-10-29 with
     `1979 - Pink Floyd - The Wall [DSD128]` (26 DSF files): it plays at the right rate but
     **gaps every few seconds to catch up on reads**. That is an underrun, not a pitch or
     clock fault, and the arithmetic says which producer is short:

     | format | bits/s/ch | channels | sustained read demanded | observed |
     |---|---|---|---|---|
     | DSD64 (Oasis) | 2,822,400 | 2 interleaved | **0.71 MB/s** | plays clean |
     | DSD128 (The Wall) | 5,644,800 | 2 interleaved | **1.41 MB/s** | gaps every few s |
     | DSD256 | 11,289,600 | 2 | 2.82 MB/s | above the 1-bit bus, unreachable |

     The same file doubles both costs, so the two candidate limits move together: the SD
     path (`sd-esp32s3.c`, 1-bit, 20 MHz card clock, theoretical 2.5 MB/s) and the CIC
     decimator (11.3 M bits/s through a 3-stage 64-bit integrator/delay loop, code resident
     in PSRAM). The gap *pattern* — play, run dry, refill in a burst — is the signature of a
     producer under real time on average rather than a one-off stall, and the periodicity is
     what an extent boundary or a fixed-size read-ahead refill would look like.

     **What I measured, and what it does not prove.** Reading 32 MB of four different DSD128
     files off the card through MSC, each from an uncached region, gave 1.05, 1.03, 1.04 and
     1.04 MB/s at 4 KB, 64 KB, 512 KB and 1 MB read sizes. The rate is **flat with
     granularity**, which is a hard cap rather than per-command overhead — but the cap could
     be USB full-speed bulk (12 Mbit/s, ~1.0-1.2 MB/s in practice), so this bounds the
     *combined* card+MSC path and says nothing about the card alone. Do not read it as
     "the card does 1 MB/s". I also wrote earlier, in this file, that sustained card read is
     "only 0.71 MB/s" — that figure was the DSD64 file's demand, not a measurement, and it
     was wrong to state it as one.

     **Settled by you on the bench: it is CPU-bound, not the transport.** The card is not
     the limit; the CIC decimator cannot keep 11.3 Mbit/s in real time on this core with the
     codec executing from PSRAM. That makes **DSD64 the practical ceiling for DSD on this
     target** and DSD128 playable-but-gapping, which is a limitation of decode throughput
     rather than a bug to chase. Two measurements would have settled it, both small, and are
     kept here only as the method for whoever reopens this:
     - **An on-device SD throughput row.** Time a multi-MB read into a scratch buffer from
       `Debug → View HW info` and print MB/s. No USB in the path, so it bounds the card
       directly, and it is reusable for every future rate question.
     - **Split the codec thread's time in `dsd.c`.** Accumulate ms spent in `read_exact()`
       versus ms in the filter loop, print one line per second over CDC. If reads dominate,
       the transport is the limit and the levers are bigger multi-block transfers
       (`sdmmc_host.c:848` `max_nr_blocks`) or a larger read-ahead; if the filter dominates,
       it is CPU and the lever is the decimator itself.

     Cheaper, no rebuild: play a DSD128 track with the screen off and EQ/replaygain off. This
     is the discriminator that was used, and it pointed at the core.

     Still open on the container side: a `.dff` (the only path in this codec that reaches
     `dst_decode()`), and whether any DSF declares the field at header offset 60 as something
     other than 1 — all 30 files here say 1, while their payloads are exactly twice the
     single-channel size, so `dsd.c`'s reading of that field as `lsb_first` and its fixed
     two-block-per-iteration interleave happen to agree with what these encoders wrote. The
     gate at `parse_dsf` accepts only 1 and 8 there, so a file that used it as a channel
     count would be refused. Unverified either way.
  3. ~~Arena row after Opus and M4A~~ — **passed 2026-10-29.** The upstream opus buffer rework
     (allocate at their limits) and the m4a 512 KB seeking rework raise peak codec demand
     against our fixed codecbuf/pluginbuf/arena; both formats play without an allocation
     failure. No `INFO_ARENA` figures were written down, so this is recorded as an
     observational pass rather than a measured one — if a codec ever reports
     `BUFLIB_ERRCODE_MEMBLOCK_ALLOC_FAILED`, that row is where the number lives.
  4. **A new tag from the rebased tree is unblocked** — items 1 and 2 are both run. Still
     worth having first, because the release notes quote battery figures: the charge leg from
     ~30% (confirms the 4.20 V ceiling fix) and the discharge leg to shutdown (replaces the
     datasheet's 1400 mAh). The manual PDF also has to be rebuilt from the rebased tree.


### Release `esp32-s3-idf-20261029`

Tag at `260c70d53f00a8a82d8ac19a7529c3f89c96edea`, carrying its own notes at
`firmware/target/xtensa/notes/releases/esp32-s3-idf-20261029.md`. Supersedes the 20261027 tag:
27 commits of measured fixes — the ES7210 PGA, the coulometer scale and the percent that
now comes from it, the four doom allocations, the doom keymap and its sound default, and
the build-rule work. Built in the container from a fresh shallow clone of the tag, tree
clean, embedded version `260c70d53f-261029`, so a receiver can match an artifact to this
commit from the device's own About screen. `espverify.txt` reports seven segments with a
valid checksum and validation hash, and `tools/meowkits3/esp-layout-check` passes: `.ext_ram.bss`
ends at `0x3c5b3214`, 55 KB below the plugin-text reservation at `0x3c5c0000`.

| File | Size | SHA-256 (first 16) |
|---|---|---|
| `rockbox-flash.bin` | 1,358,832 B | `a75a4f5f3b893e6a` — all-in-one image at `0x0` |
| `rockbox.esp32s3` | 1,293,296 B | app image |
| `rockbox.zip` | 9,877,784 B | 412 files: 43 codecs, 158 plugins + 63 Lua, 48 languages, one voice pack |
| `rockbox-fonts.zip` | 8,478,936 B | 88 fonts |
| `rockbox-manual.pdf` | 3,050,846 B | 286 pages |
| `rockbox.elf` | 11,480,148 B | shipped so a console crash address resolves without a rebuild |

**The provenance bug this release found.** The first container run produced artifacts whose
manifest said `commit=unknown`, `dirty=0` and whose image embedded `-261029` — an empty
hash. The cause is not the script: git refuses to read a repository owned by a different
uid, and a host directory mounted into a container is exactly that, so `rev-parse` failed
with "detected dubious ownership" and `tools/version.sh` had nothing to embed. Nothing
failed loudly; the artifacts were simply unattributable. Fixed by adding
`git config --global --add safe.directory` for the checkout and for IDF in
`tools/meowkits3/rb-build-artifacts.sh`, and the run was repeated. Worth knowing because the
same failure mode applies to any container that mounts a host checkout: the symptom is a
blank version string, not an error. Note also that the container *image* carries its own
copy of that script, so a stale image silently runs stale provenance logic — mount the
checkout's script over `/usr/local/bin/rb-build-artifacts.sh` when reproducing an older
tag, as this build did.

**Flashed.** `rockbox-flash.bin` went into `0x0` on 2026-10-29 over USB-Serial-JTAG at
460800 baud with the stub: 1,358,832 bytes written (664,112 compressed), `Hash of data
verified`, and the board boots after a physical RESET — esptool was run with
`--after no_reset`, because a USB-asked reset re-enters the loader on this board rather
than booting the image. Behaviour beyond boot is still being soaked (the overnight
discharge run started against this build), so the feature list above remains a statement
about the port, not a re-verification of every item on these bytes.

### Release `esp32-s3-idf-20261027`

Tag at `d83ce28c3fd32608a666001b4575b22db0bbacf2` — retagged once from `a1a3ece0b1` so
the tag itself carries `firmware/target/xtensa/notes/releases/esp32-s3-idf-20261027.md`. Built
by the container against the tag (`--build-arg RB_BRANCH=<tag>`) into
`release/esp32-s3-idf-20261027/`, which is gitignored. Embedded version is clean —
`d83ce28c3f-261027`, no `M` suffix, because `tools/version.sh` marks a dirty working tree
and a release wants a fresh checkout rather than a developer's tree.

| File | Size | SHA-256 (first 16) |
|---|---|---|
| `rockbox-flash.bin` | 1,356,880 B | `f3fa097bc54cb031` — all-in-one image at `0x0` |
| `rockbox.esp32s3` | 1,291,344 B | app image; `espverify` reports valid checksum and validation hash |
| `rockbox.zip` | 9,334,603 B | 387 files: 43 codecs, 158 plugins + 63 Lua, 48 languages, one voice pack |
| `rockbox-fonts.zip` | 8,478,936 B | 88 fonts |
| `rockbox-manual.pdf` | 3,049,050 B | 286 pages |

Plus `bootloader.bin`, `partition-table.bin`, `flash_args`, `espverify.txt`,
`rockbox.map`, `rockbox-info.txt`, `build-manifest.txt` (release label, commit,
`dirty=0`, IDF/toolchain/host compiler versions, embedded version) and `SHA256SUMS`.
`README.md` and the release notes are copied in so the folder stands alone.

**Reproducibility, measured rather than assumed.** Rebuilding the same tag produced a
payload with an identical file listing and identical per-file sizes — two runs compared:
387 entries, zero size differences — but the archive's own bytes differ, because
`.rockbox/langs/voicestrings.zip` is a nested zip and records timestamps. So `SHA256SUMS`
identifies a build, not a content fingerprint.

**Flashed.** `esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 --after
no_reset write_flash 0x0 rockbox-flash.bin` wrote 1,356,880 bytes at `0x0` and reported
"Hash of data verified". Two facts learned doing it: the distribution's `esptool` 4.7.0
ships no esp32s3 stub flasher (`FileNotFoundError: stub_flasher_32s3.json`), so flashing
from this host needs IDF's `esptool.py`; and `--after no_reset` is deliberate — RESET is
a button on this board, not wired to DTR, so esptool cannot reset it and the last step is
always a power cycle.

**The tagged release undercharges the cell.** `esp32-s3-idf-20261027` carries the REG 0x33
bug above, so a device flashed from it stops charging at 4.10 V and the gauge tops out near
83 %. That is a real defect in a published artifact, not a theoretical one: supersede the
tag once the charge path, the coulometer counting, and the TS reading have been checked on
hardware, and say so in the follow-up's notes rather than leaving the first release as the
thing people find from a link.


| File | Size | Notes |
|---|---|---|
| `rockbox.esp32s3` | 1,291,536 B (host tree) / 1,291,344 B (release build) | The board now runs the release app from `rockbox-flash.bin` at tag `d83ce28c3f`; the host build carries `M` in its version string because its working tree was dirty when it was built, which is why releases go through a fresh checkout. md5 of the host build `94ab361620c32bea76161a10914ded22` — the digest moves whenever the embedded build metadata does, so build and flash sizes can differ by a few bytes. |
| `rockbox.zip` | 9,318,431 B, 411 files | card content (387 files); re-extract after plugin changes. Regenerated after the container found the voice pack was stale — `english.voice` is now 2,512,515 B, byte-identical in size to the container's |
| `rockbox-fonts.zip` | 8,478,936 B | 88 fonts, built from the tree |
| `rockbox-themes.zip` | 17,237,059 B | audited clean, additive |
| `rockbox-flash.bin` | 1,357,072 B | all-in-one image at `0x0` (`make espfull`): bootloader + partition table + app merged by esptool. Verified structurally — `0xE9` at `0x0` and `0x10000`, `0xAA` at `0x8000`, app bytes equal to `rockbox.esp32s3` |
| `rockbox-manual.pdf` | 3,047,948 B (container: 3,047,405 B — same 286 pages, differs only in embedded build metadata) | 286 pages; four pre-existing upstream LaTeX error lines as baseline. Rebuilt after the flag batch — now renders "Backlight Fade In/Out", Bass and Treble. `build-meowkits3/manual/features.tex` must be deleted when target flags change (see `AGENTS.md` §9) |

Flash layout: bootloader `0x000000`, partition table `0x008000`, Rockbox app `0x010000`.
`make espfull` merges all three into `rockbox-flash.bin`, which goes at `0x0` as one file.
Memory: DIRAM 340,655 / 341,760 (1,105 B free), IRAM full, `.ext_ram.bss` ends `0x3c5b2e9c`,
arena `0x3c155800..0x3c575800` = 4,325,376 B, **53,604 B** clear of the plugin-text reservation
at `0x3c5c0000` (`make esplayer`).

Recipe: `export IDF_PATH=$HOME/esp/esp-idf IDF_PATH_FORCE=1; . $IDF_PATH/export.sh`,
then `make`, `make zip`, `make manual-pdf`, `make esplayer`, `make espfull`,
`make flash ESPPORT=/dev/ttyACM0`.

`tools/configure` has to run with that environment sourced. Without
`xtensa-esp-elf-cpp` on PATH it prints "Warning: Could not determine target arch" and
still writes `autoconf.h` — with `#define ARCH arch_none`. The build then fails
far away from the cause, with `field 'context' has incomplete type` in
`thread-internal.h` and `DEFAULT_STACK_SIZE undeclared` in `apps/main.c`. Re-running
configure with the environment sourced is the whole fix; `make reconf` inherits the
same requirement.

Container: `tools/meowkits3/Dockerfile` + `tools/meowkits3/rb-build-artifacts.sh`.

```sh
docker build -f tools/meowkits3/Dockerfile --build-arg RB_REFRESH=$(date +%s) -t rockbox-meowkits3 .
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
and exited 0, emitting 13 files including the all-in-one `rockbox-flash.bin` (verified in the
container's own output: `0xE9` at `0x0` and `0x10000`, `0xAA` at `0x8000`, app bytes equal
to the shipped image). Timings on a 32-core host: ~2 min for a full run (configure,
firmware, `esplayer`, `espverify`, `espfull`, voice, zip, fonts, manual, copy), 80 s for the artifacts-only
image with `SKIP_MANUAL=1`, ~110 s for a warm toolchain stage and ~19 min cold. Payload
shape matches a host build exactly: 387 files, empty `diff` of the two zip listings.
`english.voice` came out at 2,512,515 B on both sides once the host pack was
regenerated — the container had found the host's pack stale by 6,158 B, missing
prompts for the settings the feature-flag batch added. `make voice` + `make zip` on
the host closed it; the firmware is unaffected (it was already an AV build), so the
fix is a card re-extract, not a reflash.
`tools/meowkits3/espflash.make` now derives paths from `$(ROOTDIR)` rather than
`dir $(BUILDDIR)`, which is what makes an out-of-tree build (the container builds into
`/work`) work at all; in-tree `make esplayer` and `make reconf` were both re-verified on
the host afterwards.
