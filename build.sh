#!/usr/bin/env bash
# Builds the firmware with ESP-IDF 4.4.8. Uses a local idf.py if you have one,
# otherwise the same Docker image as ~/oled_wled.
#
#   ./build.sh            build
#   ./build.sh --flash    build and flash (see flash.sh for port/baud)
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="sle118/squeezelite-esp32-idfv448:4.4.8"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/oscillesp32/ccache"
mkdir -p "$CACHE"

BUILD_CMD='
set -e
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
    . "$IDF_PATH/export.sh" >/dev/null
fi
idf.py build
esptool.py --chip esp32 merge_bin -o build/oscillesp32_factory_0x0.bin \
    --flash_mode dio --flash_freq 40m --flash_size 2MB \
    0x1000 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/oscillesp32.bin
'

if command -v idf.py >/dev/null 2>&1; then
    echo "[BUILD] local ESP-IDF"
    (cd "$DIR" && bash -c "$BUILD_CMD")
elif command -v docker >/dev/null 2>&1; then
    echo "[BUILD] Docker ($IMAGE)"
    docker run --rm \
        -v "$DIR:/project" -w /project \
        -v "$CACHE:/ccache" -e CCACHE_DIR=/ccache \
        --user "$(id -u):$(id -g)" -e HOME=/tmp \
        "$IMAGE" bash -c "$BUILD_CMD"
else
    echo "[ERROR] Need idf.py (ESP-IDF 4.4) or docker." >&2
    exit 1
fi

echo
echo "Firmware: build/oscillesp32.bin (app @ 0x10000)"
echo "          build/oscillesp32_factory_0x0.bin (everything @ 0x0, e.g. for web flashers)"

if [[ "${1:-}" == "--flash" || "${1:-}" == "-f" ]]; then
    shift
    "$DIR/flash.sh" "$@"
fi
