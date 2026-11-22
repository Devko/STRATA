#!/usr/bin/env bash
# A/B the cl6x vectorisation pass: build several variants of STRATA side by side, run the same
# emulator scenarios on each, and check that every variant renders the same audio.
#
#   tools/ti_ab.sh                          the default variants and scenarios, cl6x only
#   tools/ti_ab.sh --gcc                    also every variant with ENAGA_TI_CGT=none
#   tools/ti_ab.sh --scenarios "m3 m7"      just those scenarios
#   tools/ti_ab.sh --variants "base head"   just those variants
#   tools/ti_ab.sh --base <ref>             what "base" is (default: where this branch left main)
#   tools/ti_ab.sh --render 0               skip the audio comparison
#
# Variants (docs/BUDGET.md, "The cl6x vectorisation pass", says what each one isolates):
#
#   base     the tree before the pass: <ref> as it is
#   head     this tree as committed: what merges
#   nofuse   head + STRATA_VA_FUSED 0    (the VA voice's filter, envelope and tail as three passes)
#   hints    head + STRATA_TI_HINTS 1    (MUST_ITERATE on the block loops)
#   packed   head + STRATA_TI_PACKED 1   (DOTP2 stereo pairs in SAMPLE and GRAIN)
#   all      head + both flags
#
# Each variant is a detached git worktree under $STRATA_AB_DIR (default ~/strata-ab, off /mnt/d
# for speed) with its own ENAGA_BUILD, so nothing here touches this checkout's build/ or stage.
# Enaga and the emulator are shared by symlink, read-only. The worktrees are removed at the end.
#
# The audio check renders the same seconds of the default beat from each variant at 32 bits and
# compares them with `head`. Every change in the pass is meant to be bit-exact, so anything but
# "identical" is a finding: cl6x may be reassociating float arithmetic differently once loops
# are fused, or a flag's intrinsic is wrong. The script says which and by how much; it does not
# decide whether a difference is acceptable.
#
# Paste the whole output into the thread, or into docs/BUDGET.md's table for this pass.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
# shellcheck source=tools/env.sh
source tools/env.sh

SCENARIOS="m0 m1 m3 m7 factory samples"
VARIANTS="base nofuse head hints packed all"
BASE=""
GCC=0
RENDER_SECONDS=4
while [ $# -gt 0 ]; do
    case "$1" in
        --scenarios) SCENARIOS="$2"; shift 2 ;;
        --variants)  VARIANTS="$2"; shift 2 ;;
        --base)      BASE="$2"; shift 2 ;;
        --gcc)       GCC=1; shift ;;
        --render)    RENDER_SECONDS="$2"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "error: no emulator at $ENAGA_EMULATOR (tools/fetch_emulator.sh)" >&2
    exit 1
fi
if [ "${ENAGA_TI_CGT:-}" = "none" ]; then
    echo "error: cl6x not found (~/ti/ti-cgt-c6000_*): this script is about cl6x" >&2
    exit 1
fi
strata_require_toolchain || exit 1

if [ -z "$BASE" ]; then
    git fetch -q origin main 2>/dev/null || true
    BASE=$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main)
fi
HEAD_REF=$(git rev-parse HEAD)
if ! git diff --quiet HEAD -- core dsp app; then
    echo "note: core/, dsp/ or app/ has uncommitted changes; the variants build HEAD ($HEAD_REF)," >&2
    echo "      not the working tree." >&2
fi

AB="${STRATA_AB_DIR:-$HOME/strata-ab}"
mkdir -p "$AB"
RESULTS="$AB/results.txt"
: > "$RESULTS"
EMULATOR_DIR="$(cd "$ENAGA_EMULATOR/.." 2>/dev/null && pwd)"   # the smpltrek-re clone

set_flag() {    # set_flag <tree> <NAME> <0|1>
    sed -i "s/^#define $2 [01]\$/#define $2 $3/" "$1/dsp/tihints.h"
    grep -q "^#define $2 $3\$" "$1/dsp/tihints.h" || { echo "error: could not set $2 in $1" >&2; return 1; }
}

make_tree() {   # make_tree <variant> -> prints the worktree path
    local v="$1" tree="$AB/tree-$1" ref="$HEAD_REF"
    [ "$v" = "base" ] && ref="$BASE"
    git worktree remove --force "$tree" > /dev/null 2>&1 || rm -rf "$tree"
    git worktree add -q --detach "$tree" "$ref" || return 1
    rm -rf "$tree/third_party/enaga"
    ln -s "$ROOT/third_party/enaga" "$tree/third_party/enaga"
    if [ -d "$ROOT/third_party/smpltrek-re" ]; then
        ln -s "$ROOT/third_party/smpltrek-re" "$tree/third_party/smpltrek-re"
    fi
    case "$v" in
        nofuse) set_flag "$tree" STRATA_VA_FUSED 0 ;;
        hints)  set_flag "$tree" STRATA_TI_HINTS 1 ;;
        packed) set_flag "$tree" STRATA_TI_PACKED 1 ;;
        all)    set_flag "$tree" STRATA_TI_HINTS 1 && set_flag "$tree" STRATA_TI_PACKED 1 ;;
    esac || return 1
    echo "$tree"
}

