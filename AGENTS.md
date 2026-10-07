# Rockbox — project guide

Rockbox is the open-source firmware for portable media players. It is a large,
old C codebase (2001-present) with a build system that is *not* autotools and a
target abstraction layer (TAL) that is more elaborate than it first looks.

**Current branch:** `esp32-s3-ports` — porting to an ESP32-S3-WROOM-1-N16R8 based
device. There is no Xtensa support in the tree yet, so this is a new-architecture
port, not a new-board port.

Remotes: `origin` is the working fork, `upstream` is `git.rockbox.org`.

---

## 1. The build pipeline (read this before touching anything)

```
tools/configure            -> writes Makefile + autoconf.h into the build dir
Makefile (generated)       -> includes tools/root.make
tools/root.make            -> includes the per-directory *.make files
per-directory *.make       -> populate SRC, OTHER_SRC, CORE_LIBS, custom rules
```

The non-obvious part: **`SOURCES` files are C preprocessor programs, not lists.**

```make
FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/asm/SOURCES)
SRC         += $(call preprocess, $(APPSDIR)/SOURCES)
```

`preprocess` (in `tools/functions.make`) runs the *target* compiler:

```make
$(CC) $(PPCFLAGS) -E -P -x c -include config.h <file>
```

So every `SOURCES` file is evaluated with `config.h` in scope, and `#if`
decides which files are compiled. There are ~88 of them. Linker scripts go
through the same mechanism (`preprocess2file`), which is why `firmware/rom.lds`
starts with `#include "config.h"`.

Consequence: **`config.h` is the single source of truth.** Anything you want to
compile, link, or enable must be reachable from a `CONFIG_*` / `HAVE_*` define
in `firmware/export/config/<model>.h`.

Build flow for a target:

1. `tools/configure` selects a target (`case $buildfor`), sets `modelname`,
   `target` (the `-D` define), `memory`, `t_cpu`/`t_manufacturer`/`t_model`,
   the compiler setup function (`*cc`), and the packaging `tool`.
2. `firmware/export/config.h` includes `config/<model>.h` based on that `-D`.
3. `autoconf.h` gets `ARCH`, `ARCH_VERSION`, `ARCH_PROFILE` — see §3.

---

## 2. Target tree layout

```
firmware/target/<arch>/<manufacturer>/<model>/   board level
firmware/target/<arch>/<manufacturer>/           SoC level
firmware/target/<arch>/                          arch level
firmware/target/hosted/                          OS-hosted ports + SDL sim
```

`TARGET_INC` is built in that order (model, manufacturer, [soc], arch), so a
`*-target.h` header is satisfied by the *nearest* directory that provides it.
You never write a target file in a directory that does not appear in
`TARGET_INC`.

The TAL contract is a set of `*-target.h` headers included from
`firmware/export/*.h`: `system-target.h`, `button-target.h`, `lcd-target.h`,
`backlight-target.h`, `audio-target.h`, `adc-target.h`, `i2c-target.h`,
`spi-target.h`, `dma-target.h`, `rtc-target.h`, `powermgmt-target.h`,
`clock-target.h`, `elf-target.h`, ...

`firmware/target/<arch>/` also owns the arch-wide pieces: `crt0.S`,
`app.lds`/`boot.lds`, `system-<arch>.h`, cache/MMU code, interrupt vectors,
and the exception handler.

`root.make` resolves the linker script as:

```make
RAMLDS := $(FIRMDIR)/target/$(CPU)/$(MANUFACTURER)/app.lds   # if it exists
RAMLDS := $(FIRMDIR)/target/$(CPU)/app.lds                   # otherwise
```

---

## 3. What a new architecture needs

Arch dispatch is driven by `CPU_<arch>` defines, produced in
`firmware/export/config.h` from `ARCH`. Adding Xtensa means touching these:

