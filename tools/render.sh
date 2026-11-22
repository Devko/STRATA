#!/usr/bin/env bash
# Builds STRATA and renders its audio to a WAV you can play on the host.
#
#   tools/render.sh                       # 16 s into $ENAGA_BUILD/strata.wav
#   tools/render.sh --seconds 32          # longer
#   tools/render.sh -o /tmp/beat.wav      # somewhere else
#   tools/render.sh --key 36@400000000    # press STEP 5 part-way through
#
# Why this exists rather than the web panel is in tools/render.py's header: the panel runs at
# about half real time, so it breaks up, and Enaga's own emulate.py keeps only the last 0.68 s.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "error: no emulator at $ENAGA_EMULATOR (expected cpu.py there)" >&2
    echo "       tools/fetch_emulator.sh" >&2
    exit 1
fi

OUT="$ENAGA_BUILD/strata.wav"
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--out) OUT="$2"; shift 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

strata_require_toolchain
strata_link_emulator_toolchain
tools/build.sh > /dev/null

exec python3 -B tools/render.py "$ENAGA_BUILD/stage/STRATA" -o "$OUT" ${ARGS[@]+"${ARGS[@]}"}
