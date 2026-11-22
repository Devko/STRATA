#!/usr/bin/env bash
# Run EVERY emulator scenario, and fail if any one of them fails.
#
# This exists because two scenarios were failing for weeks without anyone noticing. The habit was
# to run the newest ones after a change -- m3 through m6 -- so scenario_m0.sh went on checking for
# a log line app/ had stopped writing, and scenario_m1.sh's load tripwire had been overtaken by
# the default kit growing. A test nobody runs is worse than no test: it reads as coverage.
#
#   tools/scenarios.sh            every scenario
#   tools/scenarios.sh m3 m6      just those
set -uo pipefail

cd "$(dirname "$0")/.."

if [ $# -gt 0 ]; then
    names=("$@")
else
    names=()
    for path in tests/scenario_*.sh; do
        name="${path#tests/scenario_}"
        names+=("${name%.sh}")
    done
fi

failed=()
for name in "${names[@]}"; do
    printf '%-5s ' "$name"
    output=$(bash "tests/scenario_${name}.sh" 2>&1)
    status=$?
    load=$(printf '%s' "$output" | grep -oE 'peak load: [0-9.]+%' | tail -1)
    if [ $status -eq 0 ]; then
        echo "OK    ${load:-(no load figure)}"
    else
        echo "FAIL  ${load:-}"
        printf '%s\n' "$output" | grep -E '^  FAIL' | sed 's/^/        /'
        failed+=("$name")
    fi
done

echo
if [ ${#failed[@]} -eq 0 ]; then
    echo "all ${#names[@]} scenarios pass"
    exit 0
fi
echo "FAILED: ${failed[*]}"
exit 1
