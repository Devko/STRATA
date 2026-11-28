#!/usr/bin/env bash
# Track Launch under ARR REC in the emulator (core/arrrec.hpp, "Track Launch"): A07's part is
# queued on track 3 from LAUNCH, ARR REC starts late in A01's first pass (so that pass is
# dropped by the half rule), the launch lands at the end of A01, and the pass that follows is
# CAPTUREd into a free slot of bank A without switching ("LAUNCH IN A..."); its repeat adds no
# second capture, and STOP ends the take with one section.
#
#   PLAY                        -> A01, 16 steps: a pass is about 690 M cycles at 130 BPM
#   PROJECT, PROJECT            -> LAUNCH (a double tap: 40 M cycles apart)
#   DOWN, DOWN, STEP 7          -> T3 < A07 AT END
#   FUNC + HOME, DOWN x2, OK    -> "ARR REC ON", the menu closes; under half of the pass is left
#   (end of A01)                -> T3 A07
#   (end of the next pass)      -> LAUNCH IN A.. (the captured slot)
#   STOP                        -> "ARR 1 SECTION" (the captured slot, X2)
#
# What goes into the slot, the reuse on a repeat and the full bank are tests/test_arrrec.cpp's
# and tests/test_engine_launch.cpp's (engine_arr_rec_*); this proves the capture runs on the
# C674x at a pass boundary, from the audio side, without an overrun and under the load target.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F6 = 29, F3 = 20, F1 = 19, F4 = 27,
# DOWN = 25, OK = 23, STEP 7 = 38. A chord is FUNC and the second key 5 M cycles later. The first
# press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2900000000
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
mkdir -p "$ENAGA_BUILD"
LOG="$ENAGA_BUILD/scenario_arrlaunch.log"

keys=(--key 22@300000000 --key 20@400000000 --key 20@440000000)
keys+=(--key 25@520000000 --key 25@570000000 --key 38@650000000)
keys+=(--key 19@740000000 --key 27@745000000 --key 25@780000000 --key 25@820000000)
keys+=(--key 23@860000000)
keys+=(--key 29@2600000000)

echo "running the emulator for $CYCLES cycles: ARR REC over a launch on track 3 ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" "${keys[@]}" \
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
# The log ring keeps the last 16 lines; this run writes about a dozen after PLAY.
check("T3 < A07 AT END" in log, "STEP 7 on LAUNCH queued A07 on track 3", f"log={log}")
check("ARR REC ON" in log, "the menu's third row started a take", f"log={log}")
check("T3 A07" in log, "the launch landed at the end of A01", f"log={log}")
kept = [l for l in log if re.fullmatch(r"LAUNCH IN A\d\d", l)]
check(len(kept) == 1, "the launched pass was captured once, into a slot of bank A", f"log={log}")
check("LAUNCH NOT KEPT" not in log, "bank A had a free slot", f"log={log}")
check("ARR 1 SECTION" in log, "STOP ended the take with one section", f"log={log}")
order = ["T3 < A07 AT END", "ARR REC ON", "T3 A07"] + kept[:1] + ["ARR 1 SECTION"]
if all(l in log for l in order):
    at = [log.index(l) for l in order]
    check(at == sorted(at), "in that order", f"log={log}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: no cache model here. docs/BUDGET.md records cl6x as worth about 1.7x and the 60%")
print("      target depends on it. The unit decides.")

sys.exit(1 if failures else 0)
PY

if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
