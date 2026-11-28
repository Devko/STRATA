#!/usr/bin/env bash
# M0 emulator scenario: the app boots on the real firmware, renders frames and makes no sound.
#
# This is the middle tier of the three in docs/SPEC.md, "Testing": host tests prove logic, the
# emulator proves the app builds and runs on the firmware, and only the unit settles load and
# timing. The emulator models no caches or bus latency, so nothing here asserts a load figure.
#
# Usage: tests/scenario_m0.sh [--cycles N]
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=400000000      # emulate.py's own default; about 1 s of emulated time
while [ $# -gt 0 ]; do
    case "$1" in
        --cycles) CYCLES="$2"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "SKIP: no emulator at $ENAGA_EMULATOR" >&2
    exit 0
fi

strata_require_toolchain
strata_link_emulator_toolchain
tools/build.sh > /dev/null

STAGE="$ENAGA_BUILD/stage/STRATA"
LOG="$ENAGA_BUILD/scenario_m0.log"

echo "running the emulator for $CYCLES cycles ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

# emulate.py writes a JSON report; assert on its VALUES. Grepping the log for words like
# "overrun" matches the field name whether the count is 0 or 500, which is worse than no
# check at all -- it fails on a healthy run and would be silenced by loosening it.
rc=0
python3 - "$LOG" <<'PY' || rc=$?
import json, re, sys

text = open(sys.argv[1]).read()
match = re.search(r'^\{.*?^\}', text, re.S | re.M)
if not match:
    print("  FAIL  no JSON report in the emulator output")
    sys.exit(1)
report = json.loads(match.group(0))

CYCLES_PER_BLOCK = 496_000
failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

log = report.get("log", [])

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(report.get("app_name") == "STRATA", "package name is STRATA",
      f'name={report.get("app_name")!r}')
check(not report.get("crash_reason"), "no crash",
      f'reason={report.get("crash_reason")!r}')
# "strata ready", not "strata m0 ready": app/app.hpp's log line lost the milestone from its text
# at some point and this check was never updated, so THIS SCENARIO HAS BEEN FAILING SILENTLY --
# nobody ran it, because the habit was to run the newest scenarios after a change. A stale
# expectation in a test that nobody runs is worse than no test.
check("strata ready" in log, "init() ran to completion")
check("APP INIT FAILED" not in log, "init() returned true")

# system::device() resolves from the bootloader, so this proves the real call rather than
# a hardcoded string.
check("SMPLTREK" in log, "device detected as SMPLTREK")

blocks = report.get("audio_blocks", 0)
check(blocks > 0, "audio callback ran", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')
check(report.get("audio_status", -1) == 0, "audio status clean",
      f'status={report.get("audio_status")}')

# The skeleton outputs silence, so its peak is Enaga's own overhead plus the draw path.
# Enaga's tone example peaks near 1.6% on the emulator, so anything above a few percent
# means something unintended is running every block.
# GCC-only gets 6%: the idle mixer's loops are the cl6x ones on the unit, and that build read
# 5.08% at dacb1f1 (2026-11-28), just over the cl6x line.
peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
gcc_only = __import__("os").environ.get("ENAGA_TI_CGT") == "none"
limit = 0.06 if gcc_only else 0.05
check(0 < peak < limit * CYCLES_PER_BLOCK, f"peak load is baseline-ish (under {limit:.0%})",
      f"{peak} cycles = {percent:.2f}% of the block")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: the emulator models no caches or bus latency -- compare versions, not targets.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
