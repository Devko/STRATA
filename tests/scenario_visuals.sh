#!/usr/bin/env bash
# The live screens in the emulator (docs/CONTROLS.md, "The screen frame"; docs/BUDGET.md, "The
# screen"): the beat plays on GRID long enough for draw()'s own timer to log its average
# ("draw 0.85 ms", app/app.hpp count_draw_time), with the frame's decibel meter, the progress
# line's beat marks and every later live view drawing on the C674x with the real font. The
# figure is the emulator's (no caches), so it compares versions; the unit shows the same reading
# on SETUP.
#
#   PLAY                      -> the default beat, on GRID
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY. The first press comes late: the
# panel link is not streaming in the first ~200 M cycles. 2.6 G cycles is about 7 s at 372 MHz,
# past the 5 s the log waits for.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2600000000
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
LOG="$ENAGA_BUILD/scenario_visuals.log"

echo "running the emulator for $CYCLES cycles: the live screens and draw()'s cost ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
python3 - "$LOG" <<'PY' || rc=$?
import json, re, sys

CYCLES_PER_BLOCK = 496_000
DRAW_LIMIT_MS = 5.0       # a tenth of the 50 ms frame: past it, key handling waits on drawing

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

draws = [line for line in log if line.startswith("draw ")]
check(len(draws) == 1, "draw() logged its average once", f"log={log}")
draw_ms = None
if draws:
    m = re.match(r"draw (\d+\.\d\d) ms$", draws[0])
    check(m is not None, "in the expected form", draws[0])
    if m:
        draw_ms = float(m.group(1))
        check(draw_ms < DRAW_LIMIT_MS, "draw() is inside its limit",
              f"{draw_ms:.2f} ms of {DRAW_LIMIT_MS} ms")

blocks = report.get("audio_blocks", 0)
check(blocks > 3000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
if draw_ms is not None:
    print(f"draw(): {draw_ms:.2f} ms a frame, averaged over 64 frames")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
