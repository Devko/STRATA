#!/usr/bin/env bash
# STRATA build environment. Sourced by tools/build.sh and tools/panel.sh.
#
# Run everything from WSL (or Linux). Enaga's tools are Linux-only: there is no
# Windows build path, and this repo lives on /mnt/d, so the heavy artefacts are
# deliberately kept off the 9p mount.

STRATA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export STRATA_ROOT
export ENAGA_ROOT="$STRATA_ROOT/third_party/enaga"

# The git and gh that can work on this checkout. On Roland's PC each thread has a worktree made
# by Windows git, whose .git names a Windows path (gitdir: D:/...): WSL's git cannot read it, and
# gh and the GitHub credentials exist only on Windows. From WSL, the scripts that need git
# (tools/enaga_check.sh, tools/publish_release.sh) then run Windows' own git.exe and gh.exe,
# which WSL starts like any program. Set STRATA_GIT / STRATA_GH to choose yourself.
if [ -z "${STRATA_GIT:-}" ]; then
    STRATA_GIT=git STRATA_GH=gh
    if [ -f "$STRATA_ROOT/.git" ] && grep -qE '^gitdir: [A-Za-z]:/' "$STRATA_ROOT/.git" &&
       grep -qi microsoft /proc/version 2>/dev/null; then
        STRATA_GIT=git.exe STRATA_GH=gh.exe
    fi
fi
: "${STRATA_GH:=gh}"
export STRATA_GIT STRATA_GH

# The cross toolchain lives in the WSL filesystem, not in the repo: a GCC build
# does enough small-file I/O that building it on /mnt/d takes many times longer.
# Built by tools/build_toolchain.sh.
: "${ENAGA_TOOLCHAIN:=$HOME/strata-toolchain/tic6x}"
export ENAGA_TOOLCHAIN

# The main checkout behind a git worktree, printed as a path this shell can open: "" when
# STRATA_ROOT is the main checkout itself. A worktree's .git is a file, `gitdir: <main>/.git/
# worktrees/<name>`, and one made by Windows git names <main> as D:/..., which WSL sees as /mnt/d.
strata_main_checkout() {
    local line dir
    [ -f "$STRATA_ROOT/.git" ] || return 0
    line=$(head -n1 "$STRATA_ROOT/.git")
    dir="${line#gitdir: }"
    dir="${dir%%$'\r'}"
    case "$dir" in
        [A-Za-z]:/*) [ -d /mnt ] && dir="/mnt/$(printf '%s' "${dir:0:1}" | tr 'A-Z' 'a-z')/${dir:3}" ;;
        /*) ;;
        *) dir="$STRATA_ROOT/$dir" ;;
    esac
    dir="${dir%/.git/worktrees/*}"
    if [ -d "$dir/.git" ]; then (cd "$dir" && pwd); fi
}

# The emulator is a plain clone, not a submodule: Enaga's tools take it by env var.
# It must be the directory holding cpu.py. A worktree has no clone of its own (it is not
# tracked), so it borrows the main checkout's: without this every scenario run in a worktree
# SKIPPED, and tools/scenarios.sh used to report those as passes.
if [ -z "${ENAGA_EMULATOR:-}" ]; then
    ENAGA_EMULATOR="$STRATA_ROOT/third_party/smpltrek-re/emulator"
    if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
        strata_main=$(strata_main_checkout)
        if [ -n "$strata_main" ] && [ -f "$strata_main/third_party/smpltrek-re/emulator/cpu.py" ]; then
            ENAGA_EMULATOR="$strata_main/third_party/smpltrek-re/emulator"
        fi
        unset strata_main
    fi
fi
export ENAGA_EMULATOR

# The emulator commit STRATA's scenarios were last checked against. A plain clone has no pin of
# its own, so this is it: tools/fetch_emulator.sh checks it out and tools/enaga_check.sh fails
# when the clone is elsewhere. Move it in the same commit as anything that depends on it.
STRATA_EMULATOR_URL="https://github.com/seawaffle/smpltrek-re"
STRATA_EMULATOR_COMMIT="11ac9a0ae1fb5126728950792504c9e304257101"

# Build output, also off /mnt/d.
: "${ENAGA_BUILD:=$HOME/strata-build}"
export ENAGA_BUILD

# TI's compiler is worth about 2.2x on DSP code and the 60% load target depends on
# it (docs/SPEC.md, "CPU and memory budget"). paths.py picks the newest ~/ti/ti-cgt-c6000_*
# on its own; set ENAGA_TI_CGT=none to force the GCC-only build, which every load
# measurement must also be recorded against.
if [ -n "${ENAGA_TI_CGT:-}" ]; then
    export ENAGA_TI_CGT
elif ! compgen -G "$HOME/ti/ti-cgt-c6000_*" > /dev/null 2>&1; then
    export ENAGA_TI_CGT=none
fi

strata_require_toolchain() {
    if [ ! -x "$ENAGA_TOOLCHAIN/bin/tic6x-elf-gcc" ]; then
        echo "error: no tic6x toolchain at $ENAGA_TOOLCHAIN" >&2
        echo "       build it with tools/build_toolchain.sh (about 30 minutes)" >&2
        return 1
    fi
}

strata_cgt_label() {
    if [ "${ENAGA_TI_CGT:-}" = "none" ]; then echo "gcc-only"; else echo "cl6x"; fi
}

# The emulator disassembles code with tic6x binutils that it locates at a hardcoded
# <its repo>/tools/tic6x/bin -- there is no environment variable for it (emulator/core_demo.py).
# Our toolchain lives in the WSL filesystem instead, so link it into place. Called by the
# scripts that run the emulator, so it self-heals whichever order things were set up in.
strata_link_emulator_toolchain() {
    local emu_repo link
    emu_repo="$(dirname "$ENAGA_EMULATOR")"
    link="$emu_repo/tools/tic6x"

    [ -d "$emu_repo" ] || return 0
    [ -x "$ENAGA_TOOLCHAIN/bin/tic6x-elf-objdump" ] || return 0

    if [ -e "$link" ] && [ ! -L "$link" ]; then
        return 0                      # a real toolchain is already there; leave it alone
    fi
    if [ -L "$link" ] && [ -x "$link/bin/tic6x-elf-objdump" ]; then
        return 0                      # already linked and working
    fi

    mkdir -p "$emu_repo/tools"
    ln -sfn "$ENAGA_TOOLCHAIN" "$link"
    echo "linked emulator binutils: $link -> $ENAGA_TOOLCHAIN"
}
