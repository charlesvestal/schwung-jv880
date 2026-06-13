#!/usr/bin/env bash
# build.sh — build the render_test harness on macOS
# Run from the repo root:  bash tools/render_test/build.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOL_DIR="$REPO_ROOT/tools/render_test"
SRC="$REPO_ROOT/src/dsp"

echo "==> Building render_test"
echo "    REPO_ROOT : $REPO_ROOT"
echo "    SRC       : $SRC"

clang++ -std=c++17 -O2 \
    -I"$SRC" \
    "$TOOL_DIR/render_test.cpp" \
    "$SRC/mcu.cpp" \
    "$SRC/mcu_opcodes.cpp" \
    "$SRC/pcm.cpp" \
    -o "$TOOL_DIR/render_test" \
    -lm

echo "==> Built: $TOOL_DIR/render_test"
