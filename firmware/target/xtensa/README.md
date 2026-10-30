# Rockbox for the MeowKit ESP32-S3 — target notes

Board: MeowKit S3, an ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
with a 320x240 ST7789 panel on SPI behind a PCA9557 I2C IO expander, an ES8311
codec on I2S into a class-D amplifier, an ES7210 mic ADC on the same I2S bus, a
1-bit SDMMC slot, an FT6336 capacitive touch, an AXP173 PMU, a PCF8563 RTC and
two face buttons plus a 5-way stick. The user-facing description — what works,
what the hardware cannot do, how to flash it — is in the repository's top-level
[README.md](../../../README.md); the player manual describes the controls and
the installation steps.

This file is about the code.

## Layout

Three levels, the way `mips/ingenic_x1000/agptekrocker` does it: the manufacturer
slot carries the SoC.

```
firmware/target/xtensa/                     arch: cache maintenance, the crash
    cache-xtensa.[ch]                       record, the IDF thread layer
    paniclog.c paniclog.h
    thread-idf.c

firmware/target/xtensa/espressif_esp32s3/   SoC: everything that is ESP32-S3
    system-target.h system-esp32s3.c        registers rather than board wiring
    gpio-esp32s3.h i2c-esp32s3.c i2s-esp32s3.[ch] spi-esp32s3.[ch]
    usb-esp32s3.c sd-esp32s3.c timer-esp32s3.c pcm-esp32s3.c
    adc-esp32s3.c debug-esp32s3.c lc-esp32s3.c
    memory-esp32s3-syms.S                   the buffer symbols a linker script
                                            would hand out

firmware/target/xtensa/espressif_esp32s3/meowkits3/   board: this panel, this
    *-target.h        button, lcd, backlight, adc, i2c  codec, this PMU
    button-meowkits3.c backlight-meowkits3.c lcd-meowkits3.c
    audiohw-meowkits3.c power-meowkits3.c
    st7789.[ch] pca9557.[ch] es8311.[ch] es7210.[ch]
```

`tools/configure` sets `t_cpu="xtensa"`, `t_manufacturer="espressif_esp32s3"`,
`t_model="meowkits3"`, which is what makes the three `TARGET_INC` directories
real. A second board on the same silicon would add only a sibling of
`meowkits3/`.

`firmware/export/xtensa.h` holds the register bases, interrupt sources and the
memory-layout numbers; `firmware/asm/xtensa/thread.[ch]` the context switch;
`lib/libsetjmp/xtensa/setjmp.S` the setjmp pair; `idf-app/` the ESP-IDF project
(`main/rockbox_app.c`, `rockbox_memory.lf`, `sdkconfig.defaults`).

`apps/plugins/plugin.lds` and `lib/rbcodec/codecs/codecs.make` reach the codec
and plugin window numbers through `cpu.h` -> `xtensa.h`, the same way every
other target's linker script reads `as3525.h` or `x1000.h`.

`apps/plugins/plugin.lds` and `lib/rbcodec/codecs/codecs.make` reach the codec
and plugin window numbers through `cpu.h` -> `xtensa.h`, the same way every
other target's linker script reads `as3525.h` or `x1000.h`.

## The two domains

There is not one compile here, there are two, and mixing them up is the easiest
way to break the build:

- **Firmware.** Compiled against the platform C library, linked by ESP-IDF.
  `tools/meowkits3/idf-flags` harvests IDF's own include paths and defines into
  `$(BUILDDIR)/idf.make`, and `root.make` adds them to every firmware object.
  IDF's headers are `CONFIG_*`-driven and test those names bare, which is why
  `-Wno-undef` is scoped to `firmware/target/xtensa/` rather than applied
  globally; `-Wundef` stays on for the rest of Rockbox.
- **Freestanding.** Codecs and plugins, compiled with no C library and linked by
  `plugin.lds` against a jump table. They see no IDF header at all. Cache
  maintenance reaches them through the codec/plugin API instead.

Only `firmware/target/xtensa/cache-xtensa.c` includes the platform cache HAL.
Anything that used to include an IDF header from `system-target.h` — and
`system-target.h` is reached from `system.h`, so that meant nearly every file in
the build — now declares the entry points instead.

## What the platform owns

Rockbox does not run bare metal on this branch. ESP-IDF's second-stage
bootloader sits at `0x0`, the partition table at `0x8000`, and Rockbox is an
application image at `0x10000`. That means IDF, not Rockbox:

- owns the linker script, so Rockbox's buffer symbols come from
  `memory-esp32s3-syms.S` and the arena guard is `tools/meowkits3/esp-layout-check`
  rather than an `ASSERT()` in a section;
- owns the caches, and the esp32s3 core config has no cache instructions of its
  own, so maintenance goes through IDF's cache HAL (or, on the bare-metal
  branch, through the mask ROM's `Cache_*` entry points by absolute address);
- owns the boot: its second-stage loader arms the caches, the MMU window and the
  instruction bus before jumping to Rockbox. Octal PSRAM bring-up is still
  Rockbox's job -- neither the mask ROM nor IDF's prebuilt bootloaders do it;
- owns FreeRTOS, which Rockbox's scheduler is built on top of (`thread-idf.c`).

The bare-metal variant, with its own crt0, vectors, MMU and image packer, is on
the tag `baremetal-pre-idf`.

## Rules worth knowing before editing

- Keymap blocks in plugins are pure addition. Never wrap or relax a shared
  `#error "No keymap defined!"` — it is the only thing that tells another target
  its mapping is missing.
- Weights, register indices and bit positions in this directory were verified
  against the chip's SVD, the toolchain's dynconfig or the silicon, not
  inferred. Where a fact came from measurement, the comment says so.
- Anything a DMA touches must be `volatile` and outside the cache unless
  maintained explicitly. Descriptor rings live in internal SRAM because the GDMA
  descriptor address is a 20-bit window; buffers may live in PSRAM.
- Every C function must be entered with CALLINC 8 from hand-written assembly, or
  the window overflow handler walks a garbage frame chain.

`AGENTS.md` at the repository root is the long version of the above: the
measured memory map, the register facts, and the things that were tried and
refuted. The documents in `notes/` are working notes — plans, sprint logs and
research threads — and are not part of what a submission would carry.
