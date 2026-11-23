#!/usr/bin/env bash
# WAVE emulator scenario: the wavetable library builds at init on the C674x (its boot cost shows
# as the time to "strata ready"), and the WAVE pair plays the default beat in place of the VAs:
# track 6 (the bass) is switched to WAVE MONO and track 7 (the chord) to WAVE POLY, on keys alone,
# and the beat keeps running inside the load target with no overruns.
#
# The emulator presses keys and turns no encoders (docs/HANDOFF.md, section 2), so the tracks keep
# the VA kit's bytes: on WAVE those read as TBL 64 (FM RATIO) and the bass's MOD as POS. That is a
# fair load case: the wavetable reader costs the same on every table and position. Compare the
# peak with scenario_factory.sh on the same commit (docs/BUDGET.md, "WAVE").
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, F1 = 19 FUNC,
# DOWN = 25. EDIT's DOWN walks the track; F1+F8 opens MACHINE, whose DOWN walks the machine list
# (VA MONO 6 -> 9 WMN, VA POLY 7 -> 10 WPL: three presses each). The first press comes late: the
# panel link is not streaming in the first ~200 M cycles.
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
LOG="$ENAGA_BUILD/scenario_wave.log"

echo "running the emulator for $CYCLES cycles: play, track 6 to WAVE MONO, track 7 to WAVE POLY ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 25@370000000 --key 25@400000000 --key 25@430000000 --key 25@460000000 \
    --key 25@490000000 \
    --key 19@550000000 --key 28@555000000 \
    --key 25@590000000 --key 25@620000000 --key 25@650000000 \
    --key 28@690000000 \
    --key 25@720000000 \
    --key 19@750000000 --key 28@755000000 \
    --key 25@790000000 --key 25@820000000 --key 25@850000000 \
    --key 28@880000000 \
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
screens = [line for line in log if line.startswith("scr ")]

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
# Enaga's emulator keeps the last 16 log lines, and the six DOWN presses log a grid line each, so
# "strata ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")
check("scr MACHINE" in screens, "F1+F8 opened MACHINE", f"screens={screens}")
# The log keeps its last 16 lines; the second switch and the screens after it are the newest.
check("mch 7 WPL" in log, "track 7 reached WAVE POLY", f"log={log}")
check(not any(l.startswith("mch 7 ") and l != "mch 7 WPL" for l in log[log.index("mch 7 WPL") + 1:])
      if "mch 7 WPL" in log else False, "and stayed there", f"log={log}")
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
print("      difference is the WAVE pair against the VAs on the bass and the chord (docs/BUDGET.md).")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
