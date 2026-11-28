#!/usr/bin/env bash
# LIVE RND in the emulator (core/liverand.hpp; docs/CONTROLS.md, "LIVE RND"): FUNC + turn D in
# PERFORM sets the unit for the armed tracks (every track but the kick, D3), FUNC + push D is the
# kill for all bands there as everywhere (D2), and turning back to OFF puts the grid back. The
# emulator has no pads, so FUNC + lower pad (a track in or out) is tests/test_ui.cpp's. The emulator holds a key for 20 M cycles, and a turn of
# n detents takes 12 ms a detent, so a FUNC press with a turn 2 M cycles later is FUNC + turn.
# The notices are logged (app/app.hpp, notice_logged). What the units play is
# tests/test_scheduler.cpp's; this proves the path runs on the C674x under the load target, with
# every armed track jumping, and that PERFORM draws with the marks.
#
#   PLAY
#   FUNC + MIXER             -> PERFORM
#   FUNC + turn D +3         -> "LIVE RND4 7 TRK" (track 1, the kick, stays on the grid)
#   FUNC + push D            -> "KILL ALL", not LIVE RND's
#   FUNC + push D            -> "ALL BANDS BACK"
#   FUNC + turn D +2         -> "LIVE RND16 7 TRK"
#   FUNC + turn D -3, -2     -> "LIVE RND2 7 TRK" on the way, then "LIVE RND OFF"
#   FUNC + turn D +2         -> "LIVE RND2 7 TRK", left on for the last frame
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F7 = 21, PUSH D = 3; encoder D
# is 3. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1400000000
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
LOG="$ENAGA_BUILD/scenario_liverand.log"

echo "running the emulator for $CYCLES cycles: LIVE RND in PERFORM ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@380000000 --key 21@385000000 \
    --key 19@460000000 --turn 3:3@462000000 \
    --key 19@700000000 --key 3@705000000 \
    --key 19@760000000 --key 3@765000000 \
    --key 19@820000000 --turn 3:2@822000000 \
    --key 19@940000000 --turn 3:-3@942000000 \
    --key 19@1000000000 --turn 3:-2@1002000000 \
    --key 19@1120000000 --turn 3:2@1122000000 \
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
rnd4 = index("LIVE RND4 7 TRK")
out = index("KILL ALL", rnd4)
back = index("ALL BANDS BACK", out)
rnd16 = index("LIVE RND16 7 TRK", back)
rnd2 = index("LIVE RND2 7 TRK", rnd16)
off = index("LIVE RND OFF", rnd2)
again = index("LIVE RND2 7 TRK", off)
check(rnd4 >= 0, "FUNC + turn D set RND4 on every track but the kick", f"log={log}")
check(out > rnd4, "FUNC + push D killed every band", f"log={log}")
check(back > out, "and again brought them back", f"log={log}")
check(rnd16 > back, "FUNC + turn D on to RND16, and it stopped there", f"log={log}")
check(rnd2 > rnd16, "turning back passed RND2", f"log={log}")
check(off > rnd2, "and stopped on OFF", f"log={log}")
check(again > off, "and RND2 again for the last frame", f"log={log}")
check("TAPE STOP: SLOT 8" not in log, "FUNC + push D in PERFORM is not the tape stop hint",
      f"log={log}")
# LIVE RND writes nothing: the boot logs the pattern once at u0, an edit would log u1.
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