| Place | Why |
|---|---|
| `tools/configure` | `__XTENSA__` -> `arch="xtensa"`; `arch_xtensa`/`ARCH_XTENSA` in `autoconf.h` |
| `firmware/export/config.h` | `#if ARCH == ARCH_XTENSA` -> `#define CPU_XTENSA` |
| `firmware/include/elf-target.h` | `TARGET_ELF_MACHINE = EM_XTENSA` (188); currently `#error` |
| `firmware/rom.lds` | `OUTPUT_FORMAT(elf32-xtensa-le)`, `STARTUP(target/xtensa/crt0.o)`; currently `#error` |
| `firmware/asm/thread.c` | `#error Missing thread impl` — needs `asm/xtensa/thread.c` |
| `firmware/asm/thread.h` | needs a `struct regs` for the arch |
| `lib/libsetjmp/setjmp.h` + `SOURCES` | `_JBLEN` and `xtensa/setjmp.S` |
| `firmware/asm/SOURCES` | most have generic fallbacks (`ffs.c`, `memcpy.c`, `memset.c`), so this is optional |
| `firmware/export/system.h` | `MIN_STACK_ALIGN`, `HAVE_RB_BACKTRACE` |
| `firmware/kernel/`, `firmware/panic.c` | exception/backtrace hooks |
| `firmware/target/xtensa/` | `crt0.S`, `app.lds`, `boot.lds`, `system-xtensa.h`, `system-target.h`, cache ops |

Most `firmware/asm/*` files already have a generic fallback path
(`beep.c`, `pcm-mixer.c`, `ffs.c`, `memcpy.c`), so the hard blockers are
**thread switching**, **elf-target.h**, and **rom.lds**.

Xtensa-specific warning: the windowed ABI means a context switch must save the
register *windows*, not just a flat register list. `mips/thread.c` and
`arm/thread-micro.c` are the closest models. Consider `-mabi=call0` to make
this tractable.

---

## 4. Toolchains — `tools/rockboxdev.sh`

The script builds GNU toolchains from source for m68k, arm, mips (and the two
Linux variants). It is the only place toolchain versions are pinned
(binutils 2.38, gcc 9.5.0).

**Xtensa cannot be built this way.** Upstream GCC has no Xtensa target, and GNU
binutils only knows the "FSF" reference core, which is not any ESP32 core. Real
ESP32 cores are Tensilica/Cadence configurations, distributed as proprietary
config overlays or as runtime-loadable *dynconfig* plugins. `rockboxdev.sh`
therefore installs Espressif's prebuilt toolchain (`--target=e`).

Practical facts about that toolchain:

- It is **multilib**: one `xtensa-esp-elf` toolchain covers esp32/esp32s2/esp32s3.
  The core is selected by the dynconfig plugin, e.g. `-mdynconfig=xtensa_esp32s3.so`.
- The per-core binaries (`xtensa-esp32s3-elf-gcc`) are wrappers that resolve the
  toolchain root **relative to the executable** and inject `-mdynconfig=`. Keep
  the tree intact; expose tools via symlinks, never copy the binaries out.
- Rockbox builds with `-nostdlib -ffreestanding` and links `-lgcc` explicitly in
  `root.make`, so libgcc for the selected multilib must be reachable. No libc is
  needed — Rockbox has its own in `firmware/libc`.
- `configure` detects the arch by running `$CPP -dD` and grepping for
  `__arm__`/`__mips__`/`__XTENSA__`, so the toolchain must be on `PATH`.

---

## 5. ESP32-S3 notes

- Xtensa LX7, dual core. Rockbox is single-core in practice; `CURRENT_CORE`
  exists in `firmware/asm/corelock.c` for SMP.
