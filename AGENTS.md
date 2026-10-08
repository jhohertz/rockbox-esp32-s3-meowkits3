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
  But the prebuilt toolchain's libgcc is windowed in every multilib, so `-lgcc`
  under call0 mixes ABIs. GCC's call0 code keeps live values in a12-a15 across
  calls, and a windowed helper is free to clobber them; it also returns with
  `retw`, which needs a window rotation a call0 call never sets up. ld warns
  about exactly this. `lib/xtensa_support` supplies the helpers Rockbox needs,
  written with 32-bit operations only so they do not call libgcc again.
  `CORE_LIBS` are searched before `-lgcc`, so the Rockbox archive wins.

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
- - Cache: the toolchain's esp32s3 core config exposes single-precision FP
  opcodes (`madd.s`, `mul.s`, `ufloat.s`). Verify against the silicon before
  relying on them; Rockbox otherwise assumes soft float.
- `N16R8` = 16 MB flash + 8 MB PSRAM. Rockbox's memory model is
  `MEMORYSIZE` (MB) in `configure`, which sizes the audiobuffer and plugin buffer.
  PSRAM is cache-backed and slower than internal SRAM — plan the linker script
  around what must live in internal RAM (stack, ISR, framebuffer).

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

**Design decision:** keep the descriptor ring and the DMA buffers in internal SRAM. That sidesteps
cache maintenance entirely and matches ESP-IDF's own guidance ("Most peripheral DMA controllers
have requirements that sending/receiving buffers should be placed in DRAM and word-aligned"). The
cost is a copy from the audiobuffer into a ping-pong pair — a step no existing Rockbox audio
backend needs. A 2 × 4096-sample pair is about 32 KB, which fits inside the 337 KB static region
alongside .bss.

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

### No interrupt handler can be installed on this core config

This is the reason the audio path is polled rather than signalled.

Tested against the esp32s3 dynconfig (`xtensa_esp32s3.so`):

| | |
|---|---|
| `wsr.excb` | **not a valid opcode** — no exception base register |
| `wsr.intbase` | **not a valid opcode** — no per-level interrupt base |
| `enableint` / `disint` | **not available** |
| `wsr.vecbase` | available, but the level-1 vector offset is not published anywhere in the toolchain |
| `wsr.intenable` | available |
| memory-mapped priority registers | none in `INTERRUPT_CORE0` |
| `XCHAL_NUM_INTLEVELS` | 6, `XCHAL_NUM_INTERRUPTS` 32, `XCHAL_NUM_EXTINTERRUPTS` 26 |
| Tensilica `esprv_int_set_type/priority` | proprietary, not in the toolchain |

Windowed-ABI context switching is also impossible: the esp32s3 dynconfig has no REGB
option, so `mtsr`/`mfsr`/`spills`/`spillw` are unavailable. That is a second reason the
call0 ABI is the only workable one.

Consequence: DMA completion is detected by a helper thread polling the descriptor owner
bit. The latency is bounded by the scheduler tick (HZ = 100 → 10 ms), which is longer
than one mixer frame above ~25 kHz, so the spin is sized to a full frame period at the
current rate and the thread yields only when the DMA is already starved.

### Memory map (from the toolchain's own linker scripts, not guessed)

The prebuilt toolchain ships `lib/esp32s3/memory.elf.ld` and ESP-IDF's
`components/esp_system/ld/esp32s3/memory.ld.in`. The important part is that
**instruction and data buses see the same physical SRAM at different addresses**:

```
0x40370000  IRAM / I-cache (instruction bus, word-aligned only)
0x40378000  D/IRAM shared region, instruction view
0x3FC88000  D/IRAM shared region, data view      <- same physical RAM
0x3FD00000  DRAM / D-cache end
```

- Static `.data`/`.bss` is capped at `I_D_SRAM_SIZE = 0x54700` (**337 KB**), not the
  full 512 KB. The rest is heap only, and only after the app starts.
- A linker script must insert a dummy section to skip the overlap between
  `iram0_0_seg` and `dram0_0_seg` — Rockbox's `rom.lds` assumes one contiguous
  region and cannot express this.
- **PSRAM and flash rodata share the same 32 MB window** at `0x3C000020`
  (`extern_ram_seg` and `drom0_0_seg` are on the same bus). ESP-IDF papers over it
  with a dummy section sized by the rodata already placed there. You cannot map all
  8 MB of PSRAM and all flash rodata at the same time.
- The window is bank-switched per 64 KB page by a **512-entry table of 16-bit
  values at `0x600C5000`**: bit 15 target (0 = flash, 1 = PSRAM), bit 14 invalid,
  bits 0-13 physical page number. **The entry index is the masked address**
  (`vaddr & 0x1FFFFFF`, not `vaddr - 0x3C000000`), so `0x3C010000` is entry 1, not
  15361. Getting this wrong makes `mmu_init` return immediately and the audiobuffer
  reads flash instead of PSRAM.
- Flash code is executed through the instruction cache window at `0x42000020`.
- RTC fast memory 8 KB at `0x600FE000`, RTC slow 8 KB at `0x50000000`.

Rockbox's single-region `rom.lds` model does not survive this. The xtensa linker
script needs at least three regions (IRAM, DRAM, external) and the two dummy
sections, and the framebuffer has to be placed deliberately: PSRAM is fine for a
320×240 double buffer (2 × 150 KB) but it is cache-backed and cannot be touched by
anything that must work before the cache is enabled.

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
- **ESP32-S3 GPIO has no internal pull-up registers.** They are absent from the SVD and
  from ESP-IDF's `gpio_reg.h`, and `soc_caps.h` has no `SOC_GPIO_SUPPORT_PULL_UP` for
  this chip. The reference firmware calls `GPIO_PULLUP_ENABLE`, which cannot be relied
  on here — external pull-ups on SDA/SCL are required for bit-banged I²C.
- `rsr.intlevel` / `wsr.intlevel` do not exist on the esp32s3 core config; the level is
  the low 13 bits of `PS`.
- Boot: **decided — inherit ESP-IDF's second-stage bootloader.** Rockbox is the
  app image (`app0` at 0x10000), the bootloader stays at 0x0 and is flashed with
  plain `esptool`. `esptool elf2image` is the `tool` in `configure`; nothing about
  the flashing workflow changes.
  What that buys: caches and the SPI-RAM controller are already up when Rockbox is
  entered. What it does not buy: the loader only maps MMU pages for segments present
  in the image, and the audiobuffer is NOLOAD, so Rockbox must map its own PSRAM
  pages at runtime (`mmu-xtensa.c`).
  `crt0.S` is therefore not a reset vector — the ROM owns the vectors, the entry is
  jumped to (so `a0` is not a return address and `a1` must be set by hand), and
  `.bss` is not in the image file.
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
- Anything the DMA writes back must be `volatile`. A descriptor's owner bit changes
  through a store the compiler cannot see, so an empty spin loop on it gets folded into
  a single test and the driver never notices the buffer being handed back. Check the
  disassembly, not just the source.
- `func_sel` in the GPIO matrix is 9 bits. Masking to 6 truncates every signal above
  63, and the GPSPI signals are 101-103.
