#!/usr/bin/env bash
# Run EVERY emulator scenario, and fail if any one of them fails.
#
# This exists because two scenarios were failing for weeks without anyone noticing. The habit was
# to run the newest ones after a change -- m3 through m6 -- so scenario_m0.sh went on checking for
# a log line app/ had stopped writing, and scenario_m1.sh's load tripwire had been overtaken by
# the default kit growing. A test nobody runs is worse than no test: it reads as coverage.
#
#   tools/scenarios.sh            every scenario, side by side on every core (the full pass)
#   tools/scenarios.sh --changed  the per-PR set: what this branch changes against origin/main
#                                 needs (tools/scenario_pick.py: the scenarios that name a changed
#                                 file, the load set for DSP, and a smoke set); --changed=REF
#                                 compares against REF instead. Uncommitted changes count.
#   tools/scenarios.sh m3 m6      just those
#   tools/scenarios.sh -j 1       one at a time, in $ENAGA_BUILD itself, as before
#   tools/scenarios.sh -j 4 m3    at most four at once
#
# Every scenario takes one of $STRATA_EMU_SLOTS slots (default: the cores less two) shared by
# every run on the machine (lock files in ~/strata-build/slots), so threads in several worktrees
# running scenarios at once queue for the cores instead of all slowing down together: five runs
# at -j 4 on Roland's 16 threads sent single scenarios from 49 s to 226 s (2026-11-28). The load
# figures do not depend on it (the emulator counts guest cycles); only the wall time does.
# STRATA_EMU_SLOTS=0 turns the queue off.
#
# In parallel, each scenario gets its own build folder, $ENAGA_BUILD/par/<name>, because
# emulate.py writes its card image, capture and report to fixed names under $ENAGA_BUILD. Its
# log is therefore $ENAGA_BUILD/par/<name>/scenario_<name>.log. The app is built once, up front
# (tools/build.sh), and every scenario after that takes the same package from
# tools/enaga_cached.py instead of compiling it again; the firmware likewise. Each scenario still
# runs on its own emulator, so the load figures are the same as in a serial run. The report keeps
# the order of the names and gives each scenario's time.
set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

jobs="$(nproc 2>/dev/null || echo 2)"
names=()
full=0
changed=""
while [ $# -gt 0 ]; do
    case "$1" in
        -j)  jobs="$2"; shift 2 ;;
        -j*) jobs="${1#-j}"; shift ;;
        --changed)   changed="origin/main"; shift ;;
        --changed=*) changed="${1#--changed=}"; shift ;;
        *)   names+=("$1"); shift ;;
    esac
done

