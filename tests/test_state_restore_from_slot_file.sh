#!/usr/bin/env bash
set -euo pipefail

# An edited patch must survive a set save/reload THROUGH THE FILE Schwung
# actually writes — not just through our own compact emission.
#
# shadow_ui.js autosave nests get_param("state") in the slot wrapper and
# writes it with JSON.stringify(wrapper, null, 2). chain_patch.c does not
# re-serialize on load: it brace-matches the "state" object in the FILE TEXT
# and hands the module that raw, pretty-printed slice. So every key arrives
# as `"key": value` — a space after the colon.
#
# json_get_number's search pattern (`"key":`) happens to be a prefix of the
# pretty form, so mode/preset/octave restored. The working-patch hex did not:
# strstr(val, "\"patch\":\"") requires the quote to follow the colon
# immediately, so the pretty slice never matched, the hex restore was
# silently skipped, and every edit to the working patch was lost on set
# reload / reboot — while the correct patch number still loaded. That is
# exactly the "right patches load but modifications are missing" field
# report, and it reproduced with user presets working (their payload is
# written compact), which is what made it look like set persistence rather
# than a parser.
#
# This test runs the real pipeline on the build host: save (edit cutoff),
# store the state the way shadow_ui.js + chain_patch.c do, restore the
# pretty slice through the deferred boot path, and require the edit back.
# A compact-restore control distinguishes "parser broke on pretty JSON"
# from "round-trip broke entirely".

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK="${REPO_ROOT}/build/test-state-restore"

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

"$CC" -O0 -o "$WORK/rt" "$SCRIPT_DIR/harness/jv880_state_roundtrip.c"

# Stand-in ROMs: right sizes, no contents (see jv880_probe.c).
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

PRESET=37
CUTOFF=99

echo "== save: preset ${PRESET}, cutoff -> ${CUTOFF} =="
( cd "$WORK" && ./rt ./dsp.so . save "$PRESET" "$CUTOFF" ) \
    > "$WORK/state_compact.json" 2> "$WORK/save.log"
grep "edit readback: cutoff=${CUTOFF} preset=${PRESET}" "$WORK/save.log" \
    || { echo "FAIL: edit did not land before save"; cat "$WORK/save.log"; exit 1; }

echo "== store the way Schwung does (pretty wrapper, chain_patch slice) =="
python3 - "$WORK/state_compact.json" "$WORK/state_slice.json" <<'PY'
import json, sys
state = json.load(open(sys.argv[1]))
wrapper = {"name": "Untitled", "version": 1, "modified": False,
           "chain": {"custom_name": "Untitled", "input": "both",
                     "synth": {"module": "minijv",
                               "config": {"state": state}, "bypassed": 0},
                     "audio_fx": [], "receive_channel": 1,
                     "forward_channel": -1}}
pretty = json.dumps(wrapper, indent=2) + "\n"
i = pretty.index('"state":'); i = pretty.index("{", i)
d, j = 0, i
while j < len(pretty):
    if pretty[j] == "{": d += 1
    elif pretty[j] == "}":
        d -= 1
        if d == 0: break
    j += 1
slice_ = pretty[i:j + 1]
print("state slice as stored: %d bytes" % len(slice_))
assert len(slice_) < 16384, "state over MAX_SYNTH_STATE_LEN — dropped entirely"
open(sys.argv[2], "w").write(slice_)
PY

fails=0
check() {  # check <label> <statefile>
    local out
    out=$( cd "$WORK" && ./rt ./dsp.so . restore "$2" 0 2>/dev/null )
    if [ "$out" = "preset=${PRESET} cutoff=${CUTOFF}" ]; then
        echo "  ok  $1: $out"
    else
        echo "FAIL: $1: got '$out', want 'preset=${PRESET} cutoff=${CUTOFF}'"
        fails=$((fails + 1))
    fi
}

echo "== restore the stored (pretty) slice — the device path =="
check "pretty slice restores the edit" "state_slice.json"

echo "== control: restore the compact emission =="
check "compact state restores the edit" "state_compact.json"

[ "$fails" -eq 0 ] || exit 1
echo "PASS"
