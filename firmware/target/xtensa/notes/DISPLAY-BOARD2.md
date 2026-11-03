# Second board: panel dark on a power-on boot, content on a host-triggered boot

Two MeowKit units, same design and batch. Unit 1 shows the logo and menu on
every boot. Unit 2 shows backlight only after a RESET-button or power-on boot,
and shows the menu after a boot initiated by the host attaching to
USB-Serial-JTAG (`reset reason: 11 (usb peripheral)`). The second board's
touch panel also worked on the stock firmware, so the unit is not broken.

This note records what has been eliminated **by measurement**, what has not,
and the order in which the remaining experiments should be run. Nothing here is
a theory that has been written into a file as if it were established; the last
time that happened it cost a whole session (see the corrected JTAG claim in
`AGENTS.md`).

## Eliminated, with the evidence

| Candidate | How it was eliminated |
|---|---|
| The firmware image differs | Unit 1's bootloader (`0x0`), partition table (`0x8000`), otadata (`0xe000`) and app (`0x10000`) were read out of its flash with `read_flash` and written verbatim to unit 2, all four `Hash of data verified`. Still dark. Unit 1's stamp was `v4.0-final-2771-g32889435c3-dir`, five commits behind HEAD and built from a dirty tree; unit 2 had only ever received clean builds. |
| Something we set during development on unit 1 and never on unit 2 | No NVS usage anywhere in the target. Zero matches in the tree for `esp_efuse`, `efuse_write`, `burn_security`, `secure_boot`, `flash_encryption` (counted, not grepped through a pipeline that hides the status). sdkconfig's only `SECURE_BOOT` lines are `CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED` and `_PREFERRED`, which are capability declarations. Neither release could alter a chip. |
| eFuse state | `esp_efuse_read_field_blob` on both units: `DIS_PAD_JTAG=0`, `STRAP_JTAG_SEL=0`, `DIS_USB_JTAG=0`, `SOFT_DIS_JTAG=0`. Identical, and all four clear. |
| JTAG ownership of pads 39/40/41 | `USB_SERIAL_JTAG.conf0` reads `0x00004200` on both -- bits 9 (`dp_pullup`) and 14 (`usb_pad_enable`), with bit 16 (`usb_jtag_bridge_en`) clear. Bit 16 is not a pad mux, contrary to an earlier claim in this tree. Hand-toggle test: pads 39/40/41 driven high and low as plain GPIO on both units. Post-`spi_init()` both read `mcu=1`, `func=256/103/101`, `oen=1`, output enable set. |
| Persistent peripheral state | Single-byte inventories of AXP173 `0x00-0x47`, PCA9557 `0x00-0x03`, PCF8563 `0x00-0x0B`, ES8311 `0x00-0x1F`, ES7210 `0x00-0x11` and `0x40-0x47` on both units. PCA9557 and ES7210 byte-identical; AXP173 differs in two charge-status bytes; PCF8563 differs in time-of-day fields; ES8311 differs in mic/ADC-side bytes because unit 1 has run a recording session. `out=0x0f`, `ldo4=0x68`, `cfg=0xb0`, `latch=0x41` on both. |
| SPI clock tolerance | Running the bus at 20 MHz instead of 80 MHz changed nothing: the host-triggered boot still showed content, the button boot still did not. |
| Protocol or sequence | `lcd-meowkits3.c` sends `CASET`, `RASET` then `RAMWR` before pixel data, and `st7789_init()` runs the documented order: CS deselect, reset asserted 20 ms, released, 120 ms recovery, `SLPOUT` + 130 ms, the bias/gamma block, `COLMOD 0x55`, `MADCTL 0xA8`, `INVON`, `DISPON`. Unit 1 renders correctly with exactly this. |
| Panel supply or reset | The expander latch reads `0x41` on both, which mirrors the actual pin level, so LCD reset is released and CS is asserted in hardware, not merely in the output register. Backlight is on, which requires the rails and LEDC to be working. |

## Measured since: the clock pad does toggle

