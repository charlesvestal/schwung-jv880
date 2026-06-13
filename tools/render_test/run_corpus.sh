#!/usr/bin/env bash
# run_corpus.sh — render baseline corpus (10 WAVs)
# Usage:  bash tools/render_test/run_corpus.sh [roms_dir] [out_dir]
# Defaults: roms_dir = <repo>/roms,  out_dir = tools/render_test/baseline

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOL_DIR="$REPO_ROOT/tools/render_test"
BINARY="$TOOL_DIR/render_test"

ROMS_DIR="${1:-$REPO_ROOT/roms}"
OUT_DIR="${2:-$TOOL_DIR/baseline}"

if [ ! -x "$BINARY" ]; then
    echo "render_test not found or not executable — building first..."
    bash "$TOOL_DIR/build.sh"
fi

mkdir -p "$OUT_DIR"

# Corpus: patches 0,10,25,40,63  ×  sequences melody,chord
PATCHES=(0 10 25 40 63)
SEQS=(melody chord)

echo "==> Rendering corpus into $OUT_DIR"
for PATCH in "${PATCHES[@]}"; do
    for SEQ in "${SEQS[@]}"; do
        OUTFILE="$OUT_DIR/p${PATCH}_${SEQ}.wav"
        echo -n "  patch=${PATCH} seq=${SEQ} -> $OUTFILE  "
        "$BINARY" "$ROMS_DIR" "$OUTFILE" --patch "$PATCH" --seq "$SEQ"
        # Print quick stats
        if command -v python3 &>/dev/null; then
            python3 - "$OUTFILE" <<'PYEOF'
import sys, struct, math
path = sys.argv[1]
with open(path, 'rb') as f:
    f.seek(44)
    data = f.read()
n = len(data) // 4
if n == 0:
    print("  WARN: empty!")
    sys.exit(0)
samples = struct.unpack(f'<{n*2}h', data[:n*4])
L = samples[0::2]; R = samples[1::2]
peak = max(abs(max(L)), abs(min(L)), abs(max(R)), abs(min(R)))
rms_l = math.sqrt(sum(x*x for x in L) / len(L))
rms_r = math.sqrt(sum(x*x for x in R) / len(R))
def dbfs(x): return 20*math.log10(x/32768+1e-12)
print(f"  peak={dbfs(peak):+.1f}dBFS  rms=({dbfs(rms_l):+.1f},{dbfs(rms_r):+.1f})dBFS  frames={n}")
PYEOF
        fi
    done
done

echo ""
echo "==> Corpus complete: $(ls "$OUT_DIR"/*.wav 2>/dev/null | wc -l | tr -d ' ') WAV files in $OUT_DIR"
echo "    To verify future renders: cmp baseline/pN_seq.wav out/pN_seq.wav"
