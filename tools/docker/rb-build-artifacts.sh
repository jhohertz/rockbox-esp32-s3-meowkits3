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

set -euo pipefail

REPO="${REPO:-/src/rockbox}"
BUILD_DIR="${BUILD_DIR:-/work/build-meowkits3}"
OUT_DIR="${OUT_DIR:-/out}"
RB_TARGET="${RB_TARGET:-meowkits3}"
IDF_PATH="${IDF_PATH:-/opt/rockbox-dev/esp-idf}"

echo "== rockbox-build-artifacts =="
echo "  repo      : $REPO"
echo "  build dir : $BUILD_DIR"
echo "  out dir   : $OUT_DIR"
echo "  target    : $RB_TARGET"

# export.sh puts idf.py, the version-pinned Xtensa toolchain and IDF's own Python
# environment on PATH. Everything after this line needs that, including
# make espverify, which runs esptool.
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "error: no ESP-IDF at $IDF_PATH (run tools/rockboxdev.sh --target=e first)" >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$IDF_PATH/export.sh" >/dev/null

mkdir -p "$BUILD_DIR" "$OUT_DIR"
cd "$BUILD_DIR"

# configure must be run from inside the build directory, and only once: it writes
# Makefile and autoconf.h here. Re-running it on an existing build dir is fine, so
# a mounted source tree rebuilds incrementally.
if [ ! -f Makefile ]; then
    "$REPO/tools/configure" --target="$RB_TARGET"
fi

echo "-- firmware, tools and codecs"
make -j"$(nproc)"

echo "-- card payload (rockbox.zip)"
make zip

echo "-- font payload (rockbox-fonts.zip)"
make fontzip

if [ -z "${SKIP_MANUAL:-}" ]; then
    # features.tex is generated from apps/features.txt but the rule in
    # manual/Makefile:54 lists only that file as its dependency, while the
    # preprocess step reads config.h. In a fresh build dir it is generated
    # correctly; when flags change across builds, delete it.
    rm -f "$BUILD_DIR/manual/features.tex"
    echo "-- manual (rockbox-manual.pdf)"
    make manual-pdf
fi

echo "-- layout guard (tools/esp-layout-check)"
make esplayer

echo "-- image verification (esptool image_info)"
make espverify > "$OUT_DIR/espverify.txt" 2>&1
tail -6 "$OUT_DIR/espverify.txt"

# The three pieces a flash needs, in the order the loader expects them:
# bootloader at 0x000000, partition table at 0x008000, app at 0x010000.
IDF_BUILD="$REPO/idf-app/build"

copy() {
    if [ -f "$1" ]; then
        cp -f "$1" "$OUT_DIR/$(basename "$1")"
        printf '  %-28s %s bytes\n' "$(basename "$1")" "$(stat -c%s "$1")"
    else
        echo "  MISSING: $1" >&2
    fi
}

echo "-- artifacts"
copy "$BUILD_DIR/rockbox.esp32s3"
copy "$BUILD_DIR/rockbox.zip"
copy "$BUILD_DIR/rockbox-fonts.zip"
copy "$BUILD_DIR/rockbox-manual.pdf"
copy "$BUILD_DIR/rockbox-info.txt"
copy "$IDF_BUILD/bootloader/bootloader.bin"
copy "$IDF_BUILD/partition_table/partition-table.bin"
copy "$IDF_BUILD/flash_args"
# The map is what tools/esp-layout-check reads. Shipping it means the arena
# reservations can be re-checked by anyone who receives the artifacts, without a
# toolchain.
copy "$IDF_BUILD/rockbox.map"

echo "-- provenance"
{
    echo "commit=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "branch=$(git -C "$REPO" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "dirty=$(git -C "$REPO" status --porcelain 2>/dev/null | wc -l)"
    echo "idf=$(git -C "$IDF_PATH" describe --tags 2>/dev/null || echo unknown)"
    echo "xtensa=$(xtensa-esp-elf-gcc -dumpversion 2>/dev/null || echo unknown)"
    echo "host_cc=$(cc -dumpversion 2>/dev/null || echo unknown)"
    echo "target=$RB_TARGET"
    echo "built_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$OUT_DIR/build-manifest.txt"
cat "$OUT_DIR/build-manifest.txt"

( cd "$OUT_DIR" && sha256sum -- * 2>/dev/null | grep -v '^.*SHA256SUMS$' > SHA256SUMS )
echo "-- wrote $(ls "$OUT_DIR" | wc -l) files to $OUT_DIR"
