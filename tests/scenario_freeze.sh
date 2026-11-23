#!/usr/bin/env bash
# Freeze (FUNC menu FREEZE, dsp/frozen.hpp) in the emulator, on the factory beat while it plays:
#
#   PLAY, DOWN x6                          -> track 7 selected (the CHORD, VA POLY: the dearest)
#   FUNC + HOME, UP, OK                    -> FREEZE is the menu's last row: 1 bar armed
#   ...one bar later                       -> `frz 64`: track 7 plays its loop from RAM
#
# The emulator cannot read engine memory, so this reads the app's `frz <mask>` log line
# (app/app.hpp, update()). It also prints the LAST block's cycles, which by the end of the run
# is a block with track 7 frozen: compare it with the same figure from a run without the menu
# keys (--baseline). One block is a noisy figure -- which drum hits fall in it moves it by
# points -- so the number that counts is the unit's LOAD (docs/UNIT_CHECKLIST.md, 13n).
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F4 = 27, F5 = 22, OK = 23, UP = 17,
# DOWN = 25. A chord is FUNC pressed and the second key 5 M cycles later (tests/scenario_m6.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2400000000
BASELINE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --cycles) CYCLES="$2"; shift 2 ;;
        --baseline) BASELINE=1; shift ;;
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
LOG="$ENAGA_BUILD/scenario_freeze.log"

KEYS=(--key 22@300000000
      --key 25@360000000 --key 25@420000000 --key 25@480000000
      --key 25@540000000 --key 25@600000000 --key 25@660000000)
if [ "$BASELINE" -eq 0 ]; then
    KEYS+=(--key 19@720000000 --key 27@725000000
           --key 17@780000000
           --key 23@840000000)
fi

echo "running the emulator for $CYCLES cycles: PLAY, track 7$([ "$BASELINE" -eq 0 ] && echo ', FUNC menu FREEZE') ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" "${KEYS[@]}" \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

python3 - "$LOG" "$BASELINE" <<'PY'
import json, re, sys

CYCLES_PER_BLOCK = 496_000
baseline = sys.argv[2] == "1"

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
frozen = [line for line in log if line.startswith("frz ")]
if baseline:
    check(not frozen, "nothing froze in the baseline", f"log={frozen}")
else:
    check("frz 64" in frozen, "track 7 froze (frz 64)", f"log={log}")
    check(frozen and frozen[-1] == "frz 64", "and stayed frozen", f"last={frozen[-1:]}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
last = report.get("audio_cycles_last", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}% (the live bars before the freeze)")
print(f"last block: {100.0 * last / CYCLES_PER_BLOCK:.2f}%  ({last} cycles)"
      f"{'  -- baseline, track 7 live' if baseline else '  -- track 7 frozen'}")
print("note: run once with --baseline and compare the last-block figures; docs/BUDGET.md, Freeze.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
