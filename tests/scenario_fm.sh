#!/usr/bin/env bash
# FM4 emulator scenario: the default beat with track 7 (the chord) switched to FM4 on keys
# alone, so six FM voices a chord play inside the load target with no overruns.
#
# This scenario presses keys only (docs/HANDOFF.md, section 2, on what --turn could add), so the track
# keeps VA POLY's kit bytes: on FM4 those read as ALG, RAT, IDX and FBK from TUN, MOD, CUT and
# RES. That is a fair load case: an FM voice costs the same on every algorithm and ratio. Compare
# the peak with scenario_factory.sh on the same commit (docs/BUDGET.md, "FM4").
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, F1 = 19 FUNC,
# DOWN = 25. EDIT's DOWN walks the track; F1+F8 opens MACHINE, whose DOWN walks the machine list
# (VA POLY 7 -> 11 FM4: four presses). The first press comes late: the panel link is not
# streaming in the first ~200 M cycles.
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
LOG="$ENAGA_BUILD/scenario_fm.log"

echo "running the emulator for $CYCLES cycles: play, track 7 to FM4 ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 25@370000000 --key 25@400000000 --key 25@430000000 --key 25@460000000 \
    --key 25@490000000 --key 25@520000000 \
    --key 19@580000000 --key 28@585000000 \
    --key 25@620000000 --key 25@650000000 --key 25@680000000 --key 25@710000000 \
    --key 28@750000000 \
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
screens = [line for line in log if line.startswith("scr ")]

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
# Enaga's emulator keeps the last 16 log lines, and the DOWN presses log a grid line each, so
# "strata ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")
check("scr MACHINE" in screens, "F1+F8 opened MACHINE", f"screens={screens}")
# The log keeps its last 16 lines; the switch and the screens after it are the newest.
check("mch 7 FM4" in log, "track 7 reached FM4", f"log={log}")
check(not any(l.startswith("mch 7 ") and l != "mch 7 FM4" for l in log[log.index("mch 7 FM4") + 1:])
      if "mch 7 FM4" in log else False, "and stayed there", f"log={log}")
check(screens and screens[-1] == "scr EDIT SRC", "back on EDIT to play it", f"screens={screens}")

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
print("note: no cache model here. Compare with scenario_factory.sh on the same commit: the")
print("      difference is FM4 against VA POLY on the chord (docs/BUDGET.md).")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
