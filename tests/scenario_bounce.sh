#!/usr/bin/env bash
# Bounce to track in the emulator, on the factory beat while it plays:
#
#   PLAY                         -> the beat
#   FUNC + REC                   -> RESAMPLE: 1 bar, FROM MASTER, TO FILE (the defaults)
#   OK                           -> armed; the take starts on the next bar line and lasts a bar
#   ...written to /RECORD        -> `offer T8`: track 8 (SAMPLE in the factory kit) is offered,
#                                   since track 1 (selected, KICK) plays no file
#   OK                           -> `take T8`: the take is track 8's sample, loaded at once
#
# The emulator cannot read engine memory, so this reads the app's `offer T<n>` and `take T<n>`
# log lines (app/app.hpp: update() and put_take_on_track()). The host tests cover the rest: the
# walk, the refusal on a synth track, CLR and the card (tests/test_ui.cpp,
# ui_a_take_to_file_is_offered_...).
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 (PLAY), F1 = 19, F2 = 26 (REC), OK = 23.
# A chord is FUNC pressed and the second key 5 M cycles later (tests/scenario_m6.sh). A bar at
# 130 BPM is about 690 M cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=4600000000
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
LOG="$ENAGA_BUILD/scenario_bounce.log"

# The second OK comes well after the take (at most two bars from the arm) and its write. This
# scenario is also the only run of a take's write through Enaga's real file layer: it found that
# make_folder("/RECORD") is DENIED there (`take failed DENIED`), which the fake host now copies.
echo "running the emulator for $CYCLES cycles: PLAY, RESAMPLE 1 bar to FILE, then OK puts it on a track ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@500000000 --key 26@505000000 \
    --key 23@600000000 \
    --key 23@3300000000 \
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
# Enaga's log ring keeps the last sixteen lines; the offer and the take are late in the run.
check("offer T8" in log, "the written take was offered to track 8", f"log={log}")
check("take T8" in log, "one OK put it on track 8", f"log={log}")
if "offer T8" in log and "take T8" in log:
    check(log.index("offer T8") < log.index("take T8"), "offered before it was put", f"log={log}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak since boot")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: no cache model here. docs/BUDGET.md records the cl6x and GCC-only figures; the unit")
print("      decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
