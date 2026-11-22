#!/usr/bin/env bash
# M7 emulator scenario: the new screens render on the DSP.
#
# What this can and cannot reach. The emulator core exposes `key_input` and NOTHING else -- no
# pads and no encoders (checked: there is no pad or encoder entry point in emulator/core/core.h).
# Every send and master setting is on an encoder, so none of them can be CHANGED from here;
# tests/test_ui.cpp and tests/test_engine.cpp own that, against the fake host.
#
# What is reachable with keys alone is the thing a host test cannot prove: that the four new
# MIXER pages and PROJECT's SETUP page DRAW on the C674x, in the real build, with the real font
# and canvas. A draw that ran off the canvas or dereferenced a null machine is a hard fault on
# the unit and a silent pass on the host, where `tests/test_ui.cpp` draws into a plain array.
# The SETUP page is the riskiest of the five: it is the only screen that reads the clock state
# and the MIDI channel, which on the host come from the fake and here come from Enaga.
#
# The observable effect is app/app.hpp's `scr <MODE> <PAGE>` line, logged whenever the screen
# changes -- which is also what makes it worth logging at all: in a unit log it is the
# difference between "the app is wedged" and "the app is on a page I did not expect".
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F7 = 21, F3 = 20, SELECT_RIGHT = 9,
# OCTAVE_RIGHT = 31, OCTAVE_LEFT = 16. The first press must come LATE: Enaga's panel MCU link is
# not streaming in the first ~200 M cycles and a press before that is silently swallowed
# (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1200000000

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
LOG="$ENAGA_BUILD/scenario_m7.log"

echo "running the emulator for $CYCLES cycles: play, then every MIXER page, then SETUP ..."
python3 -B "$ENAGA_ROOT/tools/emulate.py" "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 21@380000000 \
    --key 9@440000000 \
    --key 9@490000000 \
    --key 9@540000000 \
    --key 9@590000000 \
    --key 9@640000000 \
    --key 9@690000000 \
    --key 9@740000000 \
    --key 20@800000000 \
    --key 31@860000000 \
    --key 31@910000000 \
    --key 31@960000000 \
    --key 16@1020000000 \
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# Every MIXER page, including the four that are new, and then SETUP. Each one of these means a
# whole screen was drawn on the C674x without faulting -- which is the claim this scenario makes
# and the one a host test cannot make.
wanted = [
    "scr MIXER TRK 1-4",
    "scr MIXER TRK 5-8",
    "scr MIXER DLY",
    "scr MIXER DLY+",
    "scr MIXER REV",
    "scr MIXER REV+",
    "scr MIXER ROUTE",
    "scr MIXER MST",
    "scr PROJECT SETUP",
]
for name in wanted:
    check(name in screens, f"drew {name[4:]}", "" if name in screens else f"screens={screens}")

# PROJECT walks slots, CHAIN, PATTERN, SETUP: three OCTAVE_RIGHTs reach SETUP, and one
# OCTAVE_LEFT steps back to PATTERN, so the octave keys move both ways. (The log ring holds 16
# lines; this sequence ends with exactly the first MIXER page still in it.)
check(screens and screens[-1] == "scr PROJECT PATTERN", "and left SETUP again",
      f"last={screens[-1:]}")

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
print("note: no cache model here, and this is the GCC-only build. docs/BUDGET.md records cl6x")
print("      as worth about 2.2x, and the 60% target depends on it. The unit decides.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
