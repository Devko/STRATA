#!/usr/bin/env bash
# CLEAR PATTERN (FUNC menu, docs/CONTROLS.md) in the emulator, on the factory beat while it plays:
#
#   PLAY                                  -> the beat runs, track 1's grid line is not empty
#   FUNC + HOME, UP x2, OK, OK             -> round from SAVE past LOAD DEMO to CLEAR PATTERN:
#                                            `g0000 u1`: track 1 empty, one undo level
#   FUNC + CLR                            -> the grid line is back, undo depth 0
#
# The emulator cannot read pattern memory, so this reads the app's `g<mask> u<depth>` log line
# (app/app.hpp, update()), which is track 1's first page as a bit mask. That every OTHER track is
# empty too is tests/test_ui.cpp's.
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F4 = 27, F5 = 22, OK = 23, CLR = 30,
# UP = 17. A chord is FUNC pressed and the second key 5 M cycles later (tests/scenario_m6.sh).
# The two OKs are 60 M cycles (0.16 s) apart, well inside the 2 s arm.
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
LOG="$ENAGA_BUILD/scenario_clear.log"

echo "running the emulator for $CYCLES cycles: PLAY, FUNC menu CLEAR PATTERN, FUNC + CLR ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@400000000 --key 27@405000000 \
    --key 17@460000000 --key 17@520000000 \
    --key 23@760000000 --key 23@820000000 \
    --key 19@940000000 --key 30@945000000 \
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

grids = [line for line in log if re.fullmatch(r"g[0-9A-F]{4} u[0-9]+", line)]
cleared = next((i for i, line in enumerate(grids) if line == "g0000 u1"), -1)
check(any(line != "g0000 u0" and line.endswith(" u0") for line in grids[:max(cleared, 0)]),
      "track 1 had trigs before the clear", f"grids={grids}")
check(cleared >= 0, "CLEAR PATTERN emptied track 1 as one undo level", f"grids={grids}")
check(cleared >= 0 and any(line != "g0000 u0" and line.endswith(" u0")
                           for line in grids[cleared + 1:]),
      "FUNC + CLR brought the pattern back", f"grids={grids}")
check("scr MENU" in log, "the FUNC menu opened", f"log={log}")

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
