#!/bin/bash
#
# Build the MeowKit ESP32-S3 target and emit its artifacts to $OUT_DIR.
#
# Used as the container entrypoint and equally runnable on a host, because every
# dependency it assumes is something tools/rockboxdev.sh installed rather than
# something this script installed. That is the point of keeping it separate from
# the Dockerfile: the build steps are the same steps the port's own recipe uses.
#
# Environment:
#   REPO       checkout root                     (default /src/rockbox)
#   BUILD_DIR  build directory                   (default /work/build-meowkits3)
#   OUT_DIR    where artifacts are written        (default /out)
#   RB_TARGET  argument for tools/configure       (default meowkits3)
#   RB_VOICE   1 = voice build, 0 = plain          (default 1, only honoured when
#              the build directory is fresh: configure is what sets the build type)
#   RB_LANGUAGE voice/language for the AV build    (default english)
#   IDF_PATH   ESP-IDF checkout                   (default /opt/rockbox-dev/esp-idf)
#   SKIP_MANUAL=1  omit the PDF manual (saves several minutes of LaTeX)
#   RELEASE        a label recorded in build-manifest.txt. The clone is shallow, so
#                  `git describe --tags` cannot see the tag; pass it explicitly for a
#                  release build rather than hoping the checkout knows it
#
# BUILD_DIR's *name* matters: tools/autoconf.pl infers the target from the build
# directory name, so it must end in the model name.
#
# Artifacts are copied as each goal completes, so a late failure still leaves the
# earlier ones in place -- but the run exits non-zero, and the manifest and
# checksums are only written when everything got there.

set -euo pipefail

REPO="${REPO:-/src/rockbox}"
BUILD_DIR="${BUILD_DIR:-/work/build-meowkits3}"
OUT_DIR="${OUT_DIR:-/out}"
RB_TARGET="${RB_TARGET:-meowkits3}"
RB_VOICE="${RB_VOICE:-1}"
RB_LANGUAGE="${RB_LANGUAGE:-english}"
IDF_PATH="${IDF_PATH:-/opt/rockbox-dev/esp-idf}"

echo "== rockbox-build-artifacts =="
echo "  repo      : $REPO"
echo "  build dir : $BUILD_DIR"
echo "  out dir   : $OUT_DIR"
echo "  target    : $RB_TARGET"
echo "  voice     : $RB_VOICE ($([ "$RB_VOICE" = "1" ] && echo "--type=AV, language $RB_LANGUAGE" || echo "--type=N"))"

copy() {
    if [ -f "$1" ]; then
        cp -f "$1" "$OUT_DIR/$(basename "$1")"
        printf '  %-30s %s bytes\n' "$(basename "$1")" "$(stat -c%s "$1")"
    else
        echo "error: expected artifact missing: $1" >&2
        exit 1
    fi
}

# export.sh puts idf.py, the version-pinned Xtensa toolchain and IDF's own Python
# environment on PATH. Everything after this line needs that, including
# `make espverify`, which runs esptool.
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "error: no ESP-IDF at $IDF_PATH (run tools/rockboxdev.sh --target=e first)" >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$IDF_PATH/export.sh" >/dev/null

mkdir -p "$BUILD_DIR" "$OUT_DIR"

# Git refuses to operate on a repository owned by a different uid, and a host directory
# mounted into a container is exactly that: the checkout is the host user's, the process
# is root. Without this, `rev-parse` fails with "detected dubious ownership", the
# manifest records commit=unknown and dirty=0, and tools/version.sh embeds an empty hash
# in the image itself -- so the device's About screen reads "-261029" and nothing can tie
# an artifact to a commit. Measured on the esp32-s3-idf-20261029 build, where it produced
# exactly that.
git config --global --add safe.directory "$REPO" >/dev/null 2>&1 || true
if [ -d "$IDF_PATH" ]; then
    git config --global --add safe.directory "$IDF_PATH" >/dev/null 2>&1 || true
fi

cd "$BUILD_DIR"

