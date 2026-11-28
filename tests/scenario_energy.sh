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
#   PLAY
#   FUNC + MIXER             -> PERFORM
#   FUNC + turn C -10        -> "ENERGY -20"
#   FUNC + turn C -30        -> "ENERGY -64", and it stops there
#   FUNC + turn C +32        -> "ENERGY 0"
#   FUNC + turn C +32        -> "ENERGY +64", left on to the end: the load with the ghosts
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F7 = 21; encoder C is 2. The
# first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2100000000
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
LOG="$ENAGA_BUILD/scenario_energy.log"

echo "running the emulator for $CYCLES cycles: ENERGY in PERFORM ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@380000000 --key 21@385000000 \
    --key 19@460000000 --turn 2:-10@462000000 \
    --key 19@700000000 --turn 2:-30@702000000 \
    --key 19@1100000000 --turn 2:32@1102000000 \
    --key 19@1500000000 --turn 2:32@1502000000 \
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
check("run int" in log, "the transport started", f"log={log}")

def index(line, after=-1):
    for i in range(after + 1, len(log)):
        if log[i] == line:
            return i
    return -1

screens = [line for line in log if line.startswith("scr ")]
check("scr PERFORM" in screens, "FUNC + MIXER reached PERFORM", f"screens={screens}")
down20 = index("ENERGY -20")
down64 = index("ENERGY -64", down20)
rest = index("ENERGY 0", down64)
up64 = index("ENERGY +64", rest)
check(down20 >= 0, "FUNC + turn C took ENERGY to -20", f"log={log}")
check(down64 > down20, "and on down to -64", f"log={log}")
check("ENERGY -66" not in log, "where it stopped", f"log={log}")
check(rest > down64, "turning back stopped on rest", f"log={log}")
check(up64 > rest, "and up to +64 for the last stretch", f"log={log}")
check("ENERGY +66" not in log, "where it stopped too", f"log={log}")
# ENERGY writes nothing: the boot logs the pattern once at u0, an edit would log u1.
check(not any(re.match(r"g[0-9a-f]+ u[1-9]", l) for l in log),
      "the pattern was never written", f"log={log}")

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

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
