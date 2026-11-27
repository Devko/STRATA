#!/usr/bin/env bash
# The metronome and the count-in (CyDrums S6) in the emulator: the FUNC menu's CLICK and COUNT-IN
# rows step, REC then PLAY counts one bar in (`COUNT-IN 4` .. `1` in the log) with the click
# sounding, and the load with the click on is inside the target.
#
# That the pattern holds for the count-in and starts on step 1 after it is
# tests/test_click.cpp's: the emulator cannot see a step. What only the emulator shows is that the
# menu rows, REC and PLAY reach the engine on the C674x and the click costs nothing measurable.
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F4 = 27 (FUNC + HOME: the menu), UP = 17,
# DOWN = 25, OK = 23, CLR = 30, F2 = 26 (REC), F5 = 22 (PLAY). From SAVE, UP x6 is CLICK and
# DOWN from there COUNT-IN (both above FX GLIDE). A chord is the first key pressed and the second
# 5 M cycles later. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2200000000

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
LOG="$ENAGA_BUILD/scenario_click.log"

echo "running the emulator for $CYCLES cycles: CLICK ON, COUNT-IN 1 BAR, REC, PLAY ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES"     --key 19@300000000 --key 27@305000000     --key 17@360000000 --key 17@400000000 --key 17@440000000     --key 17@480000000 --key 17@520000000 --key 17@560000000     --key 23@600000000     --key 25@640000000     --key 23@680000000     --key 30@720000000     --key 26@780000000     --key 22@840000000     > "$LOG" 2>&1 || {
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

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# The two rows stepped, REC went on, and PLAY counted four beats in, in order.
def at(line):
    return log.index(line) if line in log else -1
check(at("CLICK ON") >= 0, "CLICK stepped ON", f"log={log}")
check(at("COUNT-IN 1 BAR") > at("CLICK ON"), "COUNT-IN stepped to 1 BAR", f"log={log}")
counts = [line for line in log if line.startswith("COUNT-IN ") and not line.endswith("BAR")]
check(counts == ["COUNT-IN 4", "COUNT-IN 3", "COUNT-IN 2", "COUNT-IN 1"],
      "PLAY with REC on counted one bar in", f"log={counts}")
check(at("run int") >= 0, "the transport started", f"log={log}")

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
