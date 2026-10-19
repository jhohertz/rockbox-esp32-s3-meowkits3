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

### The xtensa build links through ESP-IDF

On the `esp32-s3-idf` branch the pipeline above stops at the archives. Rockbox
compiles everything, produces `lib/librockbox.a` plus the `CORE_LIBS` archives,
writes their paths to `idf-app/rockbox_libs.cmake`, and then runs `idf.py build`,
which links those archives with the IDF components and owns the linker script, the
startup, the cache/MMU and PSRAM bring-up, the C library and FreeRTOS.

The ordering is forced, not chosen: `idf.py reconfigure` has to run *before* Rockbox
compiles, because `tools/idf-flags` harvests the flags for IDF's headers from
`compile_commands.json` in IDF's build tree — a hand-kept include list would follow
`sdkconfig` only by hand and would fail as a code bug (header found, `CONFIG_` define
missing). `tools/configure` performs that reconfigure and writes `$(BUILDDIR)/idf.make`,
which `root.make` includes. Because `rockbox_libs.cmake` is generated *after* IDF has
been configured, `root.make` touches `idf-app/main/CMakeLists.txt` to make ninja
re-run CMake; `CMAKE_CONFIGURE_DEPENDS` cannot be used here, because IDF generates
its own ninja graph and the file then appears as an output twice.

Build with the IDF environment sourced: `. ~/esp/esp-idf/export.sh && make`.

Read "Two compilation domains", below, before changing any flag set.

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

### Use IDF's drivers; do not bit-bang peripherals

This is a standing instruction, and it has been earned twice.

The I2S/GDMA transmit path only started working once it was handed to IDF's HAL
(`i2s_hal_init`, `set_tx_clock`, `std_set_tx_slot`, `std_enable_tx_channel`). The
backlight then demonstrated the failure mode of the alternative: a hand-rolled
LEDC driver wrote the correct addresses, with offsets and bit fields each checked
against `ledc_reg.h` and `ledc_struct.h`, and **not one write reached the register
file**. Every LEDC register read back its reset value while the SYSTEM clock gate
was confirmed set. Nothing errored. The symptom was a backlight that could not be
turned off, and it survived many build-and-flash cycles because the reset state of
that particular register happened to look like the correct behaviour.

The lesson is not "be more careful with offsets" — the offsets were right. A
peripheral can be silently unreachable for reasons a datasheet does not list, and
the only cheap defence is to read the hardware back and compare against a driver
known to work. IDF's driver made the same writes land immediately.

So: if IDF has a driver for the block (`esp_driver_*`, `esp_lcd`, `esp_hw_support`,
`soc/*_reg.h` for constants), use it. Reach for raw registers only where IDF has
nothing — and when you do, add a readback probe alongside it.

Two mechanical notes for files that include IDF headers:

- `CFLAGS` already carries `$(IDF_CFLAGS)` for the whole firmware domain, so IDF
  headers are reachable from any target file. But `tools/configure` harvests those
  flags from `$rootdir/idf-app/build`, **not** from the build directory being
  configured. Adding a component to `idf-app/main/CMakeLists.txt` therefore needs
  `idf.py reconfigure` in `idf-app/` before `configure` will see its include path;
  re-running `cmake` in `build-meowkits3` updates the wrong `compile_commands.json`
  and `idf.make` silently keeps the old flag set.
- `firmware/target/xtensa/gpio-xtensa.h` declares `gpio_set_direction(int, int)` as
  a static inline, which collides with IDF's `driver/gpio.h` declaration that
  `driver/ledc.h` pulls in. The two cannot share a translation unit.

- Xtensa LX7, dual core. Rockbox is single-core in practice; `CURRENT_CORE`
  exists in `firmware/asm/corelock.c` for SMP.
- No MMU. `firmware/target/*/mmu-*.c` is optional; do not assume one.
- **ABI**: build with `-mabi=call0`. Note that the toolchain's *default* for this
  core is windowed (`-mabi` unset gives `entry a1, 32` and defines
  `__XTENSA_WINDOWED_ABI__`), so call0 is a deliberate override.

  The reason is not that the window base cannot be set — `rsr.windowbase` and
  `wsr.windowbase` **are** valid on this core config. The reason is that the core
  config has **no REGB option**, verified against the dynconfig itself, not just
  the assembler: `mtsr`, `mfsr`, `spills`, `spillw`, `refill`, `sregs`, `callinc`,
  `ubase`, `preg`, `acx`, `wob`, `save1..5` have no opcode tables in
  `xtensa_esp32s3.so` and the assembler rejects them. That means:

  - a windowed function cannot see its caller's window. Window overlap is 4
    registers (callee a4-a7 = caller a8-a11); the caller's a0-a3 and a12-a15 are
    invisible. So `store_context` can only save its own window, which is the
    thread's window + 4, and `load_context` cannot put the thread back where it
    left off;
  - `CALLINC` cannot be read or written, and `retw` needs it to find the caller's
    window.

  GCC's windowed code itself is fine on this core — it emits manual
  `s32i.n`/`l32i.n` frame stores instead of `spills`/`refill`, so no window
  overflow ever happens. It is only the context switch that cannot be written.

  Under call0, live values are in fixed physical registers and the callee-saved
  set is **a12-a15** (not a4-a11 — see `CALL_REALLY_USED_REGISTERS`,
  `xtensa_conditional_register_usage` and an empirical disassembly test; the GCC
  docs contradict the implementation). A context switch is a flat save/restore of
  a0, a1 and a12-a15.

  The cost: the toolchain's libgcc is windowed in every multilib, so `-lgcc` mixes
  ABIs. A windowed helper clobbers physical a12-a15 (its a8-a15 map to physical
  a12-a19) and returns with `retw`. Worse, `entry` rotates the window by CALLINC=4,
  so a windowed callee reads its arguments from physical a6-a19 while a call0 caller
  put them in a2-a5: the helper does not merely clobber state, it is handed the wrong
  operands. `lib/xtensa_support` supplies the helpers Rockbox needs, written with
  32-bit operations only so they do not call libgcc again. `CORE_LIBS` are searched
  before `-lgcc`, so the Rockbox archive wins.

  Codecs are a separate link (`lib/rbcodec/codecs/codecs.make`) that does not use
  `CORE_LIBS`, so `$(XTENSASUPPORTLIB)` has to be added to `CODEC_LIBS` explicitly.
  After that a full build links without opening libgcc at all.

  **Measured scope** (full-tree compile with the xtensa compiler, 2909 sources):
  26 libgcc symbols — 6 integer (`__ashldi3`, `__ashrdi3`, `__lshrdi3`,
  `__divdi3`, `__moddi3`, `__bswapsi2`) and 20 float. No `long double`, no
  exceptions/unwinding, no overflow builtins; the only builtin that maps to a
  helper is `__builtin_ctzll` in `vuprintf.c`. That is the whole problem.

  No other Rockbox arch reimplements this. Every target links `-lgcc`; only
  ARM7TDMI has an archive (`lib/arm_support`), and only for 32-bit division,
  `clz` and `popcount`, because that core has no hardware divide. Nothing in
  the tree reimplements 64-bit division or soft float.

  The alternative worth keeping open: GCC's own xtensa libgcc sources
  (`lib1funcs.S`, `ieee754-df.S`, `ieee754-sf.S`) assemble correctly with the
  prebuilt compiler under `-mabi=call0` — verified, they emit `ret.n`, not
  `retw`. That needs `xtensa-config-builtin.h`, which is generated at GCC build
  time, and libgcc compiles one `-DL_*` per object (`L_addsubdf3`, `L_cmpdf2`,
  `L_cmpsf2`, not `L_adddf3`). It covers 24 of the 26; `__divdi3`/`__moddi3`
  come from the generic `libgcc2.c`, not the xtensa asm.
- Cache: the toolchain's esp32s3 core config exposes single-precision FP
  opcodes (`madd.s`, `mul.s`, `ufloat.s`). Verify against the silicon before
  relying on them; Rockbox otherwise assumes soft float.
- `N16R8` = 16 MB flash + 8 MB PSRAM. Rockbox's memory model is
  `MEMORYSIZE` (MB) in `configure`, which sizes the audiobuffer and plugin buffer.
  PSRAM is cache-backed and slower than internal SRAM — plan the linker script
  around what must live in internal RAM (stack, ISR, framebuffer).

### The windowed ABI, and why call0 was the wrong call

The `-mabi=call0` override existed because a context switch under the windowed
ABI looked unwritable. It is writable. The esp32s3 core config has **no REGB
option**, so `spills`/`refill`/`mtsr`/`mfsr`/`save1..5` are unavailable — but the
window *exception* handlers still work, and that is all a context switch needs.

`firmware/target/xtensa/vectors.S` now carries `_WindowOverflow4/8/12` and
`_WindowUnderflow4/8/12`, byte-for-byte the same as ESP-IDF's
`xtensa_vectors.S` and libgcc's: the overflow handler saves a0-a3 into the
ancestor's base-save area at `a5-16..a5-4` (or `a9-16..a9-4` for CALLINC 8) and
issues `rfwo`; the underflow handler reverses it and `rfwu`. Alloca is not a
vector on esp32s3 — it is dispatched from EXCCAUSE 5 inside the level-1 handler,
and 0x380 is the double-exception vector.

With those in place:

- `xtensacc()` drops `-mabi=call0`: `-mlongcalls -ffunction-sections
  -mtext-section-literals -fomit-frame-pointer`.
- `lib/xtensa_support` (the hand-written soft-float and 64-bit divide helpers
  needed because libgcc was windowed and unreachable from call0 code) is gone;
  libgcc is used normally.
- `store_context()` calls `__xtensa_libgcc_window_spill()` and then saves a0-a15
  plus `ps`, `windowbase`, `windowstart`, `sar`. `load_context()` restores them.
  After `wsr windowbase` the register *names* change, so the context pointer is
  re-read through a literal in the same section (`j 3f; .long xt_context_addr; 3:
  l32r a6, 10b`) rather than from a register.
- `lib/libsetjmp/xtensa/setjmp.S` mirrors that, and `_JBLEN` for CPU_XTENSA is 20.

