#!/usr/bin/env bash
# ASR emulator scenario: EDIT's seventh page, the VA machines' note envelope (ATK SUS REL FSU),
# draws on the C674x with the real font, on a drum track ("VA MONO/POLY ONLY") and on VA POLY
# (the four names and their times), and the beat keeps playing inside the load target.
#
# The emulator presses keys and turns no encoders (docs/HANDOFF.md, section 2), so this cannot
# raise SUS: what a sustaining chord COSTS is measured separately, by the temporary factory edit
# in docs/BUDGET.md ("The ASR page", E12). Per voice it is the same code as before; what changes
# is how long a voice lives, which the shared 12-voice budget already bounds.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, SELECT_RIGHT = 9,
# DOWN = 25. The first press comes late: the panel link is not streaming in the first ~200 M
# cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=900000000

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
LOG="$ENAGA_BUILD/scenario_asr.log"

echo "running the emulator for $CYCLES cycles: play, EDIT to ASR on track 1, then on track 7 ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@360000000 \
    --key 9@400000000 --key 9@430000000 --key 9@460000000 --key 9@490000000 \
    --key 9@520000000 --key 9@550000000 \
    --key 25@600000000 --key 25@620000000 --key 25@640000000 --key 25@660000000 \
    --key 25@680000000 --key 25@700000000 \
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
check("strata ready" in log, "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")
check("scr EDIT SRC" in screens, "EDIT opened on SRC", f"screens={screens}")
check("scr EDIT ARP" in screens, "five selection presses passed the ARP page", f"screens={screens}")
check("scr EDIT ASR" in screens, "a sixth reached the ASR page", f"screens={screens}")
check(screens and screens[-1] == "scr EDIT ASR",
      "six DOWN presses (to track 7, VA POLY) stayed on ASR", f"screens={screens}")

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
print("note: no cache model here. With every ASR byte at 0 this should match scenario_factory.sh to")
print("      within noise: the page changes envelope shapes, not the per-voice code.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
