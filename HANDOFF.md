# Session handoff — Rockbox ESP32-S3 port

**Branch:** `esp32-s3-ports`
**Repo:** `/home/jhohertz/co/rockbox`
**HEAD:** `8dbde33486`, tree clean apart from the changes described below
(HANDOFF.md is a session artifact and is deliberately untracked).

**Read `AGENTS.md` first.** It is the accumulated hardware reference and every
register claim in it has been checked against ESP-IDF headers or the SVD. Do not
re-derive anything from memory; the two bugs that got in earlier were both
"remembered" register offsets.

---

## 1. Status at a glance

The port is **feature-complete in software**. Every Rockbox subsystem the board
needs has a driver, both the native and the simulator build link cleanly, and
the packed ESP image validates against `esptool image_info`.

**The board now boots.** A direct ROM boot (Rockbox image flashed at `0x0`, no
second-stage bootloader) runs `crt0.S`, maps the IROM window, calls
`psram_init()`, calls `mmu_init()`, zeroes internal SRAM and reaches `main()`.
Everything below that is now measured on silicon rather than reasoned from
ESP-IDF's register definitions.

### What is verified on hardware

| Fact | How it was established |
|---|---|
| The mask ROM loads IRAM segments and jumps to the entry point | entry `0x40378088` executes; a liveness marker written to `0x3FC90000` is present after reset |
| `irom_mmu_map()` arms MMU entries 144..149 (physical pages 8..13) and IROM fetches from `0x42900020` work | MMU snapshot in `crt0_trace`; `irom_canary()` in `.text` executes and traces |
| The Ext_RAM cache returns `0x0bad0bad` for a page whose MMU entry is not usable | direct DROM read of `mmu_init`'s page |
| A user command issued on **SPI0** stalls the flash read FSM | after `esp_rom_opiflash_exec_cmd(0, ...)`, every later fetch through the `0x42000000` window returns `0x0bad0bad` and the boot dies with `IllegalInstruction` at the first `.text` function; restoring `SPI_CTRL(0)` is not enough |
| The octal PSRAM chip does **not** answer SPI1/CS1 | MR0/MR2 reads return `0x0000ffff`; a sync write of `0x5a6b7c8d` at address 0 followed by reads at 0 and 0x1000 both return `0xffffffff` |
| Pads 33..37 are in GPIO mode at reset | they have to be forced to `SPIIO4..SPIIO7`/`SPIDQS` (`MCU_SEL = 4`); pads 27/28 likewise come back as GPIO because this module's flash is quad |

### Bring-up observability

esptool resets the chip before it can read memory, so live UART is not usable for
post-mortem, and the ROM's `ets_write_char_uart` faults (it reads `g_uart_print`
in a ROM data page that is not armed). Instead:

- `firmware/target/xtensa/bootlog.c` keeps a 4 KB ring in internal SRAM:
  `boot_log_len` at `0x3FC9BDFC`, `boot_log` at `0x3FC9BE00`.
- `crt0_trace` (currently `0x4037aa34`, in `.ibss`) holds the MMU snapshot, the
  IROM mapping status and the exception record.
- `diag_trap` in `crt0.S` writes the first two exceptions to `0x3FC90100` and
  `0x3FC90140`.

Read them with `/tmp/rbprobe.py`, which resolves the symbol addresses from the ELF
and dumps them with esptool.

### Open blocker

**PSRAM.** Everything above is fine; the chip never responds. Until it does,
`.bss`, the audiobuffer, the framebuffer and the codec buffers are unmapped and
the application cannot do anything useful. The bring-up now fails cleanly rather
than hanging, so the rest of the boot is testable while this is worked on.

What has been eliminated, by measurement rather than inference:

| Hypothesis | Result |
|---|---|
| Octal data pads (33..37) left in GPIO mode | ruled out — they read back `MCU_SEL = 4`, `FUN_DRV = 3` after `init_psram_pins()` |
| The ROM's `esp_rom_opiflash_exec_cmd` mis-marshals the phases | partly true (it leaves `USER1`/`USER2` at reset defaults), but a hand-built transaction with every phase written explicitly gives the same all-ones answer |
| SPI0 cache access blocks SPI1 user commands | ruled out — suspending the Ext_RAM I- and D-caches around the bring-up changes nothing |
| The transaction never starts | ruled out — `CMD` self-clears and `SPI_MEM_INT_RAW(1)` bit 2 (`TOTAL_TRANS_END`) is set |
| SPI1 user commands work in general | not established — a SPI1 user command on CS0 reading flash offset `0x10000` returns `0xffffffff`, not the image header |

The remaining possibilities are all physical: CS1 is not the pad the PSRAM is wired
to, or the PSRAM's data lines are not the pads we think they are. The next step is a
pin-level experiment rather than a register one.

