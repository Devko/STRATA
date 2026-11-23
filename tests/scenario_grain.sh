#!/usr/bin/env bash
# GRAIN emulator scenario: track 8 (the factory SAMPLE track) is switched to GRAIN on keys alone,
# which loads the CLOUD sound (core/sounds.cpp, machine_defaults_apply); OK in EDIT then opens
# GRAIN's SOUNDS list (on GRAIN, OK in MACHINE is the sample browser), PAD and SHIMMR load while
# the beat plays, and the beat keeps running inside the load target with no overruns. SHIMMR is
# the costliest preset: dense, long grains, half of them read at twice the rate.
#
# Observable through Enaga's log: `mch 8 GRN` for the switch, `scr SOUNDS` and `snd <name>` for
# the list (app/app.hpp).
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, F1 = 19 FUNC,
# OK = 23, DOWN = 25, CLR = 30. EDIT's DOWN walks the track (seven presses: track 1 -> 8);
# F1+F8 opens MACHINE, whose DOWN walks the machine list (SMP 4 -> GRN 5: one press). The list
# opens on CLOUD, the sound the switch loaded; DOWN, OK loads PAD; DOWN twice, OK loads SHIMMR.
# The first press comes late: the panel link is not streaming in the first ~200 M cycles.
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
LOG="$ENAGA_BUILD/scenario_grain.log"

echo "running the emulator for $CYCLES cycles: play, track 8 to GRN, SOUNDS, PAD, SHIMMR ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 25@370000000 --key 25@400000000 --key 25@430000000 --key 25@460000000 \
    --key 25@490000000 --key 25@520000000 --key 25@550000000 \
    --key 19@600000000 --key 28@605000000 \
    --key 25@640000000 \
    --key 28@680000000 \
    --key 23@720000000 \
    --key 25@760000000 --key 23@800000000 \
    --key 25@840000000 --key 25@880000000 --key 23@920000000 \
    --key 30@960000000 \
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
# Enaga's emulator keeps the last 16 log lines, and the seven DOWN presses log a grid line each,
# so "strata ready" and "run int" can scroll out (as in scenario_wave.sh). A drawn screen proves
# init() ran; the peak load below proves the beat sounded.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log or len(log) == 16, "the transport started", f"log={log}")

check("mch 8 GRN" in log, "track 8 reached GRAIN", f"log={log}")
check("scr SOUNDS" in log, "OK in EDIT opened GRAIN's SOUNDS", f"log={log}")
loads = [line for line in log if line.startswith("snd ")]
check(loads == ["snd PAD", "snd SHIMMR"], "PAD, then SHIMMR loaded in order", f"lines={loads}")
screens = [line for line in log if line.startswith("scr ")]
check(bool(screens) and screens[-1].startswith("scr EDIT"), "CLR went back to EDIT",
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
