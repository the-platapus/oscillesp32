#!/usr/bin/env bash
# Compiles the acquisition engine for the PC and runs the simulation checks.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/oscillesp32_host_sim"
g++ -std=gnu++17 -O1 -g -DHOST_TEST -Wall -Wno-unused-function \
    -Itest/stubs -Isrc \
    test/host_sim.cpp src/acquisition.cpp src/measure.cpp src/settings.cpp src/commands.cpp \
    -lm -o "$OUT"
"$OUT"