run_variant() {   # run_variant <variant> <cl6x|gcc>
    local v="$1" cc="$2" tree build
    tree="$AB/tree-$v"
    build="$AB/build-$v-$cc"
    mkdir -p "$build"
    (
        cd "$tree" || exit 1
        export ENAGA_BUILD="$build"
        export ENAGA_EMULATOR
        [ "$cc" = "gcc" ] && export ENAGA_TI_CGT=none
        if ! tools/build.sh > "$build/build.log" 2>&1; then
            echo "$v $cc build FAIL"
            tail -5 "$build/build.log" | sed 's/^/    /' >&2
            exit 0
        fi
        printf '%s %s size %s\n' "$v" "$cc" "$(stat -c %s "$build/STRATA/APP.ENA")"
        for s in $SCENARIOS; do
            [ -f "tests/scenario_$s.sh" ] || { echo "$v $cc $s missing"; continue; }
            out=$(bash "tests/scenario_$s.sh" 2>&1)
            status=$?
            line=$(printf '%s\n' "$out" | grep -E 'peak load: [0-9.]+%' | tail -1)
            pct=$(printf '%s' "$line" | grep -oE 'peak load: [0-9.]+' | grep -oE '[0-9.]+$')
            cyc=$(printf '%s' "$line" | grep -oE '\(([0-9]+) of' | grep -oE '[0-9]+')
            [ $status -eq 0 ] && ok=ok || ok=FAIL
            echo "$v $cc $s ${pct:-?} ${cyc:-?} $ok"
        done
        if [ "$RENDER_SECONDS" != "0" ]; then
            if python3 -B tools/render.py "$build/stage/STRATA" -o "$build/render.wav" \
                   --seconds "$RENDER_SECONDS" --bits 32 > "$build/render.log" 2>&1; then
                echo "$v $cc render $build/render.wav"
            else
                echo "$v $cc render FAIL"
            fi
        fi
    ) | tee -a "$RESULTS"
}

echo "base = $BASE"
echo "head = $HEAD_REF"
echo "cl6x = $(ls -d "$HOME"/ti/ti-cgt-c6000_* 2>/dev/null | tail -1)"
echo "emulator = $EMULATOR_DIR ($(git -C "$EMULATOR_DIR" rev-parse --short HEAD 2>/dev/null))"
echo

for v in $VARIANTS; do
    make_tree "$v" > /dev/null || { echo "error: no worktree for $v" >&2; exit 1; }
done

COMPILERS="cl6x"
[ "$GCC" = 1 ] && COMPILERS="cl6x gcc"
for cc in $COMPILERS; do
    for v in $VARIANTS; do
        run_variant "$v" "$cc"
    done
done

echo
python3 -B - "$RESULTS" "$VARIANTS" "$SCENARIOS" <<'PY'
import struct, sys, wave
from pathlib import Path

results, variants, scenarios = sys.argv[1], sys.argv[2].split(), sys.argv[3].split()
load, renders = {}, {}
for line in Path(results).read_text().splitlines():
    f = line.split()
    if len(f) == 6 and f[2] in scenarios:
        load[(f[0], f[1], f[2])] = (f[3], f[4], f[5])
    elif len(f) == 4 and f[2] == "render" and f[3] != "FAIL":
        renders[(f[0], f[1])] = f[3]

for cc in ("cl6x", "gcc"):
    if not any(k[1] == cc for k in load):
        continue
    print(f"Peak load per scenario, {cc} (emulator, % of the 496,000-cycle block; delta vs base)")
    print("| scenario | " + " | ".join(variants) + " |")
    print("| --- |" + " --- |" * len(variants))
    for s in scenarios:
        cells = []
        base = load.get(("base", cc, s))
        for v in variants:
            r = load.get((v, cc, s))
            if r is None:
                cells.append("-")
                continue
            pct, cyc, ok = r
            cell = pct + "%" + ("" if ok == "ok" else " FAIL")
            if v != "base" and base and base[0] != "?" and pct != "?":
                cell += f" ({float(pct) - float(base[0]):+.2f})"
            cells.append(cell)
        print(f"| {s} | " + " | ".join(cells) + " |")
    print()

sys.path.insert(0, "tools")
from render_diff import compare

for cc in ("cl6x", "gcc"):
    ref = renders.get(("head", cc))
    if ref is None:
        continue
    print(f"=== Audio against head, {cc} (32-bit render of the default beat) ===")
    for v in variants:
        if v != "head" and (v, cc) in renders:
            compare(ref, renders[(v, cc)], f"{v} vs head, {cc}")

# The same variant from both compilers. Before this pass, cl6x and GCC rendered the same audio;
# a variant whose cl6x render departs from its GCC render by more than rounding is the one cl6x
# builds wrongly.
for v in variants:
    if (v, "cl6x") in renders and (v, "gcc") in renders:
        compare(renders[(v, "cl6x")], renders[(v, "gcc")], f"=== {v}: cl6x vs gcc ===")
PY

for v in $VARIANTS; do
    git worktree remove --force "$AB/tree-$v" > /dev/null 2>&1 || true
done
git worktree prune
echo "raw results: $RESULTS"
