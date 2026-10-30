# Merge plan — conforming the Xtensa port to Rockbox's port shape

Baseline: upstream `master` = `fe5c0874af`, branch `esp32-s3-idf` at `81cfef65c0`.
Written 2026-10-29, after the core-touch audit (`81cfef65c0`), before any of it is
implemented.

This document is about **shape, not behaviour**. Nothing below changes what the port
does on hardware; it changes where files live, what a reviewer sees, and how the build
is owned. Every claim is a measurement with the command that produced it, so a later
session can re-run it rather than trust this page.

`PENDING.md` remains the status list. This is the plan for getting the port into a
state that can be proposed upstream.

---

## 0. Verdict

The port works, and the generic plumbing is already upstream-shaped: the arch switch
code, the setjmp port, the model config header, the `*-target.h` set, the plugin keymap
convention, and the untouched plugin/codec APIs all look like a normal Rockbox port.

What will draw review fire:

1. **ESP-IDF owns the link** — a second build system at repo root, ~150 KB of IDF code
   in the image, and four core-shape deviations that exist only because of it.
2. **The target tree is flat** while `tools/configure` declares a manufacturer/model
   tree whose directories do not exist.
3. **A simulator build of the model cannot be produced**, even though several native
   upstream targets can, and we ship a sim buttonmap that nothing can reach.
4. **Arch names leak into shared code** (`xtensa_breadcrumb()` inside the DWC2 driver,
   `xtensa_panic_log()` in `panic.c`, prototypes in `system.h`, a target header included
   by the shared plugin linker script).
5. **More prose than code in the tree** — 6,835 lines of planning markdown inside
   `firmware/target/xtensa/`, plus 5,754 at repo root, against 10,277 lines of port code.

Items 2–5 are mechanical. Item 1 is the only real decision, and it is Phase D.

---

## 1. Measurements

Reproduce these before acting on them.

| Fact | Value | How |
|---|---|---|
| Files differing from upstream | 284 | `git diff --name-only fe5c0874af..HEAD \| wc -l` |
| Target-tree code | 49 files, 10,277 lines | `wc -l firmware/target/xtensa/*.{c,h,S}` |
| Markdown inside the target tree | 6,835 lines | `wc -l firmware/target/xtensa/*.md firmware/target/xtensa/releases/*.md` |
| Root docs | `AGENTS.md` 5,381, `README.md` 373 | `wc -l AGENTS.md README.md` |
| IDF code in the linked image | 1,140 symbols, ~150 KB | `xtensa-esp32s3-elf-nm -S --defined-only idf-app/build/rockbox.elf` filtered on IDF name prefixes |
| Rockbox code in the image | 7,640 symbols, ~1.65 MB | same command, complement |
| Target files including IDF/FreeRTOS headers | 10 | `grep -lE '#include "(esp\|freertos\|driver\|soc\|hal)' firmware/target/xtensa/*.[ch]` |
| Upstream FreeRTOS/SDK references | none | `git grep -il freertos fe5c0874af -- firmware/ apps/ tools/` |
| `TARGET_INC` for our build | points at 2 nonexistent dirs | `grep TARGET_INC build-meowkits3/Makefile`; `ls -d firmware/target/xtensa/espressif` |
| Invented core macros (0 upstream hits) | `HAVE_SYSTEM_LIBC`, `XT_CODECBUF_ADDR`, `PSRAM_BSS_ATTR`, `HAVE_TARGET_TICK_THREAD`, `USB_AUDIO_LOGF_DISABLE`, `IDF_FIRST_CFLAGS`, `FREESTAND_CFLAGS` | `git grep -c -w <name> fe5c0874af` |
| Native sim builds upstream | clean | `--target=ondavx747 --type=s` and `--target=sansam200v4 --type=s` both `make` with host gcc, zero errors |
| Our sim build | fails | `--target=meowkits3 --type=s` then `make`: IDF response-file cflags (`-mlongcalls`, `-mdisable-hardware-atomics`), IDF `endian.h` on host gcc, unguarded xtensa `SOURCES`, `test_touchscreen.c: No keymap defined!` |
| 80-column violations | ~900 lines, worst: `usb-xtensa.c` 404, `i2s-xtensa.c` 270, `button-xtensa.c` 90, `paniclog.c` 89 | `awk 'length>80' firmware/target/xtensa/*.c \| wc -l` per file |
| Plugin/codec API | untouched | `git diff fe5c0874af..HEAD -- firmware/export/plugin.h firmware/export/codec.h` is empty |
| Bare-metal branch exists | tag `baremetal-pre-idf` = `3630b5b65f` | `git log --oneline -1 baremetal-pre-idf` |
| `rombox.elf` on the IDF path | not built | `ls build-meowkits3/rombox.elf` → absent, while `firmware/rom.lds` carries a `CPU_XTENSA` branch |

