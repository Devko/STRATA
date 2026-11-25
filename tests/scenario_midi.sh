#!/usr/bin/env bash
# MIDI out emulator scenario: track 1 becomes MID on the running beat, the MACHINE page draws its
# nine machines and the MIDI OUT line, and both STOPs run MIDI out's note-off and All Notes Off
# paths -- on the C674x, in the real build.
#
# What this can and cannot reach. It presses keys only (scenario_m7.sh), so no MIDI OUT channel
# is SET here, and the emulator has no MIDI IN. So MID plays with its
# channel OFF and sends nothing. What it proves is what the host cannot: the MACHINE page's new
# 3 x 3 grid and its MIDI OUT line draw on the device with the real font, the MID machine's
# empty render path and the engine's MIDI bookkeeping run inside audio() with the beat playing,
# and the load stays inside the target. tests/test_midi.cpp and tests/test_ui.cpp own every
# byte that would be sent; docs/UNIT_CHECKLIST.md owns hearing it from a synth.
#
# Observable through Enaga's log: `scr MACHINE` when the page opens, `mch 1 MID` when UP walks
# track 1 from KCK back round to MID (app/app.hpp, walk_machine; four presses since FM4, WPL and
# WMN came after MID in the list), `run int` and `stopped` from the
# transport.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F8 = 28, UP = 17, F6 = 29,
# F4 = 27. A press is released 20 M cycles later, so F1 at t is still down when F8 arrives at
# t + 5 M. The first press comes late: the panel link is not streaming in the first ~200 M
# cycles (scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1100000000

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
LOG="$ENAGA_BUILD/scenario_midi.log"

echo "running the emulator for $CYCLES cycles: play, track 1 to MID, both STOPs, play ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@380000000 --key 28@385000000 \
    --key 17@460000000 --key 17@490000000 --key 17@520000000 --key 17@550000000 \
    --key 27@600000000 \
    --key 29@700000000 \
    --key 29@760000000 \
    --key 22@860000000 \
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
# The log keeps its last 16 lines, and the four UP presses (and a WAVE table built while track 1
# passes WMN) can scroll "strata ready" out. A drawn screen comes only after init().
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log or len(log) == 16, "the transport started", f"log={log}")
check("scr MACHINE" in log, "F1 + EDIT drew the machine page", f"log={log}")
check(next((l for l in reversed(log) if l.startswith("mch 1 ")), None) == "mch 1 MID", "UP walked track 1 round to MID", f"log={log}")
runs = [line for line in log if line.startswith("run ")]
last_stop = len(log) - 1 - log[::-1].index("stopped") if "stopped" in log else len(log)
check(any(l.startswith("run ") for l in log[last_stop + 1:]), "the transport stopped and started again with MID on track 1",
      f"lines={runs}")

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
print("note: no cache model here. docs/BUDGET.md records cl6x as worth about 1.7x on")
print("      STRATA's scenarios, and the 60% target depends on it. The unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
