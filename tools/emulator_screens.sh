#!/usr/bin/env bash
# Redraw docs/screens/ from the device build running in the emulator (README, MANUAL, CHEATSHEET).
#
#   tools/emulator_screens.sh
#
# Builds the app (cl6x when ENAGA_TI_CGT finds it), walks the screens in five emulators side by
# side (tools/emulator_screens.py walk), draws MUTATE and SCENE on the host (the emulator has no
# pads), then picks the frames into docs/screens/*.png and overview.png. Needs ENAGA_EMULATOR.
# tools/screenshots.sh is the host-only route (no emulator, no toolchain).
set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck source=tools/env.sh
source tools/env.sh
strata_link_emulator_toolchain
tools/build.sh > /dev/null
stage="$ENAGA_BUILD/stage/STRATA"
base="$ENAGA_BUILD"

# STRATA_SCREENS_KEEP=DIR keeps every walk frame there, for a contact sheet before trusting PICKS.
host="$(mktemp -d)"
if [ -n "${STRATA_SCREENS_KEEP:-}" ]; then
    walk="$STRATA_SCREENS_KEEP"; mkdir -p "$walk"
    trap 'rm -rf "$host"' EXIT
else
    walk="$(mktemp -d)"
    trap 'rm -rf "$walk" "$host"' EXIT
fi

pids=()
for part in a b c d e f; do
    ( export ENAGA_BUILD="$base/screens-$part"; mkdir -p "$ENAGA_BUILD"
      python3 -B tools/emulator_screens.py walk "$stage" "$walk" "$part" > "$walk/log_$part.txt" 2>&1 ) &
    pids+=("$!")
done
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
if [ "$failed" -ne 0 ]; then
    tail -n 5 "$walk"/log_*.txt
    exit 1
fi

export ENAGA_BUILD="$base"
make -C host -s build
STRATA_SHOTS="$host" "$ENAGA_BUILD/host/strata-tests" --only ui_screenshots > /dev/null
python3 -B tools/emulator_screens.py pick "$walk" "$host" docs/screens
python3 -B tools/panel_diagram.py docs/screens/panel.svg