| Area | Files | State |
|---|---|---|
| Arch support (config.h, elf, rom.lds, thread switching, setjmp) | `firmware/export/config.h`, `firmware/asm/xtensa/`, `lib/libsetjmp/xtensa/` | done |
| call0 ABI + Rockbox-supplied 64-bit and soft-float helpers (no libgcc) | `lib/xtensa_support/` | done |
| Target skeleton, crt0, linker scripts, MMU | `firmware/target/xtensa/{crt0.S,app.lds,mmu-xtensa.c,vectors.S}` | done |
| Interrupt dispatch (VECBASE, no INTBASE) | `firmware/target/xtensa/{vectors.S,irq-xtensa.c}` | done |
| I2C master (command-list machine, async core) | `firmware/target/xtensa/{i2c-xtensa.c,i2c-target.h}` | done |
| PCA9557 expander, GPSPI2, ST7789 + power states, LEDC backlight | `firmware/target/xtensa/{pca9557.c,spi-xtensa.c,st7789.c,lcd-xtensa.c,backlight-xtensa.c}` | done |
| I2S + GDMA audio out, ES8311, speaker amp | `firmware/target/xtensa/{i2s-xtensa.c,es8311.c,audiohw-xtensa.c}` | done |
| Recording: I2S0 RX + GDMA IN ch 0, ES7210 | `firmware/target/xtensa/{pcm-xtensa.c,es7210.c}` | done |
| SDHOST (DesignWare MMC) storage | `firmware/target/xtensa/sdmmc-xtensa.c` | done |
| Buttons + FT6336 polled touchscreen | `firmware/target/xtensa/{button-xtensa.c,button-target.h}`, `apps/keymaps/keymap-meowkits3.c` | done |
| RTC (PCF8563), PMU (AXP173), PEK power key, clean shutdown | `firmware/drivers/rtc/rtc_pcf8563.c`, `firmware/drivers/axp173.c`, `firmware/target/xtensa/power-xtensa.c` | done |
| Octal PSRAM bring-up at 80 MHz (Rockbox must do it; ROM/bootloader do not) | `firmware/target/xtensa/psram-xtensa.c` | done |
| ESP image packing + flash/verify goals | `tools/esp-elf2image`, `tools/espflash.make` | done |
| CPU frequency ladder (240/160/80 MHz) | `firmware/target/xtensa/system-xtensa.c`, `system-target.h` | done |
| USB-OTG (DWC2 device: MSC + HID + CDC, VBUS via the PMU) | `firmware/target/xtensa/usb-xtensa.c`, `firmware/drivers/usb-designware.c` | done |
| SDL simulator (`--type=S`) | `uisimulator/buttonmap/meowkits3.c`, `uisimulator/bitmaps/UI-meowkits3.bmp`, `firmware/target/hosted/sdl/sim-ui-defines.h` | done, headless-verified only |
| CPU-only statics relocated to PSRAM | `firmware/target/xtensa/app.lds`, `crt0.S`, `mmu-xtensa.c`, `firmware/common/unicode.c`, `apps/language.c`, `firmware/logf.c` | done |
| Bring-up report card in the debug menu | `firmware/target/xtensa/debug-xtensa.c` | done |

### Last ten commits

```
5f8c3610ec ft6x06: clamp the reported point count to the parsed array
09479e178b xtensa: set the backlight PWM period and clock source correctly
8b8dc5bf57 xtensa: correct two register comments
c98e5bb8e5 tools: flash an ESP32 target from the build directory
3a2085f4c3 xtensa: run the PSRAM at its rated 80 MHz
c68ef2eeda meowkits3: add a USB serial console and a bring-up report card
fa11295173 xtensa: refuse a DMA frame that does not fit a GDMA descriptor
82f899243c xtensa: move the CPU-only statics into PSRAM
101f5bdc98 meowkits3: stop A and B from firing twice per press
f9ce9e3a9a meowkits3: make the joystick and the touch grid do the same thing
```

---

## 2. Build, verify, flash

```sh
cd /tmp/rbbuild3
PATH=/tmp/rbtest/prefix/xtensa-esp-elf/bin:/tmp/xtensa-esp-elf/bin:$PATH make -j8
```

Clean output ends with:

```
wrote /tmp/rbbuild3/rockbox.esp32s3: 5 segments, entry 0x42000034
```

`/tmp/xtensa-esp-elf/bin` contains a broken Rust wrapper that panics; the real
toolchain is under `/tmp/rbtest/prefix/xtensa-esp-elf/bin/`. Both need to be on
PATH — `configure` looks for the wrapper name, `make` uses the real one.

Simulator:

```sh
cd /tmp/rbsim
../tools/configure --target=133 --ram=8 --type=S
make
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./rockboxui
```

Image inspection without a board:

```sh
make espverify
# File size 926320, entry 42000034, 5 segments, checksum valid
```

Flashing (once a board exists):

```sh
make flash                       # or: make flash ESPPORT=/dev/ttyACM0
```

`tools/espflash.make` is included from `tools/root.make` only when
`CPU == xtensa` **and** the artifact name contains `esp32` — the simulator
shares the arch but produces `rockboxui`, so the gate is on the artifact.

Disassembly:

```sh
/tmp/rbtest/prefix/xtensa-esp-elf/bin/xtensa-esp-elf-objdump -d /tmp/rbbuild3/rockbox.elf
```

`objcopy -O binary` is useless here: it spans the gap between the two flash
windows and produces a ~100 MB file of zeros. Packaging uses the ELF directly.

Note on objdump and `.startup`: it misaligns on the padding byte at
`0x4200004e`. Disassemble from the instruction boundary
(`--start-address=0x42000050`) to see the real call sequence.

---

## 3. Commit conventions

