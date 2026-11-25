#!/usr/bin/env bash
# MOD slots emulator scenario (core/modslot.hpp, app/ui_modslots.hpp): a slot is learnt on the
# C674x with the real font, by the gesture itself, and the beat keeps playing inside the load
# target with the slot moving its parameter on every note.
#
# Play, EDIT, four selection presses to the LFO page, push C (slot 1 learns, on LFO), push C twice
# more (ENV, then VEL), four presses back to SRC, turn C twenty clicks (Enaga v0.3.0's --turn),
# and OK: the slot is VEL on track 1's third SRC knob, logged as `M1 VEL>... +20`. From there each
# kick's velocity moves that byte, so the track re-prepares on every note.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, SELECT_RIGHT = 9,
# SELECT_LEFT = 8, PUSH_C = 2, OK = 23. The first press comes late: the panel link is not
# streaming in the first ~200 M cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1500000000

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
LOG="$ENAGA_BUILD/scenario_modslots.log"

echo "running the emulator for $CYCLES cycles: learn MOD slot 1 as VEL on the kick ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@360000000 \
    --key 9@400000000 --key 9@440000000 --key 9@480000000 --key 9@520000000 \
    --key 2@560000000 --key 2@600000000 --key 2@640000000 \
    --key 8@680000000 --key 8@720000000 --key 8@760000000 --key 8@800000000 \
    --turn 2:20@840000000 \
    --key 23@1150000000 \
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
check(report.get("audio_blocks", 0) > 0, "init() ran to completion (its log line has scrolled out by the end)")
check("run int" in log, "the transport started", f"log={log}")
check("scr EDIT SRC" in screens, "EDIT opened on SRC", f"screens={screens}")
check("scr EDIT LFO" in screens, "four selection presses reached the LFO page", f"screens={screens}")
check("MOD1 LFO: TURN A KNOB" in log, "push C on the LFO page started slot 1's learn", f"log={log}")
check("MOD1 ENV: TURN A KNOB" in log and "MOD1 VEL: TURN A KNOB" in log,
      "two more pushes walked the source to ENV and VEL", f"log={log}")
learnt = [line for line in log if re.match(r"M1 VEL>\S+ \+20$", line)]
check(len(learnt) == 1, "the turn and OK learnt slot 1: VEL, +20", f"log={log}")

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
print("note: no cache model here. One slot on VEL re-prepares the kick on each of its notes; a slot")
print("      that is off costs two compares a block (docs/BUDGET.md, \"MOD slots\").")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
