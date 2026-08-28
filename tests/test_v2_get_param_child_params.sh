#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FILE="${REPO_ROOT}/src/dsp/jv880_plugin.cpp"

# The DEFINITION, not the forward declaration near the top of the file.
start=$(rg -n "^static int v2_get_param\(.*\) \{" "$FILE" | head -n 1 | cut -d: -f1 || true)
end=$(rg -n "static int v2_get_error" "$FILE" | head -n 1 | cut -d: -f1 || true)

if [ -z "$start" ] || [ -z "$end" ]; then
    echo "FAIL: could not locate v2_get_param/v2_get_error boundaries"
    exit 1
fi

section=$(sed -n "${start},${end}p" "$FILE")

for prefix in nvram_tone_ nvram_patchCommon_ sram_part_ sram_perfCommon_; do
    # A here-string, not a pipe: `rg -q` exits on the first match and closes
    # the pipe, so `echo` takes SIGPIPE and `set -o pipefail` fails the whole
    # test depending on how much it had already written -- a race that made
    # this pass or fail on the same tree.
    if ! rg -q "$prefix" <<<"$section"; then
        echo "FAIL: v2_get_param no longer handles $prefix"
        exit 1
    fi
done

echo "PASS: v2_get_param handles child parameter prefixes"
