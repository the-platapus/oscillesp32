#!/usr/bin/env bash
# Renders the OLED screens on the PC and writes oled_*.pbm to $OUTDIR (default: build/oled_preview).
set -euo pipefail
cd "$(dirname "$0")/.."
export OUTDIR="${OUTDIR:-build/oled_preview}"
mkdir -p "$OUTDIR"
g++ -std=gnu++17 -O1 -g -DHOST_TEST -Wall -Wno-unused-function \
    -Itest/stubs -Isrc \
    test/host_oled.cpp src/acquisition.cpp src/measure.cpp src/settings.cpp src/commands.cpp src/display.cpp \
    -lm -o "$OUTDIR/host_oled"
"$OUTDIR/host_oled"
