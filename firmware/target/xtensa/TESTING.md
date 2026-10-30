# Verification record — MeowKit ESP32-S3

Everything here was run on the board unless a row says otherwise. The point of
this file is that a claim about a port needs an observation behind it: for each
subsystem, what was exercised, what was looked at, and when. Longer write-ups
live in `notes/`; `notes/PENDING.md` §2 is the running list of closed items with
their detail.

## Reproducing the checks that need no board

```
cd build-meowkits3
make                    # firmware, codecs, plugins, packed image
make espverify          # esptool image_info: segments, load addresses, checksum
make esplayer           # arena guard: .ext_ram.bss vs the codec/plugin reservations
tools/meowkits3/esp-layout-check build-meowkits3/rockbox.map
```

The layout guard is the one that can fail a build rather than a boot: Rockbox
pins `codecbuf` and `pluginbuf` as absolute symbols, and a loaded image whose
`load_addr` is not exactly those addresses is rejected at load time with a
message that names neither the header nor the window.

## On a host, without hardware

The simulator is the point of review for a port, so it builds:

```sh
mkdir build-sim-meowkits3 && cd build-sim-meowkits3
../tools/configure --target=meowkits3 --type=s
make
./rockboxui --root /path/to/simdisk
```

| Build | Result | Date |
|---|---|---|
| simulator | `rockboxui` 7,645,576 B; alive under `SDL_VIDEODRIVER=dummy` + `SDL_AUDIODRIVER=dummy` until killed at 8 s; root dir, SDL 2.32.10 and the audio driver list initialised | 2026-10-30 |
| checkwps (`--type=c`) | 195,016 B | 2026-10-30 |
| database tool (`--type=d`) | 949,576 B | 2026-10-30 |
| control | fresh `--target=ondavx747 --type=s` builds clean with host gcc, which is what proved the earlier sim failures were this port's and not the sim's | 2026-10-30 |

**Not verified:** what the sim looks like. Keys responding, the stick mapping to
`BUTTON_UP/DOWN/LEFT/RIGHT`, the 3 by 3 touch grid and the 320×240 window all need a
desktop session; a dummy-video run proves the UI loop is reached, not that it renders.

The tool builds also surfaced a core defect worth knowing about: `firmware/logf.c`
kept `logfbuffer`, `logfindex` and `MAX_LOGF_SIZE` behind `#ifndef __PCTOOL__` but used
them unguarded in `logf_panic_dump()`, so any target enabling logf fails a checkwps or
database build. Fixed there rather than by turning logf off here.

## Hardware

