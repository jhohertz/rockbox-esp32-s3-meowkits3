# Merge plan — conforming the Xtensa port to Rockbox's port shape


> Internal working note. This file and the others in this directory are the
> port's own records — plans, logs and research threads. The reviewer-facing
> documents are ../README.md and ../TESTING.md, and the user-facing ones are the
> repository README and the manual chapter for this target.
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
   in the image, and four core-shape deviations that exist only because of it. Upstream
   precedent for a vendor-runtime port exists (§4.1); what does not exist upstream is the
   *way* we express it — a link rule forked by CPU, SDK flags injected from the makefile,
   and an SDK project at repo root.
2. **The target tree is flat** while `tools/configure` declares a manufacturer/model
   tree whose directories do not exist.
3. **A simulator build of the model cannot be produced**, even though several native
   upstream targets can, and we ship a sim buttonmap that nothing can reach.
4. **Arch names leak into shared code** (`xtensa_breadcrumb()` inside the DWC2 driver,
   `xtensa_panic_log()` in `panic.c`, prototypes in `system.h`, a target header included
   by the shared plugin linker script).
5. **More prose than code in the tree** — 6,835 lines of planning markdown inside
   `firmware/target/xtensa/`, plus 5,754 at repo root, against 10,277 lines of port code.

Items 2–5 are mechanical. Item 1 is a decision, and it has been taken (§4): the SDK
dependency stays, so its *expression* has to change.

---

## 1. Measurements

Reproduce these before acting on them.

| Fact | Value | How |
|---|---|---|
| Files differing from upstream | 284 | `git diff --name-only fe5c0874af..HEAD \| wc -l` |
| Target-tree code | 49 files, 10,277 lines | `wc -l firmware/target/xtensa/*.{c,h,S}` |
| Markdown inside the target tree | 6,835 lines | `wc -l firmware/target/xtensa/*.md firmware/target/xtensa/notes/releases/*.md` |
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
| Bare-metal alternative exists | tag `baremetal-pre-idf` = `3630b5b65f` | `git log --oneline -1 baremetal-pre-idf` |
| `rombox.elf` on the IDF path | not built | `ls build-meowkits3/rombox.elf` → absent, while `firmware/rom.lds` carries a `CPU_XTENSA` branch |
| Upstream precedent for an external SDK | `androidcc()` requires `ANDROID_SDK_PATH`/`ANDROID_NDK_PATH`, builds a toolchain, rebuilds `GCCOPTS` from `CCOPTS`, adds `LDOPTS`/`SHARED_LDFLAGS`/`GLOBAL_LDOPTS`, sets `thread_support`, hands `ANDROID_ARCH` to a target make | `tools/configure:695-760` |
| Upstream precedent for a target owning its link rule | `firmware/target/hosted/sonynwz/sonynwz.make` redefines `$(BUILDDIR)/rockbox.elf` without `-nostdlib`/`-T`; the generic native rules live under `ifndef APP_TYPE` | `git show fe5c0874af:tools/root.make` lines 255-284 and 111-119 |
| Upstream precedent for platform bits set by the model header | `firmware/export/config/android.h`: `#define CONFIG_PLATFORM (PLATFORM_HOSTED\|PLATFORM_ANDROID)`; `config.h` comment says the bits are OR-able and extensible | `git show fe5c0874af:firmware/export/config/android.h` |
| Upstream precedent for vendored vendor code in the target tree | `firmware/target/hosted/tinyalsa/` ships tinyalsa's `mixer.c`, `pcm.c` and its headers, added to `TARGET_INC` by `configure` | `git ls-tree -r fe5c0874af firmware/target/hosted/tinyalsa` |
| Upstream precedent for a configure-selected threading model | `thread_support="…"` → `#define ${thread_support}` in `autoconf.h` (`configure:4935`), values `ASSEMBLER_THREADS`, `HAVE_SIGALTSTACK_THREADS`, `HAVE_WIN32_FIBER_THREADS`, `HAVE_SDL_THREADS` | `grep -n thread_support tools/configure` |
| Our build's divergence from that idiom | `ifneq (xtensa,$(CPU))` fork of the link rules, `-DHAVE_SYSTEM_LIBC` injected in `root.make:104`, `idf-app/` at repo root, `thread_support` left at its default | `grep -n 'xtensa,$(CPU)' tools/root.make` |

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

