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
#   IDF_PATH   ESP-IDF checkout                   (default /opt/rockbox-dev/esp-idf)
#   SKIP_MANUAL=1  omit the PDF manual (saves several minutes of LaTeX)
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
IDF_PATH="${IDF_PATH:-/opt/rockbox-dev/esp-idf}"
IDF_BUILD="$REPO/idf-app/build"

echo "== rockbox-build-artifacts =="
echo "  repo      : $REPO"
echo "  build dir : $BUILD_DIR"
echo "  out dir   : $OUT_DIR"
echo "  target    : $RB_TARGET"

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
cd "$BUILD_DIR"

# configure must be run from inside the build directory, and it writes Makefile and
# autoconf.h here. Re-running it is harmless, so a container that keeps its build
# dir rebuilds incrementally.
if [ ! -f Makefile ]; then
    "$REPO/tools/configure" --target="$RB_TARGET"
fi

echo "-- firmware, host tools and codecs"
make -j"$(nproc)"

# The layout guard and the loader's own view of the image run before anything is
# copied out: an arena that has grown into the plugin/codec reservation must fail
# here rather than appear as a crash on a device after the artifacts shipped.
echo "-- layout guard (tools/esp-layout-check)"
make esplayer

echo "-- image verification (esptool image_info)"
make espverify > "$OUT_DIR/espverify.txt" 2>&1
tail -4 "$OUT_DIR/espverify.txt"

echo "-- flash set"
copy "$BUILD_DIR/rockbox.esp32s3"
copy "$IDF_BUILD/bootloader/bootloader.bin"
copy "$IDF_BUILD/partition_table/partition-table.bin"
# The address list IDF's own flash step uses, so a receiver can flash without
# guessing: bootloader 0x000000, partition table 0x008000, app 0x010000.
copy "$IDF_BUILD/flash_args"
# The map is what tools/esp-layout-check reads. Shipping it means the arena
# reservations can be re-checked by anyone who receives the artifacts, without a
# toolchain.
copy "$IDF_BUILD/rockbox.map"

echo "-- card payload (rockbox.zip)"
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
    echo "commit=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "branch=$(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "dirty=$(git -C "$REPO" status --porcelain 2>/dev/null | wc -l)"
    echo "idf=$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo unknown)"
    echo "xtensa=$(xtensa-esp32s3-elf-gcc -dumpversion 2>/dev/null || echo unknown)"
    echo "host_cc=$(cc -dumpversion 2>/dev/null || echo unknown)"
    echo "target=$RB_TARGET"
    echo "built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$OUT_DIR/build-manifest.txt"
cat "$OUT_DIR/build-manifest.txt"

# Hash everything present except the checksum file itself, which would otherwise
# hash its own truncated self.
( cd "$OUT_DIR" && find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\n' \
    | sort | xargs sha256sum > SHA256SUMS )

echo "-- wrote $(ls "$OUT_DIR" | wc -l) files to $OUT_DIR"
