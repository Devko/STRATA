#!/usr/bin/env bash
# FEEL in the emulator: the PATTERN page's FEEL row walked by UP while the beat plays, from OFF
# through DRIFT 1-6 to WAVE 2, every press reaching the pattern (one undo level each), and the
# project then saves with it. What FEEL does to the timing is tests/test_timing.cpp's and
# tests/test_scheduler.cpp's; this proves the row draws and the drift path runs in the scheduler
# on the C674x with the real font, under the load target and without an overrun. REL ALL and LEN
# ALL are FUNC + a knob turn, which the emulator cannot do: tests/test_ui.cpp and test_engine.cpp.
#
#   PLAY                        -> the factory beat
#   PROJECT, SELECT_RIGHT x2    -> PROJECT's PATTERN page (LEN, SWG, FEEL, the track swings)
#   RIGHT x2                    -> the FEEL row
#   UP x8                       -> FEEL DRIFT 1 .. DRIFT 6, WAVE 1, WAVE 2: logged, u1 .. u8
#   FUNC + HOME, OK             -> a save, the beat still running in WAVE 2
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F3 = 20, F4 = 27, F5 = 22, OK = 23,
# UP = 17, RIGHT = 18, SELECT_RIGHT = 9. A chord is FUNC pressed and the second key 5 M cycles
# later. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1700000000
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
LOG="$ENAGA_BUILD/scenario_feel.log"

keys=(--key 22@300000000 --key 20@400000000 --key 9@450000000 --key 9@500000000)
keys+=(--key 18@550000000 --key 18@600000000)
for i in 0 1 2 3 4 5 6 7; do keys+=(--key "17@$((660000000 + i * 60000000))"); done
keys+=(--key 19@1300000000 --key 27@1305000000 --key 23@1380000000)

echo "running the emulator for $CYCLES cycles: FEEL while the beat plays ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" "${keys[@]}" \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

python3 - "$LOG" <<'PY'
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
# The log ring keeps the last 16 lines, so the early ones can scroll out; the last cannot.
check("FEEL WAVE 2" in log, "UP x8 on the FEEL row reached WAVE 2", f"log={log}")
feels = [l for l in log if l.startswith("FEEL ")]
order = ["FEEL DRIFT 1", "FEEL DRIFT 2", "FEEL DRIFT 3", "FEEL DRIFT 4", "FEEL DRIFT 5",
         "FEEL DRIFT 6", "FEEL WAVE 1", "FEEL WAVE 2"]
check(feels == order[len(order) - len(feels):], "the values came in order, one a press",
      f"feels={feels}")
check(any(l.startswith("g") and l.endswith(" u8") for l in log),
      "every press was one undo level (u8)", f"log={log}")
saved = [l for l in log if l.startswith(("saved", "autosaved", "save failed"))]
check(any(l.startswith("saved") for l in saved), "FUNC + HOME, OK saved the project",
      f"lines={saved}")

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
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