The one rule hand-written assembly has to obey: **every C function must be
entered with CALLINC 8** (`callx8`, args in the caller's a10/a11, result in a10).
A frame entered with CALLINC 0 has no base-save area, and the overflow handler
walks the base-save area of every ancestor frame — so a `call0`/`callx0` anywhere
in the chain makes the handler read garbage as the ancestor sp and store into it.
That was the `StoreProhibited` at `ets_get_printf_channel` we were chasing.

### Booting through ESP-IDF's second-stage bootloader (no build required)

The mask ROM at flash offset 0 only copies RAM segments and jumps to the entry
point; it never arms a cache MMU entry, never enables the external caches, and
never releases the instruction bus. That is why a Rockbox image flashed at 0x0
needs crt0 to do the IROM mapping by hand.

ESP-IDF's second-stage bootloader does all of it: `cache_hal_init()`,
`mmu_hal_init()`, flash QIO/OPI setup, CS timing, WDT configuration, then
`load_image()` maps DROM and IROM MMU entries for the app's flash-mapped
segments, copies the RAM segments, and jumps. Rockbox can borrow it instead of
reimplementing it.

It does **not** have to be built. The PlatformIO ESP32 core ships prebuilt ones:

```
~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/bin/
    bootloader_dio_80m.elf   bootloader_opi_80m.elf
    bootloader_qio_80m.elf   bootloader_qio_120m.elf
```

They are ~14 KB, entirely in IRAM, and **windowed ABI** (106 `retw`, 204
`call8`/`call4`/`call12`, zero `call0`, zero `ret.n`) — which is the reason the
windowed-ABI branch can use them at all.

`tools/esp-stage2` flashes the three pieces and generates the partition table
itself (32-byte `esp_partition_info_t` entries, terminated by an `0xEBEB` entry
whose MD5 sits at offset 16, per `ESP_PARTITION_MD5_OFFSET`):

```sh
make flash-stage2 ESPBOOT=~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/bin/bootloader_opi_80m.elf ESPPORT=/dev/ttyACM0
```

Layout: bootloader at 0x0, partition table at 0x8000, Rockbox image at 0x10000
(the `app0` partition). Verified: with `ESP_STAGE2_BOOTLOADER` defined in
`config/meowkits3.h`, crt0 skips the cache-enable and `irom_mmu_map()` sequence
entirely and the MMU entries are still armed — MMU[0..7] = 1..8 for DROM and
MMU[144..148] = 9..13 for IROM.

What the stage-2 loader does *not* do is bring up the octal PSRAM: that lives in
the application, in `components/esp_psram/esp32s3/esp_psram_impl_octal.c`, so
`psram-xtensa.c` still owns it.

### Reaching the ROM from hand-written assembly

Cache maintenance cannot be done from Rockbox on this core config: there are no
cache instructions (`probeD`, `flushD`, `lockD`, `rdtag`, `wttag`, `rinvalidate`,
`binvalidate` are all invalid), and no memory-mapped cache control registers — the
SVD has no cache struct. So the ROM is the only route, and Rockbox links
`-nostdlib` and cannot use ESP-IDF. The addresses are fixed and published in
`components/esp_rom/esp32s3/ld/esp32s3.rom.ld`:

```
Cache_Get_DCache_Line_Size    0x40001608
Cache_Invalidate_DCache_Items 0x40001680
Cache_Clean_Items             0x4000168c
Cache_WriteBack_Items         0x40001698
Cache_Op_Addr                 0x400016a4
Cache_Invalidate_Addr         0x400016b0
Cache_WriteBack_Addr          0x400016c8
Cache_Invalidate_DCache_All   0x400016e0
Cache_WriteBack_All           0x400016f8
```

They are in the base file, not `esp32s3.rom.version.ld`, so they do not move
between ROM revisions.

The ROM is windowed, so a call0 caller has to make the call look like a normal
windowed call. Three things are needed:

1. **`CALLINC` = 4.** It cannot be read or written (no REGB option, so
   `mtsr`/`mfsr` are not valid opcodes), but `callx4` sets it as part of the call,
   and the callee's `entry` and `retw` both use that same value. Nothing has to be
   arranged at startup. `callx4` assembles and disassembles correctly on this core
   (`callx0`=0xc0, `callx4`=0xd0, `callx8`=0xe0, `callx12`=0xf0).
2. **`WINDOW_BASE` pinned to 0** in `crt0.S`, so that the window call0 code uses is
   window 0 and the register arithmetic below is absolute rather than relative to
   whatever the bootloader left.
3. **`a12-a15` saved around the call.** With the window rotated by 4 the callee can
   reach physical a4-a19; under call0 only a12-a15 are callee-saved, so those four
   are the ones that matter. a0-a3 are out of reach of the callee.

Arguments go in the caller's a6 and a7, which become the callee's a2 and a3. That
is checkable from a disassembly of any windowed call: GCC's `call8` callers put
their arguments in a10/a11, exactly two registers above the callee's a2/a3. The
return value works the same way - the callee's a2 is the caller's a(N+2), so a
`callx4` callee returns in the caller's **a6**, not a2. A call0 trampoline has to
copy a6 to a2 before its `ret.n`, otherwise the caller gets whatever a2 held before
the call (in Rockbox's case, the ROM function address).

`firmware/target/xtensa/rom_cache.S` is the trampoline, and `rom_cache.h` wraps the
addresses. Verified to assemble and link.

**Checked against the ROM binary itself** (`esp-rom-elfs`, `esp32s3_rev0_rom.elf`).
Each published address is a two-instruction trampoline - `l32r a9, <real address>`
then `jx a9` - so the jump does not change the window; the callee's own `entry` does.
The real functions are exactly as the trampoline assumes:

| Function | Real address | Entry | Args | Return |
|---|---|---|---|---|
| `Cache_Invalidate_Addr` | 0x4004e9ac | `entry a1, 48` | a2 = addr, a3 = size | `retw.n` |
| `Cache_WriteBack_Addr` | 0x4004ea64 | `entry a1, 48` | a2 = addr, a3 = size | `retw.n` |
| `Cache_Invalidate_DCache_All` | 0x4004eac4 | `entry a1, 32` | none | `retw.n` |

They dispatch further through `rom_cache_internal_table_ptr` at `0x3fceffc4`, which
is a `.data.interface.cache` section of the ROM image, not something ESP-IDF sets
up - so it is initialised by the mask ROM before it jumps to the image.

What is still **not** verified: that `rfe` restores correctly when `WINDOW_BASE` is
set manually before it. That is ISA behaviour and needs a hardware test.

The alternative, if the ROM turns out to be unreachable: keep everything the DMA
touches out of the D-cache address room. `Cache_Clean_Addr`'s own documentation
says "If the region is not in DCache addr room, nothing will be done", which tells
us the D-cache address room is DRAM0 (`0x3FC88000-0x3FD00000`, exactly
`SOC_DMA_LOW`/`SOC_DMA_HIGH`). RTC fast memory (0x600FE000, 8 KB) and ULP SRAM
(0x50000000, 8 KB) are outside it. SDHOST descriptors can live there because
`SDMMC_DSCADDR_REG` is a plain 32-bit register; GDMA cannot, because its descriptor
address comes from a 20-bit window based at `0x3FC00000`.

### MeowKit board (the reference hardware)

From the stock firmware (`/tmp/meowkit`, `src/bsp/config.h`):

| Block | Detail |
|---|---|
| Display | ST7789 IPS **320×240**, SPI (MOSI 40, SCLK 41, DC 39), PWM backlight GPIO 42. CS and RST are **not GPIOs** — they go through a PCA9557 I²C IO expander. |
| Touch | FT6336 capacitive, I²C 0x38, INT not connected. |
| Audio out | ES8311 DAC over I2S (MCLK 15, BCLK 14, WS 13, DOUT 16), 44100 Hz, 16-bit. The DAC's headphone outputs feed **both** the 3.5 mm jack and, through R32/R33, an NS4150B class-D amp → 1 W speaker. Amp enable is PCA9557 IO3 and gates **the speaker only** — headphones work with the amp off. |
| Audio in | ES7210 ADC, I2S DIN GPIO 3, I²C 0x41, MEMS mic. |
| Storage | SD card, **SDMMC 1-bit** (CLK 47, CMD 48, D0 21). 16 MB SPI flash for firmware only. |
| Input | Buttons A (GPIO 6), B (GPIO 4), 5-way joystick (UP 12, DOWN 18, LEFT 17, RIGHT 8). All **active-low with internal pull-ups**, 80–100 ms debounce. |
| Power | AXP173 PMU, I²C 0x34, Li-ion 1400 mAh, USB-C charging. IRQ on GPIO 43, active low. |
| RTC | PCF8563, I²C 0x51. |
| IMU | BMI270 6-axis + BMM150 magnetometer, I²C 0x68. |
| Misc | WS2812B single LED (GPIO 38), IR TX 7 / RX 5 at 38 kHz. |
| USB | Native USB-OTG, fixed pins DP 20 / DN 19. Used as CDC and MSC in stock firmware. |

Two things that are not obvious from a pin list: **half the control signals live on
an I²C IO expander** (PCA9557 at 0x19), so a Rockbox target cannot treat the display
as a plain SPI device; and the audio path is an I2S DAC with no codec controller of its
own, which is a different shape from every existing Rockbox audio backend.

Three more that the reference firmware makes clear but a pin list does not:

- **The panel's GRAM is 240×320 and it is driven as 320×240.** LovyanGFX is configured
  with `panel_width 240`, `panel_height 320` and `offset_rotation 3`; the rotation table
  then swaps the dimensions. `LCD_WIDTH`/`LCD_HEIGHT` must match the used orientation,
  not the panel's native one.
- **The backlight is active low.** `initBacklight()` writes duty 512 (full HIGH) to turn
  it *off*, and `setBrightness(255)` writes 0.
- **GPSPI's clock does not come from the 240 MHz CPU clock.** `CLK_GATE` bit 2 selects
  PLL_CLK_80M, cleared gives XTAL (40 MHz), and `CLK_EQU_SYSCLK` uses that source
  undivided. The reference driver runs the display at 80 MHz.

Mapping the board against what Rockbox already has:

| Board block | Rockbox support | Status |
|---|---|---|
| ST7789 display | `HAVE_LCD_BITMAP`, `HAVE_LCD_ENABLE`, `HAVE_LCD_SLEEP`, `HAVE_LCD_SHUTDOWN` | done |
| PCA9557 expander | (target-internal) | done |
| ES8311 + amp | `audiohw`, `HAVE_SPEAKER` | done |
| SD card | `STORAGE_SD` via `sdmmc_host.c` | done |
| Buttons | `CONFIG_KEYPAD` | done |
| Backlight PWM | `HAVE_BACKLIGHT_BRIGHTNESS` | done |
| PCF8563 RTC | `CONFIG_RTC` | done |
| AXP173 PMU | `CONFIG_CHARGING`, battery, PEK power key | done |
| FT6336 touch | `HAVE_TOUCHSCREEN`, `drivers/ft6x06.c` | done: polled from `tick_task_thread`, because the INT pin is not connected — see the poll note below |
| ES7210 mic | `HAVE_RECORDING`, `target/xtensa/es7210.c`, `target/xtensa/pcm-xtensa.c` | done: I2S0 RX in slave mode, GDMA IN channel 0 |
| PSRAM bring-up | required by the audiobuffer, framebuffer, codecbuf, pluginbuf | done in `target/xtensa/psram-xtensa.c`, run from `crt0.S` before `mmu_init()`; neither the ROM nor ESP-IDF's bootloader does it |
| USB-OTG | `usbstack` | open: the chip has a Synopsys DWC-OTG block (`soc/usb_reg.h`, registers at `SOC_DPORT_USB_BASE`, device side at +0xb00), but there is no `usb_hw` layer for this core. `usb-xtensa.c` is a stub. Not needed for charging (AXP173 reports VBUS) or storage (SD) |
| WS2812B LED, IR, IMU, temp sensor, SPI flash | nothing in Rockbox | not applicable |

### Display power states

Rockbox asks a target for three levels, and they are not interchangeable:

| Hook | Called by | Meaning |
|---|---|---|
| `lcd_enable(bool)` | `backlight_hw_on()`/`backlight_hw_off()` in the backlight driver | shallow on/off |
| `lcd_sleep()` | `backlight.c`, `LCD_SLEEP_TIMEOUT` after the backlight went off | "deeper than `lcd_enable(false)`" |
| `lcd_shutdown()` | `shutdown_hw()` in `powermgmt.c`, before `power_off()` | final |

`lcd_active()`/`lcd_set_active()` come from `drivers/lcd-memframe.c` and are backed
by the global `lcd_on`; a target driver that uses them needs `extern bool lcd_on;`.
The update path must return early when the panel is inactive - the framebuffer keeps
accumulating and is pushed in full on wake.

For the ST7789 that maps to `0x28 DISPOFF` / `0x29 DISPON` for the shallow state and
`0x10 SLPIN` / `0x11 SLPOUT` for the deep one. SLPOUT needs the same ~130 ms recovery
as after a reset. `lcd_awake()` is required by `HAVE_LCD_SLEEP` even if the driver
never calls it: `backlight.c` calls it on `SYS_POWEROFF`.

There is no "display off while audio plays" hook in Rockbox. The sleep countdown
starts when the backlight goes off, so the way to get a dark screen while listening is
the backlight timeout, and the panel follows a moment later.

### GPSPI2 display path

The panel is driven by GPSPI2 (`0x60024000`) in **polled CPU-buffer mode**, not DMA:
`SPI_DMA_CONF = 0`, data comes from the 16-word buffer at `+0x98`. Register offsets
from `components/soc/esp32s3/register/soc/spi_reg.h`: CMD `0x00`, ADDR `0x04`,
CTRL `0x08`, CLOCK `0x0C`, USER `0x10`, USER1 `0x14`, USER2 `0x18`, MS_DLEN `0x1C`,
MISC `0x20`, DMA_CONF `0x30`, W0 `0x98`.

- **`SPI_MS_DLEN` is a bit count, stored as `bit_num - 1`** (`SPI_MS_DATA_BITLEN`,
  bits [17:0]). A 64-byte chunk must be `511`. Writing `words * 8 - 1` where `words`
  counts 32-bit words sends 128 bits — a quarter of the buffer. Commands and their
  1-4 byte arguments are exactly one word, so the init sequence, MADCTL and the
  window commands all behave correctly while the pixel data is truncated. That is the
  signature of "geometry right, content garbage".
- **CONF registers must be latched before `USR`**: `CMD.UPDATE` (bit 23) copies USER,
  CLOCK and MS_DLEN from the APB domain into the SPI clock domain and self-clears.
  ESP-IDF's `spi_ll_apply_config()` is exactly `cmd.update = 1; while (cmd.update);`.
  `CMD.USR` is bit 24.
- **The clock source is selected in CLK_GATE (`+0xE8`)**, not by the CPU clock:
  bit 2 `MST_CLK_SEL` = 1 chooses PLL_CLK_80M, cleared gives XTAL (40 MHz). The SPI
  clock is `source / ((clkdiv_pre+1) * (clkcnt_n+1))`, with `clkcnt_l` [5:0],
  `clkcnt_h` [11:6], `clkcnt_n` [17:12], `clkdiv_pre` [21:18], `clk_equ_sysclk` bit 31.
  `clkcnt_h` must be `floor((clkcnt_n+1)/2 - 1)` and `clkcnt_l == clkcnt_n`.
- **`SPI_MS_DLEN` is an exact bit count, not a word count.** `SPI_MS_DATA_BITLEN`
  is bits [17:0] and holds `bit_num - 1`. The CPU FIFO is addressed in 32-bit
  words, but the transfer length does not have to be a multiple of 32. Rounding
  `MS_DLEN` up to `words*32-1` pads every one-byte command with 24 zero bits, and
  the panel reads those as argument bytes: `CASET` and `RASET` get a window made
  of the padding instead of the values sent. ESP-IDF's `spi_ll_write_buffer()` sets
  the bit length exactly; do the same and only round the *buffer fill* up to words.
- **`CASET`/`RASET` do not start a write.** The ST7789 only accepts pixel data
  after `RAMWR` (`0x2C`). LovyanGFX's `set_window_8()` ends with
  `writeCommand(cmd, 8)` and every caller passes `CMD_RAMWR`. A driver that sends
  the two window commands and then pixel bytes with DC high has no active write,
  so the whole frame is discarded and the panel keeps the GRAM left by the previous
  boot. The symptom is noise that does not change when the image changes, which is
  easy to mistake for a broken SPI link.
- **CS is an expander output, not a GPIO.** Toggling it per transfer costs a full I2C
  transaction (~1.6 ms at 100 kHz) and puts every pixel write on the same bus as the
  codec, PMU and touchscreen. The reference firmware never toggles CS: LovyanGFX is
  configured with `pin_cs = -1` and the PCA9557 simply holds CS asserted after the
  reset. Assert it once in `st7789_init()` and leave it.
- **MOSI is `FSPID_OUT` (103), not `FSPIQ_OUT` (102).** `spid_out` is MOSI and
  `spiq_out` is MISO in `components/esp_hal_gpspi/esp32s3/spi_periph.c`; the
  signal comments in `gpio-xtensa.h` had them swapped.
- The buffer is little-endian: byte 0 of the transfer sits in bits [7:0] of `SPI_W0`
  (`spi_ll_write_buffer` does `memcpy(&word, &buf[x/8], 4); hw->data_buf[x/32] = word`).

### I2S and GDMA

The I2S peripheral does have a DMA controller — "DMA controller that allows stream
sampling of data without requiring the CPU to copy each data sample". Without it the
CPU would have to feed the FIFO at the sample rate, which is not workable.

Verified from `components/soc/esp32s3/register/soc/reg_base.h` and `gdma_channel.h`:

| | |
|---|---|
| I2S0 | `0x6000F000` |
| I2S1 | `0x6002D000` |
| GDMA | `0x6003F000` |
| GDMA trigger ids | I2S0 = 3, I2S1 = 4, SPI2 = 0, SPI3 = 1 |
| GDMA channel stride | IN: `base + 0xC0 * n`; OUT: `base + 0x60 + 0xC0 * n` |
| I2S registers | `INT_RAW/ST/ENA/CLR`, `RX_CONF`, `TX_CONF`, `RX_CONF1`, `TX_CONF1`, clock mux + divider, TDM ctrl, timing |

### There is no NULL-terminated descriptor list on this GDMA

`next = 0` does **not** end a link list. With `out_auto_wrback` (`OUT_CONF0`
bit 2) set, the engine follows `next` unconditionally the moment a descriptor
retires, so `next = 0` means "fetch the next descriptor from address
`0x00000000`". It does that, reads owner clear and length zero, and raises
`out_dscr_err`. Repeat per descriptor.

The two settings are a choice, not a combination:

| want | configure |
|---|---|
| stop at the end of one descriptor, restart from software | `out_auto_wrback` **off**; `next` is then never followed |
| free-running ring | `out_auto_wrback` **on**; chain `next` through every slot and keep every slot DMA-owned and filled |

Rockbox's own `imx233` sink gets "stop at the end" from a DMA command carrying
`next = NULL`, because that peripheral really does treat NULL as a stop. That
convention does not carry over. The xtensa TX path was written against the
imx233 model and inherited the assumption while also enabling auto-writeback;
the result was one descriptor fault per audio frame, each one stalling the
transport until the handler restarted it — audible as crackle at 172 Hz.
Measured over 53,833 frames: faults went from one per frame to one for the
whole run by clearing that one bit.

Two related facts, both measured rather than read:

- **`OUT_DSCR_STATE` (`OUT_STATE` bits 19:18) is undocumented.** `gdma_struct.h`
  declares the field `/*reserved*/` and `grep -rn last_dscr` across IDF returns
  nothing. Do not interpret its value.
- **A DSCR_ERR handler must not write `link.stop` after a DONE handler has
  written `link.start`.** Both branches typically act on one snapshot of
  `INT_ST`, so when the two bits are pending together the error branch cancels
  the restart — and a stopped outlink raises no completions, so nothing ever
  restarts it. Handle the error first, and let it start rather than stop.

### Buffers can be in PSRAM; descriptors cannot

Settled by testing rather than inference.

**Buffers may live in PSRAM.** `SOC_PSRAM_DMA_CAPABLE = 1` and `GDMA_LL_AHB_PSRAM_CAPABLE = 1`
for esp32s3, and the I2S driver has a `dma_buffer_in_psram` option. The memory-mapping docs
state the catch plainly: "PSRAM can also be accessed by EDMA. Data desynchronization may
happen because hardware does not guarantee the data consistency under such condition. You
should call `esp_cache_msync()` to synchronize the Cache and the PSRAM." A PSRAM DMA target
therefore needs write-back before the DMA reads it and invalidation after the DMA writes it.

**Descriptors must be in internal RAM.** ESP-IDF's own I2S documentation says it directly: "DMA
descriptors remain in internal RAM." The reason is the 20-bit register. Verified by assigning
addresses to the bitfield and printing what survives: `0x3C010000` (audiobuffer, MMU entry 1)
becomes `0x00010000`, while `0x3FC88000` (internal SRAM) becomes `0x00088000`. The 20-bit window
is based at `0x3FC00000`, so internal SRAM maps to offset `0x88000` correctly and a PSRAM address
loses its high bits. `SOC_NON_CACHEABLE_OFFSET_PSRAM` is not defined for esp32s3, so ESP-IDF's
`gdma_new_link_list` falls back to internal memory for link items.

The asymmetry: only the *first descriptor's address* comes from the 20-bit register. Inside a
descriptor, the buffer address and the next-pointer are full 32-bit words read from memory —
which is why PSRAM buffers work and PSRAM descriptors do not.

Descriptor layout (12 bytes, from `gdma_link_list_item_t`):

| Word | Contents |
|---|---|
| 0 | `size:12`, `length:12`, reserved 4, `err_eof:1`, reserved 1, `suc_eof:1`, `owner:1` |
| 1 | buffer address, full 32-bit |
| 2 | next descriptor address, full 32-bit |

`GDMA_MAX_BUFFER_SIZE_PER_LINK_ITEM` is 4095, so anything larger needs a chain. The `owner` bit
hands a descriptor to the hardware; the CPU must check it before reuse, or enable
`in_check_owner` to have the hardware check it. ESP-IDF writes descriptors through the
non-cached alias but stores **cached** addresses in `next` ("DMA expects cached addresses in
`next`"), so the address form matters.

Consequence for Rockbox: the audiobuffer in the PSRAM window can be the DMA target, but only with
explicit cache maintenance around each transfer and the descriptor ring in internal SRAM.
Rockbox has no hook for that today — cache maintenance is a no-op in the TAL header, which is
correct for CPU access but not for a DMA that bypasses the cache.

**Design decision:** keep the descriptor ring and the DMA buffers in internal SRAM. That is what
ESP-IDF does, and it keeps the 20-bit register happy, but it does **not** sidestep cache
maintenance: DRAM0 (`0x3FC88000-0x3FD00000`) is exactly the D-cache address room, and the DMA
reads through it rather than around it. The cost is a copy from the audiobuffer into a ping-pong
pair — a step no existing Rockbox audio backend needs. A 2 × 4096-sample pair is about 32 KB,
which fits inside the 337 KB static region alongside .bss.

ESP-IDF's SDHOST code calls `esp_cache_msync()` on every descriptor it arms and on every
buffer it hands over (`components/sdmmc/sdmmc_transaction.c`), with the descriptor calls
guarded by `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`. That define is absent for esp32, esp32s2,
esp32c3 and esp32s3 alike, which is a discrepancy worth noting rather than trusting: the ROM
functions themselves say they do nothing "if the region is not in DCache addr room", so doing
the maintenance is safe whichever way the silicon behaves. Rockbox does it.

If we ever want to DMA straight out of the PSRAM audiobuffer, the requirements are: descriptors in
internal SRAM, cached addresses in the descriptor fields, and commit-before-write / invalidate-
after-read around every buffer.

Expander pins (registers: 0x00 input, 0x01 output, 0x02 polarity inversion,
0x03 configuration where 1 = input):

| Pin | Function |
|---|---|
| IO0 | LCD reset |
| IO1 | LCD chip select |
| IO2 | SPI flash chip select |
| IO3 | NS4150B amp enable (active high, 10K pull-down). Speaker only; the headphone jack is upstream of this. |
| IO6 | Touch reset |

Init order is forced by this: **I²C → PCA9557 → display**. The reference firmware
verifies the expander answers before relying on it (5 retries), then asserts resets,
waits 20 ms, releases, waits 120 ms (ST7789 needs ~120 ms after release), then
selects the LCD.

### Interrupts: no INTBASE, but VECBASE works

There is no interrupt vector table in the usual sense. Tested against the esp32s3
dynconfig (`xtensa_esp32s3.so`):

| | |
|---|---|
| `wsr.excb` | **not a valid opcode** — no exception base register |
| `wsr.intbase` | **not a valid opcode** — no per-level interrupt base |
| `enableint` / `disint` | **not available** |
| `wsr.vecbase` | **available** — `XCHAL_HAVE_VECBASE 1`, so the vectors are relocatable |
| `wsr.intenable`, `wsr.intclear`, `wsr.intset` | available |
| `rsr.interrupt` | available — the pending bitmask over all 32 slots |
| `rsr.intlevel` / `wsr.intlevel` | **not valid opcodes** — the level is `PS` bits 0-3 (`PS_INTLEVEL_SHIFT 0`, mask `0x0F`, values 0-15) |
| `rfe`, `rfi n` | available |
| memory-mapped priority registers | none in `INTERRUPT_CORE0` |

So a handler is installed by setting VECBASE to a table and unmasking a slot in
INTENABLE. Two consequences of the missing level registers:

1. **Levels cannot be programmed.** The level of each slot is fixed by the core config.
   Level-1 slots are 0-10, 12, 13, 17, 18; slot 0 is reserved for the WiFi MAC and slot 4
   for the WiFi baseband. A handler can only be entered for a slot that is already level 1.
2. **Level 1 has no vector of its own.** It is reached through the exception vectors: the
   kernel vector at VECBASE+0x300 and the user vector at VECBASE+0x340, chosen by PS.UM.
   Level 1 does **not** share the window vector. The full layout, confirmed against an
   independent ESP32-S3 bare-metal vector table:

   | Offset | Vector |
   |---|---|
   | 0x000–0x17C | window overflow/underflow for CALLINC 4, 8, 12, plus the alloca handler |
   | 0x180 | level 2 |
   | 0x1c0 | level 3 |
   | 0x200 | level 4 |
   | 0x240 | level 5 |
   | 0x280 | level 6 / debug |
   | 0x2c0 | NMI |
   | 0x300 | kernel exception (level 1 from supervisor mode) |
   | 0x340 | user exception (level 1 from user mode) |
   | 0x380 | software exception |
   | 0x3c0 | double exception |
   | 0x400 | hardware exception |

   Under call0 nothing can spill, so no window vector can be reached; they are still
   filled with self-loop traps rather than left as linker padding.

`rsr.eps1` and `rsr.sregs` are not valid either, even though `rfe` works: the saved PS and
window base live in internal state the assembler calls `sysregs`, which only `rfe` can read.
(`rsr.eps2` through `rsr.eps7` do exist.)

The vector table must be a physical address the core can fetch from, so it goes in internal
SRAM on the instruction-bus view, not in the flash cache window. And the `j` in the table has
an 18-bit signed range, so the handler has to be in the same region.

Windowed-ABI context switching is also impossible: the esp32s3 dynconfig has no REGB
option, so `mtsr`/`mfsr`/`spills`/`spillw` are unavailable. That is a second reason the
call0 ABI is the only workable one.

**`-mlongcalls` is what makes an ISR in internal SRAM able to call anything.** The vector
table must be a physical address the core can fetch from, so it is in internal SRAM, 1.6 MB
from the flash cache window where the rest of the code lives. GCC's `call8` has a 128 KB
range, so a plain call from the handler to the mixer fails at link time with
`dangerous relocation: call0: call target out of range`. `-mlongcalls` (already in
`xtensacc()`) emits `l32r` + `callx0` with an absolute address instead, which has no range
limit. It does not help hand-written asm, so the handler itself has to sit next to the table.

The audio path uses the GDMA interrupt. `OUT_DONE` (bit 0) fires when the last data of a
descriptor has actually been transmitted, which is when the buffer is safe to overwrite;
`OUT_EOF` (bit 1) fires earlier, when the data has only been read from memory and is still in
the FIFO, so using EOF would let the CPU overwrite audio that is still being played.

The handler runs on the interrupted thread's stack, so `DEFAULT_STACK_SIZE` for xtensa is
0x1000 rather than the 0x400 that m68k and mips use — same reason ARM sizes its stacks
larger.

The same applies to the kernel tick. Tested which core registers do exist:

| | |
|---|---|
| `rsr.ccount`, `wsr.ccount`, `wsr.ccompare0` | valid — the core timer is reachable |
| `rsr.intlevel`, `wsr.intlevel` | **not valid opcodes** |
| `ssrf` | **not valid** |

So the core timer can be read but its interrupt cannot be taken. `tick_start()` is a
per-target function, and every existing implementation arms a hardware timer and enables
its interrupt. For Xtensa it has to be a polling thread instead.

Note that `XCHAL_HAVE_WINDOWED` is 1 and `XCHAL_NUM_AREGS` is 64 for this core — the
hardware has register windows. What is missing is the REGB option, so `mtsr`/`mfsr` and
the spill/reflect macros are unavailable and windowed context switching cannot be written.

### The level-1 handler has to give `a1` a base-save area

The window overflow/underflow handlers do not walk the stack; they walk the **ABI base-save
chain hanging off `a1`**. For a frame created by `entry a1, N`, the four words at
`a1-16 .. a1-4` hold that frame's `a0`, `sp`, `a2`, `a3`, and `a1-12` is how the overflow
handler reaches the *ancestor* frame's stack. Every nested `call8` rotates the window; when
the rotation reaches a window that still holds a live ancestor register, the overflow handler
needs that chain.

A handler frame built with `addi a1, a1, -FR_SIZE` has no base-save area at all. The handler
then reads uninitialised stack as the ancestor's `sp` and stores through it. That is the
`StoreProhibited` we were chasing: `excvaddr` was `0x0` once and `0x84` another time, both of
them `a1 - 16` for some garbage `a1`. So the handler must synthesise the area itself, right
after saving the registers:

```asm
    addi    a1, a1, -FR_SIZE
    ... save a0-a15, ps, epc1, sar, lbeg/lend/lcount ...
    addi    a10, a1, FR_SIZE        /* the interruptee's sp */
    s32e    a0,  a1, -16
    s32e    a10, a1, -12
    s32e    a2,  a1, -8
    s32e    a3,  a1, -4
```

Three things make this work and are easy to get wrong:

- The area goes **below** the handler's frame base, i.e. into unused stack. Writing it above
  the frame overlaps the frame itself.
- `a0`, `a2` and `a3` are still the interruptee's live values at that point — they were copied
  into the frame, not overwritten — so they are valid content for the area. `a10` is scratch.
- Scratch registers are only free in the *overflow* handlers, where `a0-a3` have already been
  spilled. In the *underflow* handlers `a0-a3` are the registers being restored, so anything
  that needs scratch there has to run before the loads.

ESP-IDF's `SPILL_ALL_WINDOWS` in the handler (`XT_ISR_SPILL` in `vectors.S`) is the other way
to make the chain valid, and it is the wrong one here: it runs the rotation from the
interruptee's frame, whose ancestors are real C frames, and the extra rotations raise
overflow exceptions inside the handler. With `PS.EXCM` cleared before the dispatch the window
is pinned and nothing needs spilling, so the base-save area above is sufficient. Measured:
`XT_ISR_SPILL=1` crashes in `switch_thread` within a few context switches once the SDHOST
interrupt is unmasked; `XT_ISR_SPILL=0` boots and cycles all threads indefinitely.

### Memory map (from the toolchain's own linker scripts, not guessed)

> **Measured budgets and the full arena layout now live in
> `firmware/target/xtensa/MEMORY.md`**, generated from `rockbox.elf` rather than
> from headers, with the commands to re-derive every number. Read that first for
> current free space -- internal SRAM is down to ~1 KiB and the figures here are
> the design rationale, not the live budget.

The prebuilt toolchain ships `lib/esp32s3/memory.elf.ld` and ESP-IDF's
`components/esp_system/ld/esp32s3/memory.ld.in`. The important part is that
**instruction and data buses see the same physical SRAM at different addresses**:

```
0x40370000  IRAM / I-cache (instruction bus only)
0x40378000  D/IRAM shared region, instruction view
0x3FC88000  D/IRAM shared region, data view      <- same physical RAM
0x3FCF0000  end of the data view
0x3FD00000  DRAM / D-cache end
```

The bus offset is a constant `0x6F0000`: subtract it from an instruction-bus
address to get the data-bus address of the same bytes. This is the single most
important thing to get right in a linker script. Putting `.iram` at `0x40378000`
and `.data` at `0x3FC88000` looks like two disjoint regions and is in fact the
same memory.

- Static `.data`/`.bss` is capped at `I_D_SRAM_SIZE = 0x53700` (**337 KB**), not the
  full 512 KB. The rest is heap only, and only after the app starts.
- ESP-IDF's `iram0_0_seg` and `dram0_0_seg` overlap by design, and
  `sections.ld.in` contains a `.dram0.dummy (NOLOAD)` section whose only purpose is
  to advance the location counter: *"This section is required to skip .iram0.text
  area because iram0_0_seg and dram0_0_seg reflect the same address space on
  different buses."* Rockbox's `rom.lds` assumes one contiguous region, so the
  xtensa script reserves the instruction region up front and starts `DRAMORIG` at
  its alias. The reserve is a constant rather than `SIZEOF(.iram)` because plugins
  are linked separately and cannot see where the firmware's code ends.
- **PSRAM and flash rodata share the same 32 MB window** at `0x3C000020`
  (`extern_ram_seg` and `drom0_0_seg` are on the same bus). ESP-IDF papers over it
  with a dummy section sized by the rodata already placed there. You cannot map all
  8 MB of PSRAM and all flash rodata at the same time.
- The window is bank-switched per 64 KB page by a **512-entry table of 32-bit
  values at `0x600C5000`** (`DR_REG_MMU_TABLE`, stride 4): bit 15 target (0 = flash,
  1 = PSRAM), bit 14 invalid, bits 0-13 physical page number. **The entry index is
  the masked address** (`vaddr & 0x1FFFFFF`, not `vaddr - 0x3C000000`), so `0x3C010000`
  is entry 1, not 15361. Getting this wrong makes `mmu_init` return immediately and the
  audiobuffer reads flash instead of PSRAM.
- Mapping a page is not enough: ESP-IDF calls `cache_hal_invalidate_addr()` on the
  newly mapped range, because the cache may hold lines for an address that only
  became valid a moment earlier.
- **The ROM loader arms MMU entries past the end of `.rodata`, so the PSRAM mapping
  has to be unconditional.** Measured: `.rodata` is `0x3C000020` + `0x7A194`, i.e.
  entries 0..7, but entry 8 (`0x3C080000`) is armed to flash page 8 anyway — the
  loader rounds the page count up. A *store* through a flash-mapped Ext_RAM page has
  no write path in the hardware and stalls the core with no exception, so "the entry
  is already valid, leave it alone" is wrong for a range that must be PSRAM.
- **Ext_RAM reads and writes are not the same path.** Reads through a flash-mapped
  page work; writes through one hang. PSRAM-mapped pages read and write fine once
  the chip is up.
- **Octal PSRAM identification is self-authenticating, a sync read is not.** Writing
  `0x28` to MR0 and reading back `0x0d28` proves the chip answered: the low byte is
  what was sent, the high byte is MR1 (`0x0d` = Apotek), which was never sent. A
  single sync write/read at one address cannot distinguish a real answer from
  `SPI_W0` echoing the last transmitted word.
- Flash code is executed through the instruction cache window at `0x42000020`.
- RTC fast memory 8 KB at `0x600FE000`, RTC slow 8 KB at `0x50000000`.

Rockbox's single-region `rom.lds` model does not survive this. The xtensa linker
script needs at least three regions (IRAM, DRAM, external) and the two dummy
sections, and the framebuffer has to be placed deliberately: PSRAM is fine for a
320×240 double buffer (2 × 150 KB) but it is cache-backed and cannot be touched by
anything that must work before the cache is enabled.

### PSRAM bring-up is Rockbox's job

**The stock MeowKit firmware proves the chip is alive.** `meowkit-s3-v1.0.2.bin` is a
full flash image: ESP-IDF second-stage bootloader at 0x0 (entry `0x403c98d8`, three
segments, DROM at `0x3fce3808`), partition table at `0x8000`, app at `0x10000`. It is
arduino-esp32 on ESP-IDF **v4.4.3**, and it reports `PSRAM free: 8385839` - the 8 MB
octal PSRAM works. So every failure in Rockbox's bring-up is a software bug, not a
dead chip. Two consequences:

- The mask ROM *can* run an ESP-IDF stage-2 bootloader on this board, so the
  `ets_loader.c 78` loop we hit with our own stage-2 image is not inherent to the
  layout; the stock bootloader's DROM segment at `0x3fce3808` maps fine.
- PSRAM init is in the application, not the loader. None of the four prebuilt
  ESP32-S3 bootloaders (`bootloader_dio_80m`, `bootloader_opi_80m`,
  `bootloader_qio_120m`, `bootloader_qio_80m`) contains a PSRAM symbol.

**The mode argument to the ROM OPI command is an enum, not a flag.**
`esp_rom_spiflash_read_mode_t` from `components/esp_rom/include/esp32s3/rom/spi_flash.h`:

```
QIO=0  QOUT=1  DIO=2  DOUT=3  FASTRD=4  SLOWRD=5  OPI_STR=6  OPI_DTR=7
```

`esp_psram` passes `ESP_ROM_SPIFLASH_OPI_DTR_MODE` = **7**. Passing 4 (DOUT) makes the
ROM build a single-line transaction: `USER1` and `USER2` stay at their reset defaults
(`0x5C000007`, `0x70000000`), `MISO_DLEN` and `CACHE_FCTRL` are set, and the data phase
returns whatever is floating on the lines. That is the signature to look for.

**ESP-IDF 4.4.3's sequence** (`components/esp_hw_support/port/esp32s3/opiram_psram.c`,
`psram_enable()`), in order:

| Step | Register effect |
|---|---|
| `s_init_psram_pins()` | pad 26 -> `FUNC_SPICS1`, drive 3; `SPI_MEM_DATE_REG(0)` clock drive 3. **Does not touch pads 27/28 or 33..37** |
| `psram_set_cs_timing()` | `SPI_MEM_SPI_SMEM_AC_REG(0)`: CS_SETUP, CS_HOLD, hold time 3, setup time 3, hold delay 2 |
| `spi_timing_enter_mspi_low_speed_mode(true)` | `SPI_MEM_CORE_CLK_SEL_REG(0)` = 0 (80 MHz), `SPI_MEM_CLOCK_REG(0)` and `(1)` = div 4, `DIN_MODE`/`DIN_NUM`/`TIMING_CALI` cleared on both |
| `SET_PERI_REG_MASK(SPI_MEM_DDR_REG(1), VAR_DUMMY)` | SPI1's FMEM_DDR |
| `esp_rom_spi_set_dtr_swap_mode(1, false, false)` | |
| `s_init_psram_mode_reg(1, ...)` | MR0 read-modify-write: lt=1, read_latency=2, drive_str=0; MR8 bl=3 |
| `s_get_psram_mode_reg(1, ...)` | MR0/1, MR2/3, MR4, MR8 reads |
| `spi_timing_psram_tuning()` | sweeps `DIN_MODE`, `DIN_NUM`, `TIMING_CALI` extra dummy |
| `spi_timing_enter_mspi_high_speed_mode(true)` | core clock back to 160 MHz, flash div 2, `SPI_MEM_SRAM_CLK_REG(0)` div 2, tuning regs restored |
| `spi_flash_set_rom_required_regs()` | restores SPI1 to the ROM's init state |
| `spi_flash_set_vendor_required_regs()` | flash chip specific |
| `s_config_psram_spi_phases()` | Ext_RAM octal phases, then `Cache_Resume_DCache(0)` |

Measured reset state of the MSPI/pads when entering from the mask ROM (before Rockbox
writes anything):

| | value | reading |
|---|---|---|
| IO_MUX pads 26, 27, 28 | `0x1B00` | MCU_SEL = 1 -> GPIO, not SPI |
| IO_MUX pads 29..32 | `0x0B00` | MCU_SEL = 0 -> SPI (flash) |
| IO_MUX pads 33..37 | `0x0A00` | MCU_SEL = 0 -> GPIO, not SPIIO4-7/SPIDQS |
| `SPI_MEM_CTRL_REG(1)` | `0x00aca278` | octal phase bits already set by the ROM |
| `SPI_MEM_CACHE_SCTRL_REG(0)` | `0x0055c070` | Ext_RAM cache configured for quad, 22-bit address |
| `SPI_MEM_DDR_REG(1)` | `0x00000000` | DDR_EN clear |
| `SPI_MEM_SPI_SMEM_DDR_REG(0)` | `0x00003020` | |
| `SPI_MEM_CORE_CLK_SEL_REG(1)` | `0x00000000` | 80 MHz |
| `SPI_MEM_CLOCK_REG(1)` | `0x00070307` | div 8 of 80 MHz = 10 MHz |

ESP-IDF's assumption that pads 33..37 are already routed to the SPI function only
holds because its bootloader configured the flash for QIO first. Rockbox enters from
the mask ROM, which reads the flash in DOUT and leaves those pads as GPIO, so the
routing has to be done in `init_psram_pins()`.

Nothing else does it. The mask ROM loader maps MMU pages for the segments in the
image and leaves the external RAM alone; ESP-IDF's PSRAM init lives in the
application (`components/esp_psram/esp32s3/esp_psram_impl_octal.c`), not in the
bootloader. So `firmware/target/xtensa/psram-xtensa.c` runs from `crt0.S` before
`mmu_init()`.

The 8 MB octal PSRAM on the WROOM-1-N16R8 module hangs off **SPI1** (`0x60002000`,
the second MSPI instance, one `0x1000` below SPI0 at `0x60003000`) on CS1, and is
reached through SPI0's Ext_RAM path once that path's command/address/dummy phases
are configured for octal mode. Register offsets are from
`components/soc/esp32s3/register/soc/spi_mem_reg.h`; `REG_SPI_MEM_BASE(i) =
DR_REG_SPI0_BASE - i*0x1000`.

**Measured status (this board).** The sequence runs but the chip does not answer.
Using the ROM's `esp_rom_opiflash_exec_cmd` (`0x400008b8`, thirteen arguments,
callable from C under the windowed ABI) on SPI1 with CS1 selected, a register read
of MR0 returns `0x0000ffff` and a sync write of `0x5a6b7c8d` at address 0 followed
by reads at 0 and 0x1000 both return `0xffffffff`. Pads 33..37 (`SPIIO4..SPIIO7`,
`SPIDQS`) are in GPIO mode at reset and must be forced to `MCU_SEL = 4`; pads 27 and
28 likewise come back as GPIO because this module's flash is quad, not octal.

That pad routing was verified after the fact: dumping `IO_MUX_GPIO33..37` during
bring-up reads `MCU_SEL = 4` with `FUN_DRV = 3`, and pads 26..32 read `MCU_SEL = 0`.
So the eight data lines are connected.

A hand-built octal transaction on SPI1 - `ADDR`, `USER2` (command bit length and
value), `USER1` (address bit length, dummy cycle length), `MOSI_DLEN`, `MISO_DLEN`,
`MISC` with CS0 disabled and CS1 enabled, `USER` with the phase enables, then
`CMD |= BIT(18)` - completes: `CMD` self-clears and `SPI_MEM_INT_RAW(1)` bit 2
(`TOTAL_TRANS_END`, which the header describes as an SPI1 event) is set. The data
phase still returns nothing. So the transaction runs and the chip does not drive the
lines.

Two things that were tried and ruled out:

- **Suspending the Ext_RAM caches** (`EXTMEM_ICACHE_CTRL` and `EXTMEM_DCACHE_CTRL`
  bit 0 cleared) around the register bring-up, on the theory that the hardware
  arbitrates SPI0 cache access against SPI1 user commands (`ETS_SPI_MEM_REJECT_CACHE_INTR_SOURCE`
  = 60). No change.
- **A SPI1 user command on CS0** with the octal phase bits cleared, issuing a plain
  `0x03` read at flash offset `0x10000`, returns `0xffffffff` rather than the image
  header. SPI1's user-command path is not a route to the flash, so it cannot be used
  as a control for the PSRAM test.
- **SPI0's user-command path on CS1** returns `0x00000000` for MR0 as well, and it
  breaks the flash read FSM on the way, so the boot dies at the first `.text`
  function afterwards. There is no MSPI instance whose user-command path reaches the
  PSRAM on this board.

Two things that are easy to get wrong and were verified:

- **Never issue a user command on SPI0.** SPI0's `CTRL` `FREAD_*` bits drive the
  flash read FSM that the instruction and data caches use. A user command on SPI0
  stalls it, and every later fetch through the `0x42000000` window returns the
  Ext_RAM "no mapping" sentinel `0x0bad0bad`, which shows up as an
  `IllegalInstruction` at the first `.text` function. Restoring `SPI_CTRL(0)`
  afterwards is not sufficient. PSRAM transactions must go through SPI1.
- `0x0bad0bad` is the value the Ext_RAM cache returns for a page whose MMU entry is
  invalid, not a data value. Seeing it means the mapping, not the read, is the
  problem.

Sequence, in order, with the values ESP-IDF uses:

| Step | Register | Value |
|---|---|---|
| core clock | `SPI_MEM_CORE_CLK_SEL_REG(0)` +0xEC | 2 = 160 MHz (shared by SPI0/SPI1) |
| flash clock | `SPI_MEM_CLOCK_REG(0)` +0x14 | div 8 = 20 MHz during bring-up, div 2 = 80 MHz afterwards |
| bring-up clock | `SPI_MEM_CLOCK_REG(1)` +0x14 | div 8 = 20 MHz |
| CS1 pad | IO MUX +0x6C (`PERIPHS_IO_MUX_SPICS1_U`) | MCU_SEL = 0 (`FUNC_SPICS1_SPICS1`), FUN_DRV = 3 |
| clock pad drive | `SPI_MEM_DATE_REG(0)` +0x3FC | `SPICLK_PAD_DRV_CTL_EN` (bit 4), FMEM/SMEM FUN_DRV = 3 |
| CS timing | `SPI_MEM_SPI_SMEM_AC_REG(0)` +0xDC | setup 3, hold 3, hold delay 2, both enable bits |
| ECC | `SYSCON_SRAM_ACE0_ATTR_REG` (0x60026058) bit 8 | cleared |
| dummy mode | `SPI_MEM_DDR_REG(1)` +0xE0 | `VAR_DUMMY` set, RDAT_SWP/WDAT_SWP clear |
| MR0 | OPI `REG_WRITE` to address 0 | `lt=1` (bit 5), `read_latency=2` (bits 4:2), `drive_str=0` = 0x28 |
| connectivity | OPI write/read at address 0 | must read back what was written |
| Ext_RAM phases | `SPI_MEM_CACHE_SCTRL_REG(0)` +0x40 | USR_WCMD, USR_RCMD, SCMD_4BYTE, USR_RD/WR_DUMMY, SRAM_OCT |
| command words | `SRAM_DWR` +0x4C, `SRAM_DRD` +0x48 | bitlen 15 (i.e. 16 bits), `0x8080` / `0x0000` |
| dummy cycles | CACHE_SCTRL bits [11:6], [27:22] | 17 and 7 (chip dummies are 18 and 8 bits) |
| DDR | `SPI_MEM_SPI_SMEM_DDR_REG(0)` +0xE4 | `DDR_EN` set, swaps clear |
| octal phases | `SPI_MEM_SRAM_CMD_REG(0)` +0x44 | SDIN/SDOUT/SADDR/SCMD/SDUMMY_OCT |
| PSRAM clock | `SPI_MEM_SRAM_CLK_REG(0)` +0x50 | div 2 = 80 MHz |
| input delay | `SPI_MEM_SPI_SMEM_DIN_MODE_REG(0)` +0xC0 | mode 4 in all nine 3-bit fields (DIN0-DIN7, DQS at 24) |
| input delay cycles | `SPI_MEM_SPI_SMEM_DIN_NUM_REG(0)` +0xC4 | all nine 2-bit fields zero |
| extra read dummy | `SPI_MEM_SPI_SMEM_TIMING_CALI_REG(0)` +0xBC | `TIMING_CALI` (bit 1) set, `EXTRA_DUMMY_CYCLELEN` (bits 4:2) = 2 |

OPI command words: sync read `0x0000`, sync write `0x8080`, register read `0x4040`,
register write `0xC0C0`. Command phase 16 bits, address phase 32 bits, read dummy 18
bits (`2*(10-1)`), write dummy 8 bits (`2*(5-1)`), register dummy 8 bits. The dummy
cyclelen fields are stored as *bits minus one* because octal transfers 8 bits per
cycle.

The flash clock is slowed to 20 MHz for the duration of the bring-up and restored to
80 MHz afterwards. SPI0 and SPI1 share the flash read FSM - ESP-IDF's
`mspi_timing_enter_low_speed_mode()` says so outright, and slows both instances
rather than just SPI1. Programming octal phases on SPI1 while SPI0 is still at 80
MHz is the exact situation that comment is warning about.

Two things worth knowing:

- **The ROM's `esp_rom_opiflash_exec_cmd` (0x400008b8) is unusable from Rockbox.** It
  takes thirteen arguments and the ROM is windowed-ABI: a callee entered with
  CALLINC=4 receives its arguments in physical `a6..a19`, and call0 code can only
  name `a0..a15`. Programming the transaction directly is shorter and does exactly
  what the ROM does.
- **PSRAM runs at 80 MHz without the tuning sweep.** ESP-IDF reaches 80 MHz by
  sweeping dummy and delay values (`mspi_timing_by_mspi_delay.c`), and Rockbox has
  no equivalent. What it does instead is copy the entry ESP-IDF *falls back to* when
  the sweep fails: `mspi_timing_tuning_configs.h` gives core 160 MHz / module 80 MHz
  DTR a default_config_id of 5, i.e. `spi_din_mode = 4` on every DIN line and on
  DQS, `spi_din_num = 0`, `extra_dummy_len = 2`. That is a configuration known to
  work on this silicon, not an estimate of one. The sweep can find better numbers;
  it cannot make the default wrong.

If `psram_init()` fails, `mmu_init()` returns without mapping anything and the
audiobuffer, framebuffer and codec buffers stay unmapped. `dbg_hw_info()` reports
"PSRAM: responding / not responding" so this is visible on the device.

### Peripheral registers (verified against the SVD, not guessed)

From `components/soc/esp32s3/register/soc/reg_base.h` and the bare-metal reference
`Chalandi/Baremetal_esp32s3_nosdk`:

```
GPIO        0x60004000  OUT 0x04  W1TS 0x08  W1TC 0x0C  OUT1 0x10   (pins 32-53)
                        ENABLE 0x20 W1TS 0x24 W1TC 0x28 ENABLE1 0x2C
                        IN 0x3C   IN1 0x40
                        GPIO matrix OUT_SEL: 0x554 + 4*n
GPSPI2      0x60024000  CMD 0x00 ADDR 0x04 CTRL 0x08 CLOCK 0x0C USER 0x10
                        USER1 0x14 MS_DLEN 0x1C MISC 0x20 DMA_CONF 0x30
                        CPU-controlled TX FIFO W0..W11 at 0x98
I2C master  0x60013000
```

- **GPIO 32-53 are a separate register group.** A driver that only uses the low group
  silently ignores pins 40 and 41 — exactly the LCD MOSI and SCLK pins.
- **GPSPI signal indices are shared by SPI2 and SPI3** (CLK 66, Q/MOSI 67, D/MISO 68).
  The controller does not drive pins directly; signals must be routed through the GPIO
  matrix, so `spi_init` has to write `OUT_SEL` for each pin.
- **Pull-ups are in the IO MUX, not the GPIO block.** Searching `gpio_reg.h` or
  `gpio_struct.h` for a pull-up bit finds nothing, and `soc_caps.h` has no
  `SOC_GPIO_SUPPORT_PULL_UP`, which is why it is easy to conclude the chip has none.
  It does: `FUN_PU` is bit 8 and `FUN_PD` bit 7 of the per-pad IO MUX register at
  `IO_MUX_BASE (0x60009000) + 0x04 + 4*pin`, which `gpio_ll_pullup_en()` writes.
  `SOC_GPIO_PIN_COUNT` is 49, so GPIO 0-48 all have an IO MUX entry; GPIO 49-53 are
  the internal SPI-flash pads and have none. The schematic also has 10KΩ pull-ups on
  SDA/SCL (R30/R31 to EXP_3V3), which are load-bearing for the I²C peripheral.
- **`OUT_SEL` reset value 0x100 is not "disconnected".** `SIG_GPIO_OUT_IDX` is 256,
  and the TRM says `s=256: output of GPIO[n] equals GPIO_OUT_REG[n]`. So a plain
  `gpio_write_pin()` needs no matrix routing at all; only peripheral signals do.
- **Input enable is a separate bit.** `GPIO_ENABLE` only gates the output driver; a
  pad with `FUN_IE` (bit 9 of its IO MUX register) clear does not feed `GPIO_IN` at
  all. GPIO17 and GPIO18 default to DAC outputs, so a driver using them as buttons
  has to write `MCU_SEL` as well.
- **The GPIO matrix input slots are indexed by signal, not by pin** (`IN_SEL` array at
  `GPIO_BASE + 0x154`, one word per signal, field `in_sel` is the GPIO number and bit 7
  selects the matrix). The output slots are indexed by pin (`OUT_SEL` at `GPIO_BASE +
  0x554`). A driver that only writes the output half leaves the controller unable to see
  the bus.
- **GPIO1 and GPIO2 default to UART0 RX/TX** through the IO MUX (`MCU_SEL`, bits 14:12,
  at `IO_MUX_BASE + 0x04 + 4*pin`). A driver that uses them as I²C has to write 0 there
  first.
- `rsr.intlevel` / `wsr.intlevel` do not exist on the esp32s3 core config; the level is
  `PS` bits 0-3 (`PS_INTLEVEL_SHIFT 0`, `PS_INTLEVEL_MASK 0x0F`, 0-15). The rest of PS is
  EXCM (4), UM (5), RING (6-7), OWB (8-11), CALLINC (16-17), WOE (18) — per
  `components/xtensa/include/xtensa/corebits.h`.
- Boot: **the mask ROM loader is enough.** An ESP image flashed at 0x0 is read by
  the ROM, which copies the RAM segments, sets up the flash MMU windows and jumps
  to the entry point. The ROM exports the functions it uses, visible in
  `components/esp_rom/esp32s3/ld/esp32s3.rom.ld`: `ROM_Boot_Cache_Init`
  (0x40001668), `Cache_MMU_Init` (0x40001998), `Cache_Set_IDROM_MMU_Info`
  (0x40001950), `Cache_Ibus_MMU_Set` (0x400019a4), `Cache_Dbus_MMU_Set`
  (0x400019b0). ESP-IDF's second-stage bootloader adds only partition-table
  parsing, chip-revision validation, flash-size configuration and the RWDT setup.
  None of that is needed to boot one image, and the bare-metal reference
  (`Chalandi/Baremetal_esp32s3_nosdk`) proves it: it flashes its own image at 0x0
  and runs.
  **What neither the ROM nor the bootloader does is bring up the external PSRAM.**
  ESP-IDF does that in the application, in
  `components/esp_psram/esp32s3/esp_psram_impl_octal.c`; the bootloader only wraps
  `cache_hal_disable/enable(CACHE_LL_LEVEL_EXT_MEM, ...)` around the flash-size
  configuration. Rockbox's audiobuffer, framebuffer, codecbuf and pluginbuf are all
  in PSRAM, so the octal PSRAM bring-up has to live in Rockbox, before `mmu_init()`.
  The ROM has the pieces: `esp_rom_opiflash_exec_cmd` (0x400008b8) and
  `esp_rom_spi_set_dtr_swap_mode` (0x4000093c) are exactly the calls ESP-IDF's
  implementation uses.
  The loader does only map MMU pages for segments present in the image, and the
  audiobuffer is NOLOAD, so Rockbox still maps its own PSRAM pages at runtime
  (`mmu-xtensa.c`).
  `crt0.S` is therefore not a reset vector — the ROM owns the vectors, the entry is
  jumped to (so `a0` is not a return address and `a1` must be set by hand), and
  `.bss` is not in the image file.
- **The ROM loader maps the data side of the MMU only, so the entry point must be
  in internal IRAM.** Disassembling `ets_run_flash_bootloader` in
  `esp32s3_rev0_rom.elf` shows one `Cache_Dbus_MMU_Set` call and no
  `Cache_Ibus_MMU_Set` call; the only Ibus mapping in the ROM is in
  `ets_run_direct_boot`. That is why ESP-IDF's own second-stage bootloader is
  IRAM-only: it is the stage that maps IROM. An image whose entry is in the
  `0x42000000` window never reaches its first instruction. Rockbox therefore puts
  `.startup` in `.iram`, and `firmware/target/xtensa/irom-map.c` walks the image
  header (reachable at linear 0, i.e. `0x3C000000`, because the header is at flash
  offset 0 = physical page 0) and arms the MMU entries for every flash-mapped
  segment before anything in `.text` runs.
- **The MMU's 512 entries index one shared 32 MB linear range.** Entry =
  `(vaddr & 0x1FFFFFF) >> 16`, value = `(paddr >> 16) | target`, bit 15 = PSRAM,
  bit 14 = invalid, and `hal/esp32s3/include/hal/mmu_ll.h` asserts that IRAM0 and
  DRAM0 have the same linear range. So `0x42000000` and `0x3C000000` are entry 0.
  `IROMORIG` in `app.lds` is `0x42900020` (entry 144) rather than `0x42000020`
  (entry 0) because DROM plus the codec/plugin buffers claim entries 0..128; with
  both windows at +0x20 the two mappings overwrite each other.
- Flashing is one command, not a build step:
  ```sh
  esptool --chip esp32s3 write_flash 0 rockbox.esp32s3
  ```
  `tools/esp-elf2image` produces the image; it is byte-identical to
  `esptool elf2image --flash_mode dio --flash_freq 80m --flash_size 16MB` except
  for the digest flag and the appended SHA-256.
- **binutils gotcha (found by testing):** this binutils hits an internal error
  (`ldlang.c 1074`) when an `l32r` references a linker-script-defined symbol
  directly. Keeping the literal as a `.word` in the same section as the code avoids
  it. Also, `jmp` is not a valid mnemonic on the esp32s3 core — use `j`.
- The dummy section ESP-IDF uses for PSRAM is not needed in Rockbox's script,
  because `.rodata` and `.audiobuf` are in the same region so the location counter
  already continues past rodata. ESP-IDF needs it only because PSRAM is a separate
  region there.
- `tools/echoplayer/openocd.make` is the model for a debug/flash makefile; OpenOCD
  supports ESP32.

---

### The I2C master peripheral is a command-list machine

I2C0 is at `0x60013000` (`I2C_EXT0_CLK_EN` is bit 7 of `SYSTEM_PERIP_CLK_EN0` at
`0x600C0018`). Signals: SCL = 89, SDA = 90. The command registers are 8 slots at
`base + 0x58`, each 14 bits: `byte_num[7:0]`, `ack_check_en[8]`, `ack_exp[9]`,
`ack_val[10]`, `op_code[13:11]`, plus `DONE` at bit 31 which hardware sets when a
command has executed and software must clear before writing a new one.

`op_code`: 6 = RSTART, 1 = WRITE, 3 = READ, 2 = STOP, 4 = END.

The CMD_Controller reads the slots in order and **stops at STOP or END**. That is
the whole shape of the device: you program a transaction, latch it with
`CONF_UPGATE` (bit 11 of `CTR`, because the registers are in the APB domain and the
controller in the asynchronous `I2C_SCLK` domain), trigger with `TRANS_START` (bit
5), and wait for `TRANS_COMPLETE` (bit 7 of `INT_RAW`) or `NACK` (bit 10).

Consequences worth remembering:

- In master mode the slave address and the R/W bit are **part of TX RAM**, not a
  separate register, so `byte_num` counts them. A write of N data bytes with a
  register address is `byte_num = N + 2`.
- A read is split so the master ACKs every byte but the last: `READ(N-1)` with
  `ack_val = 0`, then `READ(1)` with `ack_val = 1`. ESP-IDF errors out if a READ
  immediately before STOP has `ack_val = 0`.
- `byte_num` is ignored for RSTART, STOP and END, so a lone RSTART is legal — but a
  command still cannot be executed on its own, so Rockbox's byte-at-a-time API
  (`i2c_start`/`i2c_outb`/`i2c_inb`/`i2c_getack`) cannot be implemented on this
  hardware. `i2c_inb()` cannot return a byte and `i2c_getack()` cannot report an ACK
  until the whole list has run. The transaction-level API maps cleanly, and that is
  all this target uses.
- TX and RX RAM are 32 × 8 bits, each byte occupying a whole word at `base + 0x100`
  and `base + 0x180`. Double addressing requires non-FIFO access, so use direct
  addressing (`NONFIFO_EN`, bit 10 of `FIFO_CONF`).
- The I2C module clock is **XTAL (40 MHz)** when `SCLK_SEL` is 0, not APB_CLK, so the
  timing registers are XTAL cycles and have nothing to do with `cpu_frequency`. At
  100 kHz that gives `clkm_div = 1`, half-cycle 200, `scl_wait_high` 98,
  `scl_high` 102, `sda_hold` 50, `sda_sample` 100.
- Bus recovery is built in: `SCL_RST_SLV_NUM` / `SCL_RST_SLV_EN` in `SCL_SP_CONF`
  (base + 0x80) generate N SCL pulses while idle, which is exactly the nine-pulse
  recovery the spec asks for.

### The transaction model is why `i2c-async` fits and the byte API does not

An `i2c_descriptor` is one START ... STOP sequence with an optional write phase
followed by a read. That is literally what the CMD_Controller executes, so the
asynchronous layer is a *better* match for this peripheral than the byte-at-a-time
`i2c_start`/`i2c_outb`/`i2c_inb` API, which cannot be implemented at all: a single
command cannot be executed on its own.

`firmware/target/xtensa/i2c-xtensa.c` therefore implements `__i2c_async_submit()`
and `i2c_irq()`, and the synchronous entry points are thin wrappers over
`i2c_async_queue()` plus a semaphore. There is then exactly one path to the
hardware and one in-flight transaction, which is what makes it safe for the
touchscreen to poll the bus every tick while the codec or the PMU is doing
something else.

Two details that are easy to get wrong:

- **The TX RAM pointer advances.** Each WRITE command consumes `byte_num` bytes
  from the *current* pointer, not from offset 0. A read-with-register therefore
  lays out `[addr, reg, addr|1]` consecutively and issues `WRITE(2)`, RSTART,
  `WRITE(1)`. Writing the second address byte at offset 0 would re-send the first
  address byte.
- **`i2c_init()` re-enters.** `pca9557_init()` calls it to recover a wedged bus.
  The async core and the interrupt routing are set up once behind a static flag,
  but bus recovery clears `INT_ENA` and it has to be restored on *every* call,
  otherwise the second `i2c_init()` silently disables the interrupt.

Interrupt source `ETS_I2C_EXT0_INTR_SOURCE` is **42** (not 41; the enum has gaps —
`LCD_CAM` jumps to 24 and `LEDC` to 35). Rockbox routes it to slot 8, which is
free: 1 is the tick, 2 the audio GDMA, 3 SDHOST, 5 the recording GDMA, 4 is the
WiFi baseband and 6/7 are the core timer and the software interrupt.

The FT6336's INT pin is not connected (`CTP_INT_PIN = -1` in the reference
firmware), so `ft6x06_irq_handler()` is called from `button_read_device()` every
tick. It queues with `I2C_Q_ONCE`, so the call cannot pile up transactions; the
effective poll rate is set by how long a transaction takes, about 1.6 ms for the
14-byte register read.

