#!/usr/bin/env bash
# Serial console at 115200. Type commands like: help, status, autoset, tdiv 1ms
# Exit with Ctrl+]
set -euo pipefail
PORT="${1:-/dev/ttyUSB0}"
docker run --rm -it --device "$PORT" --group-add "$(stat -c %g "$PORT")" \
    sle118/squeezelite-esp32-idfv448:4.4.8 \
    bash -c ". \"\$IDF_PATH/export.sh\" >/dev/null && python -m serial.tools.miniterm --raw --eol LF '$PORT' 115200"
