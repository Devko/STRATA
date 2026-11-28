#!/usr/bin/env bash
# ARR REC in the emulator (core/arrrec.hpp; docs/CONTROLS.md, "ARR REC"): the FUNC menu's third
# row starts a take on the factory beat, an ENERGY move makes a second section, STOP ends the take
# and says how many sections it wrote, and ARR UNDO swaps the chain from before back in.
#
#   PLAY                        -> A01, 16 steps: a pass is about 690 M cycles at 130 BPM
#   FUNC + HOME, DOWN x2, OK    -> "ARR REC ON", the menu closes
#   FUNC + MIXER                -> PERFORM
#   FUNC + turn C -2            -> "ENERGY -4", inside the take's second pass
#   STOP                        -> "ARR 2 SECTIONS" (the first pass, cut in early, is kept whole)
#   FUNC + HOME, UP x8, OK      -> ARR UNDO: "ARR UNDONE"
#
# What each pass writes, the half rule and the X16 split are tests/test_arrrec.cpp's and
# tests/test_engine.cpp's (engine_arr_*); this proves the path runs on the C674x, with the
# chain written from the audio side, under the load target.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F6 = 29, F1 = 19, F4 = 27, F7 = 21,
# DOWN = 25, UP = 17, OK = 23; encoder C is 2. A chord is FUNC and the second key 5 M cycles later;
# a turn 2 M cycles after FUNC is FUNC + turn (scenario_energy). The first press must come LATE
# (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=3700000000
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
LOG="$ENAGA_BUILD/scenario_arrrec.log"

keys=(--key 22@300000000)
keys+=(--key 19@340000000 --key 27@345000000 --key 25@380000000 --key 25@420000000)
keys+=(--key 23@460000000)
keys+=(--key 19@560000000 --key 21@565000000)
keys+=(--key 19@1300000000 --turn 2:-2@1302000000)
keys+=(--key 29@2600000000)
keys+=(--key 19@2800000000 --key 27@2805000000)
for i in 0 1 2 3 4 5 6 7; do
    keys+=(--key "17@$((2840000000 + i * 40000000))")
done
keys+=(--key 23@3200000000)

echo "running the emulator for $CYCLES cycles: ARR REC on the factory beat, STOP, ARR UNDO ..."
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
check("ARR REC ON" in log, "the menu's third row started a take", f"log={log}")
check(any(l.startswith("ENERGY -") for l in log), "FUNC + turn C moved ENERGY", f"log={log}")
ended = [l for l in log if re.fullmatch(r"ARR \d+ SECTIONS?", l)]
check(len(ended) == 1, "STOP ended the take and said what it wrote", f"log={log}")
if ended:
    count = int(ended[0].split()[1])
    check(count >= 2, "the ENERGY move made a section of its own", f"{ended[0]!r}")
check("ARR UNDONE" in log, "ARR UNDO swapped the chain from before back in", f"log={log}")
if "ARR REC ON" in log and ended and "ARR UNDONE" in log:
    check(log.index("ARR REC ON") < log.index(ended[0]) < log.index("ARR UNDONE"),
          "in that order", f"log={log}")

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