- `subsystem: short description`, then a body explaining *why*, then
  `Change-Id: I<40 hex chars>`. No `Co-Authored-By`.
- C only. No C++isms, no typedefs for structs, no code in headers, `/* C */`
  comments, four spaces, no tabs, under 80 columns (keymap tables routinely
  exceed it; the tree does too).
- Follow the style of the file you are editing. Do not invent a new arch
  dispatch idiom — use `#if defined(CPU_*)` as in `firmware/asm/`.

---

## 4. Verification discipline

There is no hardware. Every claim is checked one of four ways:

1. **ESP-IDF source.** A full ESP-IDF checkout is at `/tmp/espidf`; use
   `components/soc/esp32s3/register/soc/*_reg.h` and `*_struct.h` for bit
   positions, and `components/soc/esp32s3/include/soc/gpio_sig_map.h` for
   signal indices.
2. **Host mock.** Extract the driver body into `/tmp/<name>test/mock.c` with
   `rd`/`wr` redirected to a sparse array, then assert the programmed values.
3. **Disassembly.** Confirm the compiled function writes the addresses and
   values intended.
4. **Reference firmware.** `/tmp/meowkit` is the stock MeowKit firmware;
   `src/bsp/config.h` is the authoritative pin map.

Mock tests currently on disk:

| Directory | What it proves |
|---|---|
| `/tmp/freqtest` | clock ladder, LDO slave ordering, XTAL guard |
| `/tmp/dwctest` | real `usb_dw_check_hw()` against mock GHWCFG2/3/4; FIFO layout |
| `/tmp/i2ctest` | command-list budget (32 TX words, 32 RX words, 8 slots) |
| `/tmp/psramtest` | full octal PSRAM bring-up sequence replayed against an emulated chip |
| `/tmp/gdmatest` | GDMA OUT ring: exactly one descriptor in flight over 2000 frames |
| `/tmp/sdhosttest` | IDMAC descriptor ring: first/last/chain flags, ring closure, 128-block ring |
| `/tmp/keymaptest` | real keymap tables: stick and grid agree in 11 contexts × 4 directions; no key fires twice per tap |
| `/tmp/dbgtest` | report card produces 14 items, SRAM arithmetic matches the real link |
| `/tmp/ledctest` | LEDC timer/period/duty arithmetic, monotonic brightness |
| `/tmp/i2sdiv` | I2S clock divider: every supported rate lands within 1 ppm of 256×rate |

---

## 5. Memory map

### Internal SRAM (the binding constraint)

`app.lds`: `DRAMORIG = 0x3FC99000`, `DRAMSIZE = 0x57000` (356,352 bytes).

| Symbol | Address |
|---|---|
| `_datastart` | 0x3fc99000 |
| `_bssend` | 0x3fce10f4 |
| `_stackend` | 0x3fce2100 |
| `dramend` | 0x3fcf0000 |

Used: 299,264 bytes. **Free: 57,088 bytes.** That reserve is what hardware
bring-up will need, so do not spend it on anything speculative.

`.iram` is `IRAMORIG = 0x40378000`, `IRAMSIZE = 0x9000` (36,864). Used: 34,764
(`_iramstart` 0x40378000 → `_iramend` 0x403807cc). **Only 2,100 bytes free.**
Anything added to `.icode`/`.icode.irq` has to fit there.

The instruction bus and the data bus see the same physical SRAM at addresses
differing by `0x6F0000` (`0x40378000` ↔ `0x3FC88000`). `DRAMORIG` is the alias
of the end of the instruction region, reserved as a constant rather than
`SIZEOF(.iram)` because plugins are linked by a separate script that cannot see
where the firmware's code ends.

### PSRAM / DROM window

| Region | Address | Notes |
|---|---|---|
| `.rodata` | 0x3c000020 – 0x3c079f58 (499,512 bytes) | pages 0–7, mapped by the ROM loader |
| `.psrambss` | 0x3c080000 – 0x3c0a0000 | pages 8–9, NOLOAD, zeroed by crt0 |
| `.audiobuf` | 0x3c0a0000 – 0x3c65a820 | 6,006,816 bytes, sized from the location counter |
| `.framebuffer` | 0x3c65a820 | 153,600 bytes |
| `codecbuf` | 0x3c680020 | must equal `CODEC_ORIGIN` in `plugin.lds` |
| `pluginbuf` | 0x3c780020 | |
| `psramend` | 0x3c800020 | end of the MMU-mapped range |

`mmu_init()` maps MMU pages 8–128 (121 entries) to PSRAM physical pages 0–120.
The MMU table is 512 × 32-bit entries at `0x600C5000`; the **entry index is the
masked address** (`vaddr & 0x1FFFFFF`), not `vaddr - 0x3C000000`. Bit 15 = PSRAM
target, bit 14 = invalid, bits 0–13 = physical page.

Why `.psrambss` is aligned to 64 KB at **both** ends: the MMU maps whole pages,
the ROM loader has already mapped the page that ends `.rodata` to flash, and
`mmu_init()` skips entries that are already valid. Before this change the
audiobuffer started at `0x3c07928c`, inside page 7, so its first ~26 KB read
flash. The trailing ALIGN keeps the page numbering to `codecbuf` contiguous.

Statics moved to PSRAM via `PSRAM_BSS_ATTR` (defined in `meowkits3.h`, empty
default in `config.h`):