The controller's own axes are the panel's short side first: registers 0x03/0x04 run
0..240 and 0x05/0x06 run 0..320, while the panel is driven as 320x240. The reference
firmware's `CTP_Class` does exactly the rotation Rockbox's calibration does -
`CTP_REVERS_XY = 1` swaps the two registers, `CTP_REVERS_XPOS = 1` negates the
320-wide axis. Net effect: display x = 320 - raw(0x05/0x06), display y =
raw(0x03/0x04). Rockbox's ft6x06 driver reads POS_X from 0x03/0x04 and POS_Y from
0x05/0x06, so `DEFAULT_TOUCHSCREEN_CALIBRATION { A=0, B=-1, C=LCD_WIDTH, D=1, E=0,
F=0 }` reproduces the reference firmware's mapping exactly.

### The SDHOST block is a DesignWare MMC controller with an IDMAC

SDHOST is at `0x60028000` (`SDIO_HOST_CLK_EN` is bit 7 of `PERIP_CLK_EN1`, and it
defaults to 0). Rockbox already has the SD/MMC protocol layer in
`firmware/drivers/sdmmc_host.c`; a target only supplies `struct
sdmmc_controller_ops` plus `sdmmc_host_target_init()`. `sdmmc-x1000-common.c` is
the closest existing model, because the x1000 MSC is the same IP.

Things that are not obvious from the register list:

- **There is no data port.** Every transfer goes through the IDMAC, so a read or
  write is a ring of 16-byte descriptors: `DES0` (owner bit 31, card-error 30,
  end-of-ring 5, chained 4, first 3, last 2), `DES1` (buffer1 size, 13 bits),
  `DES2` buffer address, `DES3` next descriptor. `SDMMC_DSCADDR_REG` (+0x94) is a
  plain 32-bit register, so unlike GDMA the descriptor ring can live anywhere — but
  internal SRAM is still cached, so the ring has to be written back before the DMA
  reads it and invalidated before the CPU reads the owner bits back.
- **Completion is the owner bits, not an interrupt.** The IDMAC clears a
  descriptor's owner bit when it retires it. `RINTSTS` `DATA_OVER` alone only means
  the card sent the data; the transfer is finished when every descriptor in the
  ring has been retired.
