#!/usr/bin/env bash
# Build a STRATA project from als2strata.py's output folder. Run in WSL.
#
#   tools/midi_import/build_import.sh OUTDIR [--no-render] [--stems]
#
# OUTDIR holds project.inc and samples/ (als2strata.py MAP.json OUTDIR). Compiles
# import_project.cpp against the host build's objects with that project.inc, writes
# OUTDIR/card/ENAGA/DATA/STRATA/PROJECTS/<PROJECT>/*.BIN, copies the samples to
# OUTDIR/card/SAMPLES/<PROJECT>/ (the paths the kit names), and renders the chain into
# OUTDIR/audio/ unless --no-render.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tools/midi_import"
export ENAGA_BUILD=${ENAGA_BUILD:-$ROOT/build}
[ $# -ge 1 ] || { echo "usage: $0 OUTDIR [--no-render] [--stems]" >&2; exit 2; }
OUT=$(cd "$1" && pwd)
shift
[ -f "$OUT/project.inc" ] || { echo "error: no $OUT/project.inc (run als2strata.py first)" >&2; exit 1; }

make -C "$ROOT/host" build -j"$(nproc)" > "$ENAGA_BUILD/import_hostbuild.log" 2>&1 || {
    tail -20 "$ENAGA_BUILD/import_hostbuild.log" >&2; exit 1; }
OBJS=$(find "$ENAGA_BUILD/host" -name '*.o' | grep -v -e '/tests/test_' -e '/tests/runner.o' -e '/tests/apprig.o' -e '/host/host/')

# project.inc names its WAVs by Windows path when als2strata.py ran there: read them from WSL.
sed -E 's#"([A-Za-z]):[\\/]#"/mnt/\L\1/#; s#\\\\#/#g' "$OUT/project.inc" > "$ENAGA_BUILD/import_project.inc"
mkdir -p "$ENAGA_BUILD/import_inc"
cp "$ENAGA_BUILD/import_project.inc" "$ENAGA_BUILD/import_inc/project.inc"

# shellcheck disable=SC2086  # OBJS is a list of paths without spaces
g++ -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -O2 \
    -I"$ENAGA_BUILD/import_inc" -I"$ROOT/core" -I"$ROOT/dsp" -I"$ROOT/app" -I"$ROOT/tests" \
    -I"$ROOT/third_party/enaga/include" -I"$ROOT/tools/showcases" -I"$HERE" \
    "$HERE/import_project.cpp" $OBJS -o "$ENAGA_BUILD/import_project"

name=$(sed -n 's/^constexpr const char \*kName = "\(.*\)";/\1/p' "$ENAGA_BUILD/import_project.inc")
if [ -d "$OUT/samples" ]; then
    mkdir -p "$OUT/card/SAMPLES/$name"
    cp "$OUT"/samples/*.WAV "$OUT/card/SAMPLES/$name/"
fi
"$ENAGA_BUILD/import_project" "$OUT" "$@"
