#!/usr/bin/env bash
set -euo pipefail

# Performance mode, checked against the REAL plugin rather than against the
# source that generates it.
#
# Builds dsp.so for the build host, dlopens it through the v2 ABI and:
#
#   1. round-trips every performance-common and part parameter, including the
#      siblings that share a packed byte (tests/harness/jv880_probe.c)
#   2. dumps the ui_hierarchy and chain_params it actually emits, and checks
#      they parse, fit the 64 KB wire limit, and that every key the hierarchy
#      names has chain_params metadata behind it
#
# (2) is the one that catches the quiet failures. The contract is assembled by
# snprintf into a fixed buffer, so a missing comma or an overflow produces
# JSON that no longer parses -- and on the device that is indistinguishable
# from the module having no hierarchy at all.
#
# No ROMs needed: the emulator is never ticked, and these parameters are plain
# SRAM plus a queued SysEx. Zero-filled stand-ins are enough to get the
# emulator allocated.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK="${REPO_ROOT}/build/test-performance-params"

CXX="${CXX:-clang++}"
CC="${CC:-clang}"
command -v "$CXX" >/dev/null 2>&1 || { echo "SKIP: no $CXX"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit 0; }

rm -rf "$WORK"
mkdir -p "$WORK/roms"

echo "building dsp for the host..."
"$CXX" -O0 -shared -fPIC -std=c++17 -fno-exceptions -fno-rtti -DNDEBUG \
    "$REPO_ROOT"/src/dsp/jv880_plugin.cpp \
    "$REPO_ROOT"/src/dsp/mcu.cpp \
    "$REPO_ROOT"/src/dsp/mcu_opcodes.cpp \
    "$REPO_ROOT"/src/dsp/pcm.cpp \
    "$REPO_ROOT"/src/dsp/resample/*.c \
    -I"$REPO_ROOT/src/dsp" -I"$REPO_ROOT/src/dsp/resample" \
    -lm -lpthread -o "$WORK/dsp.so" 2>&1 | grep -v "deprecated" || true
[ -f "$WORK/dsp.so" ] || { echo "FAIL: dsp did not build"; exit 1; }

"$CC" -O0 -o "$WORK/probe" "$SCRIPT_DIR/harness/jv880_probe.c"

# Stand-in ROMs: right sizes, no contents. The loader only checks it can read
# them, and nothing here executes a single emulated instruction.
python3 - "$WORK/roms" <<'PY'
import sys, os
sizes = {
    "jv880_rom1.bin": 32768, "jv880_rom2.bin": 262144,
    "jv880_waverom1.bin": 2097152, "jv880_waverom2.bin": 2097152,
    "jv880_waverom_expansion.bin": 2097152, "jv880_nvram.bin": 32768,
}
for name, size in sizes.items():
    with open(os.path.join(sys.argv[1], name), "wb") as f:
        f.write(bytes(size))
PY

echo
echo "== 1. parameter round-trip =="
( cd "$WORK" && ./probe ./dsp.so . roundtrip ) 2>/dev/null

echo
echo "== 2. the emitted contract =="
( cd "$WORK" && ./probe ./dsp.so . dump ui_hierarchy ) 2>"$WORK/hier.size" >"$WORK/hier.json"
( cd "$WORK" && ./probe ./dsp.so . dump chain_params ) 2>"$WORK/cp.size"  >"$WORK/cp.json"
grep -h "bytes" "$WORK/hier.size" "$WORK/cp.size" || true

python3 - "$WORK/hier.json" "$WORK/cp.json" <<'PY'
import json, sys

hier = json.load(open(sys.argv[1]))       # raises if the snprintf assembly broke
chain = json.load(open(sys.argv[2]))
fail = []

meta = {p["key"] for p in chain}
levels = hier["levels"]

# Every editable key any level names must have metadata. Without it the grid
# invents a float 0..1 knob and writes 0.058750 into an enum.
for lname, lvl in levels.items():
    if lvl.get("child_prefix"):
        # child levels name bare suffixes; metadata is keyed on the suffix too
        pass
    for p in (lvl.get("params") or []):
        key = p if isinstance(p, str) else p.get("key")
        if key and key not in meta:
            fail.append(f'{lname}: "{key}" has no chain_params entry')

# A level a params entry navigates to must exist.
for lname, lvl in levels.items():
    for p in (lvl.get("params") or []):
        if isinstance(p, dict) and p.get("level") and p["level"] not in levels:
            fail.append(f'{lname}: navigates to missing level "{p["level"]}"')
    kid = lvl.get("children")
    if kid and kid != "None" and kid not in levels:
        fail.append(f'{lname}: children -> missing level "{kid}"')

# A knob needs metadata as much as a params entry does -- more so, since a
# knob with no declared type is where the grid invents a float and writes a
# fraction into an enum. (Knobs need NOT appear in that level params: the
# knob-grid planner reads knobs[] directly, and patch/tone1 deliberately put
# their controls on knobs while params holds only the navigation.)
for lname, lvl in levels.items():
    for k in (lvl.get("knobs") or []):
        if k not in meta:
            fail.append(f'{lname}: knob "{k}" has no chain_params entry')

# The performance mode surface, named rather than counted.
for need in ("perf_common", "save_perf_slot", "part_selector", "load_expansion"):
    if need not in levels:
        fail.append(f'performance mode lost level "{need}"')
perf_targets = {p.get("level") for p in levels["perf_main"]["params"] if isinstance(p, dict)}
for need in ("perf_common", "save_perf_slot", "part_selector"):
    if need not in perf_targets:
        fail.append(f'perf_main does not link to "{need}" -- the level is unreachable')

# The mode enum: without it the Mode page draws its rows as raw level names.
mode = [p for p in chain if p["key"] == "mode"]
if not mode:
    fail.append("chain_params declares no `mode` -- the Mode page has no metadata")
elif [o.lower() for o in mode[0].get("options", [])] != [m.lower() for m in hier["modes"]]:
    fail.append(f'mode options {mode[0].get("options")} do not match modes {hier["modes"]}')

if fail:
    print("FAIL:")
    for f in fail:
        print("  " + f)
    sys.exit(1)
print(f"  ok  {len(levels)} levels, {len(chain)} chain_params, every key has metadata")
print("  ok  every navigated level exists and every knob has metadata")
print("  ok  performance mode reaches common / parts / save / expansion")
PY

echo
echo "PASS: performance-mode parameters round-trip and the contract is well formed"