- **`INTMASK` is an enable mask, not a mask.** A set bit *unmasks* the interrupt.
  Getting this backwards silences the controller entirely.
- **Bit positions in `CTRL` are easy to misread.** `use_internal_dma` is bit 25, not
  21. In `BMOD`, `enable` is bit 7 and `fb` is bit 1; bit 0 is `sw_reset`. In
  `STATUS`, `data_busy` is bit 9 — the first four bits are FIFO flags and the next
  four are the command FSM state.
- **Two clock divider stages.** The generator (`clock` register at +0x800) divides
  XTAL or the 160 MHz BBPLL by 1..16; each slot then has `clkdiv.divN`, where 0 is
  bypass and n is a divide by 2n. `clk_sel = 1` needs the BBPLL powered up, which
  nothing guarantees when we enter Rockbox from ESP-IDF's bootloader, so XTAL is the
  only dependable source. From 40 MHz that means 400 kHz exactly (host 10, card 5)
  and 20 MHz for the "25 MHz" slot; 50 MHz is unreachable, so the target must not
  advertise `SDMMC_BUS_CLOCK_50MHZ`. Measured on this board: the BBPLL path produces
  no clock even with `BBPLL_FORCE_PU` set in `RTC_CNTL_OPTIONS0`, so every SDHOST
  clock must come from XTAL.
- **Writing the divisors is not enough — the CIU has to be told.** The clock settings
  only reach the card interface unit when a command with `update_clk_reg` (bit 21) is
  issued. It is not a real SD command: index 0, no argument, no response, plus
  `wait_complete` (13) and `start_command` (31). ESP-IDF's `sd_host` issues it after
  every clock change, in the order *disable clock → update → program divisors →
  update → enable clock + low power → update*. Without it `CMD_START` is accepted and
  then cleared, `RINTSTS` stays 0, `STATUS.cmd_fsm_state` stays 0 and `tcbcnt` (bytes
  actually sent to the card) is 0. That looks exactly like a dead peripheral; it is
  not, and no amount of register poking will reveal it.
- `phase_dout = 1` (90 degrees) is what ESP-IDF sets for the card clock, and it only
  applies to a command whose `use_hold_reg` bit is set.
- Signals for slot 0 are CLK 172, CMD 178, D0 180 — and CMD/D0 are bidirectional, so
  both the matrix input and output slots have to be connected. Writing only the
  output half leaves the controller blind.
- The SD lines have clamp diodes but no external pull-ups. The board's CLK (47), CMD
  (48) and D0 (21) are all within `SOC_GPIO_PIN_COUNT`, so the IO MUX `FUN_PU` bit can
  be set on them; the controller also has an internal CMD pull-up
  (`SDHOST_CCMD_OD_PULLUP_EN_N`, signal 176, reset default 1). `SDMMC_BUS_WIDTH_4BIT`
  is unusable because only D0 is wired. The drive is therefore non-removable:
  `is_removable = false`, no `sdmmc_poll`, and a missing card shows up as a command
  timeout.

### Two SDHOST facts that took measurement to establish

**The CIU clock is real, and `tcbcnt` is not a command counter.** `TCBCNT` and
`TBBCNT` count *data-phase* bytes; for a command-only transaction both are 0 even
when the CIU has clocked the full command. The proof that the clock is actually
running is the command duration: with `CLK = 0x132801` (host div 10, XTAL) and
`CLKDIV` card div set to 5, 50 and 255, the poll loop for `CMD_DONE` took 93, 900
and 4726 iterations. Those are ratios of 9.7 and 5.25 against an expected 10 and
5.1. A peripheral with no clock would not scale.

**Slot 0 and slot 1 are different signal sets.** ESP-IDF's `SDMMC_HOST_DEFAULT()`
for ESP32-S3 (not the S31) selects **slot 1**, not slot 0. Slot 1 uses clock signal
173, CMD 179, DAT0 181, `CLKENA` bit 1 and `CLKDIV` div1; `CLKSRC.cardN` selects
which divider stage slot N uses. The MeowKit stock firmware calls
`SD_MMC.setPins(47, 48, 21)` on that default host, so the board's SD lines are
driven from slot 1 signals. Slot 0 (172/178/180, `CLKENA` bit 0, `CLKDIV` div0) is
the one Rockbox currently uses.

**Pads 47 and 48 are not ordinary GPIO pads.** `MCU_SEL = 0` is the pad's *dedicated*
peripheral function, not GPIO. On ESP32-S3 `PIN_FUNC_GPIO` is **1** for every pad. GPIO47
and GPIO48 are the `SPICLK_P`/`SPICLK_N` pads: with `MCU_SEL = 0` they are wired to the MSPI
differential clock (`FUNC_SPICLK_P_SPICLK_DIFF`), the GPIO matrix cannot reach them at all,
and the reading that follows is the SPI clock pair - one inverted, one dead. That is exactly
the "pad 47 inverted, pad 48 dead" signature. Setting `MCU_SEL = 1` (`FUNC_SPICLK_P_GPIO47`)
makes both pads behave normally and the card responds to CMD0 and CMD8.

Routing the SDHOST clock signal to a pad and sampling `GPIO_IN`/`GPIO_IN1` for 40 reads while
the init command runs gives a clean square wave on pads 45, 46, 49, 50, 51, 38, 21, 18, 17,
14, 13, 12, 11, 10, 9, 8, 6, 4, 3, 2, 1 and 0. Pads 32-37 and 42-44 read stuck high (they are
the PSRAM SPI pads and the LEDC/AXP lines, all actively driven by other circuitry), so "reads
a constant" is not by itself a fault sign.

The board has a zener-based level-shift network on the SD CLK and CMD lines (per the
schematic), and the card-detect line is not connected at all.

### The SDHOST response and IDMAC buffer layout, measured

**Short responses are in `RESP0`, not `RESP1`.** For a 48-bit response the CIU drops the CRC
and leaves the payload in `RESP0`: CMD8 (R7) reads back `0x1aa`, CMD55 (R1) `0x120`, CMD41
(R3) the OCR. `RESP1..RESP3` stay zero.

**A 128-bit response is stored least-significant word first.** `RESP0` carries bits 31:0, so
the CSD/CID have to be reversed before handing them to `sd_parse_csd()`, which expects the
most significant word in `data[0]`. Reversed, the CSD of a 64 GB card decodes as CSD v2.0 with
124735488 blocks; unreversed it is garbage and the mount fails silently with `disk_mount_all()`
returning 0.

**The IDMAC cannot reach PSRAM.** Rockbox's disk cache buffers live in the PSRAM audiobuffer
window. Arming a descriptor with such an address retires the descriptor (the OWNER bit clears)
but transfers nothing - the IDMAC is an AHB master and does not go through the Ext_RAM MMU.
ESP-IDF encodes the same rule in `SD_HOST_SDMMC_DMA_ALLOC_CAPS`, which for esp32s3 is
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA` because `SOC_SDMMC_PSRAM_DMA_CAPABLE`
is undefined. `firmware/target/xtensa/sdmmc-xtensa.c` bounces any transfer whose buffer is
outside DRAM0 (`0x3FC88000-0x3FD00000`) through a 16 KB `.srambss` ring, and caps
`max_nr_blocks` so no transfer exceeds it.

**`rsr interrupt` reports masked slots too.** The pending bitmask covers all 32 slots
regardless of `INTENABLE`. Masking a device's slot is therefore not enough to stop its
handler from running: the tick ISR would dispatch a handler for a device whose interrupt is
masked, and that handler would consume the other device's event latches. AND the bitmask with
`INTENABLE` before dispatching, as ESP-IDF's `_xt_interrupt_pending_list()` does.

### Power and time live in the PMU, not the SoC

The board has an AXP173 (I2C 0x34) and a PCF8563 (I2C 0x51). Neither is a
peripheral Rockbox can drive with an arch driver; both are reached through the
same synchronous `i2c_readmem()`/`i2c_writemem()` API as the codec.

**AXP173.** It is the power supply, not a gauge. Register semantics that are not
obvious from the datasheet tables:

- Presence detection must use REG 0x12 (output control), not REG 0x00. On pure
  battery boot REG 0x00 legitimately reads 0x00, which looks like an absent
  device. REG 0x12 has a power-on default of 0x03 and is never 0xFF.
- Output enable is REG 0x12: bit0 DCDC1, bit1 LDO4, bit2 LDO2, bit3 LDO3, bit4
  DCDC2, bit6 EXTEN. Voltages are set in separate registers *before* enabling:
  DCDC1 0x26 and LDO4 0x27 are 700-3500 mV in 25 mV steps; LDO2/LDO3 share 0x28,
  1800-3300 mV in 100 mV steps, LDO2 in the high nibble. DCDC2 is 0x23.
  **Bit 7 is not a supply enable.** It is VBUSHC, the VBUS hardware current
  limit, and setting it clamps VBUS to about 100 mA. The ESP32-S3 browns out and
  the USB-Serial-JTAG console disappears from the host entirely - only a physical
  re-plug recovers it. The highest value that is safe to write is 0x4F.
- Four read-modify-writes on 0x12 can lose bits on a loaded bus. Write the final
  value once.
- ADC scaling: battery voltage `(hi<<4 | lo&0x0f) * 1.1` mV; charge and discharge
  current `(hi<<5 | lo&0x1f) * 0.5` mA; VBUS voltage `(hi<<4 | lo&0x0f) * 1.7` mV.
  The current registers use five low bits, the voltage registers four.
- The power button is the chip's PEK input, and it is readable over I2C. The IRQ
  enables are REG 0x40-0x43 plus 0x4A, and the matching status registers are
  0x44-0x47 plus 0x4D. **The PEK bits are not in the first pair**: the datasheet's
  IRQ table puts the short press at REG 0x46 bit1 and the long press at REG 0x46
  bit0, enabled by REG 0x42 bits 1 and 0. The reference firmware on this board reads
  0x44 for the PEK status, which is the ACIN/VBUS register - it never sees a press.
  The status bits **latch** - they are not a level - and writing 1 to a bit clears
  it. The IRQ pin is an open-drain output pulled high only when *no* status bit is
  set, so leaving the default enables (0x40 = 0xD8, 0x41 = 0xFF, 0x43 = 0xC1) makes
  the line low for battery, charge and temperature events nobody acts on, and the
  cheap GPIO check stops meaning anything.
  REG 0x36 sets the PEK timings: bits 7:6 the press duration that turns the chip on
  (128 ms/512 ms/1 s/2 s), bits 5:4 the long-press threshold (1/1.5/2/2.5 s), bit 3
  "auto power off when the press outlasts the off time", bit 2 the PWROK delay, bits
  1:0 how long the chip waits before cutting its own output (4/6/8/10 s). Bit 3 is
  set at reset; a bare write of only the on/off fields clears it and removes the
  hardware safety net. The IRQ output is on GPIO 43, active low.
  REG 0x31 bit3 is *not* a PEK enable - it is the sleep-mode PWRON short-press
  wakeup flag, and it self-clears after being written.
  This matters: a hardware PEK shutdown kills the board without Rockbox getting a
  chance to flush the audiobuffer, save settings or close the card. `button-xtensa.c`
  therefore reads GPIO 43 (cheap) and only then does the I2C read of REG 0x46; on a
  long press it calls `sys_poweroff()`, which queues SYS_POWEROFF, and the main
  thread runs `clean_shutdown()` - flush, save settings, close the card - before
  reaching `power_off()`, the PMU command. `axp173_init()` sets the long-press
  threshold to 1 s and pushes the hardware cutoff to its maximum (10 s), so Rockbox
  has nine seconds of margin, and clears every status register so a press before the
  application started does not shut the player down on the first poll.
  Pad 43 is the UART0 TX pad in the IO MUX, so it has to be switched to the GPIO
  function before `GPIO_IN` can see it.
- `power_off()` has to command REG 0x32 bit 7. The SoC cannot cut its own supply.

**PCF8563.** Register-compatible with the existing `RTC_E8564` driver, but that
one is written against the PP asynchronous I2C layer, so `drivers/rtc/rtc_pcf8563.c`
is a new driver using the generic API. Bits that matter:

- CTRL1 bit 7 stops the oscillator. A chip left stopped by previous firmware stays
  stopped, so `rtc_init()` clears it rather than trusting the default.
- Seconds bit 7 is the voltage-low flag: the chip is saying its timekeeping is not
  guaranteed. Mask it, do not treat it as data.
- Hours bit 7 is the 12h AM/PM flag; month bit 7 is the century. Both must be
  masked.
- The seven time registers accept a burst write, which is one transaction instead
  of seven.

**Speaker amplifier.** The NS4150B is enabled by PCA9557 IO3, high-active with a
10K pull-down. Nothing in the target drove it, so the DAC produced a signal that
never reached the speaker. `HAVE_SPEAKER` plus `audiohw_enable_speaker()` in
`audiohw-xtensa.c` fixes it.

**The amp is a branch, not the output stage.** An earlier version of this file
(and the pin list that inspired it) described the path as
`ES8311 → NS4150B → speaker`, which is wrong and misleading: it implies muting the
amp mutes the machine. The schematic is:

```
ES8311 HPOUL/HPOUR ──> C118/C119 ──> HP_L_OUT / HP_R_OUT ──┬──> J4  (3.5 mm jack)
                                                          │
                                                          └── R32/R33 ──> SPKL_IN/SPKR_IN
                                                                             └─> NS4150B ─> speaker
```

So the headphone jack is taken **directly off the DAC**, and PA_EN gates only the
speaker. Consequences worth remembering: a "no sound at all" report cannot be
blamed on PA_EN if the user is on headphones; the amp's state does not affect line
level out of the jack; and `speaker_mode` is genuinely optional on this board, not
a prerequisite for audio.

That exposed a gap in Rockbox itself: `audio_enable_speaker()` lives in
`apps/audio_path.c`, which `apps/SOURCES` only builds when `INPUT_SRC_CAPS != 0`.
A target with a speaker but no recording or radio therefore references a function
that is never compiled. `audio_path.c` now guards the input-source selection with
`#if INPUT_SRC_CAPS != 0` so the file can be built for speaker-only targets.

### The PCA9557 is on LDO4, and it is invisible until that rail is at 3.3 V

Established by measurement on the ESP-IDF spike, and it explains a failure that
looks like a dead chip.

After a cold board start the AXP173 leaves **REG 0x27 (LDO4) at 0x14 = 1200 mV**.
At that voltage the PCA9557 does not acknowledge its address at all - a full
0x08-0x77 scan returns six devices and the expander is simply not among them, at
any address. Writing `REG 0x27 = 0x68` (3300 mV) makes it ACK, and it keeps
ACKing. The reference BSP sets LDO4 to 3300 mV for exactly this reason, and its
comment that the PMU "powers everything, must be first" is literal: the
expander is unreachable until the PMU has been configured.

Three traps follow from this:

- **The AXP173 is not reset by an ESP32 reset.** Its registers survive a reflash
  and a watchdog reset, so once something has raised LDO4, the expander appears
  to work again by itself. The bug only reproduces after the board has been fully
  power-cycled, which is what makes it look intermittent.
- **The expander's outputs also survive.** It is powered, and its pins keep the
  state the last firmware left them in. That is why the ST7789 renders a picture
  even in a build that never addresses the expander: LCD CS and reset are still
  being driven from the previous run. A working display is not evidence that the
  expander is reachable.
- **`ESP_ERR_INVALID_STATE` from `i2c_master_transmit_receive()` is this driver's
  NACK indication**, not a driver or bus fault. It reads like a programming error
  and sends you looking in the wrong place. Likewise `i2c_master_probe()` returns
  `esp_err_t`, where **0 means the device was found** - assigning it to a `bool`
  inverts the meaning.

The full bus, with the rail correct, is seven devices: 0x18 ES8311, 0x19 PCA9557,
0x34 AXP173, 0x38 FT6336, 0x41 ES7210, 0x51 PCF8563, 0x68 BMI270. 0x44 (SHT30)
is absent on this board.

### Why the reference bypasses IDF's transaction layer

The reason is stated in LovyanGFX's own comment at `Bus_SPI.cpp:561-566`:

> Use burst mode only when the transfer length is a multiple of 4.
> When these three conditions coincide, data near the end of the DMA transfer
> gets corrupted:
> 1. transmit clock **80 MHz** (1:1 with the APB clock)
> 2. **DMA burst read** enabled
> 3. transmit data length **not a multiple of 4** (1-3 bytes of remainder)

Its mitigation chooses the burst bits by hand:

```c
dma_conf |= (length & 3) ? (SPI_OUTDSCR_BURST_EN)
                         : (SPI_OUTDSCR_BURST_EN | SPI_OUT_DATA_BURST_EN);
```

That is the whole argument for bypassing `spi_device`: **IDF chooses those bits
and the caller cannot reach them.** A large framebuffer push at 80 MHz is
exactly the shape that triggers the erratum, and the driver has no lever.

Three more pieces of the same case:

- `LGFX_SPIDMA_WORKAROUND` plus `spicommon_dmaworkaround_transfer_active()` at
  four call sites, working around a second known SPI-DMA defect.
- `buscfg.max_transfer_sz = 1` — deliberately small enough that IDF's
  transaction layer cannot be used for real transfers at all. This is intent,
  not an oversight.
- `*reg(SPI_USER_REG(port))` is rewritten on every `beginTransaction()`, so
  whatever IDF configured never survives.
- A comment on teardown: `gpio_matrix_in` "has a serious bug in ArduinoESP32
  v1.0.x (it can change settings on unrelated pins)", so it only ever touches
  the output half of the matrix.

Practical consequence for this port: **a FIFO-only driver is immune to the
80 MHz DMA tail corruption**, because there is no DMA read to corrupt. The
hand-driven path writes the 64-byte CPU buffer directly, which is why it is
both reference-faithful and the safer starting point. If we later push whole
frames over GDMA for throughput, the burst-enable rule above becomes a hard
requirement, not an optimisation.

### The reference does not use IDF's SPI transaction path

This decides the shape of the display driver, and it is easy to assume wrong.
Grepping LovyanGFX's entire ESP32 platform layer for IDF's transaction API
returns **zero hits**: no `spi_device_transmit`, `spi_device_polling_transmit`,
`spi_device_queue_trans`, `spi_transaction_t`, `spi_device_get_trans_result`.

What it uses IDF for is bus ownership and pin setup only:

```c
spi_bus_initialize(host, &buscfg, dma_channel);   // memset(&buscfg, ~0u, ...),
                                                  // max_transfer_sz = 1,
                                                  // flags = SPICOMMON_BUSFLAG_MASTER
spi_bus_add_device(host, &devcfg, &handle);       // 10 MHz, spics_io_num = -1,
                                                  // SPI_DEVICE_3WIRE | HALFDUPLEX,
                                                  // queue_size = 1
```

and then it **overwrites the peripheral registers itself**, discarding whatever
the driver configured:

```c
*reg(SPI_USER_REG(port))  = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN;
*reg(SPI_CTRL_REG(port))  = 0;
*reg(SPI_CTRL1_REG(port)) = 0;
*reg(SPI_CTRL2_REG(port)) = 0;
```

Every transfer is hand-driven: `SPI_MS_DLEN = bitlen - 1`, `memcpy` into
`SPI_W0` for 64 bytes or less, its own GDMA descriptor chain written to
`SPI_DMA_OUT_LINK` above that, then `*SPI_CMD = SPI_EXECUTE` and spin on
`SPI_USR`. DC is a plain GPIO driven through `GPIO_OUT_W1TS` / `GPIO_OUT_W1TC`,
not a peripheral signal.

Consequences:

- The reference's 80 MHz figure is a statement about hand-programmed GPSPI, not
  about `esp_lcd`. It is not evidence that `esp_lcd` works on this board.
- A driver that programs GPSPI registers on top of an IDF-initialised bus is
  **not** a deviation from the shipping firmware; it is what the shipping
  firmware does. Hand-rolled transactions here are reference-faithful, and are
  a different thing from bit-banging the clock and data lines.
- `esp_lcd` sits on IDF's transaction layer, which the reference deliberately
  bypasses. On this board that layer completes every transaction with `ESP_OK`
  and clocks the pad (verified by edge counting at a rate the counter can
  resolve), yet produces no visible image with a command sequence and byte
  order proven correct on the same pins at the same speed.

Do not run both paths in one boot to compare them. The hand-programmed path
leaves GPSPI2 registers set, `spi_bus_initialize` then returns
`ESP_ERR_INVALID_STATE`, and IDF layers its device state on top -- which makes
any `esp_lcd` result uninterpretable.

### The ST7789 pixel contract, measured on the glass

Established by drawing four labelled quadrants and reading the result, which is
the only instrument that separates a colour-format error from a geometry error:
a full-screen solid fill is invariant to rotation, window offsets and byte
order, so stripes or banding during a solid fill mean the data arriving is not
the data written.

| item | value | how it was pinned |
|---|---|---|
| `COLMOD` (0x3A) | **`0x55` must be sent** | the ST7789 powers up at `66h` = 18-bit |
| pixel format | 16-bit RGB565, **high byte first** | quadrants land correctly once COLMOD is set |
| `MADCTL` (0x36) | **`0xA8`** = MV \| MY \| BGR | with `0xA0` green and white are correct but red and blue are exchanged, which isolates bit 3 |
| window with MV set | CASET `0..319`, RASET `0..239` | MV swaps the axes; the un-swapped pair is wrong |
| `INVON` (0x21) | send it | reference sets `cfg.invert = true` |
| bus clock | **80 MHz, verified working on this glass** | reference `cfg.freq_write = 80000000`. Do not be misled by the `clock_speed_hz = 10000000` in `spi::init`'s `devcfg` -- that configures the IDF device the reference never transmits on |
| `SPI_USER` | `0x18000001` = `USR_MOSI \| USR_MISO \| DOUTDIN` | reference writes exactly this, MISO phase left on |
| `SPI_CTRL` | `0` | reference writes `*SPI_CTRL = 0` |
| `SPI_CMD` | plain write of `USR`, not read-modify-write | reference `*_spi_cmd_reg = SPI_EXECUTE` |
| DC switching | wait for `USR` to clear first | reference `dc_control()` spins on `SPI_USR` before touching the pin |
| DC before `pinMode` | drive it high first | reference `Bus_SPI::init()` calls `dc_control(true)` then `pinMode` |

The signature of **not sending `COLMOD`** is worth memorising: three equal
thirds down the screen, fine 1–2 pixel banding, one region reading cyan/yellow/
magenta and the next red/green/blue. A 2-byte stream consumed as 3-byte pixels
re-phases every three rows (the banding) and changes channel assignment with
each phase (the palette flip).

FIFO packing is not a variable: IDF's `spi_ll_write_buffer` does a little-endian
`memcpy` into `data_buf[]`, LovyanGFX does `memcpy((void*)spi_w0_reg, data,
aligned_len)`, and hand-packing `w |= data[i] << (8*b)` is the same thing. Byte
*N* of the buffer is wire byte *N* in all three.

The reference never sends `COLMOD` in its init script, which initially looked
like licence to omit it. It does not: LovyanGFX sets colour depth outside the
script, and omitting it on this panel produces the three-thirds pattern above.

### gpio_set_direction() silently undoes GPIO matrix routing

`gpio_set_direction(pin, GPIO_MODE_OUTPUT)` routes the pad to
`SIG_GPIO_OUT_IDX` as part of making it an output. So calling it **after**
`esp_rom_gpio_connect_out_signal()` (or after `spi_bus_initialize`) replaces the
peripheral signal with the plain GPIO output register, and nothing downstream
reports the problem: the peripheral's registers all read back correct, every
transfer completes with `ESP_OK`, and the pad never moves.

Order is therefore **direction first, matrix second**. The state is directly
readable — `GPIO.func_out_sel_cfg[pin]` shows `func_sel` (9 bits), `inv_sel`,
`oen_sel`, `oen_inv_sel`. A pad correctly connected to GPSPI2 clock reads
`func=101` (`FSPICLK_OUT_IDX`); a clobbered one reads `func=256`
(`SIG_GPIO_OUT_IDX`).

The same trap invalidates measurements: enabling the input path with
`gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT)` re-routes the output too, so
an edge-counting probe written that way destroys the signal it is trying to
count. Use `gpio_input_enable()`, which sets only the IO MUX `FUN_IE` bit.

Verified by counting clock edges on the pad while the controller runs: a
512-bit transfer gives 1020 counted edges (both edges per bit) with the routing
correct, and 0 with it clobbered. Validate the counter before believing a zero
— toggle the pin by hand and confirm the same loop counts it.

### The ST7789 needs three unrelated things before it shows anything

Three independent faults each produced the same symptom: backlight on, every
call returning `ESP_OK`, black glass. All three had to be fixed before the panel
displayed anything. None of them is visible from the SoC side — every transfer
completes and reports success throughout.

1. **The LCD pads are JTAG pads.** GPIO 39/40/41 are `MTCK`/`MTDO`/`MTDI` and
   come out of reset with `MCU_SEL = 0`, which connects them to the JTAG block
   rather than to the GPIO matrix. Routing `FSPICLK`/`FSPID` into the matrix and
   enabling the output driver accomplishes nothing while that is true: the pad is
   owned by something else. `gpio_set_direction()` writes only the enable bits,
   and `gpio_reset_pin()` restores the pad's *default* function, which here is
   JTAG. `esp_rom_gpio_pad_select_gpio()` is the call that actually moves the
   pad. Verified by reading bits 14:12 of `IO_MUX_BASE + 0x04 + 4*pin`: `0 -> 1`.
   The pad registers for 39/40/41 are named `PERIPHS_IO_MUX_MTCK/MTDO/MTDI_U` at
   0xa0/0xa4/0xa8, which coincides with the usual `0x04 + 4*pin` formula, so the
   formula is safe for these three.
