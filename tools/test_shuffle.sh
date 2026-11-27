#!/usr/bin/env bash
# Run the host test binary in shuffled orders, to catch a test that depends on the one before it.
#
#   tools/test_shuffle.sh BIN [SEEDS...]
#
# Each seed is one run of every test in an order `shuf` draws from that seed, all seeds side by
# side. A failure names the seed; its order is kept in $TMPDIR/strata-shuffle-SEED.txt, so
# `BIN --names that-file` replays it, and cutting the file down (halves, then pairs) finds the
# test that leaks into the failing one. Default seeds: 1 to 4.
#
# The suite passes in any order since tests/apprig.cpp's and tests/test_ui.cpp's fresh() put back
# the scenes, macros, take, UI cursors and a stale card error (docs/HANDOFF.md, "Test order").
# A new test that leaves state behind shows up here first.
set -uo pipefail

bin="${1:?usage: tools/test_shuffle.sh BIN [SEEDS...]}"
shift
seeds=("$@")
[ ${#seeds[@]} -gt 0 ] || seeds=(1 2 3 4)
[ -x "$bin" ] || { echo "error: no test binary at $bin" >&2; exit 2; }

tmp="${TMPDIR:-/tmp}"
names="$tmp/strata-shuffle-names.txt"
"$bin" --list > "$names" || { echo "error: $bin --list failed" >&2; exit 2; }

for seed in "${seeds[@]}"; do
    shuf --random-source=<(yes "$seed") "$names" > "$tmp/strata-shuffle-$seed.txt"
    "$bin" --names "$tmp/strata-shuffle-$seed.txt" > "$tmp/strata-shuffle-$seed.log" 2>&1 &
done
wait

status=0
for seed in "${seeds[@]}"; do
    log="$tmp/strata-shuffle-$seed.log"
    summary="$(tail -n 1 "$log")"
    echo "seed $seed: $summary"
    case "$summary" in
        *" 0 failures") ;;
        *) status=1; grep -A3 '^  FAIL' "$log" | head -n 40 ;;
    esac
done
exit $status