### 3.1 ESP-IDF owns the link

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

**What is acceptable here, and what is not.** Upstream already ships ports that link a
platform runtime rather than a bare-metal self-contained image — the Android and HiByOS
ports require an SDK in the environment, get their toolchain from it, link system
libraries, and let a target-local makefile own the link rule (§4.1). So "this port needs
ESP-IDF" is defensible. "This port forks `root.make`'s link rules on `CPU`, injects SDK
flags into `CFLAGS` from the makefile, and puts a CMake project at repo root" is not,
and none of it is necessary — all three have existing upstream idioms to replace them.

**Core deviations it currently forces** (each a shared-file change a reviewer must
swallow, and each removable without dropping IDF):
`HAVE_SYSTEM_LIBC` excluding Rockbox's libc in `firmware/SOURCES` and
`firmware/asm/SOURCES`; `#define sleep rockbox_sleep` and `#define rtc_init
rockbox_rtc_init` in `firmware/export/config.h`; the `BASE_CFLAGS` / `FREESTAND_CFLAGS`
/ `IDF_FIRST_CFLAGS` domain split in `tools/root.make` and `firmware/firmware.make`;
`mkdepfile_order` in `tools/functions.make`; IDF include paths added to `PPCFLAGS`;
`-Wno-undef` on every firmware-domain compile (which also silences `-Wundef` in our own
code, a self-inflicted loss).

**Adjustment.** Phase D, per §4.2. None of it requires giving up IDF; the goal is that a
reviewer reads the build diff and recognises the Android/NDK shape.

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

**Adjustment.** See §4 for the decision; Phase D removes the parts of this list that are
style rather than substance.

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
`tools/meowkits3/README`.

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

## 4. Decision: the SDK dependency stays

**Decision log — 2026-10-29.** Going off ESP-IDF is **not** on the table right now.
Rockbox has precedent for ports that run on top of a platform runtime rather than a bare
metal, and IDF is that kind of runtime even though it blurs the `PLATFORM_HOSTED`
definition. The bare-metal alternative (§4.3) is parked, not discarded, and this plan's
Phase D is about making the IDF path look like something upstream already knows how to
build.

### 4.1 Precedent the IDF path can lean on

| Precedent | What it establishes | Where |
|---|---|---|
| `androidcc()` | A port may demand an external SDK via environment variables (`ANDROID_SDK_PATH`, `ANDROID_NDK_PATH`), derive its compiler setup from it, rebuild `GCCOPTS` from `CCOPTS` (dropping `-ffreestanding`/`-nostdlib`), append `LDOPTS`/`GLOBAL_LDOPTS`, declare its threading model with `thread_support`, and hand a variable (`ANDROID_ARCH`) to a target makefile. | `tools/configure:695-760` |
| `firmware/target/hosted/sonynwz/sonynwz.make`, `hiby/hiby.make`, `hibyos.make`, `ibasso/android_ndk.make`, `packaging/psp/psp-eboot.make`, `tools/echoplayer/openocd.make` | A target owns its own link and packaging rules in a makefile **inside the target tree**, included by `root.make`'s `APP_TYPE` dispatch. The generic native rules (`-nostdlib -T$(LINKRAM)`) live under `ifndef APP_TYPE`, so a target with a different link environment simply does not get them — no CPU-keyed fork. | `tools/root.make:111-119`, `:255-284` upstream |
| `firmware/export/config/android.h` → `#define CONFIG_PLATFORM (PLATFORM_HOSTED\|PLATFORM_ANDROID)` | The model header declares the platform, and `config.h`'s comment explicitly anticipates OR-ing extra platform bits. A "runs on a vendor runtime" fact belongs here, not in a `-D` on the makefile's `CFLAGS` line. | `firmware/export/config.h:89-97`, `config/android.h` |
| `firmware/target/hosted/tinyalsa/` | Vendoring a third party's sources and headers into the target tree, with the include dir added to `TARGET_INC` by `configure`, is accepted. | `git ls-tree fe5c0874af firmware/target/hosted/tinyalsa` |
| `thread_support` → `#define ${thread_support}` in `autoconf.h` | The threading model is a configure-selected define (`ASSEMBLER_THREADS`, `HAVE_SIGALTSTACK_THREADS`, `HAVE_WIN32_FIBER_THREADS`, `HAVE_SDL_THREADS`), not an arch check scattered through `SOURCES`. | `tools/configure:30,517-531,723,4935` |
| sims for native models | `--type=s` on a native model builds with host gcc and keeps `CPU`; several native targets carry `sim => 1`. Measured here: `ondavx747` and `sansam200v4` both compile clean. | §3.3 |

