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

## Using it as an environment rather than a build

The entrypoint *is* the build script, so a shell needs `--entrypoint`:

```sh
docker run --rm -it --entrypoint bash rockbox-meowkits3
# inside: . /opt/rockbox-dev/esp-idf/export.sh && cd /work/build-meowkits3 && make
```

To build a working tree instead of the clone, mount it over the checkout. The
build directory lives outside the source tree, so `tools/configure` there is
re-run and the tree stays clean:

```sh
docker run --rm -it --entrypoint bash   -v "$PWD:/src/rockbox" -v "$PWD/out:/out" rockbox-meowkits3
```

Out-of-tree builds work because `tools/espflash.make` derives its paths from
`$(ROOTDIR)` rather than from the build directory's parent — which used to make
`make esplayer` look for `<build-dir>/../idf-app/build/rockbox.map` and fail with
`No rule to make target`.

`SKIP_MANUAL=1` leaves the PDF out; everything else is still emitted and
verified.

## Failure behaviour

The script copies each artifact as its goal completes, so a late failure still
leaves the earlier ones in the output directory. It exits non-zero in that case,
and the manifest plus `SHA256SUMS` are only written when everything arrived -- a
partial directory can never pass as a release set. `esplayer` and `espverify` run
before the image is copied out, so an arena that grew into the plugin/codec
reservation fails the run instead of shipping.

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
| `RB_REFRESH` | `0` | BuildKit keys the clone layer on its ARG values, not on the remote, so a cached clone survives every push. Pass `--build-arg RB_REFRESH=$(date +%s)` to force a fresh checkout |
| `BUILD_AT_IMAGE_BUILD` | `no` | `yes` builds during the image build, which is what `--target=rb-artifacts` requires |
| `OUT_DIR` | `/artifacts` at build time, `/out` at run time | |

Runtime environment for the entrypoint:

| Variable | Default | Effect |
|---|---|---|
| `REPO` | `/src/rockbox` | checkout to build |
| `BUILD_DIR` | `/work/build-meowkits3` | must end in the model name — `tools/autoconf.pl` infers the target from the build directory name |
| `OUT_DIR` | `/out` | where artifacts are written; mount it from the host |
| `RB_TARGET` | `meowkits3` | argument to `tools/configure` |
| `RB_VOICE` | `1` | `--type=AV --tts=espeak`, i.e. the voice payload the device actually gets. `0` gives `--type=N` and a card set without `.voice`. Only honoured when `BUILD_DIR` is fresh |
| `RB_LANGUAGE` | `english` | voice/language for the AV build |
| `IDF_PATH` | `/opt/rockbox-dev/esp-idf` | ESP-IDF checkout to source `export.sh` from |
| `SKIP_MANUAL` | unset | set to `1` to skip the LaTeX pass |
| `RELEASE` | unset | a label written into `build-manifest.txt`. The clone is shallow, so `git describe --tags` cannot see the tag it was cloned at — pass it explicitly for a release run |

## What lands in the output directory

| File | What it is |
|---|---|
| `rockbox.esp32s3` | the app image, flashed at `0x010000` |
| `rockbox-flash.bin` | the all-in-one image: bootloader + partition table + app merged by `make espfull`, one file at `0x0` |
| `bootloader.bin` | flashed at `0x000000` |
| `partition-table.bin` | flashed at `0x008000` |
| `flash_args` | the address list IDF's own flash step uses, so a receiver can flash without guessing |
| `rockbox.zip` | card payload (`.rockbox/`: fonts excluded, plugins, codecs and voice clips included — `make voice` runs before `make zip`, because `buildzip.pl` only copies clips it finds in the build directory) |
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

## Measured

On a 32-core host:

| What | Time |
|---|---|
| toolchain stage, cold (168 MB archive + IDF clone + `install.sh esp32s3`) | ~19 min |
| toolchain stage, warm (`RBDEV_DOWNLOAD` cache populated) | ~110 s |
| image size | 10.4 GB |
| `docker run` end to end — configure, firmware, `esplayer`, `espverify`, `espfull`, voice, zip, fonts, manual, copy | ~2 min, exit 0, 13 files |
| `rb-artifacts` build with `SKIP_MANUAL=1` | 80 s, exit 0 |

`espverify.txt` reports the app partition 16 % free with a valid checksum and
validation hash. The payload matches a host build exactly in shape: 411 files, and a
`diff` of the two zip listings is empty. `english.voice` is 2,512,515 B on both
sides. It started out 6,158 B apart, which turned out to be the container finding a
real defect: the host's pack predated the feature-flag batch and had no prompts for
the settings that batch added. Re-running `make voice` closed it. Both sides use
espeak-ng 1.52.0, so the encoder is identical either way.

## Not included

- **Theme packs.** `tools/rbthemes.py` pulls from the volunteer theme server; a
  reproducible container build should not depend on somebody else's uptime, and
  the result is not a build artifact.
- Nothing, for the default run: the image carries `espeak-ng` so the voice pack is
  built, which is what the device ships. Measured parity — the AV build's
  `rockbox.zip` is 411 files and includes `.rockbox/langs/english.voice`
  (2,506,357 B); with `RB_VOICE=0` it is 409 files and 7,144,205 B, i.e. 2.2 MB
  shorter, which is exactly the difference that used to make a container payload
  quietly not match a flashed card.
- **A cached `IDF_TOOLS_PATH`.** ESP-IDF's `install.sh` extracts its tools and
  creates its Python virtualenv *inside* `IDF_TOOLS_PATH`, so that path is
  installed state and must be part of the image. Mounting a BuildKit cache there
  makes the build succeed and the run fail, because cache content is never
  committed: `export.sh` then reports `ESP-IDF Python virtual environment ...
  not found`. Only `RBDEV_DOWNLOAD` is a cache mount, and only because that
  directory genuinely is scratch.

- **Flashing.** There is no device in a container. `flash_args` plus the three
  binaries are what a host needs to do it.