if [ -n "$changed" ]; then
    # Committed on this branch since it left $changed, plus whatever is not committed yet.
    base=$("$STRATA_GIT" merge-base "$changed" HEAD 2>/dev/null) || {
        echo "error: no merge base with $changed (fetch it first)" >&2; exit 2; }
    files=$({ "$STRATA_GIT" diff --name-only "$base" HEAD; "$STRATA_GIT" diff --name-only HEAD;
              "$STRATA_GIT" ls-files --others --exclude-standard; } | tr -d '\r' | sort -u)
    echo "changed against $changed: $(printf '%s\n' "$files" | grep -c . || true) file(s)"
    python3 -B tools/scenario_pick.py --why $files | sed 's/^/  /'
    mapfile -t names < <(python3 -B tools/scenario_pick.py $files)
    if [ ${#names[@]} -eq 0 ]; then
        echo "no scenario needed: nothing but prose changed"
        exit 0
    fi
fi

if [ ${#names[@]} -eq 0 ]; then
    full=1
    for path in tests/scenario_*.sh; do
        name="${path#tests/scenario_}"
        names+=("${name%.sh}")
    done
fi

# Every scenario SKIPs, with exit 0, when there is no emulator: right for a session without one
# running a single scenario, wrong here, where it read as "all scenarios pass" (a worktree with no
# ENAGA_EMULATOR did exactly that, 2026-11-25). So the runner refuses instead.
if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "error: no emulator at $ENAGA_EMULATOR (no cpu.py), so every scenario would SKIP." >&2
    echo "       tools/fetch_emulator.sh clones it; or set ENAGA_EMULATOR to a clone's emulator/." >&2
    exit 2
fi

results="$(mktemp -d "${TMPDIR:-/tmp}/strata-scenarios.XXXXXX")"
trap 'rm -rf "$results"' EXIT

# One of $STRATA_EMU_SLOTS machine-wide slots, held until this shell exits (flock on an fd).
slots="${STRATA_EMU_SLOTS:-$(( $(nproc 2>/dev/null || echo 4) > 3 ? $(nproc 2>/dev/null || echo 4) - 2 : 1 ))}"
slot_dir="${STRATA_SLOT_DIR:-$HOME/strata-build/slots}"
take_slot() {
    local i waited=0
    [ "$slots" -gt 0 ] 2>/dev/null && command -v flock > /dev/null || return 0
    mkdir -p "$slot_dir"
    while :; do
        for ((i = 0; i < slots; i++)); do
            exec {slot_fd}> "$slot_dir/$i"
            if flock -n "$slot_fd"; then
                [ "$waited" -gt 0 ] && echo "waited ${waited} s for a free emulator slot" > "$results/$1.wait"
                return 0
            fi
            exec {slot_fd}>&-
        done
        sleep 2
        waited=$((waited + 2))
    done
}
export -f take_slot
export slots slot_dir

run_one() {   # name build_dir
    local s e
    take_slot "$1"
    s=$(date +%s)
    if [ -n "$2" ]; then
        mkdir -p "$2"
        ENAGA_BUILD="$2" STRATA_NO_COPY_BACK=1 bash "tests/scenario_$1.sh" > "$results/$1.out" 2>&1
    else
        bash "tests/scenario_$1.sh" > "$results/$1.out" 2>&1
    fi
    echo $? > "$results/$1.rc"
    e=$(date +%s)
    echo $((e - s)) > "$results/$1.sec"
    # Kept for the next run's order (longest_first).
    if [ -n "$2" ]; then echo $((e - s)) > "$2/seconds"; fi
}
export -f run_one
export results

# The names, longest first, so the long ones (visuals, freeze, bounce) are not the last to start
# while every other slot sits idle: in alphabetical order, visuals (5.4 min) started near the end
# and a 12-minute run took 15 (2026-11-27). Each scenario's time is the one its last parallel run
# took; a scenario never run before is estimated from its CYCLES line (1 s per 10 M cycles, about
# what the emulator does here). Only the start order changes: the report keeps the names' order.
longest_first() {
    local name est cycles
    for name in "$@"; do
        est=$(cat "$ENAGA_BUILD/par/$name/seconds" 2>/dev/null || true)
        if [ -z "$est" ]; then
            cycles=$(grep -m1 -oE '^CYCLES=[0-9]+' "tests/scenario_$name.sh" 2>/dev/null | cut -d= -f2)
            est=$(( ${cycles:-1000000000} / 10000000 ))
        fi
        printf '%s %s\n' "$est" "$name"
    done | sort -k1,1nr | cut -d' ' -f2
}

start=$(date +%s)
if [ "$jobs" -le 1 ] || [ ${#names[@]} -le 1 ]; then
    for name in "${names[@]}"; do run_one "$name" ""; done
else
    # Build once, first: a compile error then shows once rather than fifteen times, and the
    # scenarios that follow all find the package in the cache.
    if [ -f "$ENAGA_EMULATOR/cpu.py" ]; then
        # And link the emulator's binutils once, here: fifteen scenarios doing it at once race.
        strata_link_emulator_toolchain
        echo "building once for ${#names[@]} scenarios, then $jobs at a time ..."
        if ! tools/build.sh > "$results/build.out" 2>&1; then
            cat "$results/build.out"
            echo "FAILED: the build"
            exit 1
        fi
    fi
    printf '%s\n' "${names[@]}" |
        xargs -P "$jobs" -I{} bash -c 'run_one "$1" "$2"' _ {} "$ENAGA_BUILD/par/{}"
fi
end=$(date +%s)

failed=()
worst_pct=0 worst_name=""
for name in "${names[@]}"; do
    output=$(cat "$results/$name.out" 2>/dev/null)
    status=$(cat "$results/$name.rc" 2>/dev/null || echo 99)
    sec=$(cat "$results/$name.sec" 2>/dev/null || echo "?")
    load=$(printf '%s' "$output" | grep -oE 'peak load: [0-9.]+%' | tail -1)
    pct=${load#peak load: }; pct=${pct%\%}
    if [ -n "$load" ] && awk -v a="$pct" -v b="$worst_pct" 'BEGIN { exit !(a > b) }'; then
        worst_pct=$pct worst_name=$name
    fi
    wait_note=$(cat "$results/$name.wait" 2>/dev/null || true)
    if [ "$status" -eq 0 ]; then
        printf '%-9s OK    %4ss  %s%s\n' "$name" "$sec" "${load:-(no load figure)}" "${wait_note:+  ($wait_note)}"
    else
        printf '%-9s FAIL  %4ss  %s\n' "$name" "$sec" "${load:-}"
        printf '%s\n' "$output" | grep -E '^  FAIL|^FAIL|^error' | sed 's/^/        /'
        failed+=("$name")
    fi
done

# A full pass prints its row for docs/SCENARIO_LOG.md, "Full passes", ready to paste (the Notes
# cell is yours). The other compiler's column is left for its own run.
if [ "$full" -eq 1 ]; then
    commit=$("$STRATA_GIT" rev-parse --short HEAD 2>/dev/null || echo "?")
    passed=$(( ${#names[@]} - ${#failed[@]} ))
    cell="worst **${worst_pct}%** (\`${worst_name:-?}\`)"
    if [ "$(strata_cgt_label)" = "cl6x" ]; then cl6x=$cell gcc="not run"; else cl6x="not run" gcc=$cell; fi
    echo
    echo "For docs/SCENARIO_LOG.md:"
    printf '| %s | `%s` | %s of %s | %s | %s | %s |\n' "$(date +%F)" "$commit" "$passed" \
        "${#names[@]}" "$cl6x" "$gcc" "${failed[*]:+failed: ${failed[*]}}"
fi

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "all ${#names[@]} scenarios pass in $((end - start)) s"
    exit 0
fi
echo "FAILED: ${failed[*]} ($((end - start)) s)"
exit 1
