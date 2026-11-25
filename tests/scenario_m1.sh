#!/usr/bin/env bash
# M1 emulator scenario: the transport starts, the beat plays, and the load is measured.
#
# The key press must come LATE. Enaga's panel MCU link is not streaming in the first ~200M
# cycles, so a press before that is silently swallowed and the app looks broken while being
# perfectly fine. Enaga's own documented example uses 300000000, which is the number to copy.
#
# Load figures from here only ever compare versions: the emulator models no caches or bus
# latency, and Enaga measured pure computation running about 25% faster on the unit. The real
# figure is a line in docs/UNIT_CHECKLIST.md.
#
# NOTE: this scenario measures whatever the default kit currently is, which has grown from M1's
# four tracks to M2's eight. So its number is not comparable across milestones -- a figure from
# this script is only meaningful against another figure from the same kit. docs/BUDGET.md records
# each milestone's own baseline, and the guard below is deliberately loose for that reason.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

PLAY_AT=300000000
CYCLES=1600000000       # about 3.5 s of playing after the press

while [ $# -gt 0 ]; do
    case "$1" in
        --cycles)  CYCLES="$2"; shift 2 ;;
        --play-at) PLAY_AT="$2"; shift 2 ;;
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
mkdir -p "$ENAGA_BUILD"
LOG="$ENAGA_BUILD/scenario_m1.log"

echo "running the emulator for $CYCLES cycles, PLAY at $PLAY_AT ..."
# Key 22 is ENAGA_KEY_F5, which STRATA maps to PLAY.
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" --key "22@$PLAY_AT" \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
python3 - "$LOG" <<'PY' || rc=$?
import json, re, sys

CYCLES_PER_BLOCK = 496_000

text = open(sys.argv[1]).read()
match = re.search(r'^\{.*?^\}', text, re.S | re.M)
if not match:
    print("  FAIL  no JSON report in the emulator output")
    sys.exit(1)
report = json.loads(match.group(0))

failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

log = report.get("log", [])

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check("strata ready" in log, "init() ran to completion")

# The transport logs this the first time the clock reports itself running on the internal
# source. Without it the key press never arrived and every load figure below is meaningless --
# which is exactly the failure that once made three different builds measure identically.
check("run int" in log, "the transport actually started", f"log={log}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK

# A playing four-track beat must cost materially more than silence, or nothing is sounding.
check(percent > 5.0, "the beat is actually sounding", f"{percent:.2f}% peak")

# An upper guard, loose on purpose: this is a regression tripwire, not M1's exit criterion.
# The bar of 15% belongs to the unit (docs/BUDGET.md explains why these differ).
#
# RAISED from 40 to 50, and the reason is recorded rather than the number quietly changed. It
# fired at 41.26% because the DEFAULT KIT has grown since M1 wrote this: it had four tracks and
# now has eight, with a four-note MIN7 chord on VA POLY worth +12.75 points on its own
# (docs/BUDGET.md), a sidechain duck that was dead code until M6, beat repeat's always-on ring
# and resampling's capture tap. None of that is a regression; all of it is the instrument this
# milestone's beat is now played on.
#
# 50 keeps the tripwire useful: it is nine points above today's reading, so another VA POLY voice
# would fire it, and it is still below the 60% the project is aiming at.
check(percent < 50.0, "load has not regressed badly", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: no cache model here -- compare versions, not targets. The unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
