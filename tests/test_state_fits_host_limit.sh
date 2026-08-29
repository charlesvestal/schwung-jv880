#!/bin/bash
set -euo pipefail

# get_param("state") must still FIT once Schwung has written it to disk.
#
# The saved state is not stored the way we emit it. shadow_ui.js JSON.parses
# our object into the slot patch and re-serialises the whole file with
# JSON.stringify(wrapper, null, 2), so every field arrives on disk with a
# newline, the indent for its nesting depth, and a space after its colon.
# chain_patch.c then refuses to load a state at or over MAX_SYNTH_STATE_LEN
# (16384) -- and the guard has no else, so the state is dropped in SILENCE,
# the module never receives it, and it falls back to preset 0, "A.Piano 1".
#
# Bounding the emission against our own byte count is what let that happen:
# 364 fields is 12,227 bytes here and 16,604 in the file. This test measures
# the FILE size, using the real key names taken from the source, so:
#
#   - adding tone/patch-common params can no longer silently cross the limit;
#   - lowering MARGIN or PRETTY_OVERHEAD below what the host actually costs
#     fails here rather than on a user's device a set-switch later.
#
# Issues: charlesvestal/schwung-jv880#11, #8.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FILE="${REPO_ROOT}/src/dsp/jv880_plugin.cpp"

command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 required"; exit 1; }

python3 - "$FILE" <<'PY'
import json, re, sys

path = sys.argv[1]
src = open(path).read()
fails = []

def ok(cond, what):
    print(("  ok  " if cond else "FAIL: ") + what)
    if not cond:
        fails.append(what)

def const(name):
    m = re.search(r'const int ' + name + r'\s*=\s*(\d+)', src)
    return int(m.group(1)) if m else None

def entries(name):
    m = re.search(re.escape(name) + r'\s*\[\s*\]\s*=\s*\{', src)
    if not m:
        return []
    i, depth, start = m.end(), 1, m.end()
    while depth and i < len(src):
        if src[i] == '{': depth += 1
        elif src[i] == '}': depth -= 1
        i += 1
    return re.findall(r'\{\s*"([A-Za-z0-9_]+)"', src[start:i-1])

# ---- the budget must exist, and be expressed against the HOST's limit ------
PRETTY_OVERHEAD = const("PRETTY_OVERHEAD")
HOST_LIMIT      = const("HOST_STATE_LIMIT")
MARGIN          = const("MARGIN")

ok(PRETTY_OVERHEAD is not None,
   "the flat-field budget charges a per-field PRETTY_OVERHEAD")
ok(HOST_LIMIT == 16384,
   "the budget names Schwung's MAX_SYNTH_STATE_LEN (16384), not a local guess")
ok(MARGIN is not None and MARGIN > 0,
   "the budget keeps a margin for deeper nesting and longer values")

if fails:
    print("FAILURES: %d" % len(fails)); sys.exit(1)

macros, common, tone = entries("MACRO_DEFS"), entries("PATCH_COMMON_PARAMS"), entries("TONE_PARAMS")
ok(len(macros) and len(common) and len(tone),
   "parameter tables were found in the source (%d macros, %d common, %d tone)"
   % (len(macros), len(common), len(tone)))

# ---- simulate the emission loop, then measure it the way the HOST stores it -
WORST_VALUE = -100          # widest plausible native int text
PATCH_HEX_BYTES = 0x16a     # PATCH_SIZE

state = {"version": 2, "mode": 0, "preset": 137, "performance": 0, "part": 0,
         "octave_transpose": 0, "expansion_index": 0, "expansion_bank_offset": 0,
         "macro_mode": 0, "patch": "A" * (PATCH_HEX_BYTES * 2)}

written = len(json.dumps(state, separators=(",", ":")))
projected = written + PRETTY_OVERHEAD * 12
SAFE = HOST_LIMIT - MARGIN

def emit(key):
    """Mirror the module's loop: stop once the projected FILE size is spent."""
    global written, projected
    if projected >= SAFE:
        return False
    frag = ',"%s":%d' % (key, WORST_VALUE)
    written += len(frag)
    projected += len(frag) + PRETTY_OVERHEAD
    state[key] = WORST_VALUE
    return True

emitted = 0
for m in macros:
    if emit("macro_" + m): emitted += 1
for c in common:
    if emit("nvram_patchCommon_" + c): emitted += 1
for t in range(4):
    for p in tone:
        if emit("nvram_tone_%d_%s" % (t, p)): emitted += 1

# How Schwung actually writes it: the state object nested inside the slot file.
wrapper = {"name": "Untitled", "version": 1, "modified": False,
           "chain": {"synth": {"module": "minijv", "preset": 137,
                               "config": {"state": state}},
                     "midi_fx": [], "audio_fx": []}}
pretty = json.dumps(wrapper, indent=2)

i = pretty.index('"state": {') + pretty[pretty.index('"state": {'):].index('{')
d, j = 0, i
while j < len(pretty):
    if pretty[j] == '{': d += 1
    elif pretty[j] == '}':
        d -= 1
        if d == 0:
            j += 1
            break
    j += 1
on_disk = j - i

total_fields = len(macros) + len(common) + 4 * len(tone)
print("      fields available %d, emitted %d" % (total_fields, emitted))
print("      compact %d bytes, on disk %d bytes, limit %d"
      % (written, on_disk, HOST_LIMIT))

ok(on_disk < HOST_LIMIT,
   "the saved state fits Schwung's limit once pretty-printed (%d < %d)"
   % (on_disk, HOST_LIMIT))

# The patch hex is the authoritative save and must never be what gets dropped.
ok("patch" in state and len(state["patch"]) == PATCH_HEX_BYTES * 2,
   "the working-patch hex is emitted in full -- only the UI fields are budgeted")

# ---- and the startup race: no state may be reported before the list exists --
# The GET arm specifically: the SET arm also tests loading_complete (it queues
# a deferred restore there), so the window is anchored on the get_param body
# and stops at the first emission of the state object.
gm = re.search(r'if \(strcmp\(key, "state"\) == 0\) \{(.*?)snprintf\(buf, buf_len,\s*\n?\s*"\{\\"version', src, re.S)
ok(bool(gm) and re.search(r'!inst->loading_complete\s*\)\s*\{\s*return\s+-1\s*;', gm.group(1)),
   'get_param("state") refuses (-1) until loading_complete, so autosave cannot '
   'persist the startup preset 0')

if fails:
    print("FAILURES: %d" % len(fails)); sys.exit(1)
print("PASS: state survives Schwung's on-disk encoding")
PY