---

## 2. What already conforms — leave alone

- `firmware/asm/xtensa/{thread.c,switch.S,thread.h}` and
  `lib/libsetjmp/xtensa/setjmp.S` + `_JBLEN` sit exactly where `arm/` and `mips/` put
  theirs.
- Target sources are listed in `firmware/SOURCES`; `firmware/export/config/meowkits3.h`
  is the model header, the upstream place.
- `*-target.h` set (`adc backlight button i2c lcd system`) matches what
  `firmware/target/mips/ingenic_x1000/agptekrocker/` provides, except `gpio-target.h`.
- `PLUGIN_USE_ELF` is an existing upstream variable; `tool="cp"`, `bmp2rb_*` and a
  target-local `flash:` goal have precedent (`tools/echoplayer/openocd.make`, included
  from `tools/root.make:190` upstream).
- `lc-rock.c` replaced per-arch in `firmware/SOURCES` follows `lc-unix.c`/`lc-3ds.c`.
- Plugin keymap blocks are pure additions guarded by `CONFIG_KEYPAD == ESP32S3_PAD`,
  and `apps/plugins/lib/keymaps.h` uses the existing exclusion-list idiom.
- `firmware/drivers/axp173.c`, `firmware/drivers/rtc/rtc_pcf8563.c` in shared `drivers/`
  match how upstream places PMU and RTC drivers.
- No `utils/` changes; `docs/CREDITS` entry is normal.

---

## 3. Divergences, ranked

### 3.1 ESP-IDF owns the link  — blocker

**Evidence.** `tools/root.make:404` wraps the standard `rockbox.elf`/`rombox.elf` rules
in `ifneq (xtensa,$(CPU))` and replaces them with an `idf.py` link against a generated
`rockbox_libs.cmake`. `idf-app/` at repo root carries `CMakeLists.txt`,
`sdkconfig.defaults`, `main/CMakeLists.txt` (with `PRIV_REQUIRES` on ten IDF components
and `LDFRAGMENTS rockbox_memory.lf`), `main/rockbox_app.c`, `panic_cdc.c`,
`rockbox_memory.lf`. Link hacks: `--allow-multiple-definition` so our `esp_panic_handler`
beats IDF's, a re-grouped archive list because IDF does not `--start-group`, and
`--wrap usb_enable` because IDF links `esp32s3.rom.ld`, whose ~2100 absolute symbol
assignments silently substitute the ROM routine for 15 same-named Rockbox functions
(`usb_enable` being the one that mattered).

**Why upstream will object.** No port links an external SDK or RTOS:
`git grep -il freertos fe5c0874af -- firmware/ apps/ tools/` is empty. A port that
requires ESP-IDF installed cannot be built by the build farm, cannot be reviewed
without IDF, and puts a second build system at repo root.

**Core deviations it forces** (each one is a shared-file change reviewers must swallow):
`HAVE_SYSTEM_LIBC` excluding Rockbox's libc in `firmware/SOURCES` and
`firmware/asm/SOURCES`; `#define sleep rockbox_sleep` and `#define rtc_init
rockbox_rtc_init` in `firmware/export/config.h`; the `BASE_CFLAGS` / `FREESTAND_CFLAGS`
/ `IDF_FIRST_CFLAGS` domain split in `tools/root.make` and `firmware/firmware.make`;
`mkdepfile_order` in `tools/functions.make`; IDF include paths added to `PPCFLAGS`;
`-Wno-undef` on every firmware-domain compile (which also silences `-Wundef` in our own
code, a self-inflicted loss).

