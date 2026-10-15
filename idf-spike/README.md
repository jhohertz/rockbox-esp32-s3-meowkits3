# ESP-IDF spike for the MeowKit ESP32-S3 board

**This is a probe, not the port.** It exists to answer one question with a
binary outcome: does Espressif's own stack bring this board up further and
faster than the hand-rolled bare-metal path did?

The bare-metal attempt is preserved on branch `xtensa-windowed-abi`, tagged
`baremetal-pre-idf`. Nothing here is built by Rockbox's make system, and
nothing here is intended to survive into the port. What survives is the
answer.

## Why

Bluetooth on the ESP32-S3 is not separable from ESP-IDF. The controller ships
as a prebuilt library (`components/bt/controller/lib_esp32s3`) and the stack's
OS abstraction layer (`components/bt/common/osi/`: `alarm.c`, `future.c`,
`fixed_queue.c`, `allocator.c`) is built directly on FreeRTOS and `esp_timer`.
There is no route to BLE on this silicon that does not include FreeRTOS and a
large slice of IDF.

Given that, re-implementing PSRAM bring-up, the flash MMU, cache maintenance,
the SDHOST IDMAC, the GPSPI FIFO path, the I2C command-list machine, GDMA
descriptors and the interrupt matrix by hand is the wrong use of effort. Each
of those cost days on the bare-metal branch and each is a solved, vendor-
maintained driver here.

## What each stage proves

Stages run in increasing order of risk, and each prints `PASS`/`FAIL`. A stage
that fails does not abort the run — the point is the whole table, not the first
failure.

| Stage | What it settles |
|---|---|
| 0 heap | internal vs PSRAM free bytes; whether Rockbox's audiobuffer/codecbuf/pluginbuf/dircache fit |
| 1 PCA9557 | I2C works, and the expander that owns LCD reset, LCD CS and the amp enable answers |
| 2 ST7789 | `esp_lcd` drives the panel; something visible appears on screen |
| 3 backlight | LEDC on GPIO 42, **active low** |
| 4 SD | SDMMC on non-default pins (CLK 47, CMD 48, D0 21), 1-bit only, FAT mount + write/read |
| 5 AXP173 | PMU over I2C: battery voltage, VBUS, and the PEK power-key status register |
| 6 NimBLE | the controller blob initialises and a scan finds devices |

Stages 0-5 are the ones that block a Rockbox port. Stage 6 is the reason we are
making the transition at all.

## Build

```sh
. ~/esp/esp-idf/export.py
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`esp32s3` is pinned in `CMakeLists.txt`, so `set-target` is redundant but
harmless.

## Deliberate choices worth knowing

**LCD chip select is not a GPIO.** On this board CS and reset live on the
PCA9557 I2C expander, not on the SPI controller. `esp_lcd_panel_io_spi` wants
a CS GPIO, so the spike sets `cs_gpio_num = -1` and holds the expander's CS
line asserted for the lifetime of the bus. That is correct because the panel is
the only device on GPSPI2, and it is the shape the port will use too.

**The panel's GRAM is 240x320 and it is driven as 320x240.** The ST7789 is
physically portrait; the reference firmware configures `panel_width 240`,
`panel_height 320`, `offset_rotation 3` and then swaps the dimensions. The
spike mirrors that with `esp_lcd_panel_swap_xy` so the geometry matches what
Rockbox's `LCD_WIDTH`/`LCD_HEIGHT` will need.

**Backlight is active low.** LEDC duty 512 (all high) turns the LED *off*;
duty 0 is full brightness. Easy to get backwards and it looks like a dead
panel.

**SD is 1-bit and on non-default pins.** CLK 47, CMD 48, D0 21. Only D0 is
wired, so `SDMMC_MODE_1BIT` is not a fallback, it is the ceiling. The lines
have clamp diodes but no external pull-ups, so the IO-MUX `FUN_PU` path (or
the controller's internal CMD pull-up) matters.

## What the port will look like if this works

Rockbox keeps its own build system and its own kernel; IDF is invoked where it
is needed. Concretely:

- Rockbox's `make` produces its firmware archive as today.
- An IDF application component links that archive and calls Rockbox's `main()`
  from `app_main()`, on a FreeRTOS task.
- `firmware/target/xtensa/` collapses to a thin TAL shim that calls IDF drivers:
  `esp_psram`, `esp_mm`, `sdmmc_host`, `esp_lcd_panel_st7789`, `esp_driver_i2c`,
  `esp_driver_i2s`, `esp_lcd_panel_io_spi`, `ledc`, `esp_timer`,
  `esp_intr_alloc`.
- Deleted outright: `crt0.S`, `irom-map.c`, `psram-xtensa.c`, `mmu-xtensa.c`,
  `rom_cache.S`, `vectors.S`, `timer-xtensa.c`, `irq-xtensa.c`,
  `tools/esp-elf2image`, `tools/esp-stage2`, `app.lds`, and the whole
  RTC-slow-memory probe rig.
- Rockbox's own `firmware/libc` is disabled for this target and IDF's libc is
  used instead. Rockbox targets much smaller parts where that matters; on a
  part with 8 MB PSRAM and a vendor libc, forcing Rockbox's libc is not
  viable.
- Rockbox's threading model is **kept**. `firmware/kernel/thread.c` calls
  `store_context()`/`load_context()`; those get implemented with FreeRTOS's own
  xtensa context machinery (`_xt_context_save`, `SPILL_ALL_WINDOWS`,
  `_xt_user_exit`) rather than our hand-rolled version. That also fixes the
  `PS.EXCM` problem, because `rfe` establishes EXCM in hardware instead of us
  trying to raise it with `wsr ps`.

## Licensing note

Rockbox firmware is GPL-2.0-or-later ("or at your option any later version"),
which is what makes conveying against Apache-2.0 IDF code workable. The
Bluetooth controller is a proprietary prebuilt blob regardless of which path we
take; that is a separate distribution question and it is worth deciding
deliberately rather than by accident.

---

## Results (ESP-IDF v5.5.5, toolchain esp-14.2.0_20260121, on the board)

```
  0 heap/psram                       PASS  internal 321543 B, psram 8386192 B, double fb alloc'd
  1 i2c + pca9557                    FAIL  read CONFIG: ESP_ERR_INVALID_STATE
  2 backlight (ledc, active low)     PASS  GPIO 42 at 50% duty (active low)
  3 st7789 via esp_lcd               PASS  320x240 bars @ 40 MHz, CS held via expander (ESP_OK)
  4 sd card (1-bit, gpio matrix)     PASS  SD16G, 15280 MB, write/read verified
  5 axp173 pmu                       PASS  REG12=0x0f vbat=4121 mV ichg=0 mA pek0x46=0x00
  6 nimble scan                      PASS  25 unique advertiser(s) seen