| Static | Size |
|---|---|
| `codepage_table` (`firmware/common/unicode.c`) | 65,538 |
| `language_buffer` (`apps/language.c`) | 29,008 |
| `logfbuffer` (`firmware/logf.c`) | 16,385 |

`cache_buffer` in `disk_cache.c` (32 KB) was deliberately **not** moved: the
SDHOST IDMAC writes straight into the buffer it is given, and the directory
cache passes `cache_buffer` as that buffer. GDMA/DWC reachability of PSRAM
through the MMU is unconfirmed, so anything DMA-touched stays in internal SRAM.

### ESP image

5 segments, entry `0x42000034`, 926,320 bytes, checksum valid:

| Seg | Length | Load address |
|---|---|---|
| 1 | 0x79f38 | 0x3c000020 (`.rodata`, DROM) |
| 2 | 0x024e8 | 0x3fc99000 (`.data`, DRAM) |
| 3 | 0x03bc8 | 0x40378000 (`.iram`, first half) |
| 4 | 0x5d638 | 0x42000020 (`.text`, IROM) |
| 5 | 0x04c04 | 0x4037bbc8 (`.iram`, second half) |

Flash-mapped segments must sit in the file at an offset congruent to their
address modulo the 64 KB MMU page size — that is why `IROMORIG`/`DROMORIG` carry
the `+0x20`. `.psrambss` is `SHT_NOBITS` and is skipped by `esp-elf2image`.

---

## 6. Entry sequence (verified by disassembly)

`crt0.S` is **not** a reset vector; the mask ROM owns the vectors and *jumps* to
the entry point. So `a0` is not a return address, `a1` is whatever the loader
left, and `.bss` is not in the image.

```
movi a2, 0; wsr a2, windowbase          /* pin WINDOW_BASE to 0 */
l32r a1, .Lstack                        /* own stack, top of internal DRAM */
zero .bss loop
call0 psram_init                        /* 0x4204b418 */
call0 mmu_init                          /* 0x4204b600 */
call0 psram_present                     /* 0x4204b3b0 */
beqz → skip .psrambss zero loop
zero .psrambss loop
call0 main                              /* 0x42007cd4 */
j 5b                                    /* spin */
```

Literals are written as `.word` in `.startup` rather than `l32r symbol`: this
binutils hits an internal error (`ldlang.c 1074`) when an `l32r` references a
linker-script-defined symbol directly.

The vector table is at `_vector_table = 0x40378400` (1024-aligned, as VECBASE
requires), and `xt_level1_handler` is at `0x40378804` — reachable by the table's
`j` (18-bit signed range). `.icode.irq` is placed immediately after the table in
`app.lds` for the same reason; `-mlongcalls` makes the C calls out of the
dispatch unlimited, but hand-written `call0` is not.

---

## 7. Interrupt slots

Sources are routed by writing the slot number to
`0x600C2000 + 4 * source_id`. Levels cannot be programmed (no
`rsr.intlevel`/`wsr.intlevel` on this core config), so a handler can only use a
slot that is already level 1: 0–10, 12, 13, 17, 18.

| Slot | Source | Source id |
|---|---|---|
| 1 | TIMERG1 T0/T1 | 53, 54 |
| 2 | GDMA OUT ch0 | 71 |
| 3 | SDHOST | 30 |
| 5 | GDMA IN ch0 | 66 |
| 8 | I2C0 | 42 |
| 9 | USB (DWC2) | 38 |

0 = WiFi MAC, 4 = WiFi baseband, 6 = core timer, 7 = software. No collisions.
All source ids verified against `intr_s3.h` — the enum has gaps, which is why
`I2C_EXT0` is 42 and not 41.

Slot 1 carries both TIMERG1 timers: timer 0 drives the kernel tick and timer 1
drives Rockbox's `timer_set()`. The group has one interrupt source per timer, so
both are routed to the same slot.

The **core timer** (`rsr.ccount`, `wsr.ccompare0`) is readable but its interrupt
cannot be armed on this core config — that is a different peripheral from
TIMERG1 and is not used.

---

## 8. Input semantics

Settled and mock-verified.

- **Stick**: up/down is the wheel axis (prev/next in lists, inc/dec in settings,
  volume in the WPS); left/right is the select axis (cancel/OK, skip on release,
  seek on hold). This matches the scroll-wheel targets (iriver H1x0/H3x0, Sansa
  Clip+).
- **Grid**: the four cardinal cells (`TOPMIDDLE`, `BOTTOMMIDDLE`, `MIDLEFT`,
  `MIDRIGHT`) are aliased to the same actions in `button_context_standard`, so
  the panel and the stick agree — including in contexts where
  `keymap-touchscreen.c` is empty (e.g. `CONTEXT_LIST`).
- **EQ / colour picker / time screens**: left/right is the inc/dec axis there.
  The touchscreen table leaves the top and bottom cells unclaimed in those
  contexts, which is why `keymap-meowkits3.c` has its own
  `stick_context_settings_lr_inc`.
- **A and B are buttons, not directions.** Both commit on **release** (like a
  scroll-wheel SELECT/MODE key). A = OK, A hold = context menu. B = main menu,
  B hold = quick screen. LEFT = cancel.
