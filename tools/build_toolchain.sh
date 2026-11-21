#!/usr/bin/env bash
# Builds the tic6x-elf cross toolchain that tools/build.sh needs. Run once, from WSL.
#
# Enaga's own scripts install into their own directory, which here would be inside the
# submodule on /mnt/d. A GCC build does enough small-file I/O that building it across the 9p
# mount takes many times longer, so the scripts are copied to $HOME first and the whole build
# and install stay in the WSL filesystem. tools/env.sh then finds the result through
# ENAGA_TOOLCHAIN, which is exactly what Enaga's paths.py consults.
#
# No sudo is needed: GCC fetches and builds GMP, MPFR, MPC and ISL in-tree with its own
# contrib/download_prerequisites, and MAKEINFO=true skips the missing texinfo.
#
# Binutils takes a few minutes, GCC 10-30. Progress goes to $TC/build.log.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/enaga/tools"
TC="${STRATA_TOOLCHAIN_WORK:-$HOME/strata-toolchain}"
LOG="$TC/build.log"

if [ ! -f "$SRC/build_tic6x_binutils.sh" ]; then
    echo "error: no Enaga submodule at $SRC" >&2
    echo "       git submodule update --init --recursive" >&2
    exit 1
fi

mkdir -p "$TC"
cp "$SRC/build_tic6x_binutils.sh" "$SRC/build_tic6x_gcc.sh" "$TC/"
cp -r "$SRC/patches" "$TC/"
chmod +x "$TC"/build_tic6x_*.sh

echo "building tic6x toolchain into $TC/tic6x"
echo "log: $LOG"

{
    echo "=== started $(date -Is) ==="
    echo "--- binutils ---"
    "$TC/build_tic6x_binutils.sh" || exit 1
    echo "--- gcc ---"
    "$TC/build_tic6x_gcc.sh" || exit 1
    echo "=== finished $(date -Is) ==="
} >> "$LOG" 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "toolchain build FAILED; tail of $LOG:" >&2
    tail -40 "$LOG" >&2
    exit 1
fi

echo "OK: $("$TC/tic6x/bin/tic6x-elf-gcc" --version | head -1)"
echo "OK: $("$TC/tic6x/bin/tic6x-elf-g++" --version | head -1)"
