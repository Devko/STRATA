#!/usr/bin/env bash
# D12 in the emulator: the STEP keys are steps in KEYS and PERFORM, not scale notes or macro
# assignments. Each tap must reach the pattern (a new `g<mask> u<depth>` line, one undo level
# deeper) and leave the screen where it was.
#
#   PLAY, HOME HOME          -> KEYS
#   STEP 3                   -> a trig, u1, still KEYS
#   pad-row right            -> step page 2, still KEYS
#   FUNC + MIXER             -> PERFORM
#   STEP 5                   -> a trig on the page, u2, still PERFORM
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F4 = 27, F1 = 19, F7 = 21,
# OCTAVE_RIGHT = 31, STEP n = 31 + n. The first press must come LATE (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_stepkeys.log"

echo "running the emulator for $CYCLES cycles: STEP keys in KEYS and PERFORM ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 27@360000000 --key 27@400000000 \
    --key 34@480000000 \
    --key 31@560000000 \
    --key 19@640000000 --key 21@645000000 \
    --key 36@720000000 \
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

# The log from KEYS on: every line in order, so a tap can be placed on its screen.
def after(marker, lines):
    return lines[lines.index(marker) + 1:] if marker in lines else []

keys = after("scr KEYS", log)
in_keys = keys[:keys.index("scr PERFORM")] if "scr PERFORM" in keys else keys
perform = after("scr PERFORM", log)
check("scr KEYS" in log, "HOME HOME reached KEYS", f"log={log}")
check(any(l.startswith("g") and l.endswith(" u1") for l in in_keys),
      "STEP 3 in KEYS wrote a trig (u1)", f"keys={in_keys}")
check(not any(l.startswith("scr ") for l in in_keys), "KEYS stayed on screen", f"keys={in_keys}")
check("scr PERFORM" in log, "FUNC + MIXER reached PERFORM", f"log={log}")
check(any(l.startswith("g") and l.endswith(" u2") for l in perform),
      "STEP 5 in PERFORM wrote a trig (u2)", f"perform={perform}")
check(not any(l.startswith("scr ") for l in perform), "PERFORM stayed on screen",
      f"perform={perform}")

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
