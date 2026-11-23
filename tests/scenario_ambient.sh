#!/usr/bin/env bash
# The sends' ambient modes in the emulator: the MIXER's DLY+ and REV+ pages draw on the C674x,
# push A on each throws its mode (DLY LONG, REV HALL), and the load with the default beat feeding
# the LONG delay and the HALL reverb -- with the classic delay and reverb still ringing out their
# tails for the first seconds, which is the most both sends ever cost at once -- is inside the
# target. docs/BUDGET.md, "The sends' ambient modes", asks for this figure with cl6x and with
# ENAGA_TI_CGT=none. Compare it with scenario_m3.sh, the same beat on the classic modes.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F7 = 21, SELECT_RIGHT = 9, PUSH_A = 1.
# The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1600000000

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
LOG="$ENAGA_BUILD/scenario_ambient.log"

echo "running the emulator for $CYCLES cycles: play, MIXER DLY+ LONG, REV+ HALL ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 21@380000000 \
    --key 9@440000000 \
    --key 9@460000000 \
    --key 9@480000000 \
    --key 9@500000000 \
    --key 9@520000000 \
    --key 9@540000000 \
    --key 1@580000000 \
    --key 9@620000000 \
    --key 9@660000000 \
    --key 1@720000000 \
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# Both pages drew, and each push threw its send's mode, in order.
check("scr MIXER DLY+" in screens, "drew MIXER DLY+", f"screens={screens}")
check("scr MIXER REV+" in screens, "drew MIXER REV+", f"screens={screens}")
modes = [line for line in log if line.startswith("DLY ") or line.startswith("REV ")]
check(modes == ["DLY LONG", "REV HALL"], "the pushes switched to LONG and HALL", f"log={modes}")

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
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
