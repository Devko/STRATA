#!/usr/bin/env bash
# Build the showcase projects from their generators. Run in WSL from anywhere.
#
#   tools/showcases/build.sh [--render] [--starter] [NAME ...]
#
# NAME is a generator (deephouse dub drift electro breaks chaos smash night steel); none = all.
# Each one writes build/showcases/<name>/card/ENAGA/DATA/STRATA/PROJECTS/<PROJECT>/*.BIN with
# core/'s own serializers and reads them back. --render also renders every pattern, the stems of
# its heaviest pattern and the chain with performance moves through the real Engine (host build)
# into build/showcases/<name>/audio/*.wav. --starter copies each project folder into
# content/starter/projects/<PROJECT>/, the copy the card builder ships.
#
# The generators are not part of the device or test build: they link against the host test
# build's objects (make -C host build) and use tests/fakehost.cpp for the clock.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tools/showcases"
export ENAGA_BUILD=${ENAGA_BUILD:-$ROOT/build}

render=0 starter=0 names=()
for a in "$@"; do
    case "$a" in
        --render) render=1 ;;
        --starter) starter=1 ;;
        -*) echo "error: unknown option $a" >&2; exit 2 ;;
        *) names+=("$a") ;;
    esac
done
[ ${#names[@]} -gt 0 ] || names=(deephouse dub drift electro breaks chaos smash night steel)

make -C "$ROOT/host" build -j"$(nproc)" > "$ENAGA_BUILD/showcases_hostbuild.log" 2>&1 || {
    tail -20 "$ENAGA_BUILD/showcases_hostbuild.log" >&2; exit 1; }
OBJS=$(find "$ENAGA_BUILD/host" -name '*.o' | grep -v -e '/tests/test_' -e '/tests/runner.o' -e '/tests/apprig.o' -e '/host/host/')

for n in "${names[@]}"; do
    out="$ROOT/build/showcases/$n"
    mkdir -p "$out"
    # shellcheck disable=SC2086  # OBJS is a list of paths without spaces
    g++ -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -O2 \
        -I"$ROOT/core" -I"$ROOT/dsp" -I"$ROOT/app" -I"$ROOT/tests" \
        -I"$ROOT/third_party/enaga/include" -I"$HERE" \
        "$HERE/$n.cpp" $OBJS -o "$ENAGA_BUILD/showcase_$n"
    if [ "$render" -eq 1 ]; then
        "$ENAGA_BUILD/showcase_$n" "$out"
    else
        "$ENAGA_BUILD/showcase_$n" "$out" --no-render
    fi
    if [ "$starter" -eq 1 ]; then
        for p in "$out"/card/ENAGA/DATA/STRATA/PROJECTS/*/; do
            dest="$ROOT/content/starter/projects/$(basename "$p")"
            mkdir -p "$dest"
            cp "$p"*.BIN "$dest/"
            echo "copied $(basename "$p") to content/starter/projects/"
        done
    fi
done
