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
| Audio out | ES8311 DAC over I2S (MCLK 15, BCLK 14, WS 13, DOUT 16) → NS4150B class-D amp → 1 W speaker. 44100 Hz, 16-bit. Amp enable is PCA9557 IO3. |
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
| FT6336 touch | `HAVE_TOUCHSCREEN`, `drivers/ft6x06.c` | done: polled from the button tick, because the INT pin is not connected |
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
| IO3 | NS4150B amp enable (active high, 10K pull-down) |
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

### Memory map (from the toolchain's own linker scripts, not guessed)

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

**Speaker amplifier.** The NS4150B between the codec and the speaker is enabled by
PCA9557 IO3, high-active with a 10K pull-down. Nothing in the target drove it, so
the DAC produced a signal that never reached the speaker. `HAVE_SPEAKER` plus
`audiohw_enable_speaker()` in `audiohw-xtensa.c` fixes it.

That exposed a gap in Rockbox itself: `audio_enable_speaker()` lives in
`apps/audio_path.c`, which `apps/SOURCES` only builds when `INPUT_SRC_CAPS != 0`.
A target with a speaker but no recording or radio therefore references a function
that is never compiled. `audio_path.c` now guards the input-source selection with
`#if INPUT_SRC_CAPS != 0` so the file can be built for speaker-only targets.

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

### Codecs, plugins and the image format

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