- **Invariant enforced by the mock:** no key may produce more than one action
  per tap, across 13 keys × 6 contexts. The old mapping violated this (B tap
  fired CANCEL on press and QUICKSCREEN on release).
- Grid mode is the default (`apps/settings_list.c` picks point mode only for
  `APPLICATION` and Onda targets). In the simulator the arrow keys go to the
  stick and `7 8 9 u i o j k l` address the grid cells.
- **Multi-touch**: `button-xtensa.c` ORs all reported points. `action_code_worker()`
  requires an exact match, so a two-finger combination silently returns
  `ACTION_NONE`. This is the house convention (`shanlingq1` does the same). If it
  ever annoys, the fix is to take `points[0]` in grid mode.
- `ft6x06.c` now clamps `nr_points` to `FT6x06_NUM_POINTS` — the register field
  is four bits wide and callers iterate over it.

---

## 9. USB

Rockbox's generic DWC2 driver (`firmware/drivers/usb-designware.c`) is used; the
ESP32-S3 work is target hooks only.

| Item | Value |
|---|---|
| DWC2 base | `0x60080000` |
| USB wrap | `0x60039000` |
| RTC usb_conf | `0x60008120` |
| Interrupt source | 38 → slot 9 |
| Clock enable | bit 23 of `SYSTEM_PERIP_CLK_EN0` (`0x600C0018`) |
| Endpoints | 6 total, 5 IN |
| FIFO RAM | 256 words |
| Architecture | 2 = internal DMA |
| PHY | internal full-speed (UTMI_8) |

**FIFO sizing**: RX 64, NPTX 16, PTX 40. GHWCFG4 claims five periodic FIFOs but
only four `DIEPTXF` registers exist (0x104–0x110), so PTX is chosen so
`(256 − 64 − 16) / 40 = 4` and the phantom fifth is never allocated. Total 240
of 256.

**Endpoint budget is exactly six**: EP0 + MSC bulk IN/OUT + HID interrupt IN +
CDC bulk IN/OUT. The CDC notification endpoint is optional and skipped when the
pool is exhausted. Isochronous audio needs two more, so it only fits when MSC is
inactive; the allocator then disables audio with a `logf` rather than failing.
Four IN endpoints need dedicated TX FIFOs (MSC IN, CDC bulk IN, CDC int IN, HID
IN) — exactly the four available.

**Two things the generic driver cannot do by itself:**

1. **PHY routing.** The DWC does not own the pins. `RTC_CNTL_USB_CONF_REG` bits
   20 (`SW_HW_USB_PHY_SEL`) and 19 (`SW_USB_PHY_SEL`) plus `USB_WRAP`
   `OTG_CONF` bit 18 (`PAD_ENABLE`) must be set. `phy_sel` bit 2 selects an
   *external* PHY and stays clear.
2. **VBUS.** There is no sense line on the SoC or the board. `usb-xtensa.c` runs
   a thread polling `axp173_input_status()` every 200 ms and calls
   `usb_status_event()`. Tick-based polling is not viable: an I2C transaction
   takes ~1.6 ms.

**Memory**: the DWC's DMA is an internal AHB master and the core allocator's pool
is the audiobuffer, which is PSRAM here. `USB_STATIC_ALLOC` is therefore defined
for `CONFIG_CPU == ESP32S3` in `usb_storage.c`, and the transfer buffers are
static in internal SRAM at 16 KB per direction.

`CPU_XTENSA` was added to `HAVE_CPU_CACHE_ALIGN` in `firmware/export/cpu.h`.
Cache maintenance is explicit: `USB_DW_PHYSADDR(x)` is the identity,
`NO_UNCACHED_ADDR` and `POST_DMA_FLUSH` are set, and the driver's
`COMMIT_DCACHE_RANGE`/`DISCARD_DCACHE_RANGE` resolve to the ROM trampoline in
`rom_cache.h`.

**Mode selection is runtime, not compile time.** `firmware/usb.c` enables drivers
per state: at `USB_POWERED` (bus reset seen) storage is off, HID is on, audio
follows the `usb-dac` setting; at `USB_INSERTED` (first SET_ADDRESS/SET_CONFIG/
transfer) storage turns on and HID/serial follow their settings. There is no
"MSC only" switch; the closest is charge-only audio mode.

**Latent upstream bug**: `usb_core.c`'s `hex[]` table has 16 entries indexed by a
bitmask of enabled drivers. With four drivers the id maxes at 15 — exactly the
array size. A fifth driver would overflow. Worth reporting upstream.

**If USB enumerates with inverted data**, the first thing to try is the USB wrap's
`exchg_pins` / `exchg_pins_override` — ESP32-S3 revisions have a D+/D− swap
quirk. ESP-IDF exposes it; tinyusb does not call it.

---

## 10. Pre-flash verification pass

Everything checkable without a board has now been checked. Register offsets,
bit positions, signal indices and clock arithmetic were re-verified against
ESP-IDF's esp32s3 headers. Four real defects were found and fixed:

1. **Backlight PWM** (`09479e178b`): `duty_res` is the number of resolution
   bits, not bits minus one (8 written where 9 belongs); `hpoint` was never
   written, so the PWM had no period; and bit 24 of the timer config is
   `tick_sel`, not the clock source — the low-speed clock source is
   `apb_clk_sel`, a two-bit field in `LEDC_CONF` at `+0xd0`. The driver claimed
   XTAL and silently ran on APB.
