#!/usr/bin/env bash
# M5 emulator scenario: what the performance layer costs when nobody is touching it.
#
# ## Read this before trusting it: most of M5 is NOT reachable from here
#
# `emulate.py` can press KEYS and nothing else -- it has one `--key` option and no pad support
# (third_party/enaga/tools/emulate.py). docs/SPEC.md puts seven of M5's eight tools on the upper
# PADS: FILL, REPEAT, MUTE mode, scene A and B select, THROW and MUTATE. **None of them can be
# driven by this harness at all.** Of what is left on keys, a scene capture and a macro
# assignment are both inaudible by themselves -- a macro at position 0 writes nothing and leaves
# the parameter to its knob (core/perform.hpp) -- and moving a macro or the morph needs an
# encoder, which `emulate.py` also cannot drive.
#
# So this scenario cannot show the performance layer performing. Verifying that needs either pad
# and encoder support in Enaga's emulator (upstream) or the unit, and
# `docs/UNIT_CHECKLIST.md`'s M5 section is written for the unit for exactly this reason.
#
# ## What it CAN settle, and it is the thing CLAUDE.md asks for
#
# **Beat repeat's idle cost.** `dsp/repeat` copies every block into its ring whether or not the
# pad is held, so it is a fixed per-block tax on the master output that is always on -- and the
# peak load here, against tests/scenario_m3.sh's on the same beat, IS that tax. That is the one
# load figure M5 owes and the only one obtainable without a unit.
#
# It also settles two things that are cheap and would otherwise be guesses: that the app still
# boots with M4 and M5's 85 MiB of buffers reserved (a failure there is APP INIT FAILED, not a
# crash), and that entering PERFORM, capturing a scene and assigning a macro do not fault.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F7 = 21, OK = 23, CLR = 30,
# UP = 17, STEP n = 31 + n. A press is released 20 M cycles later, so F1 at 500 M is still down
# when F7 arrives at 505 M -- which is how F1 + F7 (PERFORM) is driven here.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=700000000

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
LOG="$ENAGA_BUILD/scenario_m5.log"

echo "running the emulator for $CYCLES cycles: play, PERFORM, capture a scene, assign a macro ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@500000000 --key 21@505000000 \
    --key 23@560000000 \
    --key 35@600000000 \
    --key 19@640000000 --key 17@645000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

python3 - "$LOG" <<'PY'
import json, re, sys

CYCLES_PER_BLOCK = 496_000

# tests/scenario_m3.sh's peak on the same default beat, GCC-only, recorded when this was written.
# The difference is beat repeat's idle tax plus the performance pass. Update BOTH this number and
# docs/BUDGET.md when the baseline legitimately moves, and never update it to make a regression
# go away.
#
# It has moved once, legitimately: the default kit's CHORD track now plays a MIN7 on VA POLY's
# six voices, and three extra voices are worth about 12.7 points on their own. The gap this
# script watches is unaffected by that -- both runs play the same chord -- but the gap itself is
# now SMALLER than it was, because the peak block is the chord's rather than an idle one, and
# beat repeat's tax is a constant per block rather than a share of it.
M3_BASELINE_PERCENT = 33.07   # measured 2026-11-21, GCC-only, same build; see docs/BUDGET.md

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

# init() reserves 64 MiB for samples, 18 MiB of loader staging and 2.93 MiB for the repeat ring.
# If the heap cannot provide them, App::init returns false and Enaga shows APP INIT FAILED -- so
# this check is the memory check, and the first thing to suspect when it fails.
check(report.get("app_status") == "APP RUNNING", "app is running with 85 MiB reserved",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check("strata ready" in log, "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

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
if M3_BASELINE_PERCENT is not None:
    delta = percent - M3_BASELINE_PERCENT
    print(f"against scenario_m3's {M3_BASELINE_PERCENT:.2f}%: {delta:+.2f} points")
    print("that difference is beat repeat's always-on ring copy plus the performance pass.")
    if delta > 5.0:
        print("  MORE THAN 5 POINTS. That is too much for one block copy -- look at")
        print("  dsp/repeat::process's idle path before anything else.")
else:
    print("no M3 baseline recorded in this script yet: run tests/scenario_m3.sh on the same")
    print("build, put its peak into M3_BASELINE_PERCENT above and into docs/BUDGET.md.")
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
