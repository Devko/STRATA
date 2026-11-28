#!/usr/bin/env bash
# WAVE scan emulator scenario: the SCAN page (MOD SPD END CRV) on the C674x. Track 6 (the bass)
# is switched to WAVE MONO on keys alone, SOUNDS loads WOBBLE (GROWL scanned up and back once a
# beat: SCN PING, synced), and SELECT_LEFT from SRC lands on EDIT's SCAN page -- the ninth, past
# the note locks' hole -- which draws its scan picture. The beat keeps running inside the load
# target with no overruns.
#
# The scan is control-rate only (dsp/va.cpp, "SCN"): one step per voice per 16 samples, so the
# peak should match scenario_wave.sh on the same commit. Compare the two (docs/BUDGET.md, "WAVE
# scan").
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, F1 = 19 FUNC,
# DOWN = 25, OK = 23, CLR = 30, SELECT_LEFT = 8. EDIT's DOWN walks the track; F1+F8 opens
# MACHINE, whose DOWN walks the machine list (VA MONO 6 -> 9 WMN: three presses). OK in EDIT
# opens SOUNDS on the sound the track plays (SWEEP, row 1); thirteen DOWNs reach WOBBLE. The
# first press comes late: the panel link is not streaming in the first ~200 M cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1900000000

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
LOG="$ENAGA_BUILD/scenario_wavescan.log"

echo "running the emulator for $CYCLES cycles: play, track 6 to WAVE MONO, WOBBLE, EDIT SCAN ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 25@370000000 --key 25@400000000 --key 25@430000000 --key 25@460000000 \
    --key 25@490000000 \
    --key 19@550000000 --key 28@555000000 \
    --key 25@590000000 --key 25@620000000 --key 25@650000000 \
    --key 28@690000000 \
    --key 23@720000000 \
    --key 25@750000000 --key 25@780000000 --key 25@810000000 --key 25@840000000 \
    --key 25@870000000 --key 25@900000000 --key 25@930000000 --key 25@960000000 \
    --key 25@990000000 --key 25@1020000000 --key 25@1050000000 --key 25@1080000000 \
    --key 25@1110000000 \
    --key 23@1150000000 \
    --key 30@1190000000 \
    --key 8@1240000000 \
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
# Enaga's emulator keeps the last 16 log lines, and the six DOWN presses log a grid line each, so
# "strata ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
# "run int" comes first and can scroll out of the 16 lines too; a sounding beat proves it as well
# (checked below from the peak load).
check("run int" in log or len(log) == 16, "the transport started", f"log={log}")
check("mch 6 WMN" in log or "wav CLS" in log or "wav GRL" in log, "track 6 reached WAVE MONO",
      f"log={log}")
check("snd WOBBLE" in log, "SOUNDS loaded WOBBLE, a scanning preset", f"log={log}")
check("wav GRL" in log, "its table (GROWL) was built on demand", f"log={log}")
check(screens and screens[-1] == "scr EDIT SCAN", "SELECT_LEFT from SRC reached the SCAN page",
      f"screens={screens}")

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
print("note: no cache model here. Compare with scenario_wave.sh on the same commit: the scan is")
print("      control-rate, so the two should match within noise (docs/BUDGET.md, \"WAVE scan\").")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
