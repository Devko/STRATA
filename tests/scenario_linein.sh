#!/usr/bin/env bash
# The line input as a mixer strip, in the emulator: MIXER's IN and IN+ pages draw on the C674x
# with the default beat playing, push A turns the input on (the strip runs for the rest of the
# run, so the load below includes its chain), push D steps the gain and D turned picks the jack,
# each logged; and the load is inside the target. docs/BUDGET.md "The line input" asks for the
# peak with cl6x and with ENAGA_TI_CGT=none; compare it with scenario_m7.sh, the same beat with
# the strip off. Whatever the emulator hands audio() as the input, the chain's cost is the same:
# it runs on every block the strip is on, sound or silence.
#
#   PLAY                          -> the default beat
#   MIXER, SELECT_LEFT x4         -> IN (going round: EQ, MST, IN+, IN)
#   push A                        -> IN ON, logged
#   SELECT_RIGHT                  -> IN+
#   push D                        -> IN GAIN HIGH, logged
#   D turned one detent           -> IN LINE L, logged
#
# What each knob does to the sound is tests/test_linein.cpp's; the pages' knobs and the route
# on the codec are tests/test_ui.cpp's (ui_the_line_input_pages_...).
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F7 = 21, SELECT_LEFT = 8,
# SELECT_RIGHT = 9, PUSH_A = 1, PUSH_D = 3; encoder D is 3. The first press must come LATE
# (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1800000000

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
LOG="$ENAGA_BUILD/scenario_linein.log"

keys=(--key 22@300000000 --key 21@380000000)
for i in 0 1 2 3; do keys+=(--key "8@$((440000000 + i * 60000000))"); done
keys+=(--key 1@760000000 --key 9@840000000 --key 3@920000000 --turn 3:1@1000000000)

echo "running the emulator for $CYCLES cycles: play, MIXER IN, the input on, IN+ gain and jack ..."
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

screens = [line for line in log if line.startswith("scr ")]
check("scr MIXER IN" in screens, "drew IN", f"screens={screens}")
check(screens and screens[-1] == "scr MIXER IN+", "and ended on IN+", f"last={screens[-1:]}")
moves = [line for line in log if line.startswith("IN ")]
check(moves == ["IN ON", "IN GAIN HIGH", "IN LINE L"],
      "push A, push D and D turned each reached the strip, in order", f"moves={moves}")
check("INPUT REFUSED" not in log, "the codec took each route", f"log={log}")

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

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
