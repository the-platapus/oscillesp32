#!/usr/bin/env bash
# Flashes build/ to the ESP32 (run ./build.sh first).
#   ./flash.sh [PORT] [BAUD]      defaults: /dev/ttyUSB0 115200
# Writes the app in 128 KB slices with retries, like ~/oled_wled/flash.sh,
# because flaky USB-serial links tend to drop long transfers.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-115200}"
IMAGE="sle118/squeezelite-esp32-idfv448:4.4.8"

[[ -f "$DIR/build/oscillesp32.bin" ]] || { echo "Run ./build.sh first" >&2; exit 1; }
[[ -e "$PORT" ]] || { echo "No serial port $PORT (is the board plugged in?)" >&2; exit 1; }

docker run --rm -i -v "$DIR:/project" -w /project \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    --device "$PORT" --group-add "$(stat -c %g "$PORT")" \
    "$IMAGE" bash -c ". \"\$IDF_PATH/export.sh\" >/dev/null && python3 - \"$PORT\" \"$BAUD\"" <<'PY'
import subprocess, sys

port, baud = sys.argv[1], sys.argv[2]

def flash_part(offset, path, reset_after=False):
    cmd = ["esptool.py", "--chip", "esp32", "-p", port, "-b", baud,
           "--before", "default_reset", "--after", "hard_reset" if reset_after else "no_reset",
           "write_flash", "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "detect",
           hex(offset), path]
    for attempt in range(1, 5):
        print(f"[FLASH] {path} @ {hex(offset)} (attempt {attempt})", flush=True)
        if subprocess.call(cmd) == 0:
            return
    sys.exit(1)

flash_part(0x1000, "build/bootloader/bootloader.bin")
flash_part(0x8000, "build/partition_table/partition-table.bin")
data = open("build/oscillesp32.bin", "rb").read()
CHUNK = 0x20000   # 128 KB slices, sector aligned
n = (len(data) + CHUNK - 1) // CHUNK
for i in range(n):
    path = f"/tmp/oscillesp32_chunk_{i}.bin"
    with open(path, "wb") as f:
        f.write(data[i * CHUNK:(i + 1) * CHUNK])
    flash_part(0x10000 + i * CHUNK, path, reset_after=(i == n - 1))
print("[FLASH] done. Serial console: ./monitor.sh " + port)
PY
