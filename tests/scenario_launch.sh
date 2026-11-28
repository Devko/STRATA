#!/usr/bin/env bash
# Track Launch in the emulator: PROJECT twice opens LAUNCH, DOWN twice picks track 3, STEP 7
# queues A07's part on it while the beat plays, push B pins it, and at the end of A01 it lands
# ("T3 A07"); then on GRID a STEP on track 3 is refused because the lane is read only. Where the
# launched part's steps land is tests/test_engine_launch.cpp's; this proves the page, the queue
# and the landing on the C674x, under the load target and without an overrun. The emulator cannot
# press pads or hold OK for 0.5 s, so held-pad launches and CAPTURE are host-tested only
# (tests/test_ui.cpp, ui_launch_*).
#
#   PLAY                        -> the factory beat, track 1 selected
#   PROJECT, PROJECT            -> LAUNCH (a double tap: 40 M cycles apart)
#   DOWN, DOWN, STEP 7          -> T3 < A07 AT END
#   push B                      -> T3 PINNED
#   (end of A01)                -> T3 A07
#   HOME, STEP 1                -> T3 A07 READ ONLY
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F3 = 20, F4 = 27, DOWN = 25,
# STEP 1 = 32, STEP 7 = 38, push B = 0. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=3800000000
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
LOG="$ENAGA_BUILD/scenario_launch.log"

keys=(--key 22@300000000 --key 20@400000000 --key 20@440000000)
keys+=(--key 25@520000000 --key 25@570000000 --key 38@650000000 --key 0@720000000)
keys+=(--key 27@3300000000 --key 32@3400000000)

echo "running the emulator for $CYCLES cycles: a launch on track 3 while the beat plays ..."
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
# The log ring keeps the last 16 lines; this run writes fewer than that after PLAY.
check("T3 < A07 AT END" in log, "STEP 7 on LAUNCH queued A07 on track 3", f"log={log}")
check("T3 PINNED" in log, "push B pinned track 3", f"log={log}")
check("T3 A07" in log, "the launch landed at the end of the pattern", f"log={log}")
check("T3 A07 READ ONLY" in log, "a STEP on the launched lane in GRID was refused",
      f"log={log}")
if "T3 A07" in log and "T3 < A07 AT END" in log:
    check(log.index("T3 < A07 AT END") < log.index("T3 A07"), "queued before it landed",
          f"log={log}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1500, "audio ran throughout", f"blocks={blocks}")
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
