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
