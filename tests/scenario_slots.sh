#!/usr/bin/env bash
# Backlog D1, D2 and D5 in the emulator (2026-11-25): the performance mixer's slot 7 is MUTE BUS
# and slot 13 the scene morph, and the retired FUNC chords say where their moves went instead
# of doing them. The picks and the chords' hints are logged (app/app.hpp, notice_logged), since
# the emulator cannot read the screen; it turns no VALUE here, so the amounts are
# tests/test_perffx.cpp's and tests/test_ui.cpp's.
#
#   PLAY
#   push VALUE + STEP 7      -> "7 MUTE BUS"
#   push VALUE + STEP 13     -> "MORPH 0" (a control: picked at where the morph is)
#   push VALUE + STEP 13     -> "MORPH KEPT: VALUE TEMPO"
#   FUNC + push D            -> "KILL ALL" (D2, 2026-11-28), and no tape stop
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, PUSH VALUE = 4, STEP n = 31 + n,
# F1 = 19, PUSH D = 3. A chord is the first key pressed and the second 5 M cycles later
# (tests/scenario_m6.sh). The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=800000000
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
LOG="$ENAGA_BUILD/scenario_slots.log"

echo "running the emulator for $CYCLES cycles: mixer slots 7 and 13, a retired chord ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 4@380000000 --key 38@385000000 \
    --key 4@460000000 --key 44@465000000 \
    --key 4@540000000 --key 44@545000000 \
    --key 19@620000000 --key 3@625000000 \
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

def index(line):
    return log.index(line) if line in log else -1

bus = index("7 MUTE BUS")
morph = index("MORPH 0")
kept = index("MORPH KEPT: VALUE TEMPO")
tape = index("KILL ALL")
check(bus >= 0, "push VALUE + STEP 7 picked MUTE BUS", f"log={log}")
check(morph > bus, "push VALUE + STEP 13 picked MORPH after it", f"log={log}")
check(kept > morph, "STEP 13 again let the morph go and kept it", f"log={log}")
check(tape > kept, "FUNC + push D killed every band", f"log={log}")
check(not any(l.startswith("TAPE STOP") for l in log), "and held no tape stop", f"log={log}")
# The boot logs the pattern once at u0; a STEP that reached it would log u1.
check(not any(re.match(r"g[0-9a-f]+ u[1-9]", l) for l in log),
      "no STEP under VALUE reached the pattern", f"log={log}")

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
