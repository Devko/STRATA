#!/usr/bin/env bash
# Redraw the screenshots in docs/screens/ (README, MANUAL, CHEATSHEET) from the host build, and
# the labelled panel diagram (docs/screens/panel.svg).
#
#   tools/screenshots.sh
#
# Builds the host suite, runs the ui_screenshots test alone (so the app is on its first init and
# holds the default beat), then tools/screenshots.py draws the text in Enaga's font and writes
# the PNGs. These are the app's real draw calls on the host, not the emulator or the unit.
set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck source=tools/env.sh
source tools/env.sh
make -C host -s build
bin="$ENAGA_BUILD/host/strata-tests"

dumps="$(mktemp -d)"
trap 'rm -rf "$dumps"' EXIT
STRATA_SHOTS="$dumps" "$bin" --only ui_screenshots
python3 -B tools/screenshots.py "$dumps" docs/screens
python3 -B tools/panel_diagram.py docs/screens/panel.svg
