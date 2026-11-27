#!/usr/bin/env bash
# WID, the stereo width, in the emulator: MIXER's WID 5-8 draws on the C674x with the default beat
# playing, two pushes put track 7 (the VA POLY chords) into unison, so every chord note plays two
# voices panned hard left and right through the stereo insert chain for the rest of the run, and
# the load is inside the target. The peak below is the width's cost: docs/BUDGET.md "Stereo
# width" asks for it with cl6x and with ENAGA_TI_CGT=none. Compare it with scenario_m7.sh, the
# same beat with every track mono.
#
#   PLAY                          -> the default beat
#   MIXER, SELECT_RIGHT x6        -> WID 5-8 (TRK 5-8, DCK 1-4, DCK 5-8, DUCK, WID 1-4, WID 5-8)
#   push C, push C                -> T7 WIDTH 48, then T7 UNISON 96, logged
#
# --spread stops after the first push: the chords fanned across the field with no extra voices,
# which is the stereo chain's and the voices' panning cost alone.
#
# What the zones do to the voices is tests/test_width.cpp's; the budget with unison is
# tests/test_engine.cpp's (engine_unison_notes_count_two_voices_...).
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F7 = 21, SELECT_RIGHT = 9, PUSH_C = 2.
# The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1800000000
SPREAD=0

while [ $# -gt 0 ]; do
    case "$1" in
        --cycles) CYCLES="$2"; shift 2 ;;
        --spread) SPREAD=1; shift ;;
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
LOG="$ENAGA_BUILD/scenario_width.log"

keys=(--key 22@300000000 --key 21@380000000)
for i in 0 1 2 3 4 5; do keys+=(--key "9@$((440000000 + i * 60000000))"); done
keys+=(--key 2@820000000)
if [ "$SPREAD" -eq 0 ]; then keys+=(--key 2@880000000); fi

echo "running the emulator for $CYCLES cycles: play, MIXER WID 5-8, the chords into unison ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" "${keys[@]}" \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
SPREAD="$SPREAD" python3 - "$LOG" <<'PY' || rc=$?
import json, os, re, sys

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
check(screens and screens[-1] == "scr MIXER WID 5-8", "drew WID 5-8 and stayed there",
      f"screens={screens}")
widths = [line for line in log if line.startswith("T7 ")]
wanted = ["T7 WIDTH 48"] if os.environ.get("SPREAD") == "1" else ["T7 WIDTH 48", "T7 UNISON 96"]
check(widths == wanted, "the pushes stepped track 7 to spread, then unison", f"widths={widths}")

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