The reading: **an SDK-backed port is allowed; an SDK-backed port that rewrites the
generic build machinery is not.** Everything in §3.1's deviation list has an existing
idiom that expresses the same fact more narrowly.

### 4.2 What the IDF path must look like to be reviewable

1. **`app_type` instead of a CPU fork.** Give the build an `app_type` (proposal:
   `"esp-idf"`). Upstream's generic native link rules are already inside
   `ifndef APP_TYPE`, so the fork at `root.make:404` disappears; the IDF link rules move
   to a target makefile (`firmware/target/xtensa/espressif_esp32s3/idf.make`) that
   `root.make` includes in the same dispatch that already includes `sonynwz.make`,
   `hiby.make`, `hibyos.make` and `psp-eboot.make`. `root.make`'s diff collapses to an
   `include` line plus variable definitions.
2. **`idfcc()` in `configure`, mirroring `androidcc()`.** Require `ESP_IDF_PATH` (and a
   version check) in the environment, fail with a clear message, harvest the compile and
   link flags into the variables `root.make` already consumes (`INCLUDES`, `LDOPTS`,
   `CORE_LDOPTS`, `GLOBAL_LDOPTS`) rather than appending them to `CFLAGS`, rebuild
   `GCCOPTS` from `CCOPTS` so `-nostdlib`/`-ffreestanding` are dropped for the firmware
   domain, and hand IDF-specific variables (`IDF_PROJECT`, core name) to the target
   makefile. `tools/meowkits3/idf-flags` and `tools/meowkits3/idf-seedcheck` become implementation details of
   that function.
3. **Platform fact in the model header.** Add a platform bit for a vendor runtime
   (`config.h`'s list is explicitly extensible) and set
   `#define CONFIG_PLATFORM (PLATFORM_NATIVE|PLATFORM_ESP_IDF)` in `config/meowkits3.h`.
   Derive `HAVE_SYSTEM_LIBC` from that bit in one place, so the `SOURCES` conditions read
   as platform facts and `root.make` stops defining it.
4. **Narrow the flag surface.** Today every firmware-domain object gets `IDF_CFLAGS`.
   Reduce that to the objects that actually include IDF headers (the existing
   `XTENSA_IDF_FIRST_SRC` list plus any that pull `sdkconfig.h`), which removes the need
   for `FREESTAND_CFLAGS` in `codecs.make`/`plugins.make` and shrinks the
   `firmware.make` include-order flip to a documented per-file exception.
   `mkdepfile_order` then stays, but only for that list — same three consumers, one
   narrow purpose.
5. **`-Wno-undef` scoped, not global.** Apply it to the IDF-facing objects only, so our
   own code keeps `-Wundef`.
6. **The IDF project lives in the target tree.** `idf-app/` → under
   `firmware/target/xtensa/espressif_esp32s3/` (or `utils/meowkits3/idf/` if it is
   developer tooling rather than firmware). Nothing at repo root. `rockbox_libs.cmake`
   stays generated, but written into the build directory rather than next to the project.
7. **Threading model via `thread_support`.** Set it in `idfcc()` (proposal:
   `HAVE_IDF_THREADS` or reuse `ASSEMBLER_THREADS` if the asm switch is what actually
   runs) and select `thread-idf.c` in `firmware/SOURCES` off that define rather than off
   `CPU_XTENSA`.
8. **Keep the honest notes.** The `--wrap usb_enable` ROM collision, the
   `--allow-multiple-definition` panic-handler override and the archive re-grouping are
   real and stay — but they move into the target's makefile with their explanation, where
   a reviewer reads them as target-specific rather than as core surgery.

### 4.3 Parked: the bare-metal alternative

Kept here because it may come back. `baremetal-pre-idf` (`3630b5b65f`) is
hardware-verified and already contains `crt0.S`, `vectors.S`, `irq-xtensa.c`,
`irom-map.c`, `mmu-xtensa.c`, `psram-xtensa.c`, `rom_cache.{c,S}`, `app.lds`, `boot.lds`
and `tools/esp-elf2image` (byte-identical to `esptool elf2image` except the digest flag,
checked against esptool 4.7.0). Under it, everything in §3.1's deviation list disappears,
and the port becomes self-contained like every other native port.

What it costs, and why it is not happening now:
- PSRAM timing sweep — ESP-IDF's `mspi_timing_by_mspi_delay` search would be gone.
  Mitigation is known: IDF's own fallback entry (config id 5: `spi_din_mode=4` on every
  DIN line and DQS, `spi_din_num=0`, `extra_dummy_len=2`) is a measured-good
  configuration, not an estimate.
