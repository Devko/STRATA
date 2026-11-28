#!/usr/bin/env bash
# Hold PROJECT + STEP queues a pattern from any screen (usability review, 2026-11-28). The
# emulator cannot read the screen, so the scenario reads the log: the chord's notice
# (app/ui_project.hpp, queue_shown_pattern logs it), the `scr` line app/ui_lights.hpp writes on a
# screen change, and the `g<mask> u<undo>` line, whose undo depth would rise if a STEP under the
# chord had toggled a trig on GRID.
#
#   PLAY                                   (GRID is up from the boot)
#   hold PROJECT, STEP 2                   -> "A02 QUEUED END", no "scr PROJECT"
#   tap PROJECT                            -> "scr PROJECT"
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F3 = 20, STEP n = 31 + n. A press is
# released 20 M cycles later, so PROJECT (380 M) is still down at the STEP (385 M) and comes up
# before it, the order the chord's release handling has to cope with. The first press must come
# LATE (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_queuehold.log"

echo "running the emulator for $CYCLES cycles: hold PROJECT + STEP, then a tap of PROJECT ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 20@380000000 --key 33@385000000 \
    --key 20@600000000 \
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

queued = index("A02 QUEUED END")
project = index("scr PROJECT")
check(queued >= 0, "hold PROJECT + STEP 2 queued A02", f"log={log}")
check(project > queued, "PROJECT opened only on the later tap", f"log={log}")
# The boot logs the pattern at u0; a STEP that reached GRID would log u1.
check(not any(re.match(r"g[0-9a-f]+ u[1-9]", l) for l in log),
      "no STEP under the chord reached the pattern", f"log={log}")

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
