#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FILE="${REPO_ROOT}/src/dsp/jv880_plugin.cpp"

# Match the DEFINITIONS, not the forward declarations. Both functions are
# forward-declared on adjacent lines near the top, so `head -n 1` picked those
# and the "section" was two lines of prototypes containing none of the
# prefixes below -- this test has been failing for that reason alone, not for
# anything about the code it names.
start=$(rg -n "^static void v2_set_param\(.*\) \{" "$FILE" | head -n 1 | cut -d: -f1 || true)
end=$(rg -n "^static int v2_get_param\(.*\) \{" "$FILE" | head -n 1 | cut -d: -f1 || true)

if [ -z "$start" ] || [ -z "$end" ]; then
    echo "FAIL: could not locate v2_set_param/v2_get_param boundaries"
    exit 1
fi

section=$(sed -n "${start},${end}p" "$FILE")

for prefix in nvram_tone_ nvram_patchCommon_ sram_part_ sram_perfCommon_; do
    # A here-string, not a pipe: `rg -q` exits on the first match and closes
    # the pipe, so `echo` takes SIGPIPE and `set -o pipefail` fails the whole
    # test depending on how much it had already written -- a race that made
    # this pass or fail on the same tree.
    if ! rg -q "$prefix" <<<"$section"; then
        echo "FAIL: v2_set_param no longer handles $prefix"
        exit 1
    fi
done

echo "PASS: v2_set_param handles child parameter prefixes"
