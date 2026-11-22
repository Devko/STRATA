#!/usr/bin/env bash
# Builds STRATA and opens Enaga's emulator panel (a web UI at http://127.0.0.1:8766)
# so the app can be played by hand: keys, pads, encoders, screen and audio.
#
# Needs the emulator checkout (third_party/smpltrek-re) that tools/env.sh points
# ENAGA_EMULATOR at. The emulator models no caches or bus latency, so its load figures
# only ever compare versions -- real numbers come from the unit (docs/SPEC.md, "Testing").
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "error: no emulator at $ENAGA_EMULATOR (expected cpu.py there)" >&2
    echo "       tools/fetch_emulator.sh" >&2
    exit 1
fi

strata_require_toolchain
strata_link_emulator_toolchain
tools/build.sh "$@"

echo "panel: http://127.0.0.1:8766  (ctrl-c to stop)"
exec python3 -B "$ENAGA_ROOT/tools/panel.py" "$ENAGA_BUILD/stage/STRATA"