2. **CS must be de-selected across the panel reset pulse.** Both the reference
   BSP and our bare-metal driver drive CS high, pulse reset, wait, and only then
   assert CS. Holding CS asserted while `/RES` is low lets the interface latch
   the idle clock/data state, after which the panel ignores everything. This was
   the last of the three to be found, and it is the one that is easy to dismiss
   as stylistic.
3. **`esp_lcd`'s ST7789 driver does not run the power sequencing.**
   `esp_lcd_panel_st7789.c` sends only SLPOUT, MADCTL, COLMOD and RAMCTRL.
   Without PORCTRL (0xB2), GCTRL (0xB7), VCOMS (0xBB), LCMCTRL (0xC0), VDVVRHEN
   (0xC2), VRHS (0xC3), VDVSET (0xC4), FRCTR2 (0xC6), PWCTRL1 (0xD0) and both
   gamma tables (0xE0, 0xE1), the source and gate drivers have no bias, so the
   controller happily stores every pixel and the glass stays dark. Values are in
   LovyanGFX `Panel_ST7789.hpp` in the reference tree, and are identical to the
   list our bare-metal `st7789.c` already used.

The expander sequence worth copying verbatim from the reference: make CS, RST,
PA_EN and TOUCH_RST outputs; drive CS high and RST/PA_EN/TOUCH_RST low; 20 ms;
RST and TOUCH_RST high; 120 ms; CS low. Touch reset is pulsed *with* the panel
reset, before the display init runs.

Two things that made this debuggable rather than guesswork:

- The PCA9557 input register mirrors the pin level even for pins configured as
  outputs, so reading it distinguishes "we wrote the output register" from "the
  pin is actually at that level".
- Setting a pad to `GPIO_MODE_INPUT_OUTPUT` and reading `GPIO_IN` while driving
  it proves the pad toggles, independent of whatever the matrix is routed to.
  Bit 7 of `GPIO.in1` is GPIO 39, 8 is 40, 9 is 41.

`trans_queue_depth` in `esp_lcd_panel_io_spi_config_t` must be >= 1:
`spi_bus_add_device()` calls `xQueueCreate(queue_size, ...)` unconditionally and
FreeRTOS asserts on a zero-length queue, which reboots the board.

The reference also keeps the backlight physically off (GPIO high, active low)
until after `Lcd.init()` and `fillScreen()` complete, and only then attaches
LEDC — so a duty glitch during attach can never be seen. Worth matching.

### Backlight PWM

GPIO 42 is not a plain output: it is routed to LEDC low-speed channel 7 through the
GPIO matrix (`LEDC_LS_SIG_OUT7_IDX = 80`), and the LED is active low. ESP32-S3 has
eight low-speed channels, each **five words wide** (conf0, hpoint, duty, conf1,
duty_r), so channel *n* starts at `LEDC_BASE + 0x14*n`: LSCH7_CONF0 = 0x8c,
LSCH7_HPOINT = 0x90, LSCH7_DUTY = 0x94. The four low-speed timers are a conf/value
pair, so they are at `+0xa0 + 8*n` (conf: duty_res [3:0], clk_div [21:4], pause
[22], rst [23], tick_sel [24]). A 0x10 channel stride puts channel 7 on channel 5's
conf1 - the register the driver would write is not the one the channel reads.

`rst` is a level, not a self-clearing bit: leaving it set holds the counter in
reset and the channel outputs the idle level forever. It has to be pulsed.

With 9-bit resolution the counter runs 0..511, so a duty of 512 is "always high",
which for an active-low LED means off. That is the same convention the reference
firmware uses (`ledcWrite(7, 512)` = off).

### Recording is the receiver half of the same I2S bus

The ES7210 ADC (I2C 0x41) is driven by the same MCLK/BCLK/WS the ES8311 receives, so
recording does not need a second bus - it needs a receiver.

Two constraints shape the wiring:

- **A port cannot send and receive data at once.** ESP32-S3's signal map gives each
  port one data signal: `I2S1I_SD_IN` and `I2S1O_SD_OUT` are both 30. The ADC's data
  therefore has to come in on I2S0, whose own signals are `I2S0I_SD_IN` 25,
  `I2S0I_BCK_IN` 26, `I2S0I_WS_IN` 27.
- **The receiver needs a clock.** Rather than run a second divider that merely agrees
  with the first, I2S0's RX is put in slave mode (`RX_SLAVE_MOD`, bit 3 of RX_CONF) and
  fed the transmitter's BCLK and WS back through the GPIO matrix IN_SEL, from pins 14
  and 13. A pad can drive a matrix input while still being driven as an output, so
  those pins do double duty.

`rx_stop_mode` is two bits, not one: 0 = stop only when `rx_start` is cleared, 1 = also
stop on `in_suc_eof`, 2 = also stop when the RX FIFO is full. We leave it at 1; with a
ring of descriptors `in_suc_eof` never happens.

**GDMA receive channel 0** sits at the base of the GDMA block, not at an offset from the
transmit channel: IN conf0 0x00, conf1 0x04, int_raw 0x08, int_st 0x0C, int_ena 0x10,
int_clr 0x14, link 0x20, peri_sel 0x48. The OUT channel is at +0x60 with the same internal
layout. The IN link register has an `auto_ret` bit between the address and `stop`, so
`stop` is bit 21 and `start` is bit 22 - one higher than the OUT channel's.

`IN_DONE` (bit 0) is the last data of a descriptor landing in memory. `IN_SUC_EOF`
(bit 1) is the end of the link list, which for a ring never happens.

The descriptor's `err_eof`/`suc_eof` bits are **results** the engine writes back, not
inputs: `out_eof_mode` (conf0 bit 3) selects which of them gets set when the link list
ends. Pre-setting `suc_eof` is harmless but meaningless.

**The ES7210 clock is derived, not tabulated.** MCLK is 256 * rate. The ADC's internal
clock has to be 512 * rate in single-speed mode and 256 * rate in double-speed mode
(REG08 bit 6), and `LRCK_DIV = MCLK / rate`. That forces `adc_div = 1`, `dll = 1`, and
`doubler = 1` below 64 kHz, `0` above it. Every entry in the reference firmware's
table whose MCLK is 256 * rate agrees.

REG00 is a reset **hold**, not a pulse: 0xff holds every block in reset and the chip
only starts when it is released with 0x41.

### Two compilation domains: the firmware, and the freestanding executables

A codec and a plugin are not object files in the firmware. They are separate
executables: each gets its own crt0 (`lib/rbcodec/codecs/codec_crt0.c`,
`apps/plugins/plugin_crt0.c`), its own run of `apps/plugins/plugin.lds` (the same
file preprocessed with `-DCODEC` or `-DPLUGIN`), and a link of its own objects plus
a few archives plus `-lgcc` — **no C library at all**.

Their C library is a jump table. The firmware hands a codec a `struct codec_api *ci`
and a plugin a `struct plugin_api *pi`, and `libcodec.a` defines libc-*named*
functions as thunks through it:

```c
void   *memcpy(void *d, const void *s, size_t n) { return ci->memcpy(d, s, n); }
#undef  strlen
size_t  codec_strlen(const char *s)               { return ci->strlen(s); }
```

Everything libc-shaped that is *not* in that table has to be resolved at compile
time by Rockbox's own libc headers, which are deliberately macro-based so that no
call is emitted:

```c
firmware/libc/include/stdlib.h:  #define abs(x) ((int)ABS(x))   /* labs, llabs */
firmware/include/assert.h:       #define assert(e) ... panicf("Assertion failed: ...")
```

On this target the firmware links ESP-IDF's C library; the freestanding domain must
not. `tools/root.make` therefore carries two flag sets:

| flag set | used by | libc-replacement dirs | IDF flags |
|---|---|---|---|
| `CFLAGS` | `firmware/`, `apps/`, most of `lib/rbcodec` | `-idirafter` — lose to the system libc | yes |
| `FREESTAND_CFLAGS` | codecs, plugins | `-I` — win, as on every other target | **none** |

`firmware/firmware.make` takes `firmware/include` and `firmware/libc/include` off
`INCLUDES` entirely for xtensa and hands each domain the form it needs
(`LIBC_HDR_AFTER`, `LIBC_HDR_FIRST`); `codecs.make` and `plugins.make` build their
flags from `FREESTAND_CFLAGS`.

What it looks like when the domains leak:

- System libc headers in a codec TU → `abs` ×24, `labs`, `llabs` and
  `__assert_func` ×15 undefined at codec link time. Those names stop being macros
  and become real calls into a library that is not in the link.
- `libtremor/ctype.c` conflicting with picolibc's `_ctype_`: tremor ships its own
  ctype table, which agrees with Rockbox's `ctype.h` and not with picolibc's.
- `HAVE_SYSTEM_LIBC` defined in the *target* config instead of per domain →
  `firmware/include/file.h` reaches for `<unistd.h>`, which resolves to the
  toolchain's copy inside a `-ffreestanding` TU whose include order shadows the
  `sys/cdefs.h` that copy depends on. One header line, 24 000 parse errors. It is
  now supplied by `root.make` to the firmware domain only.

`PPCFLAGS` — `SOURCES` preprocessing, linker scripts, and `mkdepfile` — is a
firmware-domain thing, built from `BASE_CFLAGS` plus `LIBC_HDR_AFTER` plus **only**
IDF's `-I` directories. The invariant is about *defines*: a `SOURCES` file is a C
preprocessor program, and if IDF's `CONFIG_*` names reached it, IDF's configuration
would start deciding which Rockbox files get compiled. The include directories are
required, because `mkdepfile` scans under `-MG`, which does not fail on an
unresolved include — it emits the name as written, and `addtargetdir.pl` turns
`esp_err.h` into `$(BUILDDIR)/esp_err.h`, a prerequisite with no rule.

Changing a flag set does not force a rebuild: `make.dep` tracks headers, not
command lines. After touching these variables, delete the domain's objects
(`lib/rbcodec/codecs/`, `apps/plugins/`) and `make.dep`.

Verified: 43 codecs and a `.rock` (clock, with bitmaps and the plugin lib) build and
link; codec objects carry zero IDF flags and firmware objects carry them all.

The leak that found this rule: `system-target.h` including `<xtensa/hal.h>` for
whole-cache maintenance. That header only exists on IDF's include path, so every
codec failed to compile while the firmware was fine. `system-target.h` is in
*both* domains, so anything it includes must be reachable in both. It now takes
IDF's headers under `HAVE_SYSTEM_LIBC` and plain declarations otherwise.
Codecs reach the cache through `codec_api`'s jump table (`ci->commit_dcache()`),
never by calling the C function, so declarations are enough — and a codec that
called one directly would fail at link, which is better than a silent no-op.

---

### What ESP-IDF owns, and what the tree must stop pretending to do

The bare-metal startup is gone from this branch: `crt0.S`, `vectors.S`,
`irom-map.c`, `mmu-xtensa.c`, `psram-xtensa.c`, `rom_cache.c`, `irq-xtensa.c`,
`timer-xtensa.c`, `app.lds`, `tools/esp-elf2image`, `tools/esp-stage2`. All of it
is on the tag `baremetal-pre-idf`, and everything in §10–§11 above describes
that branch, not this one.

`timer-xtensa.c` is the instructive deletion. It armed the core timer and
installed its own interrupt slot — which is precisely what a guest of FreeRTOS
may not do, since IDF owns VECBASE and the interrupt allocator. So the Rockbox
tick currently has **no implementation on this branch**: `tick_start()` is
unresolved, and `apps/main.c` runs a thread that calls `call_tick_tasks()` every
`HZ/2` as a stopgap. The replacement is a `gptimer` alarm (`esp_driver_gptimer`,
`driver/gptimer.h`) whose callback calls `tick()`; with IDF owning the vectors,
C code may run in the ISR, so the "keep the level-1 ISR a leaf" rule from §10
no longer applies here and the stopgap thread can go.

Cache maintenance now has two routes, both IDF's:

| Need | Route |
|---|---|
| range (DMA buffers, descriptors) | `esp_cache_msync()` in `cache-xtensa.h` |
| whole cache | `xthal_dcache_all_writeback[_inv]()` from the xtensa HAL |

`esp_cache_msync()` rejects C2M and M2C in the same call, so commit-then-discard
is two calls in that order. It also enforces cache-line alignment unless
`ESP_CACHE_MSYNC_FLAG_UNALIGNED` is passed; Rockbox hands over 12-byte
descriptors, so the flag is always set.

`tools/idf-flags` has a trap worth knowing: it decides what counts as "the
project" from the build directory it is given, not from `IDF_PATH`. An earlier
version used `IDF_PATH`, so running it without `export.sh` in the environment
silently harvested some arbitrary **IDF component's** flags — common components
only, no driver includes — and the symptom was a Rockbox file failing on
`#include "esp_cache.h"` while the generated `idf.make` looked well-formed.
Adding a component to `idf-app/main/CMakeLists.txt` (`PRIV_REQUIRES`) therefore
requires re-running `idf-flags` (and `idf.py reconfigure` first, so
`compile_commands.json` reflects it).

One upstream bug found here: `sdmmc_host_init()` assigned `host->present`, a
member that is itself `#ifdef HAVE_HOTSWAP`, while all five other uses of it were
guarded. Every target that uses `sdmmc_host` happens to define `HAVE_HOTSWAP`,
so it never surfaced. A target with a soldered-down card reader (this one) has
no presence to track, so the assignment is now guarded like the rest.

### Getting Rockbox's scheduler to run at all: four separate faults

Rockbox's kernel had never switched a thread under ESP-IDF. Making it try
produced four distinct faults, each of which hid the next. They are recorded
separately because three of them are not about the context switch at all, and
the fourth is not a bug but a wrong model.

**1. Every thread stack was in PSRAM.** A Rockbox thread stack is not allocated
from a pool: it is a plain `static long foo_stack[DEFAULT_STACK_SIZE/sizeof(long)]`
in whichever driver creates the thread (`powermgmt.c`, `storage.c`, `usb.c`,
`codec_thread.c`, `audio_thread.c`, `apps/main.c`, and more). So the
`LDFRAGMENTS` fragment that retargeted *whole archives* to PSRAM moved every
stack in the tree with it, and the first context switch landed on a stack in the
PSRAM window: `A1 = 0x3c6eeac0`, `EXCVADDR` in the same window, fault inside the
register-window spill. A stack in cache-backed memory is not merely slow, it is
unusable for the windowed ABI.

The fix is to invert the placement policy: **internal DRAM by default, PSRAM only
for objects named as pure CPU-only data.** The exceptions cannot be enumerated
the other way round, because "is this object ever a stack?" has no complete
answer. The same reasoning excludes two more classes: objects that can be the
destination of a storage or audio DMA (`disk_cache.o`, `dir.o` — they are read
buffers handed to the SD driver, which does no cache maintenance on caller
buffers on this part), and objects holding GDMA descriptors and the ping-pong
pairs (`pcm-xtensa.o`, `i2s-xtensa.o`).

What actually has to move is much less than it looks. Measured per object across
the archives that are *linked into the firmware*: `unicode` 74 KB, `language`
31 KB, `disk_cache` 36 KB, `scroll_engine` 26 KB, `bidi` 6 KB, `buffering` 13
KB, `playlist` 8 KB. The huge `.bss` figures in `lib/rbcodec/codecs/*.o` (flac at
358 KB, wmaprodec at 292 KB) are a red herring — codecs and plugins are separate
executables and are never linked into the firmware image.

**2. The tick interrupt was switching threads.** The gptimer alarm called
`call_tick_tasks()`, which runs the registered tick functions. Those are ordinary
C: button polling does I2C, I2C on this target ends in a semaphore wait, and a
Rockbox semaphore wait calls `switch_thread()`. A context switch from interrupt
context cannot work on this core. `call_tick_tasks()` in `tick.h` is now split
into the clock (`current_tick++`) and the functions (`run_tick_tasks()`), so the
alarm advances the clock only and `apps/main.c`'s tick thread runs the functions
in thread context. That is the arrangement the tick thread already carried a
comment explaining; the gptimer rewrite had quietly undone it. **A target tick ISR
may advance `current_tick` and nothing else.**

**3. The spill took exceptions that could not be taken.** `store_context()`
called `__xtensa_libgcc_window_spill()`. That function is a chain of `entry`s
(lib2funcs.S) that works by *deliberately taking window overflow exceptions* and
letting the overflow vectors write each window into its ancestor's base-save
area. A window exception cannot be taken while `PS.INTLEVEL` is non-zero, and
`switch_thread()` masks interrupts by raising `PS.INTLEVEL` to 15. The result is
a double exception, reported with PC inside that `entry` chain, `PS = 0x00070e1f`
(INTLEVEL 0xF, EXCM set), and a backtrace whose "return address" is a data
address — i.e. an undecodable frame, which is why this looked like a corrupted
context when it was not one.

FreeRTOS's own cooperative yield (`portasm.S`, `vPortYield`) shows the correct
sequence, and it is not the libgcc function:

```asm
movi   a6, ~(PS_WOE_MASK|PS_INTLEVEL_MASK)
and    a2, a2, a6            /* clear WOE and INTLEVEL */
addi   a2, a2, XCHAL_EXCM_LEVEL
wsr    a2, PS
rsync
call0  xthal_window_spill_nw /* walks base-save areas with plain stores */
```

`xthal_window_spill_nw()` never needs an exception, and clearing `WOE` around it
means none can be attempted either. Use that, not the libgcc one, in any context
switch on this part.

**4. `CALLINC` is load-bearing in both directions.** Two separate consequences:

- Windowed C entered with `CALLINC 0` has no base-save area, and the overflow
  handler then walks the base-save area of every *ancestor* frame through garbage.
  `load_context()`'s first-run path entered the thread starter with `CALLINC 0`,
  which is what the call0 form of that file wanted. Under IDF the ABI is windowed
  and the entry must be `CALLINC 8` (`PS` bits 16-17 = 2).
- `RETW` only strips the window encoding from `a0` when `PS.CALLINC` is non-zero.
  With `CALLINC 0` it behaves as a plain return and jumps to the raw saved
  address, which still has bit 31 set. The signature is an `InstrFetchProhibited`
  at the 4-byte-aligned form of that address: `PC 0x82052047`, `EXCVADDR
  0x82052044` — the instruction after the `call8` to `switch_thread` in `sleep()`,
  with the encoding bit still on it. So the `PS` a resumed thread runs with must
  carry `CALLINC 8`, whatever the outgoing thread had.

With those four fixed the fault becomes decodable, which is the point of writing
them down: `InstrFetchProhibited`, backtrace `sleep -> switch_thread`, reached
from `dircache_enable -> init_dircache -> init -> main`.

### The context switch model: save the stack pointer, then unwind

The remaining fault is not a bug in the same sense. Rockbox's
`firmware/asm/xtensa/thread.c` saves the sixteen visible registers of the current
window plus `sp`/`ps`/`windowbase`/`windowstart`/`sar`/ZOL, and on resume writes
all of them back and lets the compiler's `retw` return into the resumed thread.
That is the model the call0 ABI makes natural, and it is not how the windowed ABI
switches. FreeRTOS's port states the rule in one line:

> "Windowed ABI defers the actual context switch until the stack is unwound to
> interrupt entry."

What FreeRTOS actually stores per task is a **stack pointer**
(`pxCurrentTCBs->pxTopOfStack = SP`) plus a frame at the bottom of that stack. It
never restores `a0`-`a15` wholesale and never writes `WINDOWBASE` or
`WINDOWSTART`. The mechanism is:

1. Spill the windows, so every live register of the whole call chain is in memory
   on that thread's own stack, in the base-save areas its `entry` frames reserved.
2. Save `sp`.
3. To resume: set `sp` to the saved value, install `PS`, and *return through the
   frame*. Each `retw` on the way out triggers a window underflow, and the
   underflow handler refills `a0`/`a2`/`a3`/`a4` from the base-save area at
   `a1-16..a1-4` of the frame it is leaving. The register contents come back from
   the new thread's stack, not from a save area, and the hardware's own window
   bookkeeping is never overridden.

That is why writing `WINDOWBASE`/`WINDOWSTART` by hand is wrong: those CSRs are
the underflow handler's input, and a synthetic value makes it refill from areas
that were never written. It is also why the first run of a thread needs a
*synthetic frame* rather than a jump — `pxPortInitialiseStack()` in FreeRTOS's
`port.c` is the reference for building one, and it builds a frame that looks
exactly like an interrupt entry, because that is the only shape the exit
dispatcher understands.

The consequence for Rockbox is narrow: only the register-level primitive changes.
`switch_thread()`, the thread table, the blocking primitives and the cooperative
scheduler are untouched; `store_context()` becomes spill-and-save-`sp`, and
`load_context()` becomes restore-`sp`-and-unwind. The outgoing and incoming
threads must both be blocked at a point where their frame chain is intact on
their own stack, which is exactly what a Rockbox thread is when it is in
`sleep()`, `mutex_wait()` or `queue_recv()`.

### What the rewrite found, measured rather than inferred

`asm/xtensa/thread.c` was rewritten to the model above: `struct regs` is now
`{sp, pc, ps, start}`, `store_context` spills and saves three words, and
`load_context` restores three words and falls into `switch_thread`'s own `retw`.
The disassembly of the resume path is exactly FreeRTOS's solicited-frame shape:
`l32i a1/a0/a7` then `wsr.ps` then `retw.n`. Three separate real bugs came out of
that work, each found by measurement and each worth recording because none of
them is obvious from the source:

1. **`xthal_window_spill_nw()` is a call0-ABI leaf.** Its exit is `ret.n`, which
   is why `portasm.S` calls it with `call0`. Calling it as an ordinary C function
   hands it the *encoded* `a0` that `call8` leaves behind, and `ret.n` does not
   strip that encoding, so the core jumps to the raw value. The whole reported
   fault was that: `PC 0x82051fdb`, `EXCVADDR 0x82051fd8` — a plausible address
   with bit 31 still on it. It has to be reached with `callx0` from asm, and the
   literal has to be emitted in the same section as the code (`l32r sym` across
   the flash/IRAM gap is out of range).
2. **`callx0` overwrites `a0`.** `a0` must be read into a C variable *before* the
   spill call, not after it. Saving it afterwards stores the spill's own return
   address, which is not window-encoded, so the resume path jumps into the middle
   of `switch_thread`. Printing the saved context is what showed this: the stored
   `pc` read `420518b6` (inside `switch_thread`, which starts at `42051880`)
   instead of the caller's encoded return address.
3. **A thread started with `jx` has no return address.** `a0` at that moment is
   whatever the outgoing thread had in it — here the `0xdeadbeef` stack canary —
   and the starter's `entry` then takes a window overflow whose handler walks up
   from that value as if it were a frame. The fault was a store at `0xdeadbecf`,
   i.e. `0xdeadbeef - 0x20`, which is `addix8 a9, a?, -0x20` in the underflow
   handler. Entering the starter with `callx8` instead gives it a genuine frame,
   and `CALLINC 8` comes from the call itself rather than from a hand-written
   `PS`. Note the argument moves with the window: a `callx8` callee sees its
   `a2`/`a3` as the caller's `a10`/`a11`.

Two things that were tried and did **not** help, so they are not in the code:

- Pinning `WINDOWBASE` to 0 for the first run. The window arithmetic is relative
  to `WINDOWBASE`, so an absolute value is not needed and pinning it makes the
  first thread's frames agree with nothing. FreeRTOS never writes it.
- Restricting `WINDOWSTART` to the current window before calling the starter.
  Byte-identical failure.

**What is still open.** The resume half of the model works — the saved `pc` is
now correctly window-encoded and the unwind reaches the intended instruction. The
first run does not: a thread started from inside `switch_thread` has the
*outgoing* thread's frames above it in the window chain, so the spill and the
overflow handler walk past its root frame and store through the other thread's
stack pointer. The visible symptom after the first switch is a double exception
giving `EXCVADDR` inside thread 0's stack while `a1` is already the new thread's,
and then a second double exception inside the ROM's `ets_write_char` — the window
state is bad enough that even console output cannot run. FreeRTOS solves this by
making the first run look like an *interrupt entry* and returning through
`_xt_user_exit`/`rfe`, which restores the whole window from a fabricated
`XtExcFrame`; a cooperative `retw`-only switch has no equivalent, because
whether the underflow handler runs at all is decided by `WINDOWSTART`, which
C-level code cannot set to a value that is both correct and thread-local.

### How a thread actually gets started, and why user mode is not optional

