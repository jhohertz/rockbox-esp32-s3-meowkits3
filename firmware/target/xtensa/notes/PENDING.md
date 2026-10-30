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
     ends at a voltage rather than at a charge. The floor is `power-xtensa.c:36-37`:
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
  `PERCENTAGE_MEASURE`, and `power-xtensa.c` now has `_battery_level()`: it anchors to the
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

## 2. Verified on hardware — closed

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
  as held state via `button_status()` (`button-xtensa.c:190-203`, `button.c:414`) and
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
  `firmware/lc-rock.c`, `lcd.h`, `lcd-xtensa.c`, `st7789.c`, `cache-xtensa.h` and
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

| new | `apps/plugins/bitmaps/remote_native/SOURCES`, plugin sources testing `CONFIG_LCD` | Two upstream `#if`s that assume a macro always exists | Found by narrowing `-Wno-undef` to the target directory, which puts `-Wundef` back on for the rest of the tree. `#if LCD_REMOTE_DEPTH == 1` appears in a `SOURCES` file that only dual-LCD targets define that for, and `#if CONFIG_LCD == LCD_SSD1815` in cube.c, clock_draw_analog.c, mandelbrot_set.c and doom/i_video.c assumes every target names its LCD controller — agptekrocker, aigoerosq, hibyr1, hidizsap80max and sansae200r define neither, so those targets emit 17 warnings today. Benign (undefined evaluates to 0 and the branch is the right one), but `defined()`-guarding them or defaulting `CONFIG_LCD` to 0 in `config.h` is a clean upstream patch. Not done here: it is upstream text and unrelated to this port. |
| new | `manual/getting_started/installation.tex`, `manual/configure_rockbox/system_options.tex`, `manual/appendix/config_file_options.tex` | Three LaTeX errors in generic manual text when built for this target | `make manual-pdf` reports "Something's wrong--perhaps a missing \item" twice in `system_options.tex` and once in `installation.tex`, plus "Float(s) lost" in `config_file_options.tex`. They survive removing this target's own `meowkits3_install.tex` section, so they are not ours; the PDF builds (3.05 MB) and the MeowKit chapters render correctly. Worth pinning down before the manual chapter is offered upstream, because a reviewer building the manual for this target sees errors. |

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

**Not yet done:** the flash that closes Phase A's gate. `rockbox-flash.bin` is built
and the guard passed; it needs the BOOT-then-RESET gesture on the board.

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
     path (`sd-idf.c`, 1-bit, 20 MHz card clock, theoretical 2.5 MB/s) and the CIC
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
