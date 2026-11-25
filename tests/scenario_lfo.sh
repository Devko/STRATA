#!/usr/bin/env bash
# LFO, ARP and per-track swing emulator scenario: the new screens draw on the C674x with the real
# font, and the beat keeps playing inside the load target while they are up.
#
# The emulator presses keys and turns no encoders (docs/HANDOFF.md, section 2), so this cannot
# switch an LFO on: it walks EDIT to its fifth page (scr EDIT LFO), pushes B there once (LFO TRIG
# RETRIG, logged: the push reaches the handler on the C674x), one more to the ARP page (scr EDIT ARP), and PROJECT to its PATTERN page
# (scr PROJECT PATTERN, where the eight track swings are), which is what proves both draw without
# a fault. The LOAD of eight running LFOs is measured separately, by the temporary factory edit in
# docs/BUDGET.md ("Per-track LFO", E7).
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, SELECT_RIGHT = 9,
# F3 = 20 PROJECT, OCTAVE_RIGHT = 31, PUSH_B = 0.The first press comes late: the panel link is not streaming
# in the first ~200 M cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=900000000

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
LOG="$ENAGA_BUILD/scenario_lfo.log"

echo "running the emulator for $CYCLES cycles: play, EDIT to LFO and ARP, PROJECT to PATTERN ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@360000000 \
    --key 9@400000000 --key 9@440000000 --key 9@480000000 --key 9@520000000 \
    --key 0@540000000 \
    --key 9@580000000 \
    --key 20@620000000 \
    --key 9@680000000 --key 9@720000000 \
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
screens = [line for line in log if line.startswith("scr ")]

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check("strata ready" in log, "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")
check("scr EDIT SRC" in screens, "EDIT opened on SRC", f"screens={screens}")
check("scr EDIT LFO" in screens, "four selection presses reached the LFO page", f"screens={screens}")
check("LFO TRIG RETRIG" in log, "push B on the LFO page walked TRIG to RETRIG", f"log={log}")
check("scr EDIT ARP" in screens, "a fifth reached the ARP page", f"screens={screens}")
check("scr PROJECT PATTERN" in screens, "PROJECT reached PATTERN (the track swings)",
      f"screens={screens}")

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
print("note: no cache model here. With every LFO and arp off this should match scenario_factory.sh to")
print("      within noise: an LFO or arp that is off costs a compare a block.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