- `set_cpu_frequency()` as `esp_pm_configure()` — CPU boost would need its own divider
  path or become inert; measurable by rechecking codec/plugin throughput and mixer
  headroom with boost off.
- IDF's SD host driver (`sd-idf.c`) — the bare-metal SDHOST/IDMAC driver exists on that
  tag but needs the card soak again (1-bit, non-removable).
- FreeRTOS tickless idle / light sleep — not enabled today, so nothing is lost now; the
  future plan would need a hand-written sleep path.

Reopen conditions: upstream rejects any external-SDK requirement; or the build farm
cannot host IDF; or we want light sleep and IDF's PM becomes a liability rather than a
help.

---

## 5. Phases

Phases A → B → C → D → E. A, B and C are independent of the SDK decision; D is now
"make the IDF dependency look like the Android/NDK shape" (§4.2), not "remove it".

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
7. Move port tooling: `tools/meowkits3/espflash.make`, `tools/esp-*`, `tools/idf-*` →
   `tools/xtensa/` (or `tools/meowkits3/`), included from `root.make` the way
   `tools/echoplayer/openocd.make` is; `esp-trailwatch.py`, `esp-usbflash.py`,
   `esp-stage2`, docker bits → `utils/meowkits3/`.
8. Add the missing `ESP32S3_PAD` keymap to `apps/plugins/test_touchscreen.c`.

**Done 2026-10-30, in four commits.** What differed from the plan as written:

- Item 2 sent the CPU header to `firmware/export/xtensa.h`, not the target directory:
  `cpu.h` includes a per-CPU header for every arch and upstream keeps them all in
  `firmware/export/` (`x1000.h`, `as3525.h`, `imx233.h`, `mcf5249.h`). Moving it into
  the target tree would have been the wrong convention.
- Item 3 followed the same logic: the layout numbers went into `xtensa.h` rather than
  `config/meowkits3.h`, which is where `as3525.h` keeps `DRAM_ORIG`/`IRAM_ORIG`. That
  removed `plugin.lds`'s target-path include entirely instead of replacing it, and
  turned the `$(wildcard $(FIRMDIR)/target/$(CPU)/memory-xtensa.h)` prerequisites into a
  conditional dependency on a core header. `memory-xtensa.h` is gone.
- Item 4 needed a change the plan had not anticipated. `-Wno-undef` was load-bearing
  because `system-target.h` included `<xtensa/hal.h>` and `cache-xtensa.h`, and
  `system-target.h` is reached from `system.h` -- so IDF headers sat in front of 919 of
  1190 objects. Declaring the three whole-cache entry points (they are `extern` in
  `hal.h`, not inline) and moving the per-range cache calls out of the inline header into
  a new `cache-xtensa.c`, the only object in the tree that includes the platform cache
  HAL, took objects reaching an IDF *API* header from 919 to 16, all of them under
  `firmware/target/xtensa/`. `-Wno-undef` is now scoped to that directory; the one IDF
  libc-header warning is silenced by supplying
  `CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY=0`, which is what this project's sdkconfig
  means. PPCFLAGS lost the flag: preprocessing the three `SOURCES` files with the real
  flags yields zero `-Wundef` warnings. Full-rebuild warnings went 683 to 17, and all 17
  are upstream behaviour other targets also emit.
