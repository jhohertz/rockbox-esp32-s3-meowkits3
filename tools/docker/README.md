# Build container for the MeowKit ESP32-S3 target

A single `Dockerfile` plus one build script. It produces the same artifacts the
port's own recipe produces on a host, and it emits them to a directory you mount
from the host so they can be verified, released or flashed elsewhere.

No CI configuration lives here on purpose. The interface is:

```sh
docker build -f tools/docker/Dockerfile -t rockbox-meowkits3 .
docker run --rm -v "$PWD/out:/out" rockbox-meowkits3
ls out/
```

## What each layer is

| Stage | Contents | Why it is its own layer |
|---|---|---|
| `rb-base` | `ubuntu:26.04` + `ca-certificates` | the only thing the base needs before apt is useful |
| `rb-deps` | core build dependencies from APT | everything below depends on it; it changes rarely |
| `rb-deps-manual` | TeX Live | only `make manual-pdf` needs ~600 MB of it; `--target=rb-deps` gives a slimmer image |
| `rb-src` | checkout of `RB_REPO`/`RB_BRANCH` | dependency install needs the repo's own script, so the checkout comes before the toolchains |
| `rb-toolchain` | xtensa-esp-elf + ESP-IDF, installed by `tools/rockboxdev.sh --target=e` | the expensive, network-heavy step, cached independently of the source |
| `rb-env` | default target: build environment + entrypoint | `docker run` builds and writes to `/out` |
| `rb-artifacts` | artifacts only | a deliverable image with no compiler in it |

## Dependencies come from the repo's own installer

`tools/rockboxdev.sh` is the upstream convention for Rockbox toolchains, and for
the ESP target it does two things (`tools/rockboxdev.sh:1172-1184`):

- `build_xtensa_toolchain esp32s3 <tag>` — installs Espressif's prebuilt
  multilib `xtensa-esp-elf` release. It is not built from GNU sources: upstream
  GCC has no Xtensa target, and real ESP32 cores are Tensilica configurations
  that only the vendor's dynconfig plugin knows.
- `build_esp_idf esp32s3 <tag>` — clones ESP-IDF into `$RBDEV_PREFIX/esp-idf`
  and runs IDF's own `install.sh esp32s3`.

So the container never hand-rolls an IDF install, and the same two commands work
on a host:

```sh
RBDEV_PREFIX=/opt/rockbox-dev RBDEV_BUILD=/tmp/rbdev-build \
  tools/rockboxdev.sh --target=e
. /opt/rockbox-dev/esp-idf/export.sh
```

The APT list is derived from the scripts rather than guessed: `rockboxdev.sh:91`
names its own requirements (`gcc g++ xz bzip2 gzip make patch makeinfo automake
libtool autoconf flex bison`), `rockboxdev.sh:511-517` refuses to install IDF
without `git cmake ninja python3`, and `tools/buildzip.pl` calls `zip` and
`install`.

## Build arguments

| ARG | Default | Notes |
|---|---|---|
| `BASE_IMAGE` | `ubuntu:26.04` | verified to exist as an upstream Docker Hub tag |
| `RB_REPO` | `https://gitea.max.optiplexus.org/jhohertz/rockbox` | the public URL for now; change it when the port moves upstream |
| `RB_BRANCH` | `esp32-s3-idf` | cloned shallow; `.git` is kept because `tools/version.sh` derives the version string from `git rev-parse` |
| `RBDEV_PREFIX` | `/opt/rockbox-dev` | single root for the toolchain *and* the IDF checkout |
| `RBDEV_ESP_TOOLCHAIN` | `esp-14.2.0_20260121` | matches `rockboxdev.sh`'s default pin |
| `RBDEV_ESP_IDF` | `v5.5.5` | same release the port is developed against |
| `RBDEV_ESP_IDF_CORE` | `esp32s3` | |
| `BUILD_AT_IMAGE_BUILD` | `no` | `yes` builds during the image build, which is what `--target=rb-artifacts` requires |
| `OUT_DIR` | `/artifacts` at build time, `/out` at run time | |

Runtime environment for the entrypoint: `REPO`, `BUILD_DIR`, `OUT_DIR`,
`RB_TARGET`, `IDF_PATH`, and `SKIP_MANUAL=1` to leave the PDF out.

## What lands in the output directory

| File | What it is |
|---|---|
| `rockbox.esp32s3` | the app image, flashed at `0x010000` |
| `bootloader.bin` | flashed at `0x000000` |
| `partition-table.bin` | flashed at `0x008000` |
| `flash_args` | the address list IDF's own flash step uses, so a receiver can flash without guessing |
| `rockbox.zip` | card payload (`.rockbox/`: fonts excluded, plugins/codecs/voice included) |
| `rockbox-fonts.zip` | the font payload, built with `make fontzip` |
| `rockbox-manual.pdf` | target manual for `meowkits3` |
| `rockbox-info.txt` | build info string |
| `rockbox.map` | the linker map `tools/esp-layout-check` reads, so the arena reservations can be re-checked without a toolchain |
| `espverify.txt` | `esptool image_info` output for the app image |
| `build-manifest.txt` | commit, branch, dirty count, IDF and toolchain versions, UTC build time |
| `SHA256SUMS` | checksums of everything above |

`make esplayer` and `make espverify` both run before anything is copied, so an
image whose arena has grown into the plugin/codec reservation fails the build
instead of shipping.

## Not included

- **Theme packs.** `tools/rbthemes.py` pulls from the volunteer theme server; a
  reproducible container build should not depend on somebody else's uptime, and
  the result is not a build artifact.
- **Voice packs.** `tools/configure --type=AV` needs a TTS engine (espeak,
  festival, piper or mimic). Left out; the default `--type=N` build does not ask
  for one.
- **Flashing.** There is no device in a container. `flash_args` plus the three
  binaries are what a host needs to do it.
