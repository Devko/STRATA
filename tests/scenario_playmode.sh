#!/usr/bin/env bash
# Play modes in the emulator: the FUNC menu's TRACK PLAY row walks the selected track's play mode
# while the beat plays, every press reaching the pattern (one undo level each), and the project
# then saves with the mode in it. What the modes play is tests/test_timing.cpp's and
# tests/test_scheduler.cpp's; this proves the row draws and the RND path runs on the C674x with
# the real font, under the load target and without an overrun.
#
#   PLAY                        -> the factory beat, track 1 selected
#   FUNC + HOME, DOWN x9        -> TRACK PLAY (under TRACK SPEED)
#   RIGHT x5                    -> T1 PLAY REV, PING, RND1, RND2, RND4: logged, u1 .. u5
#   CLR, FUNC + HOME, OK        -> a save, the beat still running in RND4
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F4 = 27, F5 = 22, OK = 23, CLR = 30,
# DOWN = 25, RIGHT = 18. A chord is FUNC pressed and the second key 5 M cycles later. The first
# press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1600000000
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
LOG="$ENAGA_BUILD/scenario_playmode.log"

keys=(--key 22@300000000 --key 19@400000000 --key 27@405000000)
for i in 0 1 2 3 4 5 6 7 8; do keys+=(--key "25@$((460000000 + i * 50000000))"); done
for i in 0 1 2 3 4; do keys+=(--key "18@$((920000000 + i * 50000000))"); done
keys+=(--key 30@1200000000 --key 19@1280000000 --key 27@1285000000 --key 23@1360000000)

echo "running the emulator for $CYCLES cycles: TRACK PLAY while the beat plays ..."
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
# The log ring keeps the last 16 lines and this run writes more, so the early ones (strata
# ready, run int, the first modes) can scroll out. The last ones cannot.
check("T1 PLAY RND4" in log, "RIGHT x5 on TRACK PLAY reached RND4", f"log={log}")
plays = [l for l in log if l.startswith("T1 PLAY ")]
order = ["T1 PLAY REV", "T1 PLAY PING", "T1 PLAY RND1", "T1 PLAY RND2", "T1 PLAY RND4"]
check(plays == order[len(order) - len(plays):], "the modes came in order, one a press",
      f"plays={plays}")
check(any(l.startswith("g") and l.endswith(" u5") for l in log),
      "every press was one undo level (u5)", f"log={log}")
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
