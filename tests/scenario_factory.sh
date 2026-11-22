#!/usr/bin/env bash
# Factory content emulator scenario: the default beat plays, and queueing A04 (PEAK, the
# heaviest factory pattern) switches to it at the end of the pass with the transport RUNNING.
#
# core/factory.hpp fills bank A with four patterns. tests/test_factory.cpp proves on the host that
# they are valid, load and play; this proves the unit's own build plays the heaviest of them
# inside the budget: PEAK has 16th hats with ratchets, an acid line, the rave stab and both sends
# open, on top of everything A01 has.
#
# The effect is app/app.cpp's `g<16 trig bits in hex> u<undo depth>` line for the selected track
# (the kick). A01's kick is on steps 1, 5, 9, 13 = g1111; PEAK adds a FILL roll on step 16, so its
# page is g9111. The switch waits for the end of the 16-step pass (queue mode END), about 1.85 s
# at 130 BPM, so the run is long enough for PEAK to play for well over a bar after it.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F3 = 20, STEP n = 31 + n.
# The first press must come LATE: Enaga's panel MCU link is not streaming in the first ~200 M
# cycles and a press before that is silently swallowed (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_factory.log"

echo "running the emulator for $CYCLES cycles: play A01, queue A04 while playing ..."
python3 -B "$ENAGA_ROOT/tools/emulate.py" "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 20@400000000 \
    --key 34@460000000 \
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
check("strata ready" in log, "init() ran to completion")

# init() now also opens the last project and allocates 85 MiB (the sample region, the loader's
# staging buffers and the beat-repeat ring). A failure there returns false from App::init, which
# Enaga reports as APP INIT FAILED rather than APP RUNNING -- so the check above is also the
# memory check, and the first thing to suspect if it fails.
check(report.get("app_name") == "STRATA", "the app Enaga booted is this one",
      f'name={report.get("app_name")!r}')

check("run int" in log, "the transport started", f"log={log}")

grids = [line for line in log if re.fullmatch(r"g[0-9A-F]{4} u\d+", line)]
check(grids and grids[0] == "g1111 u0", "it started on the factory GROOVE", f"first={grids[:1]}")
check("g9111 u0" in grids, "queueing A04 switched to PEAK at the end of the pass", f"grids={grids}")

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
print("note: no cache model here, and this is the GCC-only build -- compare versions, not")
print("      targets. docs/BUDGET.md records cl6x as worth about 2.2x. The unit decides.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