A slow-clock probe (~78 kHz from the 80 MHz PLL, `FUN_IE` open for the whole
transfer, clock restored afterwards) counted **128 level changes on GPIO41 for an
8-byte transfer** -- exactly the 64 clock periods expected, rising and falling.
The same value was recorded on a dark power-on boot through the retained trail,
and again on a host-triggered boot:

    ppbe a=00000080 b=00000f37      (edges=128, from the dark boot's trail)
    padprobe: edges=128 spins=3895  (this boot)

So the controller drives the clock pad, on the boot that shows nothing. Every
register-level explanation is now excluded, and the fault is downstream of the
pad. Drive strength is *not* the difference: IDF's SPI driver never calls
`gpio_set_drive_capability`, so the stock firmware uses the same pad default we
do -- that API is only used by SDIO-slave and the MSPI flash-pad tuning.

The probe has since been extended to count the MOSI pad (GPIO40) as well,
because "clock moves, data does not" produces exactly the observed symptom.

## The pad probe was too coarse, and two more theories are dead

The probe's own resolution check reads `per=2..23` -- as few as two loop
iterations between observed clock edges. The pad input goes through a two-stage
APB synchroniser, so the loop follows the synchroniser rather than the pin and
cannot resolve every bit period. Therefore `sclk=128, mosi=56` does **not**
mean the data line is losing edges; the shortfall is the instrument. Do not
build on that number.

`usb_jtag_bridge_en` (conf0 bit 16) was written and verified -- the trail shows
`ppcf a=00004200 b=00014200` -- and produced no change in the MOSI count and no
picture. The MTDO-contention theory is dead.

What the dark boot's retained trail does say: 40 entries, all SD read/write
records, i.e. that boot mounted the card and did normal storage work. It is
not hanging before the display, not crashing, and not skipping init. The app is
healthy and the glass is dark.

Five candidate explanations have now come back identical: image bytes, eFuse
fields, peripheral register inventories, SPI clock rate, and JTAG pad ownership.
The next thing with a chance of answering this is not a register read at all --
it is either a probe on the panel flex itself, or the vendor firmware's actual
SPI byte stream diffed against ours. Further flashing cycles are not producing
information.

## Resolved: the boot-time init was aimed at devices that were not listening yet

A third board, never flashed by this port, was captured in full first
(`tools/meowkits3/rb-board-preserve`, artifacts under `$HOME/forensics/`). Its
shipped firmware cold-boots with a clean UI, working touch and a normal battery
reading. The HEAD firmware on that same virgin unit showed a flicker and then
black.

The change that fixed it was not a change to the init sequence at all. A probe
that re-ran `st7789_init()` five seconds after boot made the panel come up, which
identified the boot-time init as the thing that failed rather than anything about
timing of pixels, byte order or the controller rules. The fix is a one-second
settle delay between raising the PMU rails and addressing the PCA9557 and the
panel, in `lcd_init_device()`.

Why this survived two releases and every board-1 observation: a chip reset does
not reset the panel. Its bias, gamma, `COLMOD`, `MADCTL`, `INVON` and `DISPON`
survive, so on any warm boot the previous run's configuration is still in force
and only the pixel writes matter. A cold power-on is the only moment our init has
to actually work, and it did not. Reads back clean either way -- every register,
the expander latch, the transfer counts -- because writing to a device whose
power-on ramp has not finished produces no error.

The delay is unconditional on purpose. Keying it on the expander's configuration
register looks attractive, since a cold PCA9557 resets to all-inputs while a warm
one still carries our setup, but a unit that ran the vendor firmware first has that
register already programmed and is exactly the cold case that fails.

Board 2 is a separate matter. Under the vendor firmware it glitches on display
during boot and has lost touch again; the panel reset, panel chip select and touch
reset are all outputs of the same PCA9557, so two subsystems on one net misbehaving
with firmware we did not write makes it a suspect unit. It was not a valid
comparison subject, and the conclusion drawn from it -- that the vendor firmware
succeeds where ours fails -- was measured on hardware that is itself unwell.

## Replacing the hand-rolled SPI path with esp_lcd fixed nothing

`st7789.c` was rewritten on ESP-IDF's `esp_lcd` -- `esp_lcd_new_panel_io_spi`
with `cs_gpio_num = -1` (CS is a PCA9557 output) and `dc_gpio_num = 39`, the
ST7789 device with BGR element order, little-endian data and 16 bpp,
`swap_xy`/`mirror_y` reproducing the measured `MADCTL = 0xA8`, the bias/gamma
block pushed through `esp_lcd_panel_io_tx_param`, and pixels passed to
`esp_lcd_panel_draw_bitmap` verbatim because `RAMCTRL` carries the byte order.
`spi-esp32s3.c` was deleted. It needed `esp_lcd` in the app's `PRIV_REQUIRES`,
an `idf.py reconfigure` and a flag re-harvest, and it needed
`CONFIG_SPI_MASTER_ISR_IN_IRAM=n` because that default pulls the GDMA ISR into
IRAM, and IRAM aliases DRAM at `0x6F0000`, so 4 KiB of instruction space is
4 KiB taken from the DRAM region this target does not have.

On the known-good board that build made the display stop working. It was
reverted and the board restored byte-for-byte from images read out of its flash
earlier -- bootloader `75f40597`, partition table `9daffb12`, otadata `dde79571`,
app `07f4b766` -- and verified by reading those regions back.

So the reverse-engineered SPI programming is not the fault. That removes the
last subsystem we could plausibly have written wrong, and it means the two
candidate explanations left are: something in the pad or clock configuration
that esp_lcd shares with our driver and that behaves differently after a cold
power-on, or something in the expander-driven CS/reset timing that a cold panel
requires and a warm one does not.

## What has NOT been measured validly

Whether the clock pad physically toggles during a real transfer. Every attempt
so far read `sclk_edges=0` **including on boots that rendered a correct
picture**, which means the instrument was wrong rather than the hardware. Two
reasons, both understood now: the pad's input path (`FUN_IE` in the IO MUX) was
not enabled while the transfers ran, so `GPIO_IN` could not see the pad at all,
and even with it enabled the input goes through a two-stage APB synchroniser
that cannot resolve an 80 MHz clock. A valid measurement needs the input enabled
for the whole transfer and a divider slow enough to sample -- tens of kHz, not
20 MHz -- with the normal rate restored afterwards.

The other unmeasured thing is unit 1's behaviour under the conditions that make
unit 2 dark. Every comparison so far has been unit 2's working boot against
unit 2's dark boot, which is why everything read identical.

## Experiment ladder, cheapest first

1. **Unit 2 on battery, no cable, press RESET.** No flashing. If the panel shows
   content with nothing attached to USB, then the discriminator is host
   attachment rather than reset type, and the suspect moves to USB pad
   arbitration (`USB_WRAP.pad_enable`, bit 18) and what `usb_init_device()` does
   to the RTC-domain pad mux. If it stays dark, the trigger story collapses and
   the panel itself is the suspect.
2. **Toggle CS around every command**, rather than holding it asserted via the
   expander for the whole boot. Cheap, and it removes "this panel wants a CS
   edge between commands" as an unknown. Costs an I2C transaction per command
   (~1.6 ms), which is acceptable for a test build only.
3. **Cycle the rail that feeds the panel** through the AXP (off, brief delay,
   on, then re-init) so the panel starts from a known power-on state instead of
   whatever the previous boot left in it. This is the only way to reset the
   panel's own registers from software without a working reset line.
4. **The valid edge counter** described above: slow divider, `FUN_IE` open,
   count transitions through a test transfer, restore 80 MHz. This distinguishes
   "the SoC drives the pad" from "the controller completed a transfer that never
   reached the pin", which is the last binary unknown.
5. **Reproduce the vendor init byte-for-byte** from a captured stock-firmware
   trace, if one can be obtained, since the stock firmware is known to work on
   unit 2. That removes every remaining difference in our sequence at once.

## Instrument lessons, so they are not repeated

- `i2c_readmem()` for multiple bytes returns the first byte and `0xFF` for the
  rest on this bus (PMU, expander, both codecs). Register inventories need one
  transaction per register.
- A GPIO readback needs `FUN_IE` set on the pad, and cannot resolve fast clocks.
- Never judge a command by the exit status of a pipeline ending in `grep`,
  `tail` or `head`. Twice in this investigation a failed lookup was read as a
  negative finding, and one of those produced a confident but unsupported
  statement about eFuses.
- `pkill -f` matches its own command line and kills the shell running it.
- A flash sets `FORCE_DOWNLOAD_BOOT`, which survives resets; with USB attach
  suppressed nothing clears it, so the board looks unbootable until a genuine
  power-on.
