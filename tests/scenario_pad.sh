#!/usr/bin/env bash
# A05 PAD emulator scenario: the factory pad (core/factory.cpp, `pad`) plays on the unit's own
# build inside the budget. A05 holds a four- or five-note chord on VA POLY for every bar, with a
# 2 s release ringing into the next, so up to six voices sound all the time: the steady cost the
# ASR page adds, where A01's stabs (E12 in docs/BUDGET.md) only overlap now and then.
#
# The selected track's `g<16 trig bits in hex> u<undo depth>` line (app/app.cpp) proves the switch.
# EDIT and six DOWNs select track 7, the chord (tests/scenario_asr.sh does the same): A01's stabs
# are on steps 4 and 11 = g0408, A05's first page has one chord on step 1 = g0001. Queueing A05
# (F3, STEP 5) switches at the end of A01's 16-step pass, about 1.85 s at 130 BPM, and the run
# goes on long enough for the pad to reach its second chord.
#
# The DOWNs are 40 M cycles apart. A press is released 20 M cycles later, and a press landing on
# the same cycle as the last one's release is never seen as a new press: the panel scan finds the
# key still down. At 20 M apart only the first DOWN counted, track 2 stayed selected (g1010, then
# A05's empty clap g0000), and the scenario failed on the PC.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, DOWN = 25, F3 = 20,
# STEP n = 31 + n. The first press comes late: the panel link is not streaming in the first
# ~200 M cycles (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_pad.log"

echo "running the emulator for $CYCLES cycles: play A01, select the chord, queue A05 ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 25@380000000 --key 25@420000000 --key 25@460000000 --key 25@500000000 \
    --key 25@540000000 --key 25@580000000 \
    --key 20@620000000 \
    --key 36@660000000 \
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
check("strata ready" in log, "init() ran to completion")

# init() now also opens the last project and allocates 85 MiB (the sample region, the loader's
# staging buffers and the beat-repeat ring). A failure there returns false from App::init, which
# Enaga reports as APP INIT FAILED rather than APP RUNNING -- so the check above is also the
# memory check, and the first thing to suspect if it fails.
check(report.get("app_name") == "STRATA", "the app Enaga booted is this one",
      f'name={report.get("app_name")!r}')

check("run int" in log, "the transport started", f"log={log}")

grids = [line for line in log if re.fullmatch(r"g[0-9A-F]{4} u\d+", line)]
check("g0408 u0" in grids, "track 7 selected on the factory GROOVE: its two stabs", f"grids={grids}")
check("g0001 u0" in grids, "queueing A05 switched to PAD at the end of the pass", f"grids={grids}")

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
print("note: no cache model here, and this is the GCC-only build -- compare versions, not")
print("      targets. docs/BUDGET.md records cl6x as worth about 2.2x. The unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
