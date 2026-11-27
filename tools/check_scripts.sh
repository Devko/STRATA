#!/usr/bin/env bash
# Every shell and Python script in tools/ and tests/ parses: `bash -n` for *.sh, Python's own
# parser for *.py. Run by CI ("Repo checks") and tools/ci_local.sh.
#
# Most of these scripts run only on Roland's PC (the scenarios, the device build, the release),
# never in CI, so a syntax slip in one surfaces only when someone needs it. This used to be a
# hand-kept list of six scripts in two places, which missed the other twenty.
#
# Usage: tools/check_scripts.sh     exit 0 when every script parses.
set -uo pipefail

cd "$(dirname "$0")/.."

fail=0
count=0
for f in tools/*.sh tests/*.sh; do
    [ -f "$f" ] || continue
    count=$((count + 1))
    bash -n "$f" || { echo "FAIL: $f does not parse"; fail=1; }
done
for f in tools/*.py tests/*.py; do
    [ -f "$f" ] || continue
    count=$((count + 1))
    python3 -B -c 'import ast, sys; ast.parse(open(sys.argv[1], encoding="utf-8").read(), sys.argv[1])' "$f" \
        || { echo "FAIL: $f does not parse"; fail=1; }
done
[ "$fail" -eq 0 ] && echo "ok    $count scripts parse"
exit $fail
