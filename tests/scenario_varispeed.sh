#!/usr/bin/env bash
# VariSpeed in the emulator (core/varispeed.hpp; docs/CONTROLS.md, "VariSpeed"): FUNC + turn VALUE
# moves the clock and every machine's pitch together, stops on rest when turned back, and holds
# at an octave. The emulator holds a key for 20 M cycles, and a turn of n detents takes 12 ms a
# detent, so a FUNC press with a turn 2 M cycles later is FUNC + turn; each detent is its own
# click, so the detent at rest stops exactly the click that lands on it. The notices are logged
# (app/app.hpp, vari_notice). What each machine does with the amount is tests/test_varispeed.cpp's
# and the clock's is tests/test_ui.cpp's; this proves the path runs on the C674x under the load
# target with the default beat an octave up (twice the tempo, SAMPLE and GRAIN reading twice as
# fast), and that every screen it passes draws.
#
# The app logs a notice for every detent and the emulator's report keeps only the newest 16 log
# lines, so the walk is two runs, each short enough to keep what it checks:
#
#   run "walk" (0.9 G cycles)
#     PLAY
#     FUNC + turn VALUE +3   -> "VARISPEED +0.1 ...", "+0.2", "+0.3"
#     FUNC + turn VALUE -3   -> "+0.2", "+0.1", "VARISPEED 0 ...": the third click lands on rest
#                               (a turn that would cross 0 stops on it; the next click leaves it,
#                               as REL ALL's and ENERGY's do, so the turn back is exactly 3)
#   run "octave" (1.9 G cycles)
#     PLAY
#     FUNC + turn VALUE +124 -> "VARISPEED +12.0 ...", held there to the end: the load. The
#                               clicks past the top repeat "+12.0" (every click shows something)
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19; VALUE's encoder is 4. The first
# press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

# Both runs together, for tools/scenarios.sh's longest-first order.
CYCLES=2800000000
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
LOG="$ENAGA_BUILD/scenario_varispeed.log"
LOG_WALK="$ENAGA_BUILD/scenario_varispeed_walk.log"
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

echo "running the emulator: VariSpeed up and back to rest ..."
emulate "$LOG_WALK" 900000000 \
    --key 22@300000000 \
    --key 19@460000000 --turn 4:3@462000000 \
    --key 19@600000000 --turn 4:-3@602000000

echo "running the emulator: VariSpeed an octave up ..."
emulate "$LOG" 1900000000 \
    --key 22@300000000 \
    --key 19@380000000 --turn 4:124@382000000

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
    # VariSpeed writes nothing: an edit would log the pattern at u1 or later.
    check(not any(re.match(r"g[0-9a-f]+ u[1-9]", l) for l in log),
          f"{label}: the pattern was never written", f"log={log}")
    blocks = report.get("audio_blocks", 0)
    check(blocks > 1000, f"{label}: audio ran throughout", f"blocks={blocks}")
    check(report.get("audio_overruns", -1) == 0, f"{label}: no audio overruns",
          f'overruns={report.get("audio_overruns")}')
    return log

def index(log, prefix, after=-1):
    for i in range(after + 1, len(log)):
        if log[i].startswith(prefix):
            return i
    return -1

walk = load(sys.argv[1])
log = common(walk, "walk")
up = index(log, "VARISPEED +0.3 ")
rest = index(log, "VARISPEED 0 ", up)
check(up >= 0, "walk: FUNC + turn VALUE took VariSpeed up to +0.3", f"log={log}")
check(rest > up, "walk: turning back landed on rest", f"log={log}")
check(rest >= 0 and index(log, "VARISPEED", rest) < 0, "walk: and stopped there", f"log={log}")
check(index(log, "VARISPEED -") < 0, "walk: never past rest", f"log={log}")

octave_run = load(sys.argv[2])
log = common(octave_run, "octave")
top = index(log, "VARISPEED +12.0 ")
check(top >= 0, "octave: FUNC + turn VALUE took VariSpeed to +12.0", f"log={log}")
# The clicks past the top repeat its notice: every VariSpeed line from there on is +12.0.
check(top >= 0 and all(l.startswith("VARISPEED +12.0 ")
                       for l in log[top:] if l.startswith("VARISPEED")),
      "octave: and it stayed there", f"log={log}")
if top >= 0:
    # The tempo it reads is twice the project's: the default beat's.
    match = re.match(r"VARISPEED \+12\.0 (\d+)\.(\d)$", log[top])
    check(match is not None, "octave: the notice names the tempo", f"line={log[top]!r}")

peak = octave_run.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
# The 60% target is set for the cl6x build that ships (CLAUDE.md); GCC-only gets its own
# tripwire, as scenario_energy's does: a regression guard, not the budget.
gcc_only = os.environ.get("ENAGA_TI_CGT") == "none"
target = 70.0 if gcc_only else 60.0
check(percent < target, f"load is under {target:.0f}% ({'GCC-only' if gcc_only else 'cl6x'})",
      f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, "
      f"{octave_run.get('audio_blocks', 0)} blocks)")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; logs: $LOG_WALK $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (logs: $LOG_WALK $LOG)"
