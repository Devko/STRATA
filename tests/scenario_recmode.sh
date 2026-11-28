#!/usr/bin/env bash
# REC MODE in the emulator (a chord track's steps record every note played into them, as locks
# 32-36; core/stepnotes.hpp), on the factory beat while it plays:
#
#   PLAY
#   F2                       -> REC on
#   FUNC + HOME, DOWN x5     -> the FUNC menu, from SAVE to REC MODE (under COUNT-IN)
#   OK                       -> "REC ADDS NOTES", the row drawn with ADD
#   OK                       -> "REC REPLACES"
#   CLR                      -> back
#
# The emulator cannot press a pad or send MIDI IN, so it cannot play a chord into a step: what
# gets recorded, and what a step with extra notes plays, are tests/test_ui.cpp's (polyrec_*) and
# tests/test_midi.cpp's. This holds the menu row and its log on the C674x, and the load.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F2 = 26, F1 = 19, F4 = 27, DOWN = 25,
# OK = 23, CLR = 30. A chord is the first key pressed and the second 5 M cycles later
# (tests/scenario_m6.sh). The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1000000000
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
LOG="$ENAGA_BUILD/scenario_recmode.log"

echo "running the emulator for $CYCLES cycles: REC on, the FUNC menu's REC MODE row ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 26@360000000 \
    --key 19@420000000 --key 27@425000000 \
    --key 25@460000000 --key 25@495000000 --key 25@530000000 --key 25@565000000 \
    --key 25@600000000 \
    --key 23@780000000 \
    --key 23@860000000 \
    --key 30@940000000 \
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

menu = index("scr MENU")
add = index("REC ADDS NOTES")
replace = index("REC REPLACES")
check(menu >= 0, "FUNC + HOME opened the menu", f"log={log}")
check(add > menu, "OK on REC MODE turned ADD on", f"log={log}")
check(replace > add, "OK again went back to REPLACE", f"log={log}")

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
