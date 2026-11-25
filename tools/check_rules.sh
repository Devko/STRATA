#!/usr/bin/env bash
# CLAUDE.md's source rules that a grep can hold, checked on every PR (.github/workflows/ci.yml,
# "Repo checks") and by tools/ci_local.sh.
#
#   1. core/ never includes an Enaga header. host/Makefile gives every file Enaga's include
#      path, so a core/ file that includes one still builds and nothing else notices.
#   2. No libm, STL containers, strings or streams, and no alloca, in core/, dsp/ or app/:
#      <cmath>, <math.h>, <vector>, <string>, <iostream>, <sstream>, <fstream>, <alloca.h>.
#      The math is <enaga/dsp.hpp>'s; the memory is taken in init().
#   3. Every source filename is unique across core/, dsp/ and app/: build_app.py stages one
#      flat folder. tools/build.sh refuses duplicates too, but it never runs in CI.
#
# Only #include lines and calls are looked at, so a rule named in a comment does not trip it.
#
# Usage: tools/check_rules.sh     exit 0 when every rule holds.
set -euo pipefail

cd "$(dirname "$0")/.."

fail=0
report() {   # rule, matches
    if [ -n "$2" ]; then
        echo "FAIL: $1"
        printf '%s\n' "$2" | sed 's/^/    /'
        fail=1
    else
        echo "ok    $1"
    fi
}

sources() { find "$@" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.hpp' -o -name '*.h' \) | sort; }

hits=$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<enaga/' $(sources core) || true)
report "core/ includes no Enaga header" "$hits"

banned='cmath|math\.h|vector|string|iostream|sstream|fstream|alloca\.h'
hits=$(grep -nE "^[[:space:]]*#[[:space:]]*include[[:space:]]*<($banned)>" $(sources core dsp app) || true)
report "no libm, STL containers, strings, streams or alloca.h in core/ dsp/ app/" "$hits"

hits=$(grep -nE '\balloca[[:space:]]*\(' $(sources core dsp app) | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*)' || true)
report "no alloca() calls" "$hits"

hits=$(sources core dsp app | xargs -n1 basename | sort | uniq -d)
report "filenames unique across core/ dsp/ app/" "$hits"

exit $fail