- No MMU. `firmware/target/*/mmu-*.c` is optional; do not assume one.
- **ABI**: build with `-mabi=call0`. The windowed ABI keeps live values in
  register windows, and the esp32s3 core config has no `mtsr`/`mfsr`, so the
  window base cannot be set — a context switch cannot be done under it.
  But the prebuilt toolchain's libgcc is built for the windowed ABI only, so
  `-lgcc` under call0 mixes ABIs (`ld` warns, and helpers such as `__divdi3`
  clobber a4-a11). Either build a call0 libgcc or supply the helpers Rockbox
  needs itself, the way `lib/arm_support` does for cores without a hardware
  divide. `CORE_LIBS` are searched before `-lgcc`, so a Rockbox library wins.
- Cache: the toolchain's esp32s3 core config exposes single-precision FP
  opcodes (`madd.s`, `mul.s`, `ufloat.s`). Verify against the silicon before
  relying on them; Rockbox otherwise assumes soft float.
- `N16R8` = 16 MB flash + 8 MB PSRAM. Rockbox's memory model is
  `MEMORYSIZE` (MB) in `configure`, which sizes the audiobuffer and plugin buffer.
  PSRAM is cache-backed and slower than internal SRAM — plan the linker script
  around what must live in internal RAM (stack, ISR, framebuffer).
- Boot: Rockbox expects a `tool` in `configure` that turns a raw binary into the
  target's firmware file. For ESP32 that is an ELF→bin objcopy plus whatever the
  bootloader expects (an ESP image header + checksum, or plain `esptool` flashing).
- `tools/echoplayer/openocd.make` is the model for a debug/flash makefile; OpenOCD
  supports ESP32.

---

## 6. Simulator

`firmware/target/hosted/sdl` + `uisimulator/`. Sim builds use the **host**
compiler, so a target can be simulated before any cross toolchain exists. This
is the fastest way to iterate on TAL behaviour, and every target should keep a
sim config (`sim => 1` in `tools/builds.pm`).

---

## 7. Conventions (from `docs/CONTRIBUTING`)

- C only. No C++isms, no typedefs for structs, no code in headers.
- Lower-case identifiers for variables/functions; UPPER_CASE for macros and enum
  constants.
- `/* C comments */` only. Four spaces, no tabs, under 80 columns.
- Follow the style of the file you are editing.

---

## 8. Commands

```bash
# toolchain (installs Espressif's prebuilt xtensa toolchain into the prefix)
./tools/rockboxdev.sh --target=e

# configure + build
mkdir build-esp32s3 && cd build-esp32s3
../tools/configure --target=<modelname>
make

# useful make goals
make bin        # firmware only
make zip        # full package
make tools      # host tools only
make tags
```

Useful configure/build variables: `RBDEV_PREFIX`, `RBDEV_DOWNLOAD`,
`RBDEV_BUILD`, `RBDEV_RESTART` (restart a toolchain build at a given step),
`RBDEV_TARGET`, `RBDEV_ESP_TOOLCHAIN` (Espressif release tag).

`tools/builds.pm` drives the build farm and `tools/list_targets.pl` prints the
target status list; a new target must be added there too. `target_id` is a
unique number (currently max 125 in `configure`, max `MODEL_NUMBER` 132 in
`config/*.h`) — use the next free one.

---

## 9. Gotchas

- `SOURCES` files are preprocessed by the *target* compiler, so a `#if` in them
  is evaluated per-target. Editing one without a build dir configured gives
  nothing useful.
- `reggen` (`firmware/reggen/`) generates peripheral header files from `.regs`
  descriptions; it is driven by `SOURCES` too. Worth using for ESP32 peripherals
  instead of hand-writing register headers.
- Dependency generation goes through `tools/multigcc.pl` + `tools/addtargetdir.pl`
  and depends on generated headers (`lang.h`, `sysfont.h`) existing — a fresh
  tree needs at least one full run.
- `tools/configure` must be run from the build directory; `tools/autoconf.pl`
  infers the target from the directory name.
- Style: the tree is consistent. Do not introduce a new arch dispatch idiom —
  follow `#if defined(CPU_*)` as used in `firmware/asm/`.
