#!/usr/bin/env bash
# Tap tempo and the nudge in the emulator (core/taptempo.hpp, docs/CONTROLS.md): four taps of
# VALUE on GRID with the beat running, then FUNC + push VALUE + one detent.
#
# What it proves is what the host cannot: the tap path (VALUE's release, value_reset, the
# clock's set_tempo from the main context) and the nudge's set_tempo and its timed return run on
# the C674x without a crash, an overrun or a stall, and the tempo the taps give is about what the
# key timing says. The emulator has no MIDI IN, so PROG CHG OUT and the song position pointer are
# host-tested only (tests/test_midi.cpp, tests/test_engine.cpp) and walked on the unit (W68).
#
# Observable through Enaga's log (notice_logged): `TAP TEMPO` on the first tap, `TAP 120.0` from
# the second, `NUDGE FASTER` on the detent; `run int` from the transport.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 (PLAY), PUSH_VALUE = 4, F1 = 19. A press
# is released 20 M cycles later (54 ms), well inside VALUE's 0.5 s tap. 186 M cycles is 500 ms
# at 372 MHz: four taps 120 BPM apart. VALUE's push comes 2 M cycles into F1's hold and the turn
# 2 M after that, inside both (FUNC + push VALUE + turn).
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
LOG="$ENAGA_BUILD/scenario_tempo.log"

echo "running the emulator for $CYCLES cycles: play, four VALUE taps, FUNC + push VALUE + turn ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 4@400000000 --key 4@586000000 --key 4@772000000 --key 4@958000000 \
    --key 19@1050000000 --key 4@1052000000 --turn 4:1@1054000000 \
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
check("run int" in log, "the transport started", f"log={log}")
check("TAP TEMPO" in log, "the first VALUE tap started a count", f"log={log}")
taps = [l for l in log if re.fullmatch(r"TAP \d+\.\d", l)]
check(len(taps) >= 3, "the next three taps each set a tempo", f"lines={taps}")
if taps:
    bpm = float(taps[-1].split()[1])
    # The keys are 500 ms apart in cycles; the emulator's millisecond clock may run a little off
    # real time, so the window is generous. A wrong stamp or a lost tap lands far outside it.
    check(100.0 <= bpm <= 140.0, "the taps give about 120 BPM", f"{bpm}")
check("NUDGE FASTER" in log, "FUNC + push VALUE + turn nudged", f"log={log}")

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

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
