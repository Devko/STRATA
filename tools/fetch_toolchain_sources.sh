#!/usr/bin/env bash
# Pre-fetch the sources tools/build_toolchain.sh needs, over HTTPS and with retries.
#
# Run this BEFORE tools/build_toolchain.sh in any environment whose outbound traffic goes through
# an HTTPS-only proxy -- which is every Claude Code cloud session. Two reasons it is needed:
#
#   1. GCC's own contrib/download_prerequisites fetches GMP, MPFR, MPC and ISL from
#      http://gcc.gnu.org/... over PLAIN HTTP. An HTTPS-only proxy does not serve that at all
#      (it answers 405), so the prerequisites step fails and takes the GCC build with it. This
#      script fetches the same four archives over HTTPS and lays them out exactly as
#      download_prerequisites would -- extracted and symlinked next to the GCC source -- which
#      is what makes build_tic6x_gcc.sh skip the step entirely.
#   2. A proxy that has just been told about a new allowed host can refuse the first request for
#      a minute or so, and the Enaga scripts fetch with a bare `curl -sSL` that gives up at
#      once. These fetches retry.
#
# Idempotent: anything already downloaded and intact is left alone, so it is safe to re-run after
# a partial failure. Nothing here is built; the build is still tools/build_toolchain.sh.
#
# It deliberately does NOT live in third_party/enaga, which CLAUDE.md forbids modifying, and it
# deliberately does not patch Enaga's scripts -- it just puts the files where they already look.
set -uo pipefail

TC="${STRATA_TOOLCHAIN_WORK:-$HOME/strata-toolchain}"
GCC_VERSION="${GCC_VERSION:-13.3.0}"
BINUTILS_VERSION="2.42"

# The prerequisite versions GCC 13.3.0's own download_prerequisites asks for. Read them from the
# script rather than hard-coding, so a different GCC gets its own.
PREREQS="gmp-6.2.1.tar.bz2 mpfr-4.1.0.tar.bz2 mpc-1.2.1.tar.gz isl-0.24.tar.bz2"

fetch() {   # fetch <url> <path>
    local url="$1" out="$2" i
    if [ -s "$out" ]; then
        echo "  have $(basename "$out")"
        return 0
    fi
    for i in 1 2 3 4 5; do
        if curl -sSL --retry 3 --retry-all-errors --retry-delay 3 --max-time 900 -o "$out" "$url"; then
            echo "  got  $(basename "$out") ($(stat -c %s "$out") bytes)"
            return 0
        fi
        echo "  attempt $i failed for $(basename "$out")" >&2
        sleep 8
    done
    echo "error: could not fetch $url" >&2
    return 1
}

mkdir -p "$TC/binutils-build" "$TC/gcc-build" || exit 1

echo "binutils $BINUTILS_VERSION into $TC/binutils-build"
fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
      "$TC/binutils-build/binutils-$BINUTILS_VERSION.tar.xz" || exit 1

echo "gcc $GCC_VERSION into $TC/gcc-build"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
      "$TC/gcc-build/gcc-$GCC_VERSION.tar.xz" || exit 1

cd "$TC/gcc-build" || exit 1
[ -d "gcc-$GCC_VERSION" ] || tar xf "gcc-$GCC_VERSION.tar.xz" || exit 1
cd "gcc-$GCC_VERSION" || exit 1

echo "gcc's prerequisites, over HTTPS rather than its own plain HTTP"
for archive in $PREREQS; do
    fetch "https://gcc.gnu.org/pub/gcc/infrastructure/$archive" "$archive" || exit 1
    base="${archive%.tar.*}"           # gmp-6.2.1
    short="${base%%-*}"                # gmp
    [ -d "$base" ] || tar xf "$archive" || exit 1
    ln -sfn "$base" "$short"
done

echo
echo "sources ready. build_tic6x_gcc.sh will now skip download_prerequisites, because"
echo "$TC/gcc-build/gcc-$GCC_VERSION/gmp exists."
echo "next: tools/build_toolchain.sh   (binutils a few minutes, gcc 10-30)"