# configure must be run from inside the build directory, and it writes Makefile and
# autoconf.h here. Re-running it is harmless, so a container that keeps its build
# dir rebuilds incrementally.
#
# Voice is on by default because that is what this port ships: `make zip` only puts
# .rockbox/langs/<lang>.voice in the payload when the build was configured with
# --type=AV, so a plain build emits a card set 2.5 MB and two files short of the one
# the device gets -- and nothing says so. RB_VOICE=0 is the lean build for someone
# who only wants the firmware image.
if [ ! -f Makefile ]; then
    if [ "$RB_VOICE" = "1" ]; then
        "$REPO/tools/configure" --target="$RB_TARGET" --language="$RB_LANGUAGE" \
            --tts=espeak --type=AV
    else
        "$REPO/tools/configure" --target="$RB_TARGET"
    fi
fi

echo "-- firmware, host tools and codecs"
make -j"$(nproc)"

# The layout guard and the loader's own view of the image run before anything is
# copied out: an arena that has grown into the plugin/codec reservation must fail
# here rather than appear as a crash on a device after the artifacts shipped.
echo "-- layout guard (tools/meowkits3/esp-layout-check)"
make esplayer

echo "-- flash and release artifact set (make espartifacts)"
# One Rockbox goal produces everything a release of an ESP target consists of:
# the app image, bootloader, partition table, IDF's own flash address list, the
# merged one-file image at 0x0, esptool's image_info of the app, and the map and
# ELF. The goal -- not this script -- knows IDF's layout, so IDF's build tree stops
# being something a packaging step has to remember.
make espartifacts > /dev/null
for f in "$BUILD_DIR"/esp-artifacts/*; do
    copy "$f"
done
tail -4 "$BUILD_DIR/esp-artifacts/espverify.txt"

echo "-- card payload (rockbox.zip)"
# `make zip` only copies voice clips it finds: buildzip.pl:686-689 globs *.voice and
# *.talk out of the build directory. Configuring --type=AV is not enough on its own,
# so the voice pass has to run before the zip or the payload comes out two files and
# 2.5 MB short of what the device gets -- silently.
if [ "$RB_VOICE" = "1" ]; then
    echo "-- voice clips (make voice)"
    make voice
fi
make zip
copy "$BUILD_DIR/rockbox.zip"
copy "$BUILD_DIR/rockbox-info.txt"

echo "-- font payload (rockbox-fonts.zip)"
make fontzip
copy "$BUILD_DIR/rockbox-fonts.zip"

if [ -z "${SKIP_MANUAL:-}" ]; then
    # features.tex is generated from apps/features.txt, but the rule in
    # manual/Makefile lists only that file as its dependency while the preprocess
    # step reads config.h. In a fresh build dir it is generated correctly; when
    # flags change across builds, delete it or the PDF silently keeps the old
    # option set.
    rm -f "$BUILD_DIR/manual/features.tex"
    echo "-- manual (rockbox-manual.pdf)"
    make manual-pdf
    # The build produces rockbox-build.pdf; the port's recipe calls the artifact
    # rockbox-manual.pdf, so rename it here rather than make the receiver guess.
    copy "$BUILD_DIR/manual/rockbox-build.pdf"
    mv "$OUT_DIR/rockbox-build.pdf" "$OUT_DIR/rockbox-manual.pdf"
fi

echo "-- provenance"
{
    if [ -n "${RELEASE:-}" ]; then
        echo "release=$RELEASE"
    fi
    echo "commit=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "branch=$(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "dirty=$(git -C "$REPO" status --porcelain 2>/dev/null | wc -l)"
    echo "idf=$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo unknown)"
    echo "xtensa=$(xtensa-esp32s3-elf-gcc -dumpversion 2>/dev/null || echo unknown)"
    echo "host_cc=$(cc -dumpversion 2>/dev/null || echo unknown)"
    echo "target=$RB_TARGET"
    echo "built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    # The version string embedded in the image itself, so a receiver can match an
    # artifact to this manifest from the device's own About screen.
    echo "embedded_version=$(grep -m1 '^Version:' "$BUILD_DIR/rockbox-info.txt" 2>/dev/null | cut -d' ' -f2 || echo unknown)"
} > "$OUT_DIR/build-manifest.txt"
cat "$OUT_DIR/build-manifest.txt"

# Hash everything present except the checksum file itself, which would otherwise
# hash its own truncated self.
( cd "$OUT_DIR" && find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\n' \
    | sort | xargs sha256sum > SHA256SUMS )

echo "-- wrote $(ls "$OUT_DIR" | wc -l) files to $OUT_DIR"
