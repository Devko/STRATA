#!/usr/bin/env bash
# Song sections in the emulator: CHAIN (the factory project's five entries), OK copies entry 1 as
# entry 2 under the cursor, push A opens its
# SECTION view ("SECT 2/6"), D turns SCN to 3, the screen key goes to MOVE ("MOVE PAGE"), B turns
# NRG off `--` and push B puts it back ("NRG --"), A turns MRP to SNAP, CLR goes back ("CHAIN").
# Then the FUNC menu's CHAIN row starts the chain; entry 2 applies the section's fields on the
# audio side and logs "SECTION 2 S3 SNAP". What each field does to the sound is
# tests/test_engine.cpp's; this proves the view draws on the C674x and that a section reaches the
# engine and back, under the load target and without an overrun. The emulator cannot hold OK for
# 0.5 s, so the quick fill is host-tested only (tests/test_ui.cpp, ui_section_*).
#
#   PLAY                        -> the factory beat
#   PROJECT, SELECT_RIGHT       -> CHAIN
#   OK                          -> entry 2 of 6
#   push A                      -> SECT 2/6
#   turn D +3                   -> SCN 3
#   SELECT_RIGHT                -> MOVE PAGE
#   turn B +1, push B           -> NRG --
#   turn A +1                   -> MRP SNAP
#   CLR                         -> CHAIN
#   FUNC + HOME, DOWN x9, OK    -> CHAIN ON: SECTION 2 S3 SNAP
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F3 = 20, SELECT_RIGHT = 9, OK = 23,
# push A = 1, push B = 0, CLR = 30, F1 = 19, F4 = 27, DOWN = 25. Encoders: A 0, B 1, D 3. A chord
# is FUNC and the second key 5 M cycles later, inside FUNC's 20 M-cycle press (scenario_controls).
# The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2400000000
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
LOG="$ENAGA_BUILD/scenario_sections.log"

keys=(--key 22@300000000 --key 20@380000000 --key 9@440000000 --key 23@500000000)
keys+=(--key 1@560000000 --turn 3:3@620000000 --key 9@680000000)
keys+=(--turn 1:1@740000000 --key 0@800000000 --turn 0:1@860000000 --key 30@920000000)
keys+=(--key 19@1000000000 --key 27@1005000000)
for i in 0 1 2 3 4 5 6 7 8; do
    keys+=(--key "25@$((1060000000 + i * 40000000))")
done
keys+=(--key 23@1460000000)

echo "running the emulator for $CYCLES cycles: a section's fields, then the chain plays it ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" "${keys[@]}" \
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
# The log ring keeps the last 16 lines; this run writes about a dozen after PLAY.
check("SECT 2/6" in log, "push A on CHAIN opened the SECTION view", f"log={log}")
check("MOVE PAGE" in log, "the screen key turned to MOVE", f"log={log}")
check("NRG --" in log, "push B put NRG back to --", f"log={log}")
check("CHAIN" in log, "CLR went back to the list", f"log={log}")
check("SECTION 2 S3 SNAP" in log, "the chain's start applied entry 2's fields", f"log={log}")
if "SECT 2/6" in log and "SECTION 2 S3 SNAP" in log:
    check(log.index("SECT 2/6") < log.index("SECTION 2 S3 SNAP"), "edited before it played",
          f"log={log}")

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
print("note: no cache model here. docs/BUDGET.md records cl6x as worth about 1.7x and the 60%")
print("      target depends on it. The unit decides.")

sys.exit(1 if failures else 0)
PY

if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