2. **I2C command-list budget** (`fbc7b3f827`): RX RAM is 32 words, one received
   byte per word; a longer read wraps the RAM and still reports OK.
3. **GDMA frame size** (`fa11295173`): a silent clamp to 4095 replaced by a
   `_Static_assert` plus `panicf()`.
4. **ft6x06 point count** (`5f8c3610ec`): four-bit field, callers iterate over
   it, array is two entries.

Also confirmed:

- GPIO matrix `func_sel` is 9 bits (a 6-bit mask truncates every signal above
  63; GPSPI is 101–103). Output slots are indexed by pin (`+0x554`), input slots
  by signal (`+0x154`). Pull-ups live in the IO MUX (`FUN_PU` bit 8), not in the
  GPIO block.
- All peripheral base addresses match `reg_base.h`: LEDC 0x60019000, I2S0
  0x6000F000, I2S1 0x6002D000, GDMA 0x6003F000, GPSPI2 0x60024000, SDHOST
  0x60028000, I2C0 0x60013000, TIMERG1 0x60020000.
- Clock gate bit positions: spi2 = 6, i2c_ext0 = 7, ledc = 11, i2s1 = 21, usb =
  23 in `perip_clk_en0`; dma = 6, sdio_host = 7 in `perip_clk_en1`.
- GDMA OUT channel offsets (conf0 0x60, conf1 0x64, int_st 0x6C, int_ena 0x70,
  int_clr 0x74, link 0x80, peri_sel 0xA8) and the IN channel at the base; OUT
  link `start` is bit 21, IN link `start` is bit 22 because of `auto_ret`.
- I2S TX/RX register offsets and every `TX_CONF1`/`TX_CONF`/`TX_TDM_CTRL` bit.
- Timer group offsets and `tnconfig` bits (`USE_XTAL` 9, `ALARM_EN` 10,
  `DIVCNT_RST` 12 write-trigger, `DIVIDER` [28:13], `AUTORELOAD` 29,
  `INCREASE` 30, `EN` 31).
- SDHOST IDMAC descriptor ring arithmetic and the two-stage clock divider: from
  XTAL (40 MHz) the target reaches 400 kHz for probe and 20 MHz for operation;
  50 MHz is unreachable, so the target must not advertise
  `SDMMC_BUS_WIDTH_50MHZ` (it does not, and only D0 is wired anyway).
- I2S clock divider: every supported rate lands within 1 ppm of 256×rate, and
  all `x`, `y`, `z` fit in their 9-bit fields.
- PSRAM MMU arithmetic: `_psrambssstart` 0x3c080000 (page 8) → `psramend`
  0x3c800020 (page 128), 121 entries, physical pages 0–120, within the 128-page
  chip.
- PSRAM timing replayed against an emulated octal chip: core clock sel 2 (160
  MHz), PSRAM div 2 (80 MHz), DIN mode 0x04924924, DIN num 0, TIMING_CALI 0x0A,
  flash restored to div 2.

---

## 11. What remains

### A. Hardware bring-up — the only real work left

Run in cascade order. Each step has a specific failure signature.

1. **crt0 entry** — ROM jumps to `0x4037802c` (in `.iram`), `a1` set by hand,
   the IROM MMU window armed, `.bss` and `.psrambss` zeroed, `WINDOW_BASE` = 0.
   crt0 prints `R I P M G` through the ROM's `ets_write_char_uart`; the ROM's own
   console is on the USB-serial in download mode, but after a flash boot it is
   UART0, which this board does not route to the USB connector — so the trace may
   not be visible on `/dev/ttyACM0` and the panel is the real indicator.
2. **PSRAM octal bring-up** at 80 MHz. `dbg_hw_info()` reports "PSRAM:
   responding / not responding". Highest-risk item: ESP-IDF reaches 80 MHz with
   a timing sweep Rockbox cannot run; we use ESP-IDF's own fallback config
   (`{din_mode 4, din_num 0, extra_dummy 2}` for core 160 MHz / module 80 MHz
   DTR), which is the configuration ESP-IDF ships when the sweep fails — so it
   is verified, not merely plausible.
3. **MMU pages 8–128 mapped**, audiobuffer reads PSRAM.
4. **Panel lights up** — PCA9557 CS/RST (init order is forced: I²C → expander →
   display), GPSPI FIFO, active-low PWM.
5. **Audio plays** — GDMA 20-bit descriptor window, `OUT_DONE` vs `OUT_EOF`,
   ROM cache trampoline.
6. **SD card mounts** — IDMAC owner bits, `INTMASK` polarity (a set bit *unmasks*),
   clock divider.
7. **USB enumerates** — PHY mux, FIFO sizing, endpoint budget at runtime,
   possibly `exchg_pins`.
8. **Buttons / touch / battery / RTC** — FT6336 calibration, AXP173 REG 0x46 PEK
   semantics.

Highest-risk assumptions, and where each now stands:

