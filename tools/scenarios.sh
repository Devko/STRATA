#!/usr/bin/env bash
# Run EVERY emulator scenario, and fail if any one of them fails.
#
# This exists because two scenarios were failing for weeks without anyone noticing. The habit was
# to run the newest ones after a change -- m3 through m6 -- so scenario_m0.sh went on checking for
# a log line app/ had stopped writing, and scenario_m1.sh's load tripwire had been overtaken by
# the default kit growing. A test nobody runs is worse than no test: it reads as coverage.
#
#   tools/scenarios.sh            every scenario, side by side on every core
#   tools/scenarios.sh m3 m6      just those
#   tools/scenarios.sh -j 1       one at a time, in $ENAGA_BUILD itself, as before
#   tools/scenarios.sh -j 4 m3    at most four at once
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
while [ $# -gt 0 ]; do
    case "$1" in
        -j)  jobs="$2"; shift 2 ;;
        -j*) jobs="${1#-j}"; shift ;;
        *)   names+=("$1"); shift ;;
    esac
done

if [ ${#names[@]} -eq 0 ]; then
    for path in tests/scenario_*.sh; do
        name="${path#tests/scenario_}"
        names+=("${name%.sh}")
    done
fi

results="$(mktemp -d "${TMPDIR:-/tmp}/strata-scenarios.XXXXXX")"
trap 'rm -rf "$results"' EXIT

run_one() {   # name build_dir
    local s e
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
}
export -f run_one
export results

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
for name in "${names[@]}"; do
    output=$(cat "$results/$name.out" 2>/dev/null)
    status=$(cat "$results/$name.rc" 2>/dev/null || echo 99)
    sec=$(cat "$results/$name.sec" 2>/dev/null || echo "?")
    load=$(printf '%s' "$output" | grep -oE 'peak load: [0-9.]+%' | tail -1)
    if [ "$status" -eq 0 ]; then
        printf '%-9s OK    %4ss  %s\n' "$name" "$sec" "${load:-(no load figure)}"
    else
        printf '%-9s FAIL  %4ss  %s\n' "$name" "$sec" "${load:-}"
        printf '%s\n' "$output" | grep -E '^  FAIL|^FAIL|^error' | sed 's/^/        /'
        failed+=("$name")
    fi
done

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "all ${#names[@]} scenarios pass in $((end - start)) s"
    exit 0
fi
echo "FAILED: ${failed[*]} ($((end - start)) s)"
exit 1
