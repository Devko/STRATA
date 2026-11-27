#!/usr/bin/env bash
# Ratchets that span the step's LEN in the emulator: the step page sets the kick's step 1 to LEN
# 16 steps and RTCH ROLL/32, and the beat plays on with a 1/32 roll across the bar (32 hits). This
# proves the roll runs on the C674x on the clock's ticks without a crash, an overrun or a stall,
# and what it costs; who hits when is tests/test_ratchetspan.cpp's (the emulator cannot hear).
#
#   PLAY                        -> the beat runs (the kick has a trig on step 1)
#   hold STEP 1 + OK            -> the step page, page 1: NOTE VEL LEN MICR
#   turn C +8                   -> LEN 1 step -> 16 steps
#   screen key right            -> page 2: PROB COND RTCH RAMP
#   turn C +10                  -> RTCH 1 -> ROLL/32 (past 8 hits: 1/64, 1/48, 1/32)
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, STEP n = 31 + n, OK = 23,
# SELECT_RIGHT = 9; encoder C is 2. A key is released 20 M cycles after its press. The first
# press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2400000000

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
LOG="$ENAGA_BUILD/scenario_ratchetspan.log"

echo "running the emulator for $CYCLES cycles: step 1 to LEN 16 steps, RTCH ROLL/32 ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES"     --key 22@300000000     --key 32@380000000 --key 23@385000000     --turn 2:8@460000000     --key 9@700000000     --turn 2:10@760000000     > "$LOG" 2>&1 || {
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# The two turns edited step 1 (undo levels) without toggling a trig (the mask held).
edits = [l for l in log if l.startswith("g") and len(l.split()) == 2]
masks = {l.split()[0] for l in edits}
depths = [int(l.split()[1][1:]) for l in edits if l.split()[1].startswith("u")]
check(bool(depths) and max(depths) >= 1, "the step page's turns edited the step",
      f"edits={edits}")
check(len(masks) == 1, "no trig was toggled", f"edits={edits}")

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
print("note: no cache model here. tools/scenarios.sh says which compiler built it; the unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
