#!/usr/bin/env bash
# ENERGY in the emulator (core/energy.hpp; docs/CONTROLS.md, "ENERGY"): FUNC + turn C in PERFORM
# thins every track down to a breakdown, stops on rest when turned back, and fills the beat with
# ghosts at the top. The emulator holds a key for 20 M cycles, and a turn of n detents takes 12 ms
# a detent, so a FUNC press with a turn 2 M cycles later is FUNC + turn; each detent is its own
# click, so the detent at rest stops exactly the click that lands on it. The notices are logged
# (app/app.hpp, notice_logged). What each position plays is tests/test_energy.cpp's; this proves
# the path runs on the C674x under the load target, with ghosts on every track that takes them
# at +64, and that PERFORM draws with the marks.
#
# The app logs a notice for every detent and the emulator's report keeps only the newest 16 log
# lines, so the walk is two runs, each short enough to keep what it checks:
#
#   run "walk" (1.0 G cycles)
#     PLAY, FUNC + MIXER     -> PERFORM
#     FUNC + turn C -3       -> "ENERGY -2", "-4", "-6"
#     FUNC + turn C +3       -> "-4", "-2", "ENERGY 0": the click onto rest
#     FUNC + turn C -2       -> "ENERGY -4" again
#     turn C -1, FUNC up     -> "ENERGY -6": C was still turning (a turn keeps its layer for
#                               250 ms of rest, about 93 M cycles), so not macro C
#     turn C -1, rested      -> macro C (not logged): no "ENERGY -8"
#   run "peak" (1.9 G cycles)
#     PLAY, FUNC + MIXER     -> PERFORM
#     FUNC + turn C +32      -> "ENERGY +64", left on to the end: the load with the ghosts
#
# -64 and the order the tracks thin in are tests/test_energy.cpp's.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F7 = 21; encoder C is 2. The
# first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

# Both runs together, for tools/scenarios.sh's longest-first order.
CYCLES=2900000000
export CYCLES
if [ $# -gt 0 ]; then
    echo "error: unknown argument: $1" >&2
    exit 2
fi

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "SKIP: no emulator at $ENAGA_EMULATOR" >&2
    exit 0
fi

strata_require_toolchain
strata_link_emulator_toolchain
tools/build.sh > /dev/null

STAGE="$ENAGA_BUILD/stage/STRATA"
mkdir -p "$ENAGA_BUILD"
LOG="$ENAGA_BUILD/scenario_energy.log"
LOG_WALK="$ENAGA_BUILD/scenario_energy_walk.log"
: > "$LOG"

emulate() {   # emulate LOGFILE CYCLES KEYS...
    local out="$1" cycles="$2"
    shift 2
    python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$cycles" "$@" > "$out" 2>&1 || {
        echo "FAIL: emulate.py exited non-zero" >&2
        tail -30 "$out" >&2
        exit 1
    }
}

echo "running the emulator: ENERGY down and back to rest ..."
emulate "$LOG_WALK" 1000000000 \
    --key 22@300000000 \
    --key 19@380000000 --key 21@385000000 \
    --key 19@460000000 --turn 2:-3@462000000 \
    --key 19@600000000 --turn 2:3@602000000 \
    --key 19@740000000 --turn 2:-2@742000000 \
    --turn 2:-1@790000000 \
    --turn 2:-1@930000000

echo "running the emulator: ENERGY at +64 ..."
emulate "$LOG" 1900000000 \
    --key 22@300000000 \
    --key 19@380000000 --key 21@385000000 \
    --key 19@460000000 --turn 2:32@462000000

rc=0
python3 - "$LOG_WALK" "$LOG" <<'PY' || rc=$?
import json, os, re, sys

CYCLES_PER_BLOCK = 496_000

failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

def load(path):
    text = open(path).read()
    match = re.search(r'^\{.*?^\}', text, re.S | re.M)
    if not match:
        check(False, f"a JSON report in {path}")
        return {}
    return json.loads(match.group(0))

def common(report, label):
    log = report.get("log", [])
    check(report.get("app_status") == "APP RUNNING", f"{label}: app is running",
          f'status={report.get("app_status")!r}')
    check(not report.get("crash_reason"), f"{label}: no crash",
          f'reason={report.get("crash_reason")!r}')
    # ENERGY writes nothing: an edit would log the pattern at u1 or later.
    check(not any(re.match(r"g[0-9a-f]+ u[1-9]", l) for l in log),
          f"{label}: the pattern was never written", f"log={log}")
    blocks = report.get("audio_blocks", 0)
    check(blocks > 1000, f"{label}: audio ran throughout", f"blocks={blocks}")
    check(report.get("audio_overruns", -1) == 0, f"{label}: no audio overruns",
          f'overruns={report.get("audio_overruns")}')
    return log

def index(log, line, after=-1):
    for i in range(after + 1, len(log)):
        if log[i] == line:
            return i
    return -1

walk = load(sys.argv[1])
log = common(walk, "walk")
check("scr PERFORM" in log, "walk: FUNC + MIXER reached PERFORM", f"log={log}")
down = index(log, "ENERGY -6")
rest = index(log, "ENERGY 0", down)
again = index(log, "ENERGY -4", rest)
check(down >= 0, "walk: FUNC + turn C took ENERGY down to -6", f"log={log}")
check(rest > down, "walk: turning back landed on rest", f"log={log}")
check(again > rest, "walk: and down again from it", f"log={log}")
check("ENERGY +2" not in log, "walk: never past rest", f"log={log}")
held = index(log, "ENERGY -6", again)
check(held > again, "walk: a click just after FUNC came up stayed on ENERGY", f"log={log}")
check("ENERGY -8" not in log, "walk: a turn after a rest was macro C again", f"log={log}")

peak_run = load(sys.argv[2])
log = common(peak_run, "peak")
check("ENERGY +64" in log, "peak: FUNC + turn C took ENERGY to +64", f"log={log}")
check("ENERGY +66" not in log, "peak: and it stopped there", f"log={log}")

peak = peak_run.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
# The 60% target is set for the cl6x build that ships (CLAUDE.md). GCC-only, ghosts on every
# track read ~61% (2026-11-28), so that build gets its own tripwire, as scenario_genload's does:
# a regression guard, not the budget.
gcc_only = os.environ.get("ENAGA_TI_CGT") == "none"
target = 70.0 if gcc_only else 60.0
check(percent < target, f"load is under {target:.0f}% ({'GCC-only' if gcc_only else 'cl6x'})",
      f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, "
      f"{peak_run.get('audio_blocks', 0)} blocks)")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; logs: $LOG_WALK $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (logs: $LOG_WALK $LOG)"
