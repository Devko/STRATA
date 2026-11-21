#!/usr/bin/env bash
# Builds STRATA into an Enaga app package (APP.ENA).
#
# Enaga's build_app.py compiles every *.cpp and *.c in ONE flat directory, with only
# Enaga's own include path -- it uses glob("*.cpp"), not rglob, so subdirectories are
# invisible to it. So this script stages core/, dsp/ and app/ flat into one directory
# first. Two consequences, both enforced below:
#
#   1. Filenames must be unique across core/, dsp/ and app/. Staging is a flat copy, so
#      two files with the same name would silently overwrite each other and one
#      translation unit would vanish from the build. That is checked and refused.
#   2. Includes use bare filenames: #include "pattern.hpp", never "core/pattern.hpp".
#
# Usage: tools/build.sh [--name NAME] [--app-version N] [-- <extra build_app.py args>]
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

APP_NAME="STRATA"
APP_VERSION=""
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --name)        APP_NAME="$2"; shift 2 ;;
        --app-version) APP_VERSION="$2"; shift 2 ;;
        --)            shift; EXTRA=("$@"); break ;;
        *)             echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

SRC_DIRS=(core dsp app)
STAGE="$ENAGA_BUILD/stage/$APP_NAME"
OUT="$ENAGA_BUILD/$APP_NAME/APP.ENA"

# --- collect sources, refusing duplicate basenames ------------------------
declare -A OWNER=()
DUPES=0
TOTAL=0

for dir in "${SRC_DIRS[@]}"; do
    [ -d "$dir" ] || continue
    while IFS= read -r -d '' path; do
        base="$(basename "$path")"
        if [ -n "${OWNER[$base]:-}" ]; then
            if [ "$DUPES" -eq 0 ]; then
                echo "error: duplicate filenames across ${SRC_DIRS[*]}" >&2
                echo "       staging is a flat copy, so these would overwrite each other:" >&2
            fi
            echo "       $base" >&2
            echo "         ${OWNER[$base]}" >&2
            echo "         $path" >&2
            DUPES=$((DUPES + 1))
        else
            OWNER[$base]="$path"
            TOTAL=$((TOTAL + 1))
        fi
    done < <(find "$dir" -maxdepth 1 -type f \
                 \( -name '*.cpp' -o -name '*.c' -o -name '*.hpp' -o -name '*.h' \) -print0)
done

if [ "$DUPES" -gt 0 ]; then
    echo "       rename one of each pair; filenames are a flat namespace here." >&2
    exit 1
fi

if [ "$TOTAL" -eq 0 ]; then
    echo "error: no sources found in ${SRC_DIRS[*]}" >&2
    exit 1
fi

# The structural check above needs no compiler, so it runs first: `tools/build.sh` is then
# still useful for checking the flat-namespace rule before a toolchain exists.
strata_require_toolchain

# --- stage ----------------------------------------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE"
for base in "${!OWNER[@]}"; do
    cp "${OWNER[$base]}" "$STAGE/$base"
done

SOURCES=$(find "$STAGE" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' \) | wc -l)
HEADERS=$((TOTAL - SOURCES))
echo "staged: $SOURCES sources, $HEADERS headers -> $STAGE"

# --- build ----------------------------------------------------------------
ARGS=("$STAGE" -o "$OUT" --name "$APP_NAME")
[ -n "$APP_VERSION" ] && ARGS+=(--app-version "$APP_VERSION")
[ ${#EXTRA[@]} -gt 0 ] && ARGS+=("${EXTRA[@]}")

echo "building with $(strata_cgt_label) ..."
# build_app.py writes -o straight out without creating its parent, so make it first.
mkdir -p "$(dirname "$OUT")"
python3 -B "$ENAGA_ROOT/tools/build_app.py" "${ARGS[@]}"

# Copy the package back into the repo so it is reachable from Windows for the SD card.
mkdir -p build
cp "$OUT" "build/APP.ENA"

printf 'BUILD OK  %s  %s bytes  (%s)\n' \
    "build/APP.ENA" "$(stat -c %s build/APP.ENA)" "$(strata_cgt_label)"
echo "card path: /ENAGA/APPS/$APP_NAME/APP.ENA"