**Adjustment.** See §4. Everything in this list disappears under D1.

### 3.2 Flat target tree, `configure` claims a tree that isn't there

**Evidence.** `build-meowkits3/Makefile`:
`TARGET_INC=-I$(FIRMDIR)/target/xtensa/espressif/esp32s3 -I$(FIRMDIR)/target/xtensa/espressif -I$(FIRMDIR)/target/xtensa`
— the first two directories do not exist. `tools/configure` sets
`t_cpu="xtensa"`, `t_manufacturer="espressif"`, `t_model="esp32s3"` while
`modelname="meowkits3"`, so even the declared model directory is not the model.
All 49 files sit in one flat directory and every one is suffixed `-xtensa`, so
board code (`st7789`, `pca9557`, `es8311`, `es7210`, `button`, `lcd`, `backlight`,
`audiohw`, `adc`) sits beside SoC code (`i2s`, `spi`, `i2c`, `gpio`, `cache`, `usb`,
`memory`, `thread`, `timer`, `lc`, `paniclog`).

**Why upstream will object.** The convention is
`target/<arch>/<manufacturer>/<model>/`, with SoC files suffixed for the SoC and board
files named for the board — e.g. `ingenic_x1000/agptekrocker/audiohw-agptekrocker.c`
next to `ingenic_x1000/pcm-x1000.c`. A flat arch directory means a second board on the
same SoC has nowhere to go, and it also means `root.make`'s
`target/$(CPU)/$(MANUFACTURER)/app.lds` resolution never resolves.

**Adjustment.** Phase B.

### 3.3 No simulator build, and dead sim artifacts

**Evidence.** Probed `--target=meowkits3 --type=s`: configures fine (host gcc, SDL2,
`ARCH_NONE`, sigaltstack threads) then fails on IDF flags reaching host gcc, on
`esp-idf/components/newlib/platform_include/endian.h`, and on
`apps/plugins/test_touchscreen.c: error: No keymap defined!`. The mechanism is that our
gates key on `CPU=xtensa` rather than on platform: upstream's `firmware/SOURCES` uses
`#elif defined(CPU_MIPS) && (CONFIG_PLATFORM & PLATFORM_NATIVE)`, and `simcc()` rebuilds
`GCCOPTS` from `CCOPTS` — our arch flags are appended to `GCCOPTS`
(`tools/configure:656-657`) so they survive into the host build, and `IDF_CFLAGS` from
the harvested `idf.make` is applied by `root.make:104` regardless.
Control measurement: `--target=ondavx747 --type=s` and `--target=sansam200v4 --type=s`
build clean here with host gcc and `CPU=mips`/`CPU=arm` retained.
Meanwhile `tools/builds.pm` says `sim => 0` while `uisimulator/buttonmap/meowkits3.c`
and `uisimulator/bitmaps/UI-meowkits3.bmp` are committed.

**Why upstream will object.** Sim builds are how reviewers try a port without hardware;
several native targets carry `sim => 1`. Shipping a buttonmap that no build can select
reads as cargo cult.

**Adjustment.** Phase C.

### 3.4 Arch names inside shared code

**Evidence.**
- `firmware/drivers/usb-designware.c` calls `xtensa_breadcrumb()` /
  `xtensa_breadcrumb_rtc()` at 5 sites under `#if defined(CPU_XTENSA)`.
- `firmware/panic.c` calls `xtensa_panic_log(panic_buf)` under `#if defined(CPU_XTENSA)`.
- `firmware/export/system.h` declares ~10 `xtensa_paniclog_*` / `xtensa_breadcrumb_*`
  prototypes.
- `apps/plugins/plugin.lds` does `#include "memory-xtensa.h"` under `#ifdef CPU_XTENSA`,
  and `lib/rbcodec/codecs/codecs.make` adds a `$(wildcard ...memory-xtensa.h)`
  dependency — a target header pulled into a shared linker script.
