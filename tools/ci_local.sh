#!/usr/bin/env bash
# Everything GitHub Actions CI runs (.github/workflows/ci.yml), on this machine, in parallel.
# One command, from WSL or Linux, from anywhere in the repo:
#
#   tools/ci_local.sh               host tests (GCC), ASan + UBSan, layout, repo checks
#   tools/ci_local.sh --quick       GCC host tests and repo checks only (no sanitizers)
#   tools/ci_local.sh --scenarios   ... and then every emulator scenario (tools/scenarios.sh),
#                                   which CI cannot run: it needs the tic6x toolchain, the
#                                   emulator and, for cl6x figures, TI's compiler (Roland's PC)
#   tools/ci_local.sh --jobs 8      processes per lane (default: every core)
#
# From Windows (Git Bash), the CLAUDE.md form:
#
#   MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- \
#       bash -c 'cd /mnt/d/DEV/STRATA && tools/ci_local.sh'
#
# The lanes run side by side and each writes a log; the summary at the end gives each lane's
# time and result, and the tail of any log that failed. Objects go under $STRATA_CI_OUT
# (default ~/strata-build/ci), in the WSL filesystem and not on /mnt/d, which is many times
# slower for a compiler's small-file I/O. Builds are incremental between runs, and use ccache
# when it is installed (`sudo apt install ccache`, optional).
#
# Exit status: 0 only if every lane passed.
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

quick=0
scenarios=0
jobs="$(nproc 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
    case "$1" in
        --quick)     quick=1; shift ;;
        --scenarios) scenarios=1; shift ;;
        --jobs)      jobs="$2"; shift 2 ;;
        -h|--help)   sed -n '2,25p' "$0"; exit 0 ;;
        *)           echo "error: unknown argument: $1 (try --help)" >&2; exit 2 ;;
    esac
done

OUT="${STRATA_CI_OUT:-$HOME/strata-build/ci}"
LOGS="$OUT/logs"
mkdir -p "$LOGS"
rm -f "$LOGS"/*.log "$LOGS"/*.rc "$LOGS"/*.sec

cxx=g++
cc=gcc
if command -v ccache > /dev/null; then
    cxx="ccache g++"
    cc="ccache gcc"
fi

if [ ! -f third_party/enaga/include/enaga/abi.h ]; then
    echo "third_party/enaga is empty: git submodule update --init --recursive" >&2
    exit 2
fi

# --- the lanes ----------------------------------------------------------------------------------

lane_gcc() {
    make -C host build -j"$jobs" ENAGA_BUILD="$OUT/gcc" CXX="$cxx" CC="$cc" &&
    make -C host test JOBS="$jobs" TEST_TIMEOUT=120 ENAGA_BUILD="$OUT/gcc" CXX="$cxx" CC="$cc" &&
    {
        # The font layout check, by name, as CI does.
        local bin="$OUT/gcc/host/strata-tests" t ok=0
        for t in $("$bin" --list | grep '^ui_layout_'); do
            "$bin" --only "$t" > /dev/null || { echo "layout FAIL: $t"; "$bin" --only "$t"; ok=1; }
        done
        echo "layout: every ui_layout_ test passes by name"
        # The screenshots still draw (CI's "Screenshots still draw"), into a scratch folder.
        local shots="$OUT/shots"
        rm -rf "$shots"; mkdir -p "$shots/dumps"
        STRATA_SHOTS="$shots/dumps" "$bin" --only ui_screenshots > /dev/null &&
            python3 -B tools/screenshots.py "$shots/dumps" "$shots/screens" > /dev/null &&
            python3 -B tools/panel_diagram.py "$shots/screens/panel.svg" > /dev/null &&
            echo "screenshots: draw" || { echo "screenshots FAIL"; ok=1; }
        return $ok
    }
}

lane_asan() {
    export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
    export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
    make -C host build -j"$jobs" SANITIZE=1 ENAGA_BUILD="$OUT/asan" CXX="$cxx" CC="$cc" &&
    make -C host test JOBS="$jobs" TEST_TIMEOUT=300 SANITIZE=1 ENAGA_BUILD="$OUT/asan" \
        CXX="$cxx" CC="$cc"
}

lane_checks() {
    local ok=0
    tools/audit_commands.sh || ok=1
    tools/enaga_check.sh || ok=1
    python3 -B tests/test_package_release.py || ok=1
    python3 -B tests/test_enaga_cached.py || ok=1
    for f in tools/publish_release.sh tools/ci_local.sh tools/test_shards.sh tools/ci_changes.sh \
             tools/scenarios.sh tools/build.sh; do
        bash -n "$f" || { echo "does not parse: $f"; ok=1; }
    done
    return $ok
}

lane_scenarios() {
    tools/scenarios.sh
}

run_lane() {   # name function
    local s e
    s=$(date +%s.%N)
    "$2" > "$LOGS/$1.log" 2>&1
    echo $? > "$LOGS/$1.rc"
    e=$(date +%s.%N)
    awk -v a="$s" -v b="$e" 'BEGIN { printf "%.0f\n", b - a }' > "$LOGS/$1.sec"
}

lanes=(gcc checks)
[ "$quick" -eq 1 ] || lanes+=(asan)

start=$(date +%s.%N)
echo "STRATA local CI: ${lanes[*]} on $jobs processes per lane, logs in $LOGS"
[ "$cxx" = g++ ] && echo "(no ccache: builds are still incremental between runs)"

pids=()
for lane in "${lanes[@]}"; do
    run_lane "$lane" "lane_$lane" &
    pids+=($!)
done
wait "${pids[@]}"

# The scenarios after the host lanes, not beside them: they are the long part, they want every
# core for the emulator, and there is no point starting them on a tree whose host tests fail.
if [ "$scenarios" -eq 1 ]; then
    lanes+=(scenarios)
    if grep -qv '^0$' "$LOGS"/gcc.rc; then
        echo "skipping the scenarios: the host tests failed"
        echo 1 > "$LOGS/scenarios.rc"; echo 0 > "$LOGS/scenarios.sec"
        echo "not run: the host tests failed" > "$LOGS/scenarios.log"
    else
        echo "host lanes done; running the emulator scenarios ..."
        run_lane scenarios lane_scenarios
    fi
fi
end=$(date +%s.%N)

# --- summary ------------------------------------------------------------------------------------

failed=0
echo
for lane in "${lanes[@]}"; do
    rc=$(cat "$LOGS/$lane.rc" 2>/dev/null || echo 99)
    sec=$(cat "$LOGS/$lane.sec" 2>/dev/null || echo "?")
    last=$(grep -E 'checks in .* shards|checks, [0-9]+ failure|scenarios pass|FAILED:' \
               "$LOGS/$lane.log" | tail -1)
    if [ "$rc" = 0 ]; then
        printf '  ok    %-10s %4ss  %s\n' "$lane" "$sec" "$last"
    else
        printf '  FAIL  %-10s %4ss  %s\n' "$lane" "$sec" "$last"
        failed=$((failed + 1))
    fi
done

for lane in "${lanes[@]}"; do
    [ "$(cat "$LOGS/$lane.rc" 2>/dev/null)" = 0 ] && continue
    echo
    echo "== $lane: last 40 lines of $LOGS/$lane.log"
    tail -40 "$LOGS/$lane.log"
done

echo
awk -v a="$start" -v b="$end" -v f="$failed" \
    'BEGIN { printf "%s in %.0f s\n", (f == 0 ? "all lanes pass" : f " lane(s) failed"), b - a }'
[ "$failed" -eq 0 ]
