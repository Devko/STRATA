#!/usr/bin/env bash
# The controls concept (docs/CONTROLS.md) in the emulator: the gestures that moved, driven as key
# presses on the C674x build, each one proved by the screen it reaches.
#
#   HOME, HOME (0.11 s apart)  -> KEYS            HOME again -> GRID
#   PROJECT, then the screen key SELECT_RIGHT    -> PROJECT CHAIN (the pages left the pad-row keys)
#   FUNC + HOME                                  -> MENU, DOWN, CLR -> back to PROJECT CHAIN
#   FUNC + PROJECT ("pool")                      -> SAMPLES
#   FUNC + HOME, OK                              -> a save (the menu opens on SAVE)
#
# The emulator presses keys and turns encoders but has no pads, so the pad lights, the
# performance-pad names and the FUNC help are tests/test_ui.cpp's. A chord is FUNC pressed and the
# second key 5 M cycles later, inside FUNC's 20 M-cycle press (tests/scenario_m6.sh).
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F3 = 20, F4 = 27, F5 = 22, OK = 23,
# CLR = 30, DOWN = 25, SELECT_RIGHT = 9. The first press must come LATE (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_controls.log"

echo "running the emulator for $CYCLES cycles: KEYS, PROJECT pages, the FUNC menu, SAMPLES, save ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 27@360000000 --key 27@400000000 \
    --key 27@480000000 \
    --key 20@560000000 \
    --key 9@620000000 \
    --key 19@680000000 --key 27@685000000 \
    --key 25@740000000 \
    --key 30@800000000 \
    --key 19@860000000 --key 20@865000000 \
    --key 19@940000000 --key 27@945000000 \
    --key 23@1000000000 \
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# Each gesture reached the screen it names, in this order.
wanted = ["scr KEYS", "scr GRID", "scr PROJECT", "scr PROJECT CHAIN", "scr MENU",
          "scr PROJECT CHAIN", "scr SAMPLES", "scr MENU", "scr SAMPLES"]
at = 0
for line in screens:
    if at < len(wanted) and line == wanted[at]:
        at += 1
check(at == len(wanted), "HOME HOME, the screen keys, the FUNC menu and FUNC + PROJECT",
      f"reached {wanted[:at]}, screens={screens}")

saved = [line for line in log if line.startswith(("saved", "autosaved", "save failed"))]
check(any(line.startswith("saved") for line in saved), "FUNC + HOME, OK saved the project",
      f"lines={saved}")

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
print("note: no cache model here, and this is the GCC-only build. docs/BUDGET.md records cl6x")
print("      as worth about 2.2x, and the 60% target depends on it. The unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