1. **ROM cache trampoline — resolved statically.** Downloaded
   `esp-rom-elfs` (`esp32s3_rev0_rom.elf`) and disassembled the real functions.
   Each published address is `l32r a9, <addr>; jx a9`, and the callee is
   `entry a1, 48` … `retw.n` reading its arguments from a2/a3 — exactly what
   `rom_cache.S` assumes. `rom_cache_internal_table_ptr` (0x3fceffc4) is a
   `.data.interface.cache` section of the ROM image, so the mask ROM initialises
   it before jumping to our image. Still untested: `rfe` after a manual
   `WINDOW_BASE` write.
2. **PSRAM timing — resolved.** `mspi_timing_tuning_configs.h` gives
   default_config_id 5 = {din_mode 4, din_num 0, extra_dummy 2} for 160 MHz core /
   80 MHz module DTR; `mspi_timing_tuning_ll.h` writes exactly those into 0xC0,
   0xC4 and 0xBC (TIMING_CALI bit 1, extra dummy bits [4:2]). Rockbox matches.
   The clock-divider formula (CLKCNT_N/H/L at 23:16, 15:8, 7:0) also matches.
   MR8 (bl=3, bt=0) is written by ESP-IDF and not by Rockbox — worth adding.
3. **Interrupt slots — resolved.** ESP-IDF's `esp_cpu_intr.c` for esp32s3 lists
   level-1 slots as 0-10, 12, 13, 17, 18. Rockbox uses 1, 2, 3, 5, 8, 9.
4. **`exchg_pins` — resolved.** MeowKit runs TinyUSB on the OTG port (MSC+CDC+HID)
   and never sets the swap, so the default polarity is right for this module.
5. **Touch calibration — resolved.** The reference firmware's `CTP_Class` uses
   `CTP_REVERS_XY=1`, `CTP_REVERS_XPOS=1`, `CTP_REVERS_YPOS=0`, i.e. display
   x = 320 − raw(0x05/0x06), display y = raw(0x03/0x04). Rockbox's
   `DEFAULT_TOUCHSCREEN_CALIBRATION` computes the same thing.

Five bugs found while doing this, all fixed:

- `rom_cache.S` never copied the ROM function's return value. A windowed callee
  returns in the caller's a(N+2), so `callx4` returns in a6; `xt_rom_call`
  returned a2, which still held the function pointer. `cache_dcache_line_size()`
  is the only caller that reads it.
- `backlight-xtensa.c` used a 0x10 LEDC channel stride and a 0x04 timer stride.
  Channels are five words wide (0x14) and timers two (0x08), so channel 7's
  writes landed on channel 5's conf1. `ledc_reg.h`: LSCH7_CONF0 = 0x8c.
- `vectors.S` claimed the window vector at 0x000 is shared with the level-1
  interrupt. It is not: window vectors run 0x000–0x17C and level 2 starts at
  0x180. The gaps were linker padding (zeros); they are now self-loop traps.
- `usb_core.c` indexed `hex[16]` with a bitmask of up to six drivers (≤ 63).
  Masked to four bits.
- **The entry point was in the IROM window.** `ets_run_flash_bootloader` in the
  mask ROM calls `Cache_Dbus_MMU_Set` and never `Cache_Ibus_MMU_Set`; only
  `ets_run_direct_boot` maps the instruction side. ESP-IDF's bootloader is
  IRAM-only for exactly this reason. `.startup` is now in `.iram`, and
  `irom-map.c` arms the IROM entries before anything in `.text` runs.
- **IROM and DROM shared MMU entry 0.** The 512 MMU entries index one shared
  32 MB linear range (`(vaddr & 0x1FFFFFF) >> 16`), so `0x42000020` and
  `0x3C000020` were the same entry and their mappings overwrote each other.
  `IROMORIG` is now `0x42900020` (entry 144), above the entries DROM and the
  codec/plugin buffers claim, and `app.lds` asserts it.

### B. Simulator interactive pass

`cd /tmp/rbsim && ./rockboxui` with a real SDL window. Walk menus, the EQ, the
colour picker, the time screen and the USB screen to confirm the keymap behaves
as the mock predicted. Headless runs boot but cannot inject key events.

### C. Upstreaming

98 commits ahead of `upstream/master`, tree clean, both builds clean. Needs the
Gerrit review flow. `tools/builds.pm` has `status => 1` for meowkits3; raise to
2 when the board boots, 3 when it is usable day to day.

### D. Small optional items

- Report the `usb_core.c` `hex[]` overflow upstream.
- Consider adding SDHOST card presence to `dbg_hw_info()` — risky, it needs a
  real transaction.
- `DEFAULT_TOUCHSCREEN_CALIBRATION` in `meowkits3.h` is inferred from the panel
  rotation (`x' = LCD_WIDTH − y`, `y' = x`). Unverified on hardware.
- The touchscreen is polled every tick; if the bus proves contended, poll every
  other tick.

---

## 12. Facts worth not re-deriving

- **Boot**: the mask ROM loader is sufficient. It reads the image header at flash
  offset 0, copies the RAM segments, maps the flash MMU windows and jumps to the
  entry point. It does **not** bring up external PSRAM — that is Rockbox's job,
  before `mmu_init()`.
- **No interrupt vector table in the usual sense.** No `wsr.excb`, no
  `wsr.intbase`, no `enableint`. `wsr.vecbase` works (`XCHAL_HAVE_VECBASE 1`).
  Level 1 is reached through the exception vectors at VECBASE+0x300 (kernel) and
  +0x340 (user), selected by PS.UM.