- Item 5 rewrapped 1745 comment lines across the 51 files this branch created, using a
  tool that asserts two invariants before writing anything: everything outside a comment
  byte-identical, and the comment prose identical as a word sequence. Pre-formatted
  paragraphs (register tables, aligned columns) were left alone, which is why some long
  lines survive. Makefiles and shell scripts were left alone -- upstream's own
  `tools/configure` has 100 lines over 80 columns.
- Item 6 became README.md + TESTING.md in the target directory with the ten working
  notes moved to `notes/`, rather than folding the research documents into two files.
  User facts went into `manual/getting_started/meowkits3_install.tex`, not
  `manual/platform/meowkits3.tex`, because that second file is option plumbing and the
  install page is where upstream puts per-target prose.
- Item 7 landed in `tools/meowkits3/` (the `tools/agptek_rocker/` shape, Dockerfile and
  README included) rather than splitting across `tools/xtensa/` and `utils/meowkits3/`.
  Four of the files the plan listed to move -- `esp-elf2image`, `esp-stage2`,
  `esp-trailwatch.py`, `esp-usbflash.py` -- are not in this tree at all; they belong to
  the `baremetal-pre-idf` tag. `esp-layout-check` also gained repo-relative defaults so
  it can be run from a build directory.

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

### Phase D — conform the SDK dependency to the existing idioms (~3–5 days)

Per §4.2. Sub-order, each step independently verifiable:

1. `app_type="esp-idf"` in the target entry; delete the `ifneq (xtensa,$(CPU))` fork and
   move the link/image rules to `firmware/target/xtensa/espressif_esp32s3/idf.make`,
   included from `root.make`'s `APP_TYPE` dispatch. Also move `tools/meowkits3/espflash.make`
   alongside it.
2. `idfcc()` in `configure`: `ESP_IDF_PATH` requirement and version check, flag harvest
   via `tools/meowkits3/idf-flags`, `GCCOPTS` rebuilt from `CCOPTS`, link flags handed to the
   variables `root.make` already reads, `thread_support` set.
3. Platform bit for a vendor runtime in `config.h`, set from `config/meowkits3.h`;
   `HAVE_SYSTEM_LIBC` derived from it; the `sleep`/`rtc_init` renames move under that bit
   with their measurement kept as the comment.
4. Narrow `IDF_CFLAGS` to the objects that include IDF headers; drop `FREESTAND_CFLAGS`
   from `codecs.make`/`plugins.make` if the freestanding domain no longer needs a
   separate variable; scope `-Wno-undef` to that object list.
5. Move `idf-app/` into the target tree (or `utils/meowkits3/idf/`), and generate
   `rockbox_libs.cmake` into the build directory.
6. Select `thread-idf.c` from `thread_support`, not from `CPU_XTENSA`.

**Gate:** `make` produces a byte-comparable image (same segments, same load addresses,
identical `.text`/`.data` sizes); `make espverify` passes; `make flash` still works; a
board run confirms boot, audio, SD, USB, recording and one plugin. Then the negative
tests: `tools/root.make` diff must not mention `xtensa` at all, and an
`--target=meowkits3` configure with `ESP_IDF_PATH` unset must fail with the same clarity
as `androidcc()` does.

**Open risk to state plainly:** upstream may still refuse a port that requires ESP-IDF in
the build environment, however well-shaped. §4.1 is the argument for it; §4.3 is the
fallback if the argument fails.

### Phase E — submission shape (after A–D)

Cut a `upstream/xtensa-port` branch containing only what belongs upstream. Split:

1. **Series 1 — arch plumbing, no SDK in it:** `ARCH_XTENSA`/`CPU_XTENSA` dispatch,
   `elf-target.h` `EM_XTENSA`, `rom.lds`, `firmware/asm/xtensa/*`,
   `lib/libsetjmp/xtensa/*`, `tools/configure` (`arch_xtensa`, `xtensacc`),
   `tools/rockboxdev.sh` `--target=e` plus the archive-name and `getfile_ex` fixes,
   `tools/builds.pm`, `list_targets.pl`. Deliberately SDK-free so a reviewer can accept
   the architecture support before deciding about ESP-IDF.
