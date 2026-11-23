#!/usr/bin/env bash
# SOUNDS emulator scenario: OK in EDIT opens the factory sound list for the selected track
# (core/sounds.hpp), OK loads a preset into it while the beat plays, and OK on AS WAS puts the
# track's own sound back -- all on keys, which is all the emulator can press.
#
# Observable through Enaga's log: app/app.hpp logs `scr SOUNDS` when the screen opens and
# `snd <name>` for each load (`snd AS WAS` for the way back). The loads go through the command
# ring into a running engine, so the beat must still be sounding with no overrun afterwards.
#
# Track 1 (KICK) is selected on a fresh project, and the list opens on the sound it plays now,
# 909 on row 1. DOWN then OK loads 808; DOWN twice then OK loads PUNCH; LEFT (a page up: the first
# row) then OK is AS WAS; CLR goes back to EDIT.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, OK = 23, DOWN = 25,
# LEFT = 24, CLR = 30. The first press comes late: the panel link is not streaming in the first
# ~200 M cycles (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1100000000

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
LOG="$ENAGA_BUILD/scenario_sounds.log"

echo "running the emulator for $CYCLES cycles: play, EDIT, SOUNDS, 808, PUNCH, AS WAS ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 23@380000000 \
    --key 25@420000000 --key 23@460000000 \
    --key 25@540000000 --key 25@580000000 --key 23@620000000 \
    --key 24@700000000 --key 23@740000000 \
    --key 30@800000000 \
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
check("strata ready" in log, "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

check("scr SOUNDS" in log, "OK in EDIT opened SOUNDS on the kick track", f"log={log}")
loads = [line for line in log if line.startswith("snd ")]
check(loads == ["snd 808", "snd PUNCH", "snd AS WAS"],
      "808, then PUNCH, then AS WAS loaded in order", f"lines={loads}")
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
