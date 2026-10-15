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