- `firmware/include/xtensa.h` is a new arch header in the core include directory.

**Why upstream will object.** Upstream precedent for arch conditionals in these exact
files uses arch-neutral names: `panic.c` has
`#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)` calling `panicf_f`, and
`cpu_reset()`/`backtrace.h`. A shared DWC2 driver calling `xtensa_*` is a review
objection on its own, and it makes the driver untestable on any other DWC2 target.
Arch headers live under `firmware/target/<arch>/` (`ingenic-soc.h`, `irq-x1000.h`,
`mipsr2-endian.h`). `plugin.lds` includes only `config.h` and `cpu.h` upstream.

**Adjustment.** Phase A items 1–4.

### 3.5 Documentation volume

**Evidence.** 6,835 lines of markdown inside `firmware/target/xtensa/` across 11 files
(`BT-AUDIO`, `CDC-UI-RESEARCH`, `DOC-GAPS`, `MEMORY`, `PENDING`, `TX-IDF-MIGRATION`,
`USB-AUDIO-RESEARCH`, `USB-SPRINT`, `UX-PLAN`, `releases/*.md`) against 10,277 lines of
code, plus `AGENTS.md` (5,381) and `README.md` (373) at repo root, plus
`tools/docker/README.md`.

**Why upstream will object.** Upstream tolerance is one doc per port
(`firmware/target/arm/s5l8702/ipodnano3g/TESTING.md`) and host utilities under
`utils/<target>/`. Working notes, status lists and research logs are not part of a port;
`AGENTS.md` at repo root is an agent harness file that would collide with upstream's own
conventions.

**Adjustment.** Phase A item 6: consolidate to `README.md` + `TESTING.md` inside the
target directory, move user-facing facts into `manual/platform/meowkits3.tex`, keep the
rest out of the submission tree (they stay on this branch; a submission is cut from a
separation branch, see §5 Phase E).

### 3.6 Style

**Evidence.** Zero tabs. ~900 lines over 80 columns, concentrated in `usb-xtensa.c`
(404), `i2s-xtensa.c` (270), `button-xtensa.c` (90), `paniclog.c` (89), `debug-xtensa.c`
(79), `i2c-xtensa.c` (77), `system-xtensa.c` (61). An upstream target file
(`audiohw-agptekrocker.c`) has 0. Four `//` comments (`lc-xtensa.c`, `sd-idf.c`,
`usb-xtensa.c`), which upstream tolerates sporadically in target code.

**Adjustment.** Phase A item 5, reflow only, comment text preserved verbatim (a lot of it
is measured numbers, so re-flowing must not paraphrase).

### 3.7 Smaller items

- Port tooling sits at `tools/` root: `espflash.make`, `esp-stage2`, `esp-elf2image`,
  `esp-layout-check`, `esp-trailwatch.py`, `esp-usbflash.py`, `idf-flags`,
  `idf-seedcheck`, `docker/`, `rbthemes.py`. Precedent puts these in `tools/<target>/`
  (`tools/echoplayer/`, `tools/agptek_rocker/`, `tools/hibyos_nativepatcher/`) or
  `utils/<target>/` (`utils/ipodnano3g/`, `utils/jztool/`).
- `firmware/rom.lds` carries a `CPU_XTENSA` branch mapping a `rombox.elf` that the IDF
  path never builds — dead until §3.1 is resolved.
- `memory=8` in `configure` is the PSRAM size, not the 337 KB static region. It is
  commented, but a reviewer will read it as the audiobuffer living in fast RAM. Worth
  rewording in `configure` and stating the split in the target README.
- `target_id=126`, `MODEL_NUMBER 134`, `RTC_PCF8563 29`, `ESP32S3_PAD 81`: unique today,
  re-verified at rebase time (the last rebase produced three collisions that only a build
  caught).
- `docs/CREDITS`: fine.

---

## 4. The one decision: who owns the link

### D1 — bare-metal default, IDF optional  (recommended)

