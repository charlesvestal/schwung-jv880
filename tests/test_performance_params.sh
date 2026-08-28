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
echo "== 3. packed byte 12 against real performance data =="

# The round-trip above CANNOT catch a wrong bit boundary on its own: it starts
# from a zeroed byte and only ever compares our packer against our unpacker,
# so any self-consistent layout passes. It did, and the layout was wrong.
#
# The fixture below is every distinct value of performance-common byte 12
# across the 48 factory performances, read out of a real ROM2 + NVRAM. Real
# data is the only thing that pins a field BOUNDARY, because a field that has
# drifted into its neighbour still round-trips perfectly.
python3 - "$REPO_ROOT/src/dsp/jv880_plugin.cpp" <<'PY'
import re, sys

# Distinct byte-12 values across all 48 JV-880 factory performances
# (Preset A + Preset B from ROM2, Internal from NVRAM). Bit 5 is set in none
# of them, which is what leaves room for reverbtype to be three bits wide.
OBSERVED = [0x00, 0x02, 0x03, 0x04, 0x0b, 0x0c, 0x0f, 0x40, 0x41, 0x42, 0x43,
            0x44, 0x45, 0x47, 0x4b, 0x54, 0x57, 0x80, 0x81, 0x82, 0x84, 0x87]

src = open(sys.argv[1]).read()
block = re.search(r"PERF_COMMON_PARAMS\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
if not block:
    print("FAIL: could not find PERF_COMMON_PARAMS"); sys.exit(1)

rows = re.findall(
    r'\{"(\w+)",\s*(0x[0-9A-Fa-f]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(\d+)\}',
    block.group(1))
byte12 = [(n, int(o), int(sh), int(w), int(lo), int(hi))
          for n, _sx, o, sh, w, lo, hi in rows if int(o) == 12]
if len(byte12) != 3:
    print(f"FAIL: expected 3 params packed into byte 12, found {len(byte12)}")
    sys.exit(1)

fail = []
# Fields must not overlap, or a write to one silently corrupts the other.
masks = {n: ((1 << w) - 1) << sh for n, _o, sh, w, _lo, _hi in byte12}
for a in masks:
    for b in masks:
        if a < b and masks[a] & masks[b]:
            fail.append(f"{a} and {b} overlap in byte 12")

# Every real performance must decode inside every field's declared range.
for b in OBSERVED:
    for name, _off, sh, w, lo, hi in byte12:
        v = (b >> sh) & ((1 << w) - 1)
        if not (lo <= v <= hi):
            fail.append(f"byte12=0x{b:02x} decodes {name}={v}, outside {lo}..{hi}")

# A field that is constant across 22 distinct bytes is not being read.
for name, _off, sh, w, _lo, _hi in byte12:
    if len({(b >> sh) & ((1 << w) - 1) for b in OBSERVED}) == 1:
        fail.append(f"{name} never varies across the 48 performances")

if fail:
    print("FAIL:")
    for f in dict.fromkeys(fail):
        print("  " + f)
    sys.exit(1)
print(f"  ok  {len(OBSERVED)} real byte-12 values decode in range for "
      + ", ".join(n for n, *_ in byte12))
print("  ok  the three fields do not overlap")
PY

echo
echo "PASS: performance-mode parameters round-trip and the contract is well formed"