2. **Series 2 — the SDK-backed target build:** `app_type="esp-idf"`, `idfcc()` with its
   `ESP_IDF_PATH` requirement and version pin (documented the way `rockboxdev.sh` pins
   binutils and gcc), the target's `idf.make`, the platform bit, and the IDF-facing
   object group. This is where the Android/NDK precedent (§4.1) is cited, and it is the
   series most likely to be argued about — keeping it separate means series 1 and the
   generic fixes (§3) survive that argument.
3. **Series 3 — generic core fixes**, one per bug, subject line naming the bug not the
   port (inventory already in `PENDING.md` §5): skin_engine `skin_is_loaded()`,
   `audio_path.c` input-source guard, `tick.h` `run_tick_tasks()` split, `i2c-async`
   submit seam, `usb_core.c` serial index mask, `ft6x06` point clamp, metronome `bpm`
   clamp, `usb_audio.c` `as_playback_freq_idx` init + packet-size consistency,
   `puzzles.make` explicit rules + `SOURCES.games`, `rolo.c` `ICODE_ATTR`,
   `keymaps.h` touch exclusion, `pcm_record.c` short-grant panic, `fs_defines.h` /
   `dircache.h` seams, `settings_list.c` seams, `thread.c` `THREAD_EXTRA_CHECKS`,
   `xworld.tex` opt token, `make_archive`.
4. **Series 4 — the port itself:** SoC and board drivers, model config, manual and
   keymap wiring, sim buttonmap, plugin keymap blocks.
5. **Series 5 — tooling:** `tools/xtensa/`, `utils/meowkits3/`.

Commit-message policy: a reviewer should be able to read a series and say "this cannot
break my target". Anything that changes behaviour on another target goes in series 3
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
- Build-shape scan: `grep -n 'xtensa\|idf\|ESP' tools/root.make` should show only the
  `include` line for the target make and configure-provided variables; no forked link
  rule, no `-D` defining platform facts. Nothing at repo root but the normal tree.
- `git diff fe5c0874af..HEAD -- tools/root.make tools/functions.make firmware/firmware.make`
  read line by line: every hunk must be explainable as "a target makefile needs this hook",
  not "this target is special".
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
  behaviour and are proposed in series 3 as shape fixes, not as port requirements.
- `docs/CREDITS`, `tools/configure`'s `--target=` hyphen fix, `make_archive` — keep,
  propose separately.

---

## 8. Open questions for you

Settled:
- **The SDK dependency stays** (2026-10-29). Bare-metal is parked as §4.3 with explicit
  reopen conditions.
- `idf-app/` leaves the repo root — target tree or `utils/`, either is fine, but not root.

Still open:
1. **`app_type` name.** `"esp-idf"` reads best next to `"sonynwz"`, `"hibyos"`,
   `"psp-homebrew"`. Alternative: `"esp32"` if we want the name to survive a future
   non-IDF path.
2. **Platform bit name.** `PLATFORM_ESP_IDF` (names the runtime) versus
   `PLATFORM_VENDOR_RT` (names the category). `config/android.h` precedent suggests
   naming the runtime.
3. **How far to narrow the IDF flag surface.** Step 4 of Phase D could remove
   `FREESTAND_CFLAGS` entirely, or keep it as a documented two-domain split. The narrow
   version is more reviewable but needs a build test to prove no firmware object secretly
   needs IDF's include path.
4. **Whether to vendor the small IDF pieces we rely on** the way `hosted/tinyalsa/`
   vendors tinyalsa — e.g. the cache/MMU and PSRAM helpers — so a reviewer can read them
   without an IDF checkout. Apache-2.0 permits it with attribution; the trade is a
   vendoring problem (drift from IDF versions) versus a smaller external surface.
5. Where the working docs live once the submission branch is cut — stay on
   `esp32-s3-idf` only, or move to a separate docs branch so the submission mirrors
   upstream exactly.
6. Sim buttonmap and UI bitmap: keep (Phase C makes them reachable) or drop with
   `sim => 0`.
