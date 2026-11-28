#!/usr/bin/env bash
# Offline kick + rumble lab: renders the KICK presets on the host and the rumble variants in
# numpy (tools/kick_lab/lab.py). Sound design only; nothing here builds for the unit.
#
#   tools/kick_lab/run.sh [OUTDIR]        # default build/kick-lab
# Needs python3 with numpy and scipy.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${1:-build/kick-lab}"
make -C host build -j8 > /dev/null
B="${ENAGA_BUILD:-$PWD/build}/host"
mkdir -p "$OUT"
g++ -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -O2 -Icore -Idsp -Iapp -Itests \
    -Ithird_party/enaga/include tools/kick_lab/kick_render.cpp "$B"/core/*.o "$B"/dsp/*.o \
    "$B"/third_party/enaga/runtime/dsp/*.o -o "$OUT/kick_render"
python3 -B tools/kick_lab/lab.py "$OUT/kick_render" "$OUT"