Make Rockbox compile *and* link, as every other port does. Restore from the
`baremetal-pre-idf` tag (`3630b5b65f`), which is hardware-verified:
`crt0.S`, `vectors.S`, `irq-xtensa.c`, `irom-map.c`, `mmu-xtensa.c`, `psram-xtensa.c`,
`rom_cache.{c,S}`, `app.lds`, `boot.lds`, `tools/esp-elf2image` (byte-identical to
`esptool elf2image` except the digest flag, verified against esptool 4.7.0).

Then delete from the merge tree: `idf-app/`, `HAVE_SYSTEM_LIBC`, the `sleep`/`rtc_init`
renames, `BASE_CFLAGS`/`FREESTAND_CFLAGS`/`IDF_FIRST_CFLAGS`, `mkdepfile_order`,
`-Wno-undef`, `--wrap usb_enable`, `--allow-multiple-definition`, `tools/idf-*`, and the
`rockbox_libs.cmake` generation. Memory placement returns to `app.lds` plus a
`PSRAM_BSS_ATTR` that the target defines — or, better, to the ldfragment equivalent
Rockbox already uses elsewhere; note upstream has **no** `.lf` files, so the fragment
must live in `firmware/target/xtensa/` and be consumed by Rockbox's own link.

Keep IDF as an alternate for developers: a configure flag (`--with-idf` or a separate
`make idf` goal) that runs the current path. The IDF branch stays as a regression
harness and as the source of PSRAM/PM behaviour.

**What we lose on the default path, and what re-verifying it costs:**
- PSRAM timing sweep — ESP-IDF's `mspi_timing_by_mspi_delay` search. Mitigation already
  measured: copy the entry IDF falls back to when the sweep fails (config id 5:
  `spi_din_mode=4` on every DIN line and DQS, `spi_din_num=0`, `extra_dummy_len=2`),
  which is a known-good configuration rather than an estimate.
- `set_cpu_frequency()` as `esp_pm_configure()` (and thus `cpu_boost`). Bare-metal needs
  its own divider path, or CPU boost becomes a no-op — measurable: recheck codec/plugin
  throughput and the mixer's headroom with boost inert.
- IDF's SD host driver (`sd-idf.c`) — replaced by the bare-metal SDHOST/IDMAC driver that
  already exists on the tagged branch; needs the card soak again (1-bit, non-removable).
- FreeRTOS tickless idle / light sleep — not enabled today, so nothing lost now; it was
  the future plan and would have to go back to a hand-written sleep path.
- IDF's windowed spill helper and its vectors — the bare-metal branch already carries
  `_WindowOverflow*/_WindowUnderflow*` vectors and the same context switch.

**Acceptance test for D1:** image boots to the root menu; audio plays and records; SD
mounts and reads at ≥0.7 MB/s; USB MSC/CDC/DAC work; doom and mpegplayer worst case run;
`make espverify` passes; PSRAM reports "responding" via `dbg_hw_info()`; audiobuffer,
framebuffer, codecbuf and pluginbuf land in the PSRAM window in the map.

### D2 — keep IDF, submit around it

Accept that the port cannot merge as-is. Submit only the arch plumbing
(`ARCH_XTENSA`/`CPU_XTENSA`, `elf-target.h`, `rom.lds`, `configure`, `xtensacc`,
`rockboxdev.sh --target=e`, `builds.pm`, manual wiring) plus the generic core fixes from
`PENDING.md` §5, and keep the port downstream. Cheaper, but the port stays a fork
forever and the build farm never covers it.

**Recommendation: D1, staged.** Do not delete the IDF path — demote it to opt-in. The
bare-metal pieces are already written and hardware-verified, which is what makes this
tractable rather than a rewrite.

---

## 5. Phases

Phases A, B and C are worth doing regardless of the D decision. Order: A → B → C, then
D, then E.

### Phase A — conformance with no hardware risk (~1 day)

