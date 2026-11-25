#!/usr/bin/env bash
# NOTE view (docs/CONTROLS.md, knobs; core/tuning.hpp) in the emulator, on the factory beat:
#
#   PLAY, EDIT                  -> track 1 (KICK) on its SRC page, A is TUN
#   push A                      -> `TUN AS NUMBERS` logged
#   push A again                -> `TUN AS NOTES` logged
#
# The emulator can push a knob but not turn one or read the screen, so this checks that the push
# reaches the switch on the C674x build and that nothing else (the kill EQ, a lock clear) took it.
# What the note reads and how a turn moves are tests/test_tuning.cpp and tests/test_ui.cpp.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 (PLAY), F8 = 28 (EDIT), PUSH_A = 1.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1100000000
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
LOG="$ENAGA_BUILD/scenario_tune.log"

echo "running the emulator for $CYCLES cycles: PLAY, EDIT, push A twice ..."
python3 -B "$ENAGA_ROOT/tools/emulate.py" "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@400000000 \
    --key 1@600000000 \
    --key 1@800000000 \
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

numbers = log.index("TUN AS NUMBERS") if "TUN AS NUMBERS" in log else -1
notes = log.index("TUN AS NOTES") if "TUN AS NOTES" in log else -1
check(numbers >= 0, "push A on EDIT SRC switched TUN to numbers", f"log={log}")
check(notes > numbers >= 0, "push A again switched it back to notes", f"log={log}")
check(not any(l.startswith("KILL") for l in log), "the push was not the kill EQ", f"log={log}")

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