- **Windowed context switching is impossible** on this core config: no REGB
  option, so `mtsr`/`mfsr`/`spills`/`spillw` are invalid opcodes. Hence
  `-mabi=call0`, and hence `lib/xtensa_support` supplying the libgcc helpers
  Rockbox would otherwise need.
- **Cache maintenance is ROM-only.** No cache instructions exist on this core
  config (`probeD`, `flushD`, `lockD`, `rdtag`, `wttag`, `rinvalidate`,
  `binvalidate` are all invalid) and there is no memory-mapped cache control.
  `firmware/target/xtensa/rom_cache.S` is the trampoline; the addresses are in
  `components/esp_rom/esp32s3/ld/esp32s3.rom.ld` and do not move between ROM
  revisions. `callx4` sets CALLINC as part of the call, so nothing has to be
  arranged at startup beyond pinning `WINDOW_BASE`.
- **binutils gotcha**: an `l32r` referencing a linker-script-defined symbol
  directly triggers an internal error (`ldlang.c 1074`). Keep the literal as a
  `.word` in the same section. Also `jmp` is not a valid mnemonic on this core —
  use `j`.
- **In a linker script**, an assignment to `.` whose expression contains a bare
  absolute constant is treated as an absolute section address, and region usage
  is then computed from that address instead of the section size — a spurious
  overflow of roughly the region origin. Use `ORIGIN()`/`LENGTH()`.
- **Anything the DMA writes back must be `volatile`.** A descriptor's owner bit
  changes through a store the compiler cannot see; an empty spin loop on it gets
  folded into a single test.
- **GDMA descriptors must be in internal SRAM** (20-bit address register, window
  based at `0x3FC00000`). Buffers may be in PSRAM but need explicit cache
  maintenance. `GDMA_MAX_DESC_LEN` is 4095.
- **SDHOST completion is the descriptor owner bits, not an interrupt.** The IDMAC
  clears a descriptor's owner bit when it retires it; `RINTSTS` `DATA_OVER` only
  means the card sent data.
- **I2C is a command-list machine.** `op_code`: 6 RSTART, 1 WRITE, 3 READ, 2 STOP,
  4 END; the controller stops at STOP or END. In master mode the slave address and
  R/W bit are part of TX RAM, so `byte_num` counts them. A read is split so the
  master ACKs every byte but the last. The byte-at-a-time API
  (`i2c_start`/`i2c_outb`/`i2c_inb`) cannot be implemented at all; the target uses
  the transaction-level API and the synchronous entry points are wrappers over
  `i2c_async_queue()`.
- **The I2S module clock is XTAL (40 MHz)** when `SCLK_SEL` is 0, independent of
  `cpu_frequency`.
- **AXP173 PEK status is in REG 0x46**, not 0x44 (which is ACIN/VBUS). The status
  bits latch and are cleared by writing 1. REG 0x36 bit 3 ("auto power off when
  the press outlasts the off time") is set at reset; a bare write of only the
  on/off fields clears it and removes the hardware safety net. `power_off()` must
  command REG 0x32 bit 7 — the SoC cannot cut its own supply.
- **Backlight is active low** and GPIO 42 is routed to LEDC low-speed channel 7
  through the GPIO matrix (`LEDC_LS_SIG_OUT7_IDX = 80`). With 9-bit resolution
  and `hpoint = 512`, a duty of 512 means "always high" = off.
- **`DEFAULT_STACK_SIZE` is 0x1000** for xtensa (`firmware/asm/xtensa/thread.h`),
  larger than m68k/mips because the level-1 handler runs on the interrupted
  thread's stack.
- **Configure target**: `--target=133` (`meowkit-esp32s3`), `target_id = 126`,
  `MODEL_NUMBER 133`, output `rockbox.esp32s3`.
- **esptool 4.7.0** supports `image_info`, `write_flash`, `chip_id`, `flash_id`
  but **not** `chip_info`.

---

## 13. Reference material on disk

| Path | What it is |
|---|---|
| `/tmp/espidf` | Full ESP-IDF checkout — the authoritative register headers |
| `/tmp/meowkit` | Stock MeowKit firmware; `src/bsp/config.h` is the pin map |
| `/tmp/baremetal` | SDK-free ESP32-S3 reference, SVD-derived headers |
| `/tmp/tunecfg_s3.h` | ESP-IDF PSRAM/flash timing config tables |
| `/tmp/psram_octal.c` | ESP-IDF octal PSRAM implementation |
| `/tmp/axp_ds.pdf` / `/tmp/axp_ds.txt` | AXP173 datasheet (`pdftotext -layout`) |
| `/tmp/rbbuild3` | Working native build directory |
| `/tmp/rbsim` | Working simulator build directory |
| `/tmp/rbtest/prefix/xtensa-esp-elf` | The working toolchain |
| `/tmp/*test/` | Host mock tests (see §4) |

If a header is missing from `/tmp/espidf`, fetch it:

```sh
curl -sL https://raw.githubusercontent.com/espressif/esp-idf/v5.2.5/components/soc/esp32s3/register/soc/<name>_reg.h
```

(raw.githubusercontent has been intermittently returning 404 for these paths;
the local checkout is more reliable.)
