#!/usr/bin/env bash
# Load check of a showcase card on the device build in the emulator. Run in WSL after
# tools/build.sh (cl6x when the TI compiler is installed).
#
#   tools/showcases/runtest.sh NAME [SLOT ...]     SLOT 0-5 = A01-A06, default 4 (A05)
#
# Loads build/showcases/NAME/card (tools/showcases/build.sh first), opens its one project, plays
# the slot for 8 s and prints one JSON line: peak_pct, avg_pct, p99_pct of the 496,000-cycle audio
# block, overruns. One emulator at a time: they share the card image.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
# shellcheck source=tools/env.sh
source tools/env.sh
name=$1; shift
slots=${*:-4}
card="$ROOT/build/showcases/$name/card"
[ -d "$card" ] || { echo "error: no $card (run tools/showcases/build.sh $name)" >&2; exit 1; }
for s in $slots; do
    python3 -B tools/showcases/emuload.py "$ENAGA_BUILD/stage/STRATA" "$card" "$s" 8 | grep '^{'
done