The first-run problem the section above describes was solved by giving the thread
the one context shape the ABI already treats as self-rooted: the frame an
exception entry builds. `xt_init_frame()` in `asm/xtensa/thread.c` lays an
`XtExcFrame` (from IDF's `xtensa_context.h`) at the top of the thread's stack, and
`load_context()` leaves it the way FreeRTOS leaves one on its
`.L_frxt_dispatch_stk` path:

```c
rsil  a7, XCHAL_EXCM_LEVEL   /* PS.EXCM set, so rfe is legal; user-mode insn */
callx0 _xt_context_restore   /* SAR, LBEG/LEND/LCOUNT, a2-a15 from the frame */
l32i  a0, a1, XT_STK_EXIT    /* the frame's exit field */
jx    a0                     /* -> _xt_user_exit -> rfe */
```

Four things about that are worth knowing before trying it yourself:

- **`PS_UM` must be set in the frame.** ESP-IDF implements
  `_KernelExceptionVector` as `break 1, 0` -- an explicit "unhandled kernel
  exception" stub, at `xtensa_vectors.S:624` -- because nothing of its own runs at
  ring 0 after startup. A thread started without UM takes its first level-1
  interrupt (the tick, in practice) into that stub and dies with a BREAK and a
  perfectly intact backtrace underneath. With UM set the tick and every device
  interrupt go to the user vector, which IDF does implement.
- **`wsr.ps` from a task is fine.** The reason IDF uses `rsil` rather than
  `rsr`/`wsr` is atomicity, not privilege: `vPortYield` itself does `rsr a2, PS`
  then `wsr a2, PS` in ordinary task context. So Rockbox's `store_context` and the
  resume path keep their `wsr ps` and stay legal in user mode.
- **`WINDOWSTART` is narrowed to the current window before the restore**, which is
  what the real entry vectors do once they have spilled. It is load-bearing twice:
  it stops the spill and the window handlers from walking into the previous
  thread's windows, and it is the flag that makes a later `retw` take a window
  underflow and refill from the base save areas the spill wrote.
- **The frame's argument goes in `a6`, not `a2`.** The starter is entered as if it
  had been `call4`'d (`PS_CALLINC(1)`), so its `a2` is the *caller's* `a6`. This is
  the same convention `pxPortInitialiseStack()` uses; getting it wrong hands the
  thread function a garbage pointer.

That this works is not inferred. Six threads now start, run, block and resume:
the probe in `switch_thread()` showed first runs with `start` set to
`dircache_thread`, `scroll_thread`, `backlight_thread`, `power_thread` and
`storage_thread`, followed by resumes with correctly window-encoded return
addresses into `yield()` and `sleep()`. The dircache thread drives real SD
`readdir` traffic, times out, and returns -- which is normal Rockbox behaviour
(`dircache_thread` breaks out of its loop on `SYS_TIMEOUT`) and was previously
unreachable.

### A thread stack hiding inside a "data" object

`rockbox_memory.lf` had `scroll_engine` and `playlist` on the PSRAM list because
they look like inert data by size (26 KB and 7.6 KB). They were moved back. Both
objects also contain a thread stack -- `scroll_engine.c:196` has
`static long scroll_stack[]` and `playlist.c:217` has `playlist_stack` -- and the
archive-level entry moves the whole `.bss` including them. The symptom was a
deterministic `Cache error` / `MMU entry fault` the first time the scheduler
resumed one of those threads, with the same `EXCVADDR` on every boot.

The rule is therefore not "is this object big and inert". It is "does this object
contain something a core can be executing on", and the only safe way to answer it
is to check the object for a stack, not the archive for a size.

### The memory layout, measured rather than assumed

The esp32s3 has **one** MMU table, not two: `DR_REG_MMU_TABLE` at `0x600C5000`,
512 entries of 32 bits, stride 4. `mmu_ll.h` reads and writes it as
`DR_REG_MMU_TABLE + entry_id * 4` with `entry_id = (vaddr & SOC_MMU_VADDR_MASK)
>> 16` and `SOC_MMU_VADDR_MASK = 0x1FFFFFF`
(`components/soc/esp32s3/include/soc/ext_mem_defs.h`). Consequences that are
easy to get wrong:

- The index is the **masked address**, not the offset from the window base.
- `0x3C000000` and `0x42000000` both mask to entry 0, so the data window
  (`drom0_0_seg`, `0x3C000020`) and the instruction window (`iram0_2_seg`,
`0x42000020`) are two views of the *same* linear pages, backed by one entry.
  A page is flash or PSRAM for both buses at once; there is no per-bus target.
- Entry bits: 15 = target (0 flash, 1 PSRAM), 14 = invalid, 0-13 = physical
  64 KB page. `SOC_MMU_VALID` is literally 0.

What IDF actually arms for this build, dumped from the table at `app_main`:

| entries | target | physical | what lives there |
|---|---|---|---|
| 0..7 | flash | 10..17 | `.flash.text` at `0x42000020` (482 KB) |
| 8..16 | flash | 1..9 | `.flash.rodata` at `0x3C080020` (529 KB) |
| 17..144 | **PSRAM** | 0..127 | `.ext_ram.bss` -- 128 pages = exactly the 8 MB |
| 145..511 | invalid (`0x4000`) | -- | nothing |

`_ext_ram_bss_start = 0x3c110000`, `_ext_ram_bss_end = 0x3c6d127c`. The
`.ext_ram.dummy` section in `sections.ld` is what pushes PSRAM past the flash
rodata: `extern_ram_seg` and `drom0_0_seg` share origin `0x3c000020` and the same
bus, so the dummy advances the location counter by the rodata size and aligns to
64 KB. Rockbox's buffers land inside the armed range: `audiobuffer` at
`0x3c14d2c0` (entry 20), `codecbuf` at `0x3c54d2c0` (84), `pluginbuf` at
`0x3c64d2c0` (100).

Verified by touching it rather than by reading the linker script: writing
`0x5a5a5a5a` at `audiobuffer + 0x60` and reading it back works, and a task
diffing all 512 entries every 200 ms across the whole boot saw **zero** changes.

### "MMU entry fault" names a reason, not an address

The panic text comes from `EXTMEM_MMU_ENTRY_FAULT_ST` in
`components/esp_system/port/soc/esp32s3/cache_err_int.c`, whose entry in the
decode table is:

    { .bit = EXTMEM_MMU_ENTRY_FAULT_ST, .msg = "MMU entry fault error",
      .fault_addr_reg = 0, .fault_size_reg = 0 }

`fault_addr_reg = 0` means **there is no fault address register for this error**,
so the `EXCVADDR` in the register dump is whatever that register last happened to
hold. IDF says the equivalent outright for the sibling bit: "The following
backtrace may not indicate the code that caused Cache invalid access". Chasing
the printed `EXCVADDR` and the printed backtrace sent this investigation a long
way down the wrong path -- the address looked like a `thread_entry` slot and a
register holding `audiobuffer + 0x60` looked like a PSRAM access, and neither was
the cause.

The fault was real; the reporting was not. A windowed-ABI context switch that did
not carry `WINDOWBASE` was storing through garbage "ancestor" pointers and
corrupting the affected thread's own stack data. The cache error was that
corrupted data being used later, somewhere else entirely. See "The context switch
model: save the stack pointer, then unwind" for the fix and for the ASCII-shaped
chain that proved it.

Two rules for reading a fault here:

- A cache-error panic gives a *reason*, not an address. Trust the reason and the
  register contents; ignore `EXCVADDR`.
- If the backtrace says `CORRUPTED`, the stack is the suspect, not the reporter.
  This toolchain emits no `.xtensa-debug-module.ext-frame-info` sections at all,
  so IDF's decoder is running on heuristics -- which makes a clean backtrace
  itself evidence that the window state is sane.

### The switch is IDF's flat frame, not a hand-rolled window chain

The hand-rolled "save sp, let `retw` unwind the window chain" model was the
IDF avoidance, and it is gone. `struct regs` now *is* ESP-IDF's `XtExcFrame`,
and there is one code path for first run and for resume:

- `store_context`: fill `pc`/`a0`/`a1`/`ps`/`exit` by hand, then move `sp` to
  the frame and `call0 _xt_context_save`, which fills a2..a15, SAR and the loop
  registers. It is entered with `call0` because that is how IDF's own vectors
  enter it, and it requires its caller to have put a12/a13 in the frame first
  (`XT_STK_A12`/`XT_STK_A13`, byte offsets 60 and 64).
- `load_context`: `sp` = frame, `rsil` to `XCHAL_EXCM_LEVEL` (this supplies
  `PS.EXCM`, without which `rfe` is not a legal instruction, and it is a
  user-mode instruction, so an ordinary task can use it -- which is how
  `vPortYield` reaches the same path), `call0 _xt_context_restore`, then
  `l32i a0, sp, XT_STK_EXIT` and `jx a0` into `_xt_user_exit`, which loads
  PS/PC/A0/A1 from the frame and executes `rfe`.

`XtExcFrame` has no `WINDOWBASE` or `WINDOWSTART` field at all. IDF never
preserves the window chain across a switch; it flattens it. That is correct
because every switch happens at the same call depth (`switch_thread`), so every
thread presents the same window layout above that point and the frame's
a2..a15 mean the same thing for all of them regardless of where `WINDOWBASE`
happens to sit. **Depth, not base, is what has to match.**

`struct regs` is declared as a bare word array in `asm/xtensa/thread.h`
because that header is also read by the freestanding domain (codecs, plugins),
which has no IDF include path. `asm/xtensa/thread.c` then `static_assert`s
every offset against the real struct, so the two cannot drift apart.

Two things this cost, both worth remembering:

- **`_xt_context_save` destroys a12 and a13.** It copies them to
  `XT_STK_TMP0`/`TMP1` and then uses the registers themselves as scratch.
  Under the windowed ABI a12-a15 are callee-saved, so an inline asm block that
  calls it must list them as clobbered; without that, GCC is told nothing about
  a caller-saved-looking destruction of callee-saved state. Symptom was a
  `LoadProhibited` at `EXCVADDR 0x28` several switches later, nowhere near the
  cause. (Fixing it did not clear this particular fault -- see below -- but the
  declaration is required regardless.)
- **`make clean` deletes directories the build then needs.** After a clean,
  `ar` fails with `lib/librockbox.a: No such file or directory`; the `lib`,
  `fonts`, `icons`, `lang`, ... directories have to exist first.

### The frame must be sized for what IDF writes, not for XtExcFrame

`sizeof(XtExcFrame)` on esp32s3 is **112 bytes**. `XT_STK_FRMSZ` -- the amount
IDF actually allocates -- is **192**. The gap is the coprocessor state:
`XCHAL_HAVE_FP` is 1, and `_xt_context_save` ends with
`addi a2, a1, XT_STK_EXTRA` (112) then `call0 xthal_save_extra_nw`, which
stores the FP register file at frame byte 112 and on. On this config the struct
has **no `extra` member at all**, so an assert written as
`sizeof(XtExcFrame) <= frame` passes while the real requirement is 192 bytes.

Sizing the frame from the struct overran `struct regs` by 32 bytes and landed
on the `thread_entry` fields that follow it -- `stack` (168), `name` (172),
`tmo_tick` (176), `rtr` (180), `tmo.next` (188). The visible symptom was the
timeout list walking into a bogus node: `l32i a6, a7, 188` with
`a7 = 0xffffff6c`, i.e. `THREAD_FROM(node, tmo)` where the node pointer was
`0x28` -- a float register that happened to hold a stack address, read back as
a list link. `head == tail` said one node; the node's `next` said otherwise.

Rule: **assert against `XT_STK_FRMSZ`, never `sizeof(XtExcFrame)`.**

### a0's top two bits are the window increment, and `rfe` does not reconstruct them

In the windowed ABI `callw` stores the return address in a0 with **bits 31:30
replaced by CALLINC** -- not flagged alongside the address, *overwritten*. So
the address's own top bits are gone. `retw` reconstructs them; `_xt_user_exit`
does not: it writes the frame's pc straight to EPC_1 and `rfe` jumps to that
value verbatim.

Masking bit 31 alone is wrong -- it leaves bit 30 wrong, and the fault comes
back as `InstrFetchProhibited` at `0x020522f3` with `EXCVADDR` one below.
ESP-IDF's own decoder (`esp_cpu_utils.h`, `esp_cpu_process_stack_pc`) maps the
pair back with `(pc & 0x3fffffff) | 0x40000000`, which is valid because every
executable region on this chip -- flash .text at 0x42xxxxxx, IRAM at
0x4037xxxx, the PSRAM codec window -- has bits 31:30 equal to 01.

So the two frame slots take different forms of the same word: `pc` gets the
mapped-back address, `a0` keeps the value verbatim.

### `_xt_context_save` destroys a12 and a13

It copies them to `XT_STK_TMP0`/`TMP1` and then uses the registers themselves
as scratch. Under the windowed ABI a12-a15 are callee-saved, so inline asm
that calls it must list a12/a13 as clobbered. Without that, GCC is told nothing
about a caller-saved-looking destruction of callee-saved state, and whatever it
parked there is silently lost.

### The tick: a hand-rolled gptimer was the last IDF avoidance here

Rockbox's clock used to come from its own gptimer alarm. It now comes from
FreeRTOS's own tick: `CONFIG_FREERTOS_HZ=100` (Rockbox's `HZ` is 100) plus
`CONFIG_FREERTOS_USE_TICK_HOOK=y`, and `vApplicationTickHook()` advances
`current_tick` -- on core 0 only, because SMP FreeRTOS ticks on both cores and
counting on both would double the clock. `tick_start()` now only checks that
the kernel agrees with `HZ`.

Measured, before and after, with the breadcrumb ring:

- With the private gptimer: `current_tick` stopped at 46 while `INTENABLE` was
  unchanged (0x1300132e) and the timer's raw counter was still counting. The
  interrupt was armed and simply never delivered.
- With the tick hook: core **1**'s hook fires forever; core **0**'s stops at
  48. `INTENABLE` on core 0 is identical (`0x1300032e`) at the last core-0 tick
  and at every later switch, and `PS` is clean there (`0x00060020`: INTLEVEL 0,
  EXCM clear, UM set). So it is not a masking problem.
- **Independent of the SD card**: with `sd_init()` stubbed out the core-0 tick
  still stops, at 39. That hypothesis is dead.

Still open. What is established is the shape of the remaining fault: one core's
tick interrupt stops being delivered while its enable bit stays set and its
PS says interrupts are allowed.

### Two build traps found on the way

- **`make clean` deletes directories the build then needs.** After a clean,
  `ar` fails with `lib/librockbox.a: No such file or directory`; `lib`, `fonts`,
  `icons`, `lang` and friends have to be recreated by hand.
- **`sdkconfig.defaults` only applies when `sdkconfig` is regenerated.** Adding
  a line to the defaults and rebuilding changes nothing; the existing
  `idf-app/sdkconfig` wins. Delete it to pick the new defaults up.
- `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n`: Rockbox owns core 0 outright
  and, when every Rockbox thread is blocked, waits for the next tick inside the
  FreeRTOS task rather than returning from it, so that core's idle task never
  runs and never feeds the watchdog.

### The OS tick on esp32s3 is a SYSTIMER alarm, and what a dead tick looks like

`CONFIG_ESP_TIMER_IMPL_SYSTIMER=y`, and the FreeRTOS tick uses the same block
(`components/freertos/port_systick.c`, `vSystimerSetup`):

- one shared counter, **unit 1** (`SYSTIMER_COUNTER_OS_TICK`), running at
  1.6 MHz here;
- **alarm 0 -> core 0, alarm 1 -> core 1**, each in `SYSTIMER_ALARM_MODE_PERIOD`
  with `target_conf.period = 1000000 / CONFIG_FREERTOS_HZ` (160000 at 100 Hz);
- interrupt source `ETS_SYSTIMER_TARGET0_INTR_SOURCE + cpuid` = 57 / 58.

Those two sources have **no `_INTR_MAP` register** in the esp32s3 SVD: unlike
almost every other peripheral, the systimer alarms are not routed through the
programmable matrix. `esp_intr_alloc` still calls
`esp_rom_route_intr_matrix()` for them, but there is nothing in the SVD to read
back, so the usual "check the matrix entry" diagnostic does not exist here.

Register layout (`DR_REG_SYSTIMER_BASE = 0x60023000`, offsets derived from the
member order of `systimer_dev_t`, which is the only reliable way -- the
`*_REG` defines omit the interrupt registers):

| Offset | Field |
|---|---|
| 0x00 | `conf` -- bit 24 `target0_work_en`, bit 23 `target1_work_en`, bit 22 `target2_work_en` |
| 0x1c..0x30 | `target_val[0..2]` hi/lo -- the **one-shot** compare; reads 0 in period mode |
| 0x34..0x3c | `target_conf[0..2]` -- bits 25:0 period, bit 30 period mode, bit 31 unit select |
| 0x40..0x4c | `unit_val[0..1]` hi/lo -- the counters themselves |
| 0x50..0x58 | `comp_load[0..2]` -- write 1 to latch a compare value |
| 0x64 / 0x68 / 0x6c / 0x70 | `int_ena` / `int_raw` / `int_clr` / `int_st` |

Reading `target_val` in period mode is the trap: it comes back zero and looks
like the alarm was never programmed. The period lives in `target_conf`.

`SysTickIsrHandler` runs, clears its own alarm bit, and only calls
`xPortSysTickHandler()` when `counter/period` has advanced past a per-core
count, `s_handled_systicks[cpuid]`. That means a dead tick has two
indistinguishable faces -- "the ISR never ran" and "the ISR ran and decided
there was nothing to do" -- and they are fixed differently. Tell them apart by
reading IDF's own counter: it is a file-static, but the linker map gives its
address (`grep -A1 .bss.s_handled_systicks rockbox.map`). If it is frozen, the
ISR never ran.

### What the dead tick turned out to be, measured

Every hypothesis that was on the list is now eliminated by measurement:

| Check | Result |
|---|---|
| `conf` / `target_conf[0]` / `int_ena` | all correct and unchanged: alarm armed, period mode, period 160000, int_ena bit 0 set |
| shared counter | advancing the whole time |
| re-arm the alarm from the other core (`int_clr`, rewrite `target_conf`, `comp_load`, set `target0_work_en`) | **does not revive it** |
| core's `INTENABLE` | armed, unchanged across the death (`0x1300032e`) |
| core's `PS` at every switch | `0x00060020` -- INTLEVEL 0, EXCM clear, UM set |
| `rsr interrupt` on the dying core | `0x00018040` = CCOMPARE0/1/2 only, none enabled. **No peripheral line is ever asserted to that core again.** |
| a gptimer allocated on that core *after* the death | **not a valid test** -- the probe task meant to allocate it was never created, so `rb_late_err == 0` was just its initial value. Retracted. |
| SD card stubbed out | still dies, at 39 |

And the finding that reframes it: **the fault follows Rockbox.** Pin the
Rockbox task to core 0 and `s_handled_systicks[0]` freezes at 49 while core 1
runs freely; pin it to core 1 and `s_handled_systicks[1]` freezes at 49 while
core 0 runs freely past 128. It is not a core-0 defect -- it is whatever
Rockbox does to whichever core runs it.

The task watchdog that follows is a consequence, not a second bug: the idle
task on the dead core enters WFI and is never woken, so it stops feeding the
watchdog. `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n` was there to hide
exactly this.

Two smaller facts worth keeping:

- **`rsr intstatus` is not a valid instruction on this core config.** Read
  `rsr interrupt` and AND it with `INTENABLE` in software.
- Pinning Rockbox to a core moves the failure but does not change it, and it
  moves the watchdog to the *other* core's idle task. If you swap cores for an
  experiment, swap `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1` with it or the
  result is uninterpretable.

### The switch is not what kills the tick, and neither is the peripheral

Two more bisects, both decisive:

- **`switch_thread()` made a complete no-op**: the tick still freezes at
  exactly 49. Rockbox's context switch is exonerated. What does this is ordinary
  Rockbox code on that core, not the register-level switch.
- **`real_target_val[]`** (0x74/0x78/0x7c -- the values the hardware actually
  compares against; `target_val` reads 0 in period mode and tells you nothing)
  **advances for alarm 0 the whole time**, in exact `8 * 160000` steps between
  samples. The peripheral keeps generating core 0's alarms on schedule.

  Trap: `int_raw` read clear for alarm 0, which looks like "it never fired". It
  read clear because the re-arm probe in the same build writes `int_clr` bit 0.
  Instrumentation clearing the very thing being measured.

So: the alarm fires, the peripheral asserts, and **the core never vectors to
it.** Confirmed from a completely independent angle -- a probe task pinned to
the dying core at *higher* priority than the Rockbox task reaches its body,
calls `vTaskDelay(2000)`, and is never resumed. A runnable task on a core whose
tick is dead stays ready-but-unscheduled forever, because that core is never
preempted again.

What is left, and the shortest path to it: the interrupt is pending at the
peripheral and not reaching the core, while that core's `INTENABLE` is armed and
its `PS` is clean. The one number never established is **which CPU interrupt
number IDF assigned that core's systimer handler** -- `esp_intr_alloc` picks it
dynamically, the systimer sources have no matrix register to read back, and the
allocator's table is file-static and absent from the map, so "is that specific
bit still set" has not been checkable.

The experiment that settles it without that number: from a task pinned to the
dying core, call `esp_intr_alloc(ETS_SYSTIMER_TARGET0_INTR_SOURCE, ...)` with
**our own** handler. If ours fires, a fresh allocation reaches the core and
IDF's original binding is what was lost -- and re-allocating becomes the fix.
If ours does not fire either, the core's intake is genuinely closed and the
fault is in the CPU-side path.

### The dead tick was a missing register-window spill

The symptom was core 0 stopping all interrupt intake at a fixed point (~tick
47): the systimer tick, a hand-armed gptimer on a different slot, and the
watchdog's own panic interrupt all died at once, and the hardware escalated to
`TG1WDT_SYS_RST` because the panic interrupt could not be taken either. Nothing
source-specific was wrong, and the peripheral was innocent -- `real_target_val`
kept advancing.

The cause: **`switch_thread()` never spilled the register windows.** It is the
analogue of FreeRTOS's `vPortYield`, and `vPortYield` (portasm.S) does

```asm
movi  a6, ~(PS_WOE_MASK|PS_INTLEVEL_MASK)
and   a2, a2, a6            /* clear WOE and INTLEVEL */
addi  a2, a2, XCHAL_EXCM_LEVEL
wsr   a2, PS
rsync
call0 xthal_window_spill_nw
```

before saving context. Without that spill there is no base-save area: this core
config has no REGB option, so nothing else ever writes `sp-16..sp-4` of a frame,
and GCC's windowed code stores only its own frame, never its caller's. The
resumed thread reaches `retw` in `yield()`/`rockbox_sleep()`; `retw` does not
merely jump, it hands the caller's a0-a3 back, and the underflow handler reads
them from a base-save area that was never written. The thread returns to a
garbage address with interrupts masked and runs wild. Every downstream symptom
-- dead tick, dead gptimer, un-taken watchdog interrupt -- follows from that one
missing call.

Three things that made it look like something else:

- The saved PC was always correct. `t41` records the frame PC after the store,
  and it decodes to the instruction right after `call8 switch_thread` in the
  blocking primitive (`yield+6`, `rockbox_sleep+0x1b`). Only the *window refill*
  was wrong, so the resume looked healthy in every field we sampled.
- Decoding a trace against the wrong build's ELF sends you nowhere. Address
  layout shifts between rebuilds; pair each log with the ELF that produced it.
- `PS`, `INTENABLE` and the pending mask all read clean at every switch point,
  because the corruption happens *after* the resume, outside anything we had
  instrumented.

`xthal_window_spill()` (the exception-taking form) and
`__xtensa_libgcc_window_spill()` are both unusable here: they work by *taking*
window overflow exceptions, and `switch_thread()`'s `disable_irq()` has just
masked them. Only the `_nw` form, entered `call0`-style with WOE clear, works.

**It cannot be inline asm.** xtensa GCC does not honour clobbers of windowed
registers: an inline block that borrowed a6/a7 left the compiler believing a7
still held the frame pointer, and the next C statement executed `l32i a9, a7, 0`
with a7 = `0x40000` -- the WOE constant from the block's own literal pool -- and
faulted. Make it a real function and the ABI handles the conflict.

### The OS tick hook runs on one core, and the Rockbox clock noticed

`vApplicationTickHook` bumped `current_tick` only under
`if (esp_cpu_get_core_id() == 0)`. In SMP FreeRTOS the hook runs on whichever
core won the tick increment that period -- measured on this board, **always core
1**, all 148 samples, even though core 0's tick ISR was running fine (IDF's
per-core served counter advanced on both). So `current_tick` sat at zero
forever: no Rockbox timeout ever expired, every thread blocked and stayed
blocked, and a live, ticking, healthy core looked idle in `core_sleep()`.

Mirror the kernel's counter instead, which is one global advanced exactly once
per tick regardless of which core delivers it:

```c
current_tick = (long)xTaskGetTickCount();   /* CONFIG_FREERTOS_HZ = 100 */
```

`freertos/FreeRTOS.h` must precede `freertos/task.h`, and both must come after
`config.h` or the Rockbox headers get parsed inside FreeRTOS's macro soup.

### A solicited frame is three words, not a register file

ESP-IDF's solicited-yield path saves three things and nothing else:

```asm
vPortYield:
    entry   sp, XT_SOL_FRMSZ
    rsr     a2,  PS
    s32i    a0,  sp, XT_SOL_PC
    s32i    a2,  sp, XT_SOL_PS
    ... spill, rsil, save coprocessor state ...
```

and `_frxt_dispatch`'s `.L_frxt_dispatch_sol` branch restores them with
`wsr PS`, `l32i a0, sp, XT_SOL_PC`, and `retw`. No `_xt_context_restore`, no
`rfe`, no a2-a11.

Saving and restoring the whole register file through
`_xt_context_save`/`_xt_context_restore` -- the *interrupt* frame model -- is
wrong for a yield, and not merely wasteful. With CALLINC 8 the current window's
a8-a15 are not this function's registers, they belong two windows up, and
`_xt_context_restore` writes them into the current window. Those are the
registers the window machinery itself uses: `xthal_window_spill_nw` walks
windows taking its base-save pointers from `a9` and `a13`, and
`_WindowUnderflow8` takes its ancestor pointer from `a9`. Feeding it a8-a15
from an unrelated thread is how the spill ends up storing into the wrong place.

a2-a11 are caller-saved and dead across the blocking primitive by ABI, so not
restoring them costs nothing.