1. Rename arch-named hooks to generic ones and implement them in the target:
   - `firmware/drivers/usb-designware.c`: `xtensa_breadcrumb*()` → a weak
     `dbg_breadcrumb(const char *tag, uint32_t a, uint32_t b)` (no-op default), declared
     in a core header, implemented in the target.
   - `firmware/panic.c`: `xtensa_panic_log()` → a panic-console hook guarded the way
     `HAVE_RB_BACKTRACE` guards the ARM one.
   - `firmware/export/system.h`: drop the `xtensa_*` prototypes; they belong in
     `firmware/target/xtensa/paniclog.h` included by target code and by the target's own
     debug menu.
2. Move `firmware/include/xtensa.h` → `firmware/target/xtensa/xtensa.h`.
3. Put the codec/plugin buffer numbers in `config/meowkits3.h` and drop
   `#include "memory-xtensa.h"` from `apps/plugins/plugin.lds` (and the `$(wildcard)`
   dependency in `codecs.make`). `plugin.lds` already includes `config.h`.
4. Remove `-Wno-undef` from the global firmware-domain `CFLAGS`; apply it only to the
   IDF-facing object group (`XTENSA_IDF_FIRST_SRC`).
5. Reflow the 80-column violations in the seven worst target files. Comment text stays
   verbatim.
6. Consolidate docs: `firmware/target/xtensa/README.md` (build, memory map, IDF-vs-
   bare-metal, board wiring) and `TESTING.md`. Move user-facing content into
   `manual/platform/meowkits3.tex`. Keep `PENDING.md`/`MEMORY.md`/research logs on this
   branch only — they are excluded when the submission branch is cut (Phase E).
7. Move port tooling: `tools/espflash.make`, `tools/esp-*`, `tools/idf-*` →
   `tools/xtensa/` (or `tools/meowkits3/`), included from `root.make` the way
   `tools/echoplayer/openocd.make` is; `esp-trailwatch.py`, `esp-usbflash.py`,
   `esp-stage2`, docker bits → `utils/meowkits3/`.
8. Add the missing `ESP32S3_PAD` keymap to `apps/plugins/test_touchscreen.c`.

**Gate:** xtensa build clean, image hash unchanged for code (byte-compare segments),
`make espverify` passes, payload rebuilt. No board behaviour change expected; flash once
to confirm boot.

### Phase B — target tree restructure (~1 day)

Layout, matching the `ingenic_x1000/agptekrocker` convention:

```
firmware/target/xtensa/                     arch: vectors.S, crt0.S, thread switch,
                                            cache-*, system-xtensa.c, xtensa.h, app.lds
firmware/target/xtensa/espressif_esp32s3/   SoC: i2s, spi, i2c, gpio, gdma, usb, memory,
                                            mmu, psram, timer, sdmmc, rom_cache, irq
firmware/target/xtensa/espressif_esp32s3/meowkits3/
                                            board: lcd, st7789, pca9557, es8311, es7210,
                                            button, backlight, audiohw, adc, power,
                                            all *-target.h with board pins
```

- `tools/configure`: `t_manufacturer="espressif_esp32s3"`, `t_model="meowkits3"`; the
  alias list (`133|meowkits3|meowkit-esp32s3`) stays.
- Rename files: `-xtensa` → `-esp32s3` for SoC drivers, `-meowkits3` for board drivers.
- Add `gpio-target.h` at board level if the GPIO helpers are board-facing, else SoC.
- `*-target.h` move to the nearest directory that satisfies them, per the TAL rule.

**Gate:** `TARGET_INC` dirs all exist; native build succeeds; `esp-layout-check` passes;
segment sizes and load addresses identical to before (this phase must not move code).

### Phase C — simulator (~1–2 days)

1. Re-key every xtensa/IDF gate on platform, not on `CPU`:
   `root.make` (`ifeq (xtensa,$(CPU))` blocks for IDF flags, dep scan and link),
   `firmware/firmware.make` (include-order split, `XTENSA_IDF_FIRST_*`),
   `lib/rbcodec/codecs/codecs.make` and `apps/plugins/plugins.make` already key on
   `$(ARCH),arch_xtensa`, which is correct — copy that idiom.
2. Put xtensa arch flags where `simcc()` resets them (derive from `CCOPTS`, not
   `GCCOPTS +=`), so a host build never sees `-mlongcalls`.