| Subsystem | Exercised | Evidence | Date |
|---|---|---|---|
| Boot, flash layout | image at `0x10000` behind IDF's stage-2 bootloader; whole-package image at `0x0` | `make espverify` reports 7 segments, checksum and validation hash valid. Flashing needs download mode (hold BOOT, press RESET) because the USB-CDC port has no DTR/RTS on the boot straps, which is why `sdkconfig.defaults` sets `--before no_reset`; after RESET the host enumerates `1234:4002 Rockbox media player` with `ttyACM0` on its CDC interface, which proves the firmware is running and its composite descriptor is up. Re-confirmed on 2026-10-30 after the Phase B tree restructure. The main menu itself is a screen observation, not something the host can see | 2026-10-30 |
| Panel | ST7789 320x240, backlight PWM, `lcd_enable`/`lcd_sleep`/`lcd_shutdown` | on-glass pixel contract measured against the reference driver; sleep and wake push the framebuffer in full | 2026-10-27 |
| Audio out | WAV, FLAC, MP3, OGG, ALAC, WMA, APE, speex, AAC at 44.1 kHz | audible through speaker and headphone; codec soak run over the full shipped codec set | 2026-10-28 |
| High-rate PCM | 176.4 kHz / 24-bit WAV | plays at correct pitch, DSP resampler path engaged | 2026-10-29 |
| DSD | DSD64 DSF, 145,283,752 B file with embedded art | plays, album art decodes from the DSF metadata | 2026-10-29 |
| DSD ceiling | DSD128 | closed as a CPU-bound ceiling, not a bug: the decoder cannot keep the sink fed | 2026-10-29 |
| Recording | mic at 44.1/48/88.2/96 kHz | WAVs read off the card: 96 kHz clip is 96,000 Hz / 16-bit / stereo with **1.3% of its energy above 22 kHz**, which a 44.1 kHz capture cannot produce | 2026-10-29 |
| Mic gain | ES7210 two-stage PGA at 0 dB and 30 dB | measured span after the REG41 bias byte and `SPEED_MODE` fixes; 0 dB reference RMS -56 dBFS, 30 dB -23.9 dBFS | 2026-10-27 |
| Bus isolation | capture while playback is idle, and the reverse | TX and RX are exclusive on this port; the switch is explicit and `pitch_detector` sees signal | 2026-10-27 |
| Speaker amp | `Settings -> Speaker mode` | amplifier enable is PCA9557 IO3 and follows the setting; amp is switched off when nothing plays | 2026-10-26 |
| USB mass storage | insert, hold a button, copy both directions | host sees the card; writes verified by reading them back off the card afterwards | 2026-10-24 |
| USB audio (DAC) | UAC1 at 48 kHz | enumerates and plays; 48 kHz is the FIFO ceiling (`GHWCFG3` reads 200 words, not the 256 TinyUSB assumes), not a software limit | 2026-10-25 |
| USB serial console | CDC, idle and wake | console survives suspend/resume; it is CDC, not USB-Serial-JTAG, so the pads handover is explicit | 2026-10-24 |
| USB HID | keypad profile | wired; the keypad lives on the USB screen only | 2026-10-25 |
| SD card | 1-bit SDMMC, hot-plug absent by design | directory browsing, seek-heavy playback, tagcache build | 2026-10-26 |
| Charging | AXP173, USB-C | VBUS reported, charge current budget respected; coulometer scale measured rather than taken from the datasheet | 2026-10-29 |
| Battery percentage | coulometer-backed `battery_level()` | cumulative mAh in `battery_bench`, direction-change markers verified | 2026-10-29 |
| Idle current | dark screen, nothing playing | 42 mA floor after the amplifier policy and DFS; overnight dark-idle run measured | 2026-10-27 |
| Light sleep | sleep enabled, wake sources | MSPI/MMU table and the GDMA TX row survive sleep/wake | 2026-10-25 |
| Power off | held PEK | long press queues `SYS_POWEROFF`; flush, settings save and card close run before the PMU cuts | 2026-10-27 |
| Panic path | deliberate faults | crash record survives the reset in RTC slow RAM and prints over CDC | 2026-10-26 |
| Arena | 4,325,376 B in the PSRAM window | `esp-layout-check` passes; Doom run at worst case (largest WAD, full sound) without an OOM | 2026-10-29 |
| Codecs and plugins | full shipped set | load addresses match `codecbuf`/`pluginbuf` in the linked image, verified from the ELF program headers as well as on hardware | 2026-10-30 |

## Plugin input

Every plugin keymap in the tree was compiled for this pad in a synthetic
harness: 3,516 (target, touchscreen) combinations across 32 plugin files. That
is what caught the branch in `apps/plugins/solitaire.c` that had MeowKit keymap
text inside the Sony NWZ branch — a syntax error that only manifests when a
*different* target compiles that file, and therefore invisible to every build of
this target. `apps/plugins/test_touchscreen.c` gained the mapping this file used
to be missing.

## Not verified on hardware

- Codecs and plugins executing out of PSRAM: they link there and load there, and
  playback works, but the instruction cache only covers the IROM window. This is
  a hardware question that has not been settled by measurement.
- The full charge leg from ~30% into the taper, and a discharge run to shutdown.
  The runtime figures quoted in the README come from partial runs and a
  datasheet capacity, so they are estimates.
- USB on a Windows host, and on every host OS's driver stack beyond the Linux
  ones used here.
- Anything that needs a second board: no multi-unit, no long-term reliability run.

## What would change a conclusion

Rows above that say "measured" name the measurement. If a later build contradicts
one, the observation wins and this file is wrong — say so in `notes/PENDING.md`
rather than quietly updating the claim.