### `wsr ps` is not `rfe`

`rfe` restores PS from SPS **and clears PS.EXCM**; the exception entry had
already raised INTLEVEL for the handler. A plain `wsr ps` installs whatever is
in the frame, every bit of it, with none of those corrections. Two have to be
applied by hand at store time:

- **INTLEVEL.** `switch_thread()` is reached through `disable_irq()`, so the PS
  sampled a few instructions earlier has INTLEVEL 15. Writing it back leaves the
  resumed thread with interrupts masked forever -- which is indistinguishable
  from a dead tick, because the alarm fires in hardware and the core never
  vectors to it.
- **EXCM.** A thread must run with EXCM clear. With it set, any exception the
  thread takes is by definition a second one, so the core goes to
  `_DoubleExceptionVector`, and IDF implements that (and the kernel and hardware
  vectors) as `_xt_panic`, which prints nothing and spins. That is why the
  watchdog's `Saved PC` kept landing in `_xt_context_save` and `_xt_panic` with
  no Guru Meditation line ever reaching the console -- and the `Saved PC` is the
  watchdog handler's own PC, not the stuck loop's, so it tells you nothing about
  the cause.

Everything else in PS is left exactly as the core had it: UM decides which
exception vector a level-1 interrupt uses (`_KernelExceptionVector` is
`break 1, 0` here), WOE has to stay set for the window underflow that refills
the caller's a0-a3 to be taken at all, and CALLINC has to stay non-zero because
`retw` uses it to strip the window encoding from a0.

### Two frame-placement invariants, both about sp

- **A never-run frame has to sit at the top of the thread's stack, and its A1
  slot has to name the frame itself.** IDF's exception entry gives it that for
  free. Both halves of the never-run path address the frame through sp:
  `_xt_context_restore` ends by reloading sp from the frame's own A1 slot, and
  `_xt_user_exit` then reads PS, PC and A0 back through sp before removing the
  frame. A frame parked in the thread table with an A1 slot naming the thread's
  stack therefore has its sp replaced halfway through, and PS, PC and A0 are
  read off the top of that stack instead.
- **On the resume path sp must be the thread's saved sp, not the frame.** `retw`
  refills the caller's a0-a3 from the base-save area at `a1-16 .. a1-4`, and
  that area is inside the blocking primitive's own frame on the thread's stack.
  Point sp at the thread table and the refill reads 16 bytes of unrelated thread
  entries. Use a scratch register to find the saved values and assign a1 last.

With those in place the spill is demonstrably doing its job: the base-save word
at `sp-16` reads 0 before `xthal_window_spill_nw` runs and `0x82046352` -- a
live window-encoded return address -- after it, and is still intact when that
thread is resumed later.

### ESP-IDF brings this board's PSRAM up on the first try

`octal_psram: vendor id 0x0d (AP), dev id 0x02 (generation 3), density 0x03
(64 Mbit), good-die Pass`. The whole hand-rolled bring-up on the bare-metal
branch, which never got an answer from the chip, was never a hardware problem.
`CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y` and IDF's own driver is enough.

The resulting map, read out of the 512-entry table at `0x600C5000`:

| entries | target |
|---|---|
| 0..7 | flash pages 10..17 |
| 8..16 | flash pages 1..9 |
| 17..144 | **PSRAM pages 0..127** -- all 8 MB |
| 145..510 | invalid |
| 511 | flash page 1 |

Rockbox's PSRAM objects and the entries they land in: `audiobuffer`
`0x3c14d2c0` (entries 20..83, 4 MB), `codecbuf` `0x3c54d2c0` (84, 1 MB),
`pluginbuf` `0x3c64d2c0` (100). All inside the mapped range, and the table is
still clean while the system runs -- sampled from core 1 every eight ticks over
fourteen crash cycles, entries 16..143, zero violations.

### What the current crash is, and what it is not

The panic is reported through the **EXTMEM MMU-entry-fault status bit**
(`components/esp_system/port/soc/esp32s3/cache_err_int.c`), which is an
asynchronous cache-error interrupt, not a CPU exception. IDF says so outright
next to the other causes: "The following backtrace may not indicate the code
that caused Cache invalid access" -- and indeed the backtrace is
`prvIdleTask` -> `esp_vApplicationIdleHook` -> `xt_utils_wait_for_intr`. The
reported PC and backtrace are the idle task, not the culprit.

The hardware does have fault-info registers -- `EXTMEM.cache_mmu_fault_vaddr`
and `cache_mmu_fault_content` (the MMU entry content plus an operation code:
1 cpu miss, 2 preload miss, 3 writeback, 6 external DMA tx, 7 external DMA rx,
bit 3 selecting the icache). IDF never reads them for this cause. They were
read from the live core on every eighth tick and **both stay zero**, which is
consistent with IDF's table listing no address register for this cause. So
hardware gives no address, and the address has to be obtained some other way.

Ruled out by measurement, so it does not get re-attempted:

- **The MMU table.** Intact at storage init and intact while running.
- **`__cores[].running` being corrupted.** Checked on core 0 at every switch and
  from core 1 every tick against the bounds of `__thread_entries`; never out of
  range. The earlier reading that it pointed into the audiobuffer came from
  trusting an objdump of literal-pool-heavy code, where `l32r` + `l32i` is a
  literal load of the symbol's *address*, not a dereference of its contents.
- **The window spill.** It writes the right base-save area, verified before and
  after.

### No hardware data watchpoints on this core config

`XCHAL_HAVE_DEBUG` is 1 and `XCHAL_NUM_DBREAK` is 2 in the toolchain's
`xtensa-config.h`, which suggests watchpoints are available. They are not:
`wsr a2, dbreakc0` assembles, but `dbreak0`, `dbreak1`, `dbreak`, `dbrk0` and
`ibreak0` are all unknown opcodes -- the address registers are missing from the
dynconfig's opcode tables, same class of gap as the absent REGB options. A
watchpoint on a specific word would have named a stray writer immediately; that
route is closed.

### Where it stands now

The thread layer is now FreeRTOS tasks, not a hand-rolled switch — see
"The thread layer: stop owning the context switch" near the end of this file.
Measured after that change: no panics, asserts or aborts from the thread layer,
and boot reaches SD bring-up, the MMU dump, the dircache scan and playlist
init — further than the hand-rolled switch ever got.

Current blocker, and it is ordinary Rockbox code rather than ABI machinery:
`dc_init_filerefs()` (apps/playlist.c:203) writes through NULL because
`core_get_data(playlist->dcfrefs_handle)` returned NULL for a handle that
passed the non-NULL test two lines earlier. That is buflib/arena territory —
the arena is the `audiobuffer` region that `memory-xtensa-syms.S` places, and
the same file already records that `codecbuf` and `plugin.lds`'s
`CODEC_ORIGIN` no longer agree now that IDF owns the linker script. Suspect the
arena setup, not the scheduler.

**Sections above describing `XtExcFrame` in `struct regs`, the solicited
three-word frame, `wsr ps` versus `rfe`, frame placement, the window spill and
the flat-frame dispatch are history.** They document the implementation that
was deleted, and several of them are records of bugs that only existed because
that implementation existed. The findings that survive the deletion are: the
window spill is mandatory before any switch on this core config (no REGB
option, so nothing spills by itself); `current_tick` must mirror
`xTaskGetTickCount()` because the tick hook runs on whichever core won the
increment; and `esp_rom_printf()` is the only safe print from a kernel path.

### The arena and the dircache cannot both want everything

A **synchronous** dircache build takes `audio_buffer_available()` outright --
the whole arena -- capped only by `DIRCACHE_LIMIT`, which defaults to 6 MB
against this target's 4 MB arena (`XT_AUDIOBUF_SIZE`, half of PSRAM). So the
cache claimed everything and `playlist_init()`'s 117 KB fileref table came back
as `BUFLIB_ERRCODE_MEMBLOCK_ALLOC_FAILED` (-2).

That surfaced as a write through NULL three frames away, because buflib reports
failure as a **negative** error code while `dc_init_filerefs()` tests only for
zero -- `if (!playlist->dcfrefs_handle)` passes a failure straight through to
`core_get_data()`. When a Rockbox allocation "succeeds" and then dereferences
NULL, check for that pattern before believing the handle.

`DIRCACHE_RESERVE/MIN/LIMIT` are now overridable and `meowkits3.h` caps the
cache at 2 MB. Handle went from -2 to 4.

### Rockbox threads must be pinned to core 0

`xTaskCreateStatic` is only a macro for `xTaskCreateStaticPinnedToCore` with
`tskNO_AFFINITY`, so SMP FreeRTOS spread Rockbox threads across both cores. The
task watchdog caught it directly: `CPU 0: tick` while `CPU 1: scroll`, with
core 1's idle task starved.

Rockbox's kernel is single-core here (`NUM_CORES` is 1), the gate handoff assumes
outgoing and incoming threads share a core, and `switch_thread()` holds no
cross-core lock. Pinning is therefore load-bearing, not a tuning choice -- and it
also frees core 1's idle task, which is what the watchdog was complaining about.

Measured after both fixes: one boot, no panic, abort, assert or watchdog in 65 s;
13984 context switches and `current_tick` past 9400, still running when
reflashed. Boot now reaches SD, the MMU dump, dircache and playlist init.

### A breadcrumb ring that survives the crash