3. Guard the xtensa block in `firmware/SOURCES` with
   `&& (CONFIG_PLATFORM & PLATFORM_NATIVE)`, like `CPU_MIPS`.
4. Make `config/meowkits3.h` sim-safe (it already guards `PSRAM_BSS_ATTR`/
   `IRAM_LCDFRAMEBUFFER` with `#ifndef SIMULATOR`; extend to LCD size, storage,
   `CONFIG_RTC`, keypad).
5. Verify `uisimulator/buttonmap/meowkits3.c` + `UI-meowkits3.bmp` are actually reached;
   set `sim => 1` in `tools/builds.pm` only once the sim build is proven.
6. Also build `--type=c` (checkwps) and `--type=d` if they pick up the model.

**Gate:** `--target=meowkits3 --type=s` builds and runs; the sim shows the 320×240
panel, the joystick buttonmap responds, and the touch grid maps; native build unchanged.

### Phase D — the build owner (~1–2 weeks if D1)

Per §4. Sub-order:
1. Restore bare-metal startup/link from `baremetal-pre-idf` onto the current tree
   (files listed above), keeping the current driver layer where it is hardware-verified.
2. Re-verify on hardware in this order: boot + panic trail → cache/MMU + PSRAM
   ("responding", audiobuffer read/write test) → LCD → I2S output → GDMA completion
   semantics → SD → USB (MSC, CDC, DAC) → recording → codec load and plugin load.
3. Make IDF an opt-in path: `--with-idf` sets the CMake link, and the IDF-only drivers
   (`sd-idf.c`, `timer-xtensa.c` gptimer, `backlight-xtensa.c` LEDC, `pcm-xtensa.c`
   i2s_common, `system-xtensa.c` esp_pm, `thread-idf.c`) get bare-metal counterparts or
   a compile-time choice. Each IDF-facing driver needs both variants before the default
   flips.
4. Delete `HAVE_SYSTEM_LIBC`, the symbol renames, the CFLAGS domain split,
   `mkdepfile_order`, `--wrap usb_enable`, `--allow-multiple-definition` from the
   default path.
5. Retire `idf-app/` from the merge tree into `utils/meowkits3/idf/` for the opt-in path.

**Gate:** D1 acceptance test in §4, plus a measured comparison against the IDF image
(size, boot time to root menu, sustained audio, card read rate, plugin arena figures).

### Phase E — submission shape (after A–D)

Cut a `upstream/xtensa-port` branch containing only what belongs upstream. Split:

1. **Series 1 — arch plumbing:** `ARCH_XTENSA`/`CPU_XTENSA` dispatch, `elf-target.h`
   `EM_XTENSA`, `rom.lds`, `firmware/asm/xtensa/*`, `lib/libsetjmp/xtensa/*`,
   `tools/configure` (`arch_xtensa`, `xtensacc`, the target entry), `tools/rockboxdev.sh`
   `--target=e` plus the archive-name and `getfile_ex` fixes, `tools/builds.pm`,
   `list_targets.pl`, manual and keymap wiring, target tree.
2. **Series 2 — generic core fixes**, one per bug, subject line naming the bug not the
   port (inventory already in `PENDING.md` §5): skin_engine `skin_is_loaded()`,
   `audio_path.c` input-source guard, `tick.h` `run_tick_tasks()` split, `i2c-async`
   submit seam, `usb_core.c` serial index mask, `ft6x06` point clamp, metronome `bpm`
   clamp, `usb_audio.c` `as_playback_freq_idx` init + packet-size consistency,
   `puzzles.make` explicit rules + `SOURCES.games`, `rolo.c` `ICODE_ATTR`,
   `keymaps.h` touch exclusion, `pcm_record.c` short-grant panic, `fs_defines.h` /
   `dircache.h` seams, `settings_list.c` seams, `thread.c` `THREAD_EXTRA_CHECKS`,
   `xworld.tex` opt token, `make_archive`.
3. **Series 3 — the port itself:** SoC and board drivers, model config, manual, sim
   buttonmap, plugin keymap blocks.
