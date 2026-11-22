#!/usr/bin/env bash
# Run the host test binary as N shards side by side and report like one run.
#
#   tools/test_shards.sh BIN [JOBS] [TIMEOUT_S] [SHARDS]
#
# `make -C host test JOBS=N` calls this; CI and tools/ci_local.sh do too. The suite is cut into
# SHARDS blocks (default 4 x JOBS) and JOBS processes take blocks as they finish, because the cost
# is lumpy: one test (ui_audit_every_input_in_every_screen...) is 40% of the suite, so equal
# blocks on exactly JOBS processes leave most of them idle. Each shard is a
# contiguous block of the registration order (tests/runner.cpp, --shard), so every test still runs
# after the same predecessors it has in the serial run, or first in its process. TIMEOUT_S is the
# per-test watchdog (--timeout): a test that never returns fails with its name instead of holding
# the job until something outside kills it.
#
# Output: one line per shard (tests, checks, seconds), then every FAIL/TIMEOUT block in full, then
# one summary line. Exit status 0 only if every shard exited 0.
set -uo pipefail

bin="${1:?usage: tools/test_shards.sh BIN [JOBS] [TIMEOUT_S] [SHARDS]}"
jobs="${2:-$(nproc 2>/dev/null || echo 2)}"
timeout_s="${3:-120}"
shards="${4:-$((jobs * 4))}"

[ -x "$bin" ] || { echo "error: no test binary at $bin" >&2; exit 2; }
[ "$jobs" -ge 1 ] 2>/dev/null || { echo "error: JOBS must be a positive number" >&2; exit 2; }
[ "$shards" -ge 1 ] 2>/dev/null || { echo "error: SHARDS must be a positive number" >&2; exit 2; }

work="$(mktemp -d "${TMPDIR:-/tmp}/strata-shards.XXXXXX")"
trap 'rm -rf "$work"' EXIT

start=$(date +%s.%N)
run_one() {   # bin k shards timeout work
    local s e
    s=$(date +%s.%N)
    "$1" --shard "$2/$3" --timeout "$4" > "$5/$2.out" 2>&1
    echo $? > "$5/$2.rc"
    e=$(date +%s.%N)
    awk -v a="$s" -v b="$e" 'BEGIN { printf "%.1f\n", b - a }' > "$5/$2.sec"
}
export -f run_one
# Biggest-first would be better still, but the blocks are in registration order and the cost of
# each is not known up front; many small blocks get most of the way.
seq 1 "$shards" | xargs -P "$jobs" -I{} bash -c 'run_one "$@"' _ "$bin" {} "$shards" "$timeout_s" "$work"
end=$(date +%s.%N)

failed=0
checks=0
for k in $(seq 1 "$shards"); do
    rc=$(cat "$work/$k.rc" 2>/dev/null || echo 99)
    sec=$(cat "$work/$k.sec" 2>/dev/null || echo "?")
    summary=$(grep -E '^[0-9]+ checks?, [0-9]+ failures?$' "$work/$k.out" | tail -1)
    running=$(grep -m1 -E '^running ' "$work/$k.out" | sed 's/^running //')
    n=$(printf '%s' "$summary" | awk '{print $1}')
    checks=$((checks + ${n:-0}))
    status="ok"
    if [ "$rc" != "0" ]; then
        status="FAILED (exit $rc)"
        failed=$((failed + 1))
    fi
    if [ "$status" != "ok" ] || [ -n "${STRATA_SHARDS_VERBOSE:-}" ]; then
        printf 'shard %2d/%d  %-36s %-28s %6ss  %s\n' "$k" "$shards" "${running:-?}" \
            "${summary:-no summary}" "$sec" "$status"
    fi
done

if [ "$failed" -gt 0 ]; then
    echo
    for k in $(seq 1 "$shards"); do
        [ "$(cat "$work/$k.rc" 2>/dev/null)" = "0" ] && continue
        echo "== shard $k/$shards"
        # The failure blocks (FAIL + its indented detail lines), a TIMEOUT, or -- for a crash with
        # no summary at all -- the last lines it printed.
        if grep -qE '^  (FAIL|TIMEOUT)' "$work/$k.out"; then
            awk '/^  (FAIL|TIMEOUT)/ {p=1; print; next} p && /^    / {print; next} {p=0}' "$work/$k.out"
        else
            tail -20 "$work/$k.out"
        fi
        # And the test that was running, if the process died mid-test (a crash, a sanitizer).
        if ! grep -qE '^[0-9]+ checks?, ' "$work/$k.out"; then
            echo "  (no summary: the process died; last test started after:"
            grep -E '^  ok   ' "$work/$k.out" | tail -1 | sed 's/^/   /'
            echo "  )"
        fi
    done
fi

wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.1f", b - a }')
echo
echo "$checks checks in $shards shards on $jobs processes, $wall s wall; $failed shard(s) failed"
[ "$failed" -eq 0 ]