Some faults on this platform cannot be printed. A double exception runs
`break 1, 4` in `_DoubleExceptionVector` before anything is recorded, this core
config exposes no `DEPC`/`DEXCCAUSE` to recover the original cause from, and
`printf` is unavailable from the paths that matter (the thread switch masks
interrupts and IDF's libc `printf` takes a `_lock_t`). So trace by *writing*,
and read it back after the reboot:

```c
static struct scan_ring ring __attribute__((section(".noinit"), aligned(4)));
```

`.noinit` is `(NOLOAD)` in IDF's generated `sections.ld` — it is neither in the
image nor cleared by startup — and a panic reboots through `RTC_SW_CPU_RST`,
which leaves internal SRAM alone. Measured: a ring written before the panic is
read intact by the next boot. Two things to get right:

- The slots must be wiped when the ring is dumped. `.noinit` is never cleared,
  so an undumped ring mixes the current boot's records with an older boot's
  leftovers and the output reads as garbage after a certain point.
- `__builtin_frame_address(0)` is not usable on this windowed ABI. It returned
  `0xc03465cf`, which is not a stack address. Read `a1` with inline asm instead,
  or trace values that mean something in the code being traced.

This is what found the next fault. The ring stopped at the record written just
before `thread_load_context()`, and every `load_context()` before that had been
a *first run* — so the resume path had never once executed.

### The resume path returns through the frame; the window registers are not its business

A resumed thread needs its stack pointer, its window-encoded return address and
`PS`. That is the whole list. FreeRTOS's resume of a solicited frame
(`_frxt_dispatch`'s `.L_frxt_dispatch_sol`) is exactly:

```
l32i    a3,  sp, XT_SOL_PS
l32i    a0,  sp, XT_SOL_PC
wsr     a3,  PS
retw
```

Our resume path hand-wrote `WINDOWBASE` and `WINDOWSTART` immediately before its
`retw.n`, using `a8` and `a9` as scratch to do it. The file's own comment said
those registers were "deliberately not touched here; they are that handler's
input" — and the code contradicted the comment. `WINDOWSTART` is the flag that
decides whether a `retw` takes a window underflow at all, and the underflow
handler indexes the base save area through a register in the `a8`/`a9` range
(`_WindowUnderflow8` does `l32e a0, a9, -16`), so the scratch registers are the
handler's operands.

Measured, with the ring above:

| | before | after |
|---|---|---|
| last record | seq 67, the one before `thread_load_context` | seq 21865 |
| dircache scan | never completes | completes, 1706 entries built |
| fault | double exception, cause unrecoverable | cache error in `_xt_handle_exc`, *after* the resume |

Note the interaction with the previous fix. Saving `WINDOWBASE` per thread was
correct and is still needed by the *store* path, which narrows `WINDOWSTART` to
that one window after the spill. It does not follow that the resume path has to
write them back; the store's narrowing is what makes the `retw` chain refill
registers from the base save areas, and that works without touching the window
state on the way in.

What is left is a different fault, further along: `Cache error / MMU entry
fault` with `PC` in `_xt_handle_exc` (`xtensa_vectors.S:744`), reached after a
successful resume rather than during one.

### Codecs, plugins and the image format

> **Superseded on this branch.** The image is now produced by ESP-IDF's own
> `esp_app_format`/`esptool_py` components via `idf.py build`; Rockbox no longer
> packs it, and `tools/esp-elf2image` belongs to the bare-metal branch tagged
> `baremetal-pre-idf`. What still applies is the linker-script contract below.

`apps/plugins/plugin.lds` is a second linker script, run per codec and per plugin.
It needs a `CPU_XTENSA` branch, and `CODEC_ORIGIN` it computes has to equal the
`codecbuf` symbol in `app.lds`: `apps/codecs.c` rejects a codec whose header
`load_addr` is not exactly `codecbuf`. The only way to make two scripts agree is to
pin the address, so `codecbuf` is placed at the top of the PSRAM part of the
external cache window and plugin.lds derives the same number from `MEMORYSIZE`,
`PLUGIN_BUFFER_SIZE` and `CODEC_SIZE`.

`tools/esp-elf2image` produces the ESP32 v1 image: 24-byte header, a table of
(load address, length) pairs, then segment data, then a checksum byte. Two things
are not obvious from the format description:

- A flash-mapped segment must sit in the file at an offset congruent to its address
  modulo the 64 KB MMU page size, because the cache MMU requires
  `paddr % 64KB == vaddr % 64KB`. This is why `IROMORIG`/`DROMORIG` carry the `+0x20`.
- The checksum covers **every** segment, padding included, not just the RAM ones.

Padding to reach the page boundary is taken from the head of a RAM segment rather
than written as a page of zeros. Verified byte-for-byte against esptool 4.7.0; the
only difference is the append-digest flag, which we clear because we do not append
the SHA-256 that flag promises.

`root.make` packs xtensa from `rockbox.elf`, not `rockbox.bin`: `objcopy -O binary`
has to span the gap between the two flash windows (0x3C000020 to 0x42000020) and
produces a 100 MB file of zeros.

**Unverified:** codecs and plugins execute out of PSRAM. The instruction cache only
covers the IROM window; PSRAM is on the data bus. ESP-IDF supports running from
PSRAM but it is slower, and whether Rockbox's codec code behaves correctly there is
a hardware question.

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
```

### One `make` builds everything — do not run CMake in the Rockbox build dir

For this target Rockbox drives ESP-IDF itself. `tools/root.make` has the rule:

```make
$(BUILDDIR)/$(BINARY): $(IDFDIR)/rockbox_libs.cmake
	cd $(IDFDIR) && $(IDFPY) build
	cp $(IDFDIR)/build/$(IDF_PROJECT).bin $@
```

so the whole firmware is built by the ordinary Rockbox sequence, from one
directory, with one `make`:

```bash
mkdir build-meowkits3 && cd build-meowkits3
../tools/configure --target=meowkit-esp32s3
make            # compiles Rockbox, then calls idf.py build, then copies the image
```

**IDF's binary directory is `idf-app/build`, never the Rockbox build dir.**
`tools/configure` harvests flags from `idf-app/build/compile_commands.json`
(running `idf.py reconfigure` there if absent), and `idf.py build` writes to the
same place.

#### The trap, and why it used to be documented as required

Running `cmake .` inside `build-meowkits3` makes CMake treat that directory as a
binary dir (`CMAKE_HOME_DIRECTORY` = `idf-app`) and **overwrite Rockbox's
`Makefile` with CMake's**. From then on plain `make` compiles no Rockbox source:
it re-links whatever archives are already there and exits 0, so an edit to any
`.c` or `.h` vanishes from the image silently. It also removes the `zip` and
`rocks` goals.

An earlier version of this file presented that as the required sequence
(`configure; make; cmake .; make`). That was wrong and self-perpetuating: the
second half existed only to undo the first. Nothing needs CMake to run in the
Rockbox build dir, and a single `make` has always been sufficient — verified by
removing the CMake debris and rebuilding clean.

If a `CMakeCache.txt` ever appears in a Rockbox build dir, it is debris; delete
it along with `CMakeFiles/` and `cmake_install.cmake`. `make zip` and
`make rocks` are the tell that the Makefile is Rockbox's again.

Sanity check that an edit landed, before flashing:

```bash
strings rockbox.elf | grep -F 'a string only your edit adds'
```

Link-only changes (the `rockbox_memory.lf` fragment, linker script edits) do go
through with a bare `make`, which is what makes the trap easy to miss — half the
time the one-step build is genuinely sufficient.

```bash
# useful make goals
make bin        # firmware only
make zip        # full package
make tools      # host tools only
make tags
make flash      # esptool write_flash 0x0 rockbox.esp32s3
make espverify  # image_info on the packed image, no board needed
```

`make flash` and `make espverify` come from `tools/espflash.make`, included from
`root.make` only when the build output is an ESP image (the simulator shares the
arch but produces a host executable, so it has no flash goal). Overridable
variables: `ESPTOOL` (default `esptool`), `ESPPORT` (empty means let esptool
probe), `ESPCHIP` (derived from the artifact name), `ESPADDR` (default `0x0`).

Useful configure/build variables: `RBDEV_PREFIX`, `RBDEV_DOWNLOAD`,
`RBDEV_BUILD`, `RBDEV_RESTART` (restart a toolchain build at a given step),
`RBDEV_TARGET`, `RBDEV_ESP_TOOLCHAIN` (Espressif release tag).

`tools/builds.pm` drives the build farm and `tools/list_targets.pl` prints the
target status list; a new target must be added there too. `target_id` is a
unique number (currently max 126 in `configure`, max `MODEL_NUMBER` 132 in
`config/*.h`) — use the next free one.

---

## 9. Bring-up discipline: clocks, reset and routing

Most "the peripheral does nothing" bugs on this port are not logic bugs. They are one
of three setup facts that were never established, and each one costs a session to
rediscover. Before debugging a peripheral's behaviour, confirm all three.

**1. The clock gate is enabled.** Every peripheral has a bit in
`SYSTEM_PERIP_CLK_EN0` (`0x600C0018`) or `SYSTEM_PERIP_CLK_EN1` (`0x600C001C`).
Several default to 0 — SDHOST (bit 7 of EN1), I2C0 (bit 7 of EN0), timer group 1.
Reading ID registers can still work while the functional clock is off, so a good
`VERID` is not proof the block is clocked.

**2. The reset is released.** The matching bits are in `SYSTEM_PERIP_RST_EN0`
(`0x600C0020`) and `SYSTEM_PERIP_RST_EN1` (**`0x600C0024`**). The group offsets are
not symmetric with the enable registers, and using `+0x20` for group 1 is the classic
mistake: the write lands in RST_EN0, the peripheral stays held in reset, and every
write to it appears to be ignored. Some reset bits read **1** in the reset state, so
"the register already has the bit set" means *held in reset*, not *already released*.

**3. The clock source actually exists.** A divider fed by an unpowered source is zero.
For SDHOST that means either powering the BBPLL (`RTC_CNTL_OPTIONS0` bit 11
`BBPLL_FORCE_PU`, bit 10 `BBPLL_FORCE_PD` cleared) or using XTAL. Derive the divisors
from the source you can guarantee, not from the datasheet's nominal figure.

**4. The signal is routed.** On this chip no peripheral drives a pad directly: the
signal has to be connected through the GPIO matrix.

| What | Where | Trap |
|---|---|---|
| Output routing | `OUT_SEL[pin]` = `GPIO_BASE + 0x554 + 4*pin` | indexed by **pin** |
| Input routing | `IN_SEL[signal]` = `GPIO_BASE + 0x154 + 4*signal` | indexed by **signal**, not pin |
| `func_sel` width | 9 bits | masking to 6 truncates every signal above 63 |
| Bidirectional signals | both halves must be written | SD CMD (178) and D0 (180) need `OUT_SEL` *and* `IN_SEL`; writing only the output half leaves the controller blind |
| Pad function | IO MUX `MCU_SEL`, bits 14:12 of `IO_MUX_BASE + 0x04 + 4*pin` | GPIO1/2 default to UART0, GPIO17/18 to DAC, pad 43 to UART0 TX, pads 33-37 to SPIIO4-7. A pad left in its default function does not reach the matrix at all |
| Input enable | IO MUX `FUN_IE`, bit 9 | `GPIO_ENABLE` only gates the output driver; without `FUN_IE` the pad never feeds `GPIO_IN` |
| Pull-ups | IO MUX `FUN_PU`, bit 8 | `gpio_reg.h`/`gpio_struct.h` have no pull-up bit and `soc_caps.h` has no `SOC_GPIO_SUPPORT_PULL_UP`, which invites the wrong conclusion that the chip has none |

Signal indices worth having at hand: SDHOST CLK 172, CMD 178, D0 180; I2C0 SCL 89,
SDA 90; GPSPI2/3 CLK 66, Q/MOSI 67, D/MISO 68 (shared between the two controllers);
LEDC low-speed channel *n* is signal 80 + *n*.

**5. The interrupt is both mapped and unmasked.** Mapping a source to a vector slot
(`INTERRUPT_CORE0_BASE + 4*source = slot`) is not the same as enabling it: `INTENABLE`
must have the slot bit set. Slot levels are fixed by the core config — level-1 slots
are 0-10, 12, 13, 17, 18, and there is no way to program a level because
`wsr.intlevel` is not a valid opcode on this core.

### Ordering a peripheral bring-up

Do it in this order, and do not move to behaviour debugging until each step is
confirmed by a readback:

1. Enable the clock gate; read it back.
2. Clear the reset bit **in the correct group register**; read it back.
3. Program the clock from a source you have powered; read the register back — the
   hardware rewrites some fields, so the readback is the real configuration.
4. Issue the peripheral's own latch/update transaction if it has one (SDHOST:
   `update_clk_reg`).
5. Route every signal, both directions for bidirectional ones; read `OUT_SEL` and
   `IN_SEL` back.
6. Only then look at the FSM.

### Pad directionality is four registers, not one

Writing `GPIO_OUT` does not make a pad an output. Four independent fields decide
what reaches the wire, and a bring-up that sets only one of them reads back as a
dead peripheral.

| Register | Field | Effect |
|---|---|---|
| `IO_MUX_GPIO<n>` = `0x60009000 + 0x04 + 4n` | `MCU_SEL` [14:12] | 0 = GPIO matrix. Anything else is a fixed peripheral function and the matrix cannot reach the pad at all |
| | `FUN_IE` [9] | gates the pad **input**. Clear, and `GPIO_IN` never sees the pad however the matrix is routed |
| | `FUN_PU` [8], `FUN_PD` [7] | internal pull-up / pull-down |
| | `FUN_DRV` [11:10] | drive strength 0..3 |
| `GPIO.pin[n]` = `0x74 + 4n` | `pad_driver` [2] | 1 = open-drain (this is the I2C fix) |
| | `sync1_bypass` [5:4], `sync2_bypass` [1:0] | input synchroniser; the default is two APB flip-flops, so a readback immediately after a write can lag |
| | `config` [13:12] | ESP-IDF's `gpio_ll_set_dir` writes this, **not** `GPIO_ENABLE` |
| `func_out_sel_cfg[pin]` = `0x554 + 4*pin` | `func_sel` [8:0] | matrix signal driving the pad; 256 = `SIG_GPIO_OUT_IDX` = `GPIO_OUT[pin]` |
| | `oen_sel` [10] | **1** = output enable is `GPIO_ENABLE[pin]`; **0** = the peripheral's own output-enable signal |
| | `oen_inv_sel` [11] | invert that enable |
| `func_in_sel_cfg[signal]` = `0x154 + 4*signal` | `func_sel` [5:0] | which pad's input feeds this signal |

`oen_sel` is the field that matters for a bidirectional line, and ESP-IDF never
sets it: `esp_rom_gpio_connect_out_signal(pin, sig, false, false)` passes
`use_oen = false`, which `gpio_ll_set_output_enable_ctrl` turns into
`oen_sel = 0` — "use the peripheral's output-enable signal". That lets the
SDHOST assert the pad driver for the command/data phase and release it so the
card can drive the same wire. Pinning `oen_sel = 1` with `GPIO_ENABLE` set makes
the pad permanently driven: correct for CLK (host-to-card only), wrong for CMD
and DAT0.

Measured on the MeowKit board with a pad test that drives high, drives low, then
tri-states with the pull-up on, for each `oen_sel` value:

| pad | role | driven high | driven low | tri-state |
|---|---|---|---|---|
| 21 | SD DAT0 | 1 | 0 | 1 |
| 45 | unused, control | 1 | 0 | 0 |
| 47 | SD CLK | 0 | 0 | 1 |
| 48 | SD CMD | 0 | 0 | 0 |

Pads 47 and 48 do not follow `GPIO_OUT1` at all, while an unused pad in the same
register group does. Sampling `GPIO_IN1` bit 15 across CMD0's 80-clock init
sequence reads a flat 0, so the CIU clock never reaches the CLK pad. This is the
open item in the SD bring-up: the command FSM completes, `CMD_DONE` is set, and
no response is captured because the wire is never driven.

A useful discriminator: if the peripheral's `VERID`/`USRID` read correctly but its
FSM state never changes and its byte counters stay zero, the problem is the clock or
the latch, not the command you are sending.

### Reading runtime register state

`esptool read_mem` **hard-resets the chip before attaching**, so every peripheral read
returns reset defaults, never what the running firmware set. To see runtime state, dump
the registers from inside the firmware into the boot-log ring (`boot_trace_hex`) or into
`xt_isr_diag`/`xt_exc_dump`.

## 10. Gotchas

> Bare-metal branch material. These describe the `baremetal-pre-idf` tree, where
> Rockbox owned the vectors, the cache and the timer. On `esp32-s3-idf`, ESP-IDF
> owns all three; the ISA and pad-level findings still apply, the interrupt and
> startup ones do not.

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
- Anything the DMA writes back must be `volatile`. A descriptor's owner bit changes
  through a store the compiler cannot see, so an empty spin loop on it gets folded into
  a single test and the driver never notices the buffer being handed back. Check the
  disassembly, not just the source.
- `func_sel` in the GPIO matrix is 9 bits. Masking to 6 truncates every signal above
  63, and the GPSPI signals are 101-103.
- In a linker script, an assignment to `.` whose expression contains a bare absolute
  constant is treated as an absolute section address, and the region usage is then
  computed from that address instead of the section size — a spurious overflow of
  roughly the region origin. Express the same value with `ORIGIN()`/`LENGTH()`.
- Xtensa section headers are 40 bytes, not the 32 an ELF32 reader expects; the first
  24 bytes are still name, type, flags, address, offset, size.

### Scheduler bring-up: three separate faults that each hid the next

Boot appeared to "hang" at `storage_init`. It was three independent bugs, and
each one masked the next, so fixing one revealed the following one:

1. **The main thread's stack is not munged.** `switch_thread()` tests
   `thread->stack[0] != DEADBEEF` to detect overflow. `create_thread()` writes
   the pattern into every stack it allocates, but the main thread's stack is the
   linker's `.stack` region, which nothing initialises. The first scheduler
   switch therefore called `thread_stkov()` → `panicf()`. The MIPS crt0s
   (`ingenic_x1000/crt0.S`, `ingenic_jz47xx/crt0.S`) already fill it with
   `0xDEADBEEF`; `firmware/target/xtensa/crt0.S` now does the same, after the
   `.srambss` zero loop and before the first call into C.

2. **`load_context()` zeroed PS on a thread's first run.** `PS.WOE` is bit 18,
   so `wsr a7, ps` with `a7 = 0` also disabled window overflow. `xt_thread_start`
   then faulted on its own `entry` with `EXCCAUSE_ILLEGAL` (the exception dump
   reads `0x0` in the cause slot and `epc1 = xt_thread_start`). Only
   `windowbase`, `windowstart` and `sar` need clearing; write `0x40000` to PS.

3. **`core_sleep()` was a bare `waiti 0`.** `switch_thread()` enters the idle
   loop after `disable_irq()`, which on this port raises `PS.INTLEVEL` to 15.
   `WAITI n` waits for a slot of level `n+1` or higher that can actually be
   taken, and level 1 cannot be taken while the core is already at level 15 — so
   the core stalled with the tick pending and never woke. Drop the level before
   waiting (ARM's `core_sleep()` does the equivalent with `enable_irq()` after
   `wfi`).

### Keep the level-1 ISR a leaf

The tick ISR cannot dispatch into C. Anything that nests rotates the register
window through the overflow/underflow handlers, and the handler's frame is made
with `addi`, not `entry`, so it has no ABI base-save area for the chain to walk.
`timer_irq()` now only reads `TN_INT_ST`, advances `current_tick` and clears the
interrupt. The registered tick tasks (`button_tick`, the backlight timeout, ...)
run from `tick_task_thread()` in `apps/main.c`, which does `sleep(1)` then
`call_tick_tasks()`. Same rate, no window rotation in the interrupt path.

For runtime register state see §9, "Reading runtime register state".

---

## 11. How the image is entered, and what the entry code may not touch

> Bare-metal branch material — see the note at §10. Under ESP-IDF the entry is
> `app_main()` and none of this is Rockbox's problem.

Established by instrumenting `crt0.S` with markers in RTC slow memory and by
flashing purpose-built minimal images, rather than by reading the ROM.

**The ESP-IDF stage-2 bootloader does reach our entry point.** An earlier
conclusion that it did not was an artefact: the marker was at `0x50000D60`, which
`boot_trace_start()` deliberately zeroes. With the ESP-IDF second-stage loader
(stock `meowkit` bootloader at 0x0, partition table at 0x8000, image at 0x10000)
crt0 runs to completion. The PlatformIO prebuilt bootloaders do not run on this
board; the stock one does.

**Both loaders jump to the entry, and both leave a usable window state.** Do not
"fix" it.

| Loader | PS | windowbase | windowstart | a1 | vecbase |
|---|---|---|---|---|---|
| mask ROM (image at 0x0) | `0x60520` | 3 | `0x8` | `0x3fceb570` | `0x40000000` |
| ESP-IDF stage two | `0x60920` | 9 | `0x280` | `0x3fceb350` | `0x40000000` |

Both are `INTLEVEL 0, UM 1, CALLINC 8, WOE 1, EXCM 0`.

**Writing `PS.EXCM` with `WSR.PS` breaks the very next `ENTRY`.** Measured, one
variable at a time, `callx8` into a callee that does `entry a1,48` … `retw.n`.
(`WSR.PS` really does raise EXCM — writing `0x60530` reads back as `0x60530`, so
this is not a silently-ignored bit.) Even with the ESP-IDF-identical window
handlers installed at VECBASE, and at recursion depth 1 where no window can
have wrapped, the callee never executes its first instruction:

| PS written | callee entered | returned |
|---|---|---|
| `0x60520` (the loader's value, EXCM=0) | yes | yes |
| `0x60530` (`+EXCM`) | **no** | no |
| `0x60510` (`-UM`, `+EXCM`) | **no** | no |
| `0x40520` (`CALLINC=0`, EXCM=0) | yes | yes |

The fault that follows lands at a ROM address (`epc1 = 0x40044290`, EXCCAUSE 0)
with the interruptee PS still `0x60530`, i.e. control leaves our image as soon as
`ENTRY` is executed with EXCM set. This is the open question on the port: the
windowed ABI is only correct with EXCM=1, but on this core config `ENTRY` faults
with EXCM=1 when the window state is the one the loader handed us.

Clearing `WINDOWSTART` to 0 is separately fatal for a different reason: with no
window marked live, no overflow ever fires, so no base-save area is ever written
and `retw` reads an uninitialised frame.

`EXCM` is hardware-managed: it is set on **exception return** (`rfe`/`rfi`) and
cannot be raised by software. The practical consequence on this port is that the
first tick interrupt's `rfe` silently switches the whole firmware from `EXCM=0`
to `EXCM=1`, and the window vectors only become live at that moment. Anything in
the hand-written assembly that was written against `EXCM=0` has to be re-checked.

The toolchain's own `crt0.o` for esp32s3 is the reference for how little an entry
point should do — it is literally:

```
_start:  l32r a4, main ; jx a4      /* R_XTENSA_32 -> main */
```

No special-register write, no stack set-up. `firmware/target/xtensa/crt0.S` now
matches that: it sets `a1` and masks interrupts, and touches nothing else.

> **Superseded on this branch.** The switch described here was deleted: Rockbox
> threads are FreeRTOS tasks now, and IDF owns the register work. Kept because
> the measurements in it are about the core config, not about the code.

### The context switch is the part that is still wrong

> *Superseded.* The diagnosis here is partly right and the conclusion is wrong.
> `wsr ps` cannot set `EXCM`, true, but the fix is not "make the switch take a
> software exception": FreeRTOS's cooperative yield is an ordinary C-visible
> function and switches correctly without one, because it saves only the stack
> pointer and lets the `retw` chain refill the windows. See "The context switch
> model: save the stack pointer, then unwind" above, which is what the IDF branch
> implements.

`store_context()` masks the saved `PS` with `-31` (clearing `INTLEVEL` and
`EXCM`) and `load_context()` writes it back with `wsr ps` — which cannot set
`EXCM`. So every restored thread runs with `EXCM=0`, i.e. with the window
overflow/underflow vectors disabled, and GCC's windowed code silently destroys
ancestor windows once the call chain is four `call8`s deep.

ESP-IDF and FreeRTOS do not restore a context by returning from a C function.
`_xt_context_restore` is followed by an exit dispatcher (`_xt_user_exit`) that
installs the saved values through the exception registers and then executes
`rfe`:

```
_xt_user_exit:
    l32i  a0, sp, XT_STK_PS    /* interruptee PS  -> WSR PS      */
    wsr   a0, EXCSAVE_1 ... 
    l32i  a0, sp, XT_STK_PC    /* interruptee PC  -> WSR EPC1    */
    l32i  a0, sp, XT_STK_A0    /* interruptee a0  -> (EXCSAVE1)  */
    l32i  sp, sp, XT_STK_A1    /* deallocate the frame           */
    rsync
    rfe                        /* this is what sets PS.EXCM = 1  */
```

That is only consistent because FreeRTOS *enters* the scheduler through a
syscall exception, so the hardware captures `a0`/`a1`/`PS`/`PC` as a coherent
frame. Rockbox's `switch_thread()` calls `thread_store_context()` and
`thread_load_context()` as ordinary C functions and lets the compiler's `retw`
return, which is why the saved window state cannot be made valid. Aligning this
means making the switch take a software exception rather than a call.

### RTC slow memory is not uniformly writable

Stores to some offsets silently drop. Verified working: the log ring
`0x50000000..0x50000BFF`, `0x50000C00` (live block), `0x50000D60`, `0x50000D64`,
`0x50000D80..0x50000D8C`, `0x50001004`. Verified dropping stores: `0x50000C04`,
`0x50000D68` (sometimes), `0x50000D80` in some builds, `0x50000E40`. When a
marker reads back as a stale value from an earlier run, suspect the slot before
suspecting the code — put results in the log ring with `s8i` instead.

### Tick path, two independent faults

Both had to be fixed together; either one alone leaves `current_tick` at zero.

1. `system_init()` zeroes `INTENABLE` when it takes over the vectors, and only
   the GDMA slots were ever unmasked. The timer slot (`XT_SLOT_TIMERG1`, 1) was
   never enabled, so the alarm fired in `TN_INT_RAW` forever.
2. The asm level-1 handler incremented `xt_tick_count` and, by a stale literal,
   `xt_exc_cause` — not `current_tick`. `timer_irq()` is never called, because
   the handler is deliberately a leaf.

With both fixed the scheduler runs: `switch_thread` iterates, SD initialises
(`CMD41` OCR `0x00ff8000` in the log), and a full 320x240 frame is pushed
(4842 SPI chunks).

## The thread layer: stop owning the context switch

Every fault in this port has come from re-implementing something ESP-IDF already
does. The context switch is the worst instance: `asm/xtensa/thread.c` grew
`store_context`, `load_context`, `xt_init_frame`, `rb_window_spill`,
`rb_spill_asm` and a 192-byte `XtExcFrame` in `struct regs`, and each of the
base-save geometry, the spill ordering, `wsr.PS` not being `rfe`, and the
literal-pool placement became its own debugging campaign. Hand-transcribing
IDF's `vPortYield` + `_frxt_dispatch` into a `.S` file produced three new bugs in
an hour (the window spill overwriting a stash slot inside the frame; a literal
pool landing inside the function; `movi`-as-address-load not existing outside
IDF's own build). All three are non-events in IDF's code because it is IDF's,
assembled by IDF against the real core config.

**The design that removes the layer:** each Rockbox thread *is* a FreeRTOS task,
built with `xTaskCreateStatic` on the stack Rockbox already allocated, and the
right to run is a per-thread binary semaphore that only the Rockbox scheduler
holds.

- `store_context(out)` — nothing. IDF owns the registers.
- `load_context(in)` — `xSemaphoreGive(in->gate)`, then `xSemaphoreTake(own_gate)`.
  The take returning *is* the resumption, so it returns into `switch_thread`
  into the blocking primitive, which is exactly Rockbox's cooperative shape.
- `THREAD_STARTUP_INIT` — create the gate and the task. Slot reuse is handled by
  `vTaskDelete`ing the previous occupant from the creator's context.
- Thread 0 adopts `xTaskGetCurrentTaskHandle()` and gets a gate lazily on its
  first switch out.
- `core_sleep()` — `set_irq_level(0); waiti 0; set_irq_level(15);`. `waiti` ends
  on *any* interrupt, so a device ISR that unblocks a thread does not wait for
  the tick, and no FreeRTOS API is called from a masked region.

Cooperative semantics hold by construction: a task runs only if its gate was
given, and exactly one gate is held, so exactly one Rockbox thread runs and only
at Rockbox's own blocking primitives. The scheduler, thread table, IDs, names,
priorities, run queue, timeout list and every blocking primitive are untouched.

**Blocker found, not yet fixed.** `FreeRTOS.h` implicitly includes
`idf_additions.h` at its end (`FreeRTOS.h:1533`, under `ESP_PLATFORM`, marked for
deprecation in IDF v6.0). In this translation unit that implicit include lands
before `QueueHandle_t` exists, so `idf_additions.h` fails on `QueueHandle_t`,
`SemaphoreHandle_t` and `queueQUEUE_TYPE_*` -- even though `idf_additions.h`
includes `freertos/queue.h` itself four lines in, which is what makes it a real
puzzle rather than a simple ordering mistake. Reordering `queue.h`/`task.h`/
`semphr.h` around it did not help.

**A correction, so the next person does not chase a dead end.** An earlier
version of this note claimed a minimal `#include <freertos/FreeRTOS.h>` test
compiled cleanly "with the same flags", which narrowed the suspects to Rockbox
headers included before it. That test was invalid: `firmware/kernel/thread.c` is
built by Rockbox's own make and does **not** appear in
`idf-app/build/compile_commands.json`, and the flags were silently harvested from
`pthread.c` instead. The failure is un-narrowed. Reproduce it from the real
command line -- `touch ../firmware/kernel/thread.c && make V=1
firmware/kernel/thread.o` in the build dir -- never from
`compile_commands.json`, which only knows the CMake-built half of the tree.

Until that is resolved, the hand-rolled switch stays. It works; it is just the
wrong thing to own.

## GPIO 39/40/41 are JTAG pads, and MCU_SEL=1 may not be enough

The panel's DC, MOSI and SCLK are GPIO 39, 40 and 41 -- MTCK, MTDO and MTDI.
`gpio_set_direction()` calls `io_mux_select_gpio()`, and measured all three read
`mcu=1` (GPIO function) in the IO MUX, with `func_out_sel_cfg` routing them
correctly and `oen_sel` set. So the pads are *nominally* released.

They may not be *effectively* released. The JTAG block can hold those pads
independently of the per-pad MCU_SEL mux, which is why the display still shows
nothing while every register on the path reads correct. The user recalls this
from earlier work on the board, and recalls the cost correctly: releasing JTAG
from the pads takes away the JTAG path, after which reflashing needs the
boot button. Since the console and flashing here both ride USB-Serial-JTAG
(`CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED=y`, and the host enumerates
`303a:1001 USB JTAG/serial debug unit`), the two are entangled and this is not
a change to make casually.

What is known for certain, measured this session:

- CS and RST are fine. The PCA9557 answers on INPUT, OUTPUT and CONFIG (`rd=7`),
  zero write failures, `cfg=0xb0` leaves bits 0/1/2/3/6 as outputs, `out=0x41`
  holds RST released and CS asserted, and `in=0x41` confirms the pins are
  electrically at those levels.
- The SPI controller completes every transfer: 2405 for a full 320x240 flush,
  zero with USR stuck.
- DC's `oen_sel` was clear (pad tri-stated) and has been fixed -- `func=256
  oen=1 en1=1` now, matching SCLK and MOSI. That was a real bug and is worth
  keeping, but it did not by itself bring pixels up.

So the remaining candidates are, in order: JTAG still holding pads 39-41; the
GPSPI2 clock source or the pad drive strength; and the ST7789 init sequence
itself. The decisive measurement is the spike's edge-counting probe -- sample
`GPIO_IN1` for pin 41 during a known 64-byte transfer at 1 MHz and count
transitions. 512 edges means the pad is really clocking and the fault is at the
panel; zero means it is still the pad, and the JTAG question is then the answer.

**Do this change as its own session**, batched with anything else that costs a
boot-button cycle. The IDF API that releases the JTAG pads was not located in
this tree's `esp_hw_support`/`esp_system` Kconfig during this pass -- find it
before assuming `usb_serial_jtag_ll_phy_enable_pad()` is the right lever; that
bit selects whether the JTAG PHY is on the USB pads, which is a different
question from whether legacy JTAG drives GPIO 39-42.

## GPSPI2 needs spi_bus_initialize() for clock routing, not just the gate bit

Setting `SYSTEM_SPI2_CLK_EN` (bit 6 of `PERIP_CLK_EN0` at `0x600C0018`, whose
reset default is already 1) does **not** give GPSPI2 a usable clock. Measured
with the gate bit confirmed set and the base address confirmed correct:

| | value |
|---|---|
| every GPSPI2 register | reads `0x00000000` |
| write `0x5a5a5a5a` to `SPI_ADDR` | discarded, reads back 0 |
| `SPI_CMD.USR` after starting a transfer | never sets (`spins max=0`) |
| SCLK edges at the pad | **0** |

A peripheral that acknowledges on the APB and captures nothing is one whose
module clock has never been routed. The symptom is not an error — it is silence
that looks like success: `USR` reads 0, so the completion loop exits at once and
the driver reports thousands of clean, instant, empty transfers.

The recipe the spike wrote down and the reference follows: call
**`spi_bus_initialize()` for pin and clock routing**, then hand-program
`SPI_USER` / `SPI_MS_DLEN` / `SPI_CMD` yourself. Skipping the first half is what
made the display dead while every check passed. After adding it: 1024 edges for
a 512-bit transfer, `clk_gate=0x00000007`, `SPI_ADDR` reads back.

Two build consequences for any Rockbox file that includes an IDF driver header:

- it needs `CFLAGS := $(IDF_FIRST_CFLAGS)` in `firmware/firmware.make` (the
  per-file rule `sd-idf.o` already had), and
- the component must be in `idf-app/main/CMakeLists.txt` `PRIV_REQUIRES`, or
  `tools/idf-flags` never harvests its include directory.

`idf.make` is generated by `tools/configure`, so adding a component requires
`idf.py reconfigure` in `idf-app/` **and** a re-run of
`tools/idf-flags ../idf-app/build -o idf.make` from the build directory. `make`
alone will not notice.

## JTAG does not hold GPIO 39/40/41 — measured, so no boot-button cycle needed

The note above about MCU_SEL and the debug block is superseded for these pins.
The edge probe's self-test reconnects SCLK to `SIG_GPIO_OUT_IDX` and hand-toggles
it while reading `GPIO_IN1`: **20/20 edges counted**. A pad held by JTAG cannot
do that. Pads 39/40/41 are free; the fault was the unclocked controller.

## The display works — what it actually took

Confirmed on the glass: Rockbox's boot logo renders correctly, colours and
geometry included, drawn through Rockbox's own drawing code rather than a test
pattern. Three separate faults were in series, and every one of them reported as
"the panel is dead":

1. **GPSPI2 had no module clock routed.** `spi_bus_initialize()` is required for
   clock and pin routing even when every transfer is hand-programmed afterwards.
   Setting `SYSTEM_SPI2_CLK_EN` is not enough. Symptom: every register reads
   `0x00000000`, writes are discarded, `CMD.USR` never sets, and the driver
   reports thousands of clean instant transfers that carried nothing.
2. **DC's pad was tri-stated.** `func_out_sel_cfg` bit 10 (`oen_sel`) was clear,
   so output enable came from the signal's own oen line rather than from
   `GPIO_ENABLE`. Fixed with `gpio_matrix_out(PIN_LCD_DC, SIG_GPIO_OUT_IDX)`.
3. **`SPI_CLOCK` must be `0x80000000`** — `CLK_EQU_SYSCLK`, source undivided from
   the 80 MHz PLL selected by `CLK_GATE` bit 2. There is no pre-divider field on
   this block, so the `1u << 18` terms this file used to carry wrote reserved
   bits, and the resulting `N=1 H=0 L=1` violates the hardware's `N >= H >= L >= 1`
   and yields a malformed waveform rather than a slow clock.

Final register set, matching the painting reference byte for byte:
`clk=0x80000000 user=0x18000001 ctrl=0 gate=7`, `MS_DLEN` an exact bit count.

### Measurement discipline this cost a full day

- **A test pattern must not match the reference's.** The spike leaves RGBW
  quadrants in GRAM, and GRAM survives a reflash and a reset. Drawing the same
  quadrants made "still the quadrants" uninterpretable. Three cyan/magenta/yellow
  bands settled it in one flash.
- **Check what produced a number before reasoning from it.** Reading `0x00f8` out
  of the framebuffer for a "solid red" frame proved nothing, because the fill that
  put it there wrote raw bytes `F8,00` by hand. Reasoning from that produced a
  pixel byte-order "fix" that swapped both halves of every pixel — yellow came
  out blue — and had to be reverted.
- **Count a signal where it changes.** Counting DC inside `spi_write`'s wait loop
  reported zero forever, because `dc()` moves the pin *before* `spi_write()` is
  entered and holds it for the whole transfer. The transition is structurally
  outside the sampling window.

### Remaining blocker

The board reboots roughly every nine seconds. `rb_task_body -> thread_exit ->
thread_exit_final -> THREAD_PANICF -> abort`. `thread_exit_final()` is
`NORETURN` and panics after `switch_thread()` returns, on the kernel's assumption
that abandoning the current thread never comes back — which the FreeRTOS task
model does not honour. The panel now renders Rockbox's own panic screen, so this
is visible on the glass as well as the console.

## Codecs load now; they cannot execute from either window as things stand

Two separate facts, both measured this session, and they conflict.

**The load path is fully working.** With `codecbuf` pinned to the address
`plugin.lds` computes (see `XT_CODECBUF_ADDR` in `memory-xtensa.h`), a codec from
the card validates completely:

```
codec: lc_open -> 0x3c680020
codec: hdr=... magic=0x52434f44 want 0x52434f44   ("RCOD")
        tid=126 want 126
        load=0x3c680020 codecbuf=0x3c680020
        end=0x3c6f146c limit=0x3c780020
codec: entering entry_point at 0x3c6803f0
```

**It cannot be executed from the data window.** Entering that address gives
`InstructionFetchError` with `PC == EXCVADDR` -- the first instruction never
fetches. `0x3C000020` is `DROMORIG`, the data-cache window.

**It cannot be used as data from the instruction window either.** Moving
`codecbuf`/`pluginbuf` to `0x42000020` (`IROMORIG`) makes the codecs link and
fetch there, but then anything touching those buffers as *data* faults at boot:

```
A2 = 0x42780020 (pluginbuf)   EXCVADDR = 0x4278003c   EXCCAUSE = 3 (LoadStoreError)
```

The two windows share MMU entries -- index is `(vaddr & 0x1FFFFFF) >> 16`, and
they differ by exactly `0x06000000` -- but the *bus* still decides what is legal:
fetch only from `0x42…`, load/store only from `0x3c…`.

So the fix is not a base address. It is splitting the two roles: images stay
linked at the instruction-window address, and every data access to `codecbuf` /
`pluginbuf` -- `lc_open`'s `read()` into `hdr.load_addr`, the bounds checks in
`apps/codecs.c` and `apps/plugin.c`, and the codec's own heap at
`&codecbuf[codec_size]` -- has to go through a data-window alias. That touches
core files, so it wants a deliberate pass rather than a patch.

`CONFIG_SPIRAM_XIP_FROM_PSRAM` / `FETCH_INSTRUCTIONS` / `RODATA` are enabled and
do work -- the firmware's own `.text` now runs from `0x4202xxxx` on PSRAM, and
there are no regressions -- but they do not by themselves make a freshly loaded
image fetchable, because IDF only re-maps ranges that were in the image at link
time.

### Two silent-failure traps worth knowing

**1. `pluginlib_actions.c` compiles a deaf plugin on any touchscreen target.**
Both keypad chains in `apps/plugins/lib/pluginlib_actions.c` end in:

```c
#else
#   ifndef HAVE_TOUCHSCREEN
#       error pluginlib_actions: No actions defined
#   endif
#endif
```

So a target that defines `HAVE_TOUCHSCREEN` but has an unlisted `CONFIG_KEYPAD`
does **not** fail the build. It gets a `pla_main_ctx` with no directional and no
select entries, and every plugin using `pluginlib_getaction()` (53 of them)
loads, draws, and ignores every key. MeowKit hit exactly this: 2048 ran perfectly
and was completely unresponsive, including to the key that would exit it. If
plugins load but no button works, check for an `ESP32S3_PAD` case here before
suspecting the loader.

**2. A wrong comment can disable a feature permanently.** The touch poll in
`button_read_device()` was `if (0) ft6x06_irq_handler();` with the justification
that the function "runs inside the level-1 handler" and a 1.5 ms I2C transfer per
tick would cost a third of the CPU in the interrupt. That premise was false:
`apps/main.c` runs the tick tasks in `tick_task_thread`, and the xtensa gptimer
alarm advances `current_tick` only — `button_read_device()` is ordinary thread
context. `ft6x06_irq_handler()` is also non-blocking (`i2c_async_queue` with
`TIMEOUT_NOBLOCK`), so there was never a cost to avoid. Meanwhile `AGENTS.md`
claimed touch was "done: polled from the button tick", so nothing looked wrong.
The code was the only honest artefact.

**Verifying a call is live:** the symbol table lies. `ft6x06_irq_handler` looked
absent from `nm` output twice — once because a `head -20` truncated it, once
because the check ran between the Rockbox link and the IDF relink. The
disassembly of `button_read_device` showing `call8 <ft6x06_irq_handler>` is what
actually proved it.