4. **Series 4 — tooling:** `tools/xtensa/`, `utils/meowkits3/`.

Commit-message policy: a reviewer should be able to read a series and say "this cannot
break my target". Anything that changes behaviour on another target goes in series 2
with its own bug report.

---

## 6. Pre-submission checklist

Run all of these against `upstream/master` at submission time, not against memory:

- `git diff --name-only upstream/master..HEAD | grep -v '^firmware/target/xtensa/\|^firmware/export/config/meowkits3.h'`
  — every remaining file gets a justification.
- `git diff --numstat` vs `git diff -w --numstat` — any file whose ignoring-whitespace
  diff has no deletions was reformatted, not changed.
- Shared files must contain no `xtensa`/`ESP32`/`meowkit` token outside a
  `CONFIG_KEYPAD == ESP32S3_PAD`, `CPU_XTENSA`, `arch_xtensa` or a config-seam guard.
- Duplicate-`#define`-in-one-branch and string-literal-followed-by-identifier detectors
  (from the `81cfef65c0` audit), compared against upstream so pre-existing duplicates are
  not reported as ours.
- Synthetic keymap harness: every touched plugin file × every keypad × touch on/off.
  `test_touchscreen.c` must be in the set.
- Builds: `--target=meowkit-esp32s3`, `--target=meowkit-esp32s3 --type=s`,
  `--target=ondavx747 --type=s`, `--target=sansam200v4 --type=s`, `--target=sdlapp`,
  and one ARM plus one MIPS native build if a toolchain is available.
- Manual PDF builds for our target and for `sansafuze` (proves the manual wiring is not
  ours-only).
- `perl tools/list_targets.pl` output sane; `docs/CONTRIBUTING` style pass
  (4-space, C comments, <80 columns, no tabs).
- `git diff upstream/master -- utils/ tools/echoplayer/ firmware/target/hosted/` shows
  only what we intend.
- Identity scan: `MODEL_NUMBER`, `target_id`, `RTC_*`, `*_PAD` unique against upstream
  tip; two targets built after any rebase.
- `make zip` payload figures recorded; `make espverify` passes.
- `PENDING.md` §5 inventory updated to match what was actually proposed.

---

## 7. Deliberately not touched

- The memory map, MMU window arithmetic, PSRAM bring-up sequence, GDMA/SDHOST descriptor
  rules, cache maintenance through ROM trampolines: these are hardware facts we
  measured, and no upstream convention asks us to change them.
- The MeowKit plugin keymap convention (directions move, `A` confirms, `B` backs out,
  grid cells for the rest) — consistent across 50 files, and pure addition.
- `status => 2` in `builds.pm` until an unattended soak and a calibrated battery gauge
  exist.
- The weak seams (`DEFAULT_SPEAKER_MODE`, `DEFAULT_LIST_LINE_PADDING`,
  `DIRCACHE_*`, `MAX_OPEN_FILES`/`DC_NUM_ENTRIES`, `THREAD_EXTRA_CHECKS`,
  `NR_BUFFERS`/`MINIMUM_BUFFERS_QUEUED`, `USB_AUDIO_*`): all default to upstream
  behaviour and are proposed in series 2 as shape fixes, not as port requirements.
- `docs/CREDITS`, `tools/configure`'s `--target=` hyphen fix, `make_archive` — keep,
  propose separately.

---

## 8. Open questions for you

1. **D1 or D2.** D1 costs roughly two weeks and re-tests the whole board; D2 costs
   nothing but keeps the port a fork permanently.
2. If D1: do we keep IDF reachable as `--with-idf`, or drop it entirely from the tree
   and keep it only on this branch? Keeping it means maintaining two driver variants for
   five files.
3. Where do the working docs live once the submission branch is cut — stay on
   `esp32-s3-idf` only, or move to a separate `docs/meowkits3` branch so this branch can
   mirror upstream exactly?
4. `release/` artifacts: currently gitignored and out of the tree — confirm that stays.
5. Sim buttonmap and UI bitmap: keep (Phase C makes them reachable) or drop with
   `sim => 0`.