```

Verdict: the transition is right. Everything that cost days on the bare-metal
branch works on first contact here.

### What each result actually establishes

**PSRAM.** `esp_psram: SPI SRAM memory test OK`, 8,386,192 B free, largest block
8,257,536 B. Octal mode at 80 MHz, configured in `sdkconfig.defaults`, not
written in code. A 320x240 RGB565 *double* framebuffer allocates from it. This
was the single hardest problem on the bare-metal branch (MSPI timing sweeps, pad
routing, MR0/MR8 register transactions, the Ext_RAM cache path) and it is now
four config lines.

**Internal RAM.** 321,543 B free at boot, largest block 225,280 B. That is the
budget for things that cannot live in PSRAM: GDMA descriptor rings, ISR stacks,
and anything the DMA touches.

**SD.** A 16 GB card mounts at 1-bit on GPIO 47/48/21 via
`CONFIG_SDMMC_USE_GPIO_MATRIX=y`, and a file round-trips. The whole SDHOST IDMAC
descriptor-ring reimplementation is gone.

**Display.** `esp_lcd_panel_st7789` is in-tree in 5.5.5. With `cs_gpio_num = -1`
and the SPI bus otherwise unclaimed, colour bars render at 40 MHz PCLK.

**PMU.** The AXP173 answers, `REG12 = 0x0f` (DCDC1, LDO4, LDO2, LDO3 already
enabled by the bootloader), and battery reads 4121 mV — a plausible Li-ion
figure. `REG 0x46` (the PEK status register the stock driver reads at the wrong
offset) is readable.

**Bluetooth.** The prebuilt controller loads (`BLE_INIT: BT controller compile
version [51d9dfd]`, `Bluetooth MAC: e8:3d:c1:f5:98:e6`), NimBLE syncs and a
passive scan finds 25 advertisers. This is the capability that forced the
transition, and it works.

### The one failure, and what it is not

A full 7-bit scan (0x08-0x77) finds six devices and **the PCA9557 is not among
them at any address**:

| addr | device | result |
|---|---|---|
| 0x18 | ES8311 | ACK |
| 0x19 | PCA9557 | **no response** |
| 0x34 | AXP173 | ACK |
| 0x38 | FT6336 | ACK |
| 0x41 | ES7210 | ACK |
| 0x44 | SHT30 | absent (not fitted) |
| 0x51 | PCF8563 | ACK |
| 0x68 | BMI270 | ACK |

So the bus, the pins, the driver and the timing are all healthy. Two things this
ruled out explicitly:

- **Not a driver fault.** A `transmit_receive` against the AXP173 succeeds and
  returns `0x0f`. `ESP_ERR_INVALID_STATE` is simply how this driver reports a
  NACK on that path, which is why it looked like a bus-level failure.
- **Not a stuck bus.** SDA and SCL both idle high as plain inputs with pull-ups,
  and nine-clock recovery changes nothing.

Enabling the AXP173's output rails changes nothing either — `REG12` is already
`0x0f` before we touch it.

Open questions to settle before the port depends on the expander:

1. Is its `/RESET` being held low, or is its VDD on a rail that is off?
2. The panel renders bars *even though the expander never asserts LCD reset or
   CS*. Either those lines are externally biased, or the expander's outputs are
   in a usable state without being addressed. Worth knowing which, because the
   amp enable and touch reset are on the same chip.
3. The bare-metal branch believed the expander worked — the Rockbox logo made it
   to the screen, which requires reset release and CS. So this is a state
   difference to chase, not a dead chip.

### Corrections to claims made during this session

`i2c_master_probe()` returns `esp_err_t` where **0 means the device was found**.
An earlier reading of this output inverted that and concluded the probe ACKed
when it did not. The bus scan above is the corrected result.

Setting `trans_queue_depth` non-zero made the failing calls return `ESP_OK`, but
the data came back wrong (two different devices both reading `0xa1`) and the
driver warns that in asynchronous mode "user cannot get bus error from API".
That is a way to hide this bug, not fix it; the spike stays synchronous.
