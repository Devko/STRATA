#!/usr/bin/env bash
# FX GLIDE and the performance mixer's new STEP release in the emulator (backlog Phase 2, LOCK
# + FX GLIDE, 2026-11-25), on the factory beat while it plays, with the glide at its default of
# a beat:
#
#   PLAY
#   push VALUE + STEP 4      -> "4 ECHO RISER"
#   push VALUE + STEP 10     -> "10 PUMP" (ECHO rides out over the beat; at 0 there is nothing
#                               to ride, but the ride code runs on the C674x)
#   push VALUE + STEP 10     -> "FX OFF: VALUE TEMPO", now on the RELEASE (a held STEP is the
#                               momentary push)
#   FUNC + HOME, UP x5       -> the FUNC menu, round from SAVE to FX GLIDE (above FREEZE)
#   RIGHT                    -> "FX GLIDE 1 BAR", and the menu drawn with the new row
#   CLR                      -> back
#
# The emulator holds a key for 54 ms and cannot press a pad or turn VALUE, so the momentary push
# (a STEP held 300 ms), the rides' amounts and LOCK (FUNC + the MUTATE pad) are
# tests/test_ui.cpp's, tests/test_perffx.cpp's and tests/test_lockstore.cpp's.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, PUSH VALUE = 4, STEP n = 31 + n,
# F1 = 19, F4 = 27, UP = 17, RIGHT = 18, CLR = 30. A chord is the first key pressed and the
# second 5 M cycles later (tests/scenario_m6.sh). The first press must come LATE
# (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_glide.log"

echo "running the emulator for $CYCLES cycles: mixer picks with FX GLIDE, the menu row ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 4@380000000 --key 35@385000000 \
    --key 4@460000000 --key 41@465000000 \
    --key 4@540000000 --key 41@545000000 \
    --key 19@620000000 --key 27@625000000 \
    --key 17@650000000 --key 17@680000000 --key 17@720000000 --key 17@760000000 \
    --key 17@800000000 \
    --key 18@860000000 \
    --key 30@920000000 \
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

echo = index("4 ECHO RISER")
crush = index("10 PUMP")
off = index("FX OFF: VALUE TEMPO")
menu = index("scr MENU")
glide = index("FX GLIDE 1 BAR")
check(echo >= 0, "push VALUE + STEP 4 picked ECHO RISER", f"log={log}")
check(crush > echo, "push VALUE + STEP 10 picked PUMP over it", f"log={log}")
check(off > crush, "STEP 10 again let it go on the release", f"log={log}")
check(menu > off, "FUNC + HOME opened the menu", f"log={log}")
check(glide > menu, "FX GLIDE stepped from 1 BEAT to 1 BAR", f"log={log}")
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
