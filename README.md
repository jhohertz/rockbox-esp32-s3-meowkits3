# Rockbox on the MeowKit ESP32-S3

Rockbox, running on a [MeowKit](https://meowkit.cc) — a credit-card-sized ESP32-S3
board with a 320×240 touchscreen, an audio codec, a microphone, an SD card slot and
a lithium battery. This branch adds an Xtensa architecture port to Rockbox and a
board port for that machine.

It plays music. It has themes, fonts, voice prompts, album art, a recorder, USB
mass storage, USB DAC mode, plugins and games, and a 286-page manual that describes
this device specifically.

---

## 1. What this repository is

This is a **working fork, not an official release channel**. The branch
[`esp32-s3-idf`](https://gitea.max.optiplexus.org/jhohertz/rockbox) carries the port
while it gets shaped, and the intention is for it to be **merged upstream** into
[Rockbox](https://www.rockbox.org). Until then, treat this repo as temporary: expect
rebases, expect the commit history to move, and expect the file layout to follow
upstream conventions rather than invent its own.

Two consequences of that intention:

- **Core behaviour is untouched.** Everything target-specific lives under
  `firmware/target/xtensa/` and `firmware/export/config/`, with the arch's
  register and layout constants in `firmware/export/xtensa.h` alongside every
  other CPU header. Where core code needed a hook, the hook is a generic seam a
  target opts into (`HAVE_DBG_BREADCRUMB`, `HAVE_PANIC_CONSOLE`, a weak default)
  that preserves upstream behaviour for every other target. There are no target
  hacks pretending to be platform policy.
- **This was not a sanctioned port to begin with.** Nothing here touches `rbutil`,
  the theme server or the build farm, because those depend on infrastructure that
  only exists on the other side of acceptance. What *is* maintained is the hygiene
  acceptance would require: a target id in `tools/configure`, an entry in
  `tools/builds.pm` (which is what `tools/list_targets.pl` reads), a config header, a platform
  manual chapter, and a simulator config that keeps building.

If you want to see where things stand, `firmware/target/xtensa/notes/PENDING.md` is the
single list: open items, things verified on hardware, things deferred permanently,
and the core fixes worth proposing upstream.

Two other documents live in the target directory: `firmware/target/xtensa/README.md`
explains the code -- the two compilation domains, what the platform owns, and the
rules an edit to this target has to follow -- and `firmware/target/xtensa/TESTING.md`
records how each subsystem was checked on hardware, and what has *not* been checked.
The rest of that directory (`notes/`) is the port's own working record.

---

## 2. Two things the hardware cannot do

Worth being blunt about, because both were on the original wish list.

### No Bluetooth audio

The plan was wireless headphones: A2DP for the audio, AVRCP for the controls.
**The ESP32-S3 cannot do it.** Its radio is Bluetooth Low Energy only — there is no
BR/EDR baseband, which is what A2DP needs. Verified against ESP-IDF v5.5.5 rather
than assumed:

- `SOC_BT_CLASSIC_SUPPORTED` exists in exactly one SoC header, `esp32`'s.
- `BT_CLASSIC_ENABLED` cannot be selected on esp32s3, and Espressif's own classic
  Bluetooth tests list `esp32|esp32`.
- LE Audio would need BLE 5.2 isochronous channels (CIS) and an LC3 codec. Neither
  the capability nor the stack is present.

So Bluetooth audio is closed as a hardware limit, not a software backlog item. The
sink design that came out of the investigation
([`firmware/target/xtensa/notes/BT-AUDIO.md`](firmware/target/xtensa/notes/BT-AUDIO.md)) is still
kept, because it is a general Rockbox PCM-sink design and transfers to any future
classic-capable co-processor.

### Mono output

The MeowKit is very close to the perfect Rockbox form factor: touch screen, real
buttons, SD card, battery and PMU, RTC, headphone jack, microphone, USB-C. But the
**ES8311 is a single-channel codec**, so both the headphone jack and the 1 W speaker
are mono. Everything you hear here is mono, including stereo source files, which are
summed. There is no stereo path to fix — it is not a software limitation and not a
wiring mistake; the DAC has one channel and the headphone jack is taken straight off
it.

Other limits, briefly, so nobody discovers them by surprise:

| | |
|---|---|
| Headphone detection | The jack has no detect signal. "Speaker mode" switches the class-D amp, and that is the only lever. |
| SD card | 1-bit SDMMC at 20 MHz. Fine for playback; large playlist sorts are noticeably slow. |
| App partition | 1.5 MB (ESP-IDF's single-app-large). The firmware fits with ~16 % to spare; everything else belongs on the card. |
| Plugin and codec code | Runs from PSRAM. Works, but PSRAM is slower than internal RAM and this is a hardware characteristic of the port. |
| Video | `mpegplayer` plays MPEG-2 Program-Stream files at 320×240. Frame rate is a property of the encode, not a knob: a 25 fps clip asks 7,500 macroblocks a second of the software IDCT, the recipe's 15 fps clip asks 4,500. Needs ~2.44 MB of free arena to start at all. |
| Hardware EQ / DRC | The ES8311 exposes none, so bass and treble are Rockbox's software tone controls. |
| IR receiver, WS2812B LED, IMU | Present on the board, unused by Rockbox. |

---

## 3. What works

Everything below was tested on the device, not just compiled.

| Area | Status |
|---|---|
| **Display** | ST7789 320×240, RGB565, partial updates, backlight with fade in/out, LCD sleep and shutdown states |
| **Touch** | FT6336 capacitive, calibrated to the panel, used across menus, WPS, themes and plugins |
| **Buttons** | 5-way joystick + A + B, 80 ms debounce, plus the AXP173 power-key (long press = clean shutdown: buffers flushed, settings saved, card closed) |
| **Playback** | 44 codecs on the card, including FLAC, MP3, OGG, Opus, WMA, APE, WavPack, TTA, Musepack, ALAC, AAC, AIFF, WAV, Speex, DSD, and the chiptune family (SID, SPC, NSF, GBS, VGM, MOD, AY, HES, KSS, …). Verified at 44.1, 48 and 96 kHz, including resume from idle, and the whole set re-played after the rebase moved every codec to a uniform `-O2` |
| **Audio settings** | Hardware volume in 0.5 dB steps, software tone controls (bass/treble), replay gain, crossfeed, A-B repeat, ID3-tag browsing |
| **Recording** | ES7210 microphone over I2S, WAV recording, verified at a usable level. The chip has two gain stages — its own PGA and Rockbox's software gain — and both needed fixing: `REG41` was written with bits that disabled the PGA, and `REG08`'s speed bit is a different bit than the driver assumed |
| **USB** | Mass storage (auto-holds on plug), DAC mode — the host's audio enters Rockbox's mixer, up to 48 kHz — a HID endpoint available alongside either, and a CDC serial console in the debug menu. The profiles are CDC+HID+MSC or CDC+HID+Audio: the device has seven IN endpoints and both combinations use all of them, so all three at once isn't possible |
| **Storage** | SD card through ESP-IDF's SDMMC driver, dircache, tagcache/database, bookmarks |
| **Battery** | AXP173 PMU: percentage from the coulometer counters on a scale measured against hardware (187,786 LSB/mAh observed, 180,000 derived), anchored to the voltage-derived percent at first reading; voltage, charging state, VBUS current and temperature. The cell's total capacity is still the datasheet's 1400 mAh until a run-to-shutdown measures it |
| **RTC** | PCF8563: timekeeping and alarm read/write |
| **Themes** | Built-in `cabbiev2` plus failsafe, album art (with BMP scaling), touch regions on skin bars, and third-party themes from the theme server |
| **Fonts** | 88 fonts buildable from the tree; DPI-correct list row heights |
| **Voice** | 950-clip English voice pack generated from this tree, playable from the card |
| **Plugins** | 158 compiled plugin binaries plus 63 Lua ones, games mapped to the stick and buttons, key-lock, and the usual convention: stick moves, `A` acts, `B` taps for menu/pause, `B` held quits. Doom is the deliberate exception — fire and use are the two actions that dominate play, so they own the physical buttons and the grid carries escape, map, weapon, autorun and strafe |
| **Video plugins** | `mpegplayer` with working YUV blit, `pictureflow` |
| **Power management** | Backlight timeout → LCD sleep, CPU frequency scaling, light sleep, amp off when idle |
| **Manual** | `make manual-pdf` builds a 286-page PDF that covers this target's keys, settings and quirks |
| **Simulator** | The SDL simulator still builds from the same tree, so UI work can be checked without hardware |

The honest caveats: Bluetooth audio is out (above), the output is mono (above), and
some settings whose values become boot-time allocations — like the maximum playlist
size — need a reboot to take effect properly, because they size a buffer that is
allocated once at startup.

---

## 4. The MeowKit, and what it was meant for

The MeowKit is not a media player. It is sold as a **pocket-sized multi-tool for
makers**: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM), ST7789 display with
FT6336 touch, ES8311 audio codec plus ES7210 mic ADC, ES8311-driven 1 W speaker, an
SD slot, an AXP173 power-management chip, a PCF8563 RTC, a BMI270 IMU plus BMM150
magnetometer, an IR transceiver, a WS2812B LED, native USB-OTG, and a lithium cell.
The reference firmware is an Arduino/ESP-IDF build that leans on the accelerometer,
IR and magnetometer for things like gesture input and measurement.

- Vendor site: <https://meowkit.cc>
- Stock firmware: <https://github.com/mingolucky/meowkit-s3-firmware>
- Official documentation: <https://github.com/mingolucky/meowkit-s3-docs>
- Official web installer: <https://github.com/mingolucky/meowkit-s3-installer>

Reading the stock firmware was how this port got a pin map that is *real* rather than
inferred — which matters on this board, because half the control signals (LCD reset,
LCD chip select, the speaker amp enable, the touch reset) live on a **PCA9557 I²C I/O
expander**, not on GPIOs, and the backlight is active-low. If you are porting
something else to this machine, start from that firmware.

Rockbox is a different job for the same silicon. Nothing here is a criticism of what
the board was designed to be.

---

## 5. Flashing it

Our firmware is a standard ESP32 image in the same shape as the stock MeowKit
firmware, so you flash it the same way. There are two files you might reach for:

| File | What it is |
|---|---|
| `rockbox-flash.bin` | **The one to use.** Bootloader + partition table + app merged into a single image that starts at flash offset `0x0`, ~1.36 MB |
| `rockbox.esp32s3` | The app image alone, for people who want to place the three pieces themselves |

### With a web installer (no software to install)

<https://esptool.spacehuhn.com/> runs in a Chromium-based browser with serial access.

1. Get the device into **download mode**: with the device powered, put a pin through
   the small hole on the back and **hold BOOT**, then tap **RESET** (or connect USB
   while holding BOOT). Release BOOT after it enumerates.
2. Open the installer, pick the serial port, and **Connect**.
3. Add `rockbox-flash.bin` with offset **`0x0`**.
4. **Delete any extra rows that have no file** — a row with an offset and nothing in
   it will make the flash fail.
5. **Flash**. When it reports done, **power-cycle the device**: unplug USB, then press
   the power button (the board's PEK line) to start Rockbox.

### From the command line

With ESP-IDF's own tooling, which is what `make flash` does:

```sh
make flash ESPPORT=/dev/ttyACM0
```

Or plainly, with esptool from IDF's Python environment:

```sh
esptool --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 rockbox-flash.bin
```

If you'd rather place the pieces yourself, they go where IDF expects them:

| Offset | File |
|---|---|
| `0x000000` | `bootloader.bin` |
| `0x008000` | `partition-table.bin` |
| `0x010000` | `rockbox.esp32s3` |

`flash_args`, which ships with the artifacts, is exactly that list.

No board handy? `make espverify` runs `esptool image_info` on the image and reports
the segment table, load addresses and the hash — and `make esplayer` checks the memory
layout, so an image that has grown into the plugin reservation fails the build instead
of showing up as a crash on a device.

> Flashing this firmware rewrites the partition table at `0x8000`. Flashing the stock
> firmware back restores its own table, and your SD card content is untouched either
> way.

---

## 6. Preparing the SD card

Format a card as **FAT32** with a single partition — a few gigabytes is plenty — and
unzip the payloads into the **root** of it. `.rockbox/` must be at the root, not
inside a folder.

```sh
unzip rockbox.zip         -d /mnt/card
unzip rockbox-fonts.zip   -d /mnt/card      # optional: 88 fonts
unzip rockbox-themes.zip  -d /mnt/card      # optional: extra themes
```

Then put your music anywhere on the card.

What's in `rockbox.zip` (387 files, ~9.3 MB):

- the built-in themes — `cabbiev2`, the failsafe theme and the default icon set — with
  their WPS files, icons and backdrops
- 44 codecs, 157 plugin binaries plus 63 Lua plugins, codepages and documentation
- `.rockbox/langs/english.voice` — the English voice pack, alongside 48 language files
- the firmware image itself, so the card carries a copy of what the device runs

There is no `rockbox.cfg` in the archive: Rockbox writes its own settings file on
first boot, which is also why a card with a stale `.rockbox/` from another build can
be left in place if you only want to update codecs and plugins.

Fonts are deliberately **not** in `rockbox.zip` (`tools/root.make:712`); they come
from `rockbox-fonts.zip`. Themes from the theme server
(<https://themes.rockbox.org/>) are also `.rockbox/`-rooted archives and unzip the
same way.

On first boot Rockbox builds its database from your collection, so the first browse
after a big card is slower than the ones after it.

---

## 7. Building it

### The container (recommended)

`tools/meowkits3/Dockerfile` builds a bare Ubuntu image with the toolchain installed by
Rockbox's own `tools/rockboxdev.sh --target=e`, so it's the same recipe a host uses.
One run produces every artifact plus checksums into a directory you mount:

```sh
docker build -f tools/meowkits3/Dockerfile -t rockbox-meowkits3 .
docker run --rm -v "$PWD/out:/out" rockbox-meowkits3
```

See [`tools/meowkits3/README`](tools/meowkits3/README) for the layers, the arguments
and what it verified.

### On a host

```sh
# one-time: install the Xtensa toolchain and ESP-IDF
RBDEV_PREFIX=/opt/rockbox-dev RBDEV_BUILD=/tmp/rbdev-build \
  tools/rockboxdev.sh --target=e
. /opt/rockbox-dev/esp-idf/export.sh

mkdir -p build-meowkits3 && cd build-meowkits3
../tools/configure --target=meowkits3
make -j$(nproc)
make zip          # card payload
make fontzip      # the 88 fonts
make espfull      # rockbox-flash.bin, one file at 0x0
make espverify    # esptool image_info, no board needed
make manual-pdf   # the per-target PDF
make flash ESPPORT=/dev/ttyACM0
```

Two things to know: `tools/configure` must be run with the IDF environment sourced, or
it warns once and writes `autoconf.h` with no architecture, which shows up much later
as a compile error; and the build directory's name is how the target is inferred, so
keep it `build-meowkits3`.

---

## 8. Using it

Most interaction is on the touch screen, and the stick does whatever the current
screen needs — navigation in lists, volume on the playing screen.

| Gesture | Where | Does |
|---|---|---|
| Stick up / down | lists, menus | move selection |
| Stick left | menus | back |
| Stick right | menus | accept |
| `A` tap | menus | accept; on the playing screen, play/pause |
| `A` held | menus | context menu; on the playing screen, stop |
| `B` tap | menus | menu; on the playing screen, back to the browser |
| `B` held | menus | quick screen; on the playing screen, context menu; in a plugin, exit |
| `A` + `B` held | anywhere | key lock — deliberate, so a pocket doesn't do it |
| Stick left / right held | playing screen | seek; tap to skip tracks |
| `A` + stick left / right | playing screen | set A/B repeat points; `A` + `B` clears them |
| Power button held ~1 s | any state | clean shutdown, then the PMU cuts power |

Voice prompts, themes, EQ, LCD sleep delay and backlight fading are all in Settings
and documented in the built manual.

---

## 9. Notes for anyone porting something else to Xtensa or ESP32

The architecture layer is deliberately generic; the MeowKit-specific parts are the
board drivers. What an ESP32-S3 port needed, and what is written down rather than
buried in code:

- [`firmware/target/xtensa/notes/MEMORY.md`](firmware/target/xtensa/notes/MEMORY.md) — the real
  memory map: instruction and data buses alias the same SRAM at `+0x6F0000`, the
  512-entry cache-MMU table, the PSRAM window, and the measured budgets.
- [`AGENTS.md`](AGENTS.md) — the build pipeline, how `SOURCES` files are C
  preprocessor programs, the windowed ABI and why the context switch works the way it
  does, GDMA and cache maintenance, PSRAM bring-up, I²S, SDHOST, the PMU, and the
  register-level facts that were verified against hardware or a ROM image.
- [`firmware/target/xtensa/notes/PENDING.md`](firmware/target/xtensa/notes/PENDING.md) — status.

The interesting discoveries are in there: the esp32s3 core config has no REGB option
(so no `spills`/`refill`/`mtsr`), there is no `INTBASE` but `VECBASE` works, the ROM
exports cache maintenance functions and nothing else does, DMA descriptors must sit in
internal SRAM while DMA *buffers* may sit in PSRAM, and the mask ROM never arms the
instruction-side MMU entries.

ESP32 boards are everywhere in the hobbyist community, and this is a lot of working
infrastructure — an Xtensa ABI and context switch, interrupt vectors, GDMA and SDHOST
drivers, cache and MMU handling, PSRAM bring-up, an I²C command-list driver, a PMU
driver, and an ESP-IDF app shell. Another ESP32-S3 board with an LCD and a codec
should be a config header and a set of board drivers away, which is the hope: that
this is a seed rather than a one-off.

---

## 10. Thanks

- **[Rockbox](https://www.rockbox.org)** and everyone who has built, debugged and
  documented it over two decades — [the source](https://git.rockbox.org/), the
  [wiki](https://www.rockbox.org/wiki/), and the theme and plugin ecosystems that make
  it feel like a platform rather than a firmware. The architecture this port had to
  fit into is well-designed enough that a new CPU was possible at all, and the manual
  Rockbox lets each target build is a genuinely good artifact: 286 pages describing
  this little device, its keys and its settings, generated from the tree it describes.
- **[MeowKit](https://meowkit.cc)** and
  [their published firmware](https://github.com/mingolucky/meowkit-s3-firmware), which
  made the board's wiring knowable.
- **Espressif** for ESP-IDF, the prebuilt Xtensa toolchains and the ROM symbols.
- **Qwen** for their open models — <https://huggingface.co/Qwen>.
- **ISTA-DASLab** for their quantization work — <https://huggingface.co/ISTA-DASLab>.
- **Strata**, for enabling leading-edge inference just about anywhere —
  <https://github.com/Niko1221/Strata>.

---

## 11. Status summary

| | |
|---|---|
| Branch | `esp32-s3-idf`, at <https://gitea.max.optiplexus.org/jhohertz/rockbox> |
| Releases | [`esp32-s3-idf-20261029`](https://gitea.max.optiplexus.org/jhohertz/rockbox/src/tag/esp32-s3-idf-20261029) — current — with [its notes](firmware/target/xtensa/notes/releases/esp32-s3-idf-20261029.md), superseding [`esp32-s3-idf-20261027`](https://gitea.max.optiplexus.org/jhohertz/rockbox/src/tag/esp32-s3-idf-20261027) and [its notes](firmware/target/xtensa/notes/releases/esp32-s3-idf-20261027.md). Both were flashed at `0x0` and verified |
| Target name | `meowkits3` (target id 126), `MODEL_NAME "MeowKit ESP32-S3"` |
| Toolchain | xtensa-esp-elf 14.2.0, ESP-IDF v5.5.5, single-core, windowed ABI with hand-written register-window overflow/underflow handlers |
| Firmware image | ~1.29 MB app in a 1.5 MB partition; `rockbox-flash.bin` 1,373,376 B at `0x0` |
| Card payload | 9,888,675 B, 422 files (44 `.codec`, 157 `.rock` plugins, 63 Lua, 48 `.lng` languages, 2 voice clips + one voice pack, 41 WPS/backdrops/themes) |
| Upstream sync | Rebased onto upstream `master` (`fe5c0874af`, 2026-10-28) on 2026-10-29: 651 commits replayed, 257 upstream commits absorbed. The plugin API went 285 → 286, so every shipped `.rock` is rebuilt and older ones are refused. Upstream `master` also removed `frotz` (157 plugin binaries now) and added a DSD codec. DSD64 DSF plays on hardware with its embedded album art decoded; DSD128 is CPU-bound and gaps, so DSD64 is this target's practical ceiling |
| Not yet done | a run-to-shutdown capacity measurement to replace the datasheet's 1400 mAh, a theme that renders correctly at 320×240, upstream submissions, two mpegplayer fixes upstream (`disk_buf_init` accepting a zero page count, libmpeg2 not checking its allocations), and a release tag from the rebased tree (the codec soak is done and the manual PDF is built — `build-meowkits3/rockbox-manual.pdf`, 3,056,519 B — so the remaining blocker is the capacity run) |

Bugs, questions and patches welcome — and if any of this looks like something that
belongs upstream, that's exactly the point.
