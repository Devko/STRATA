#!/usr/bin/env bash
# Your own sounds emulator scenario (core/usersounds.hpp): in SOUNDS, FUNC+DOWN saves the kick
# track's sound as KCK 1 to the card, OK loads a factory sound and then KCK 1 back, FUNC+UP copies
# the kick's sound, and on track 2 the PASTE row puts it there (switching the machine) before
# AS WAS takes it back -- all on keys, which is all the emulator can press.
#
# Observable through Enaga's log: app/app.hpp logs `scr SOUNDS` when the screen opens and
# `snd <what>` for each action (app/ui_browsers.hpp, log_sound); app/storage.cpp logs
# `sounds written` once the bank's file is on the card. Everything runs with the beat playing,
# so the beat must still be sounding with no overrun afterwards.
#
# The steps, on the default beat (track 1 KICK, track 2 SNARE):
#   PLAY, EDIT, OK: SOUNDS on track 1, the cursor on 909 (row 1).
#   FUNC+DOWN: SAVE SOUND offering KCK 1; OK saves it and puts the cursor on its row, the last.
#   UP, OK: HARD, the last factory kick. DOWN, OK: KCK 1 again.
#   FUNC+UP: copy. Screen key right: track 2, the cursor on CLAP (row 2, under PASTE).
#   LEFT (a page up: row 0), DOWN, OK: PASTE. LEFT, OK: AS WAS. CLR: back to EDIT.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, F1 = 19 FUNC,
# OK = 23, UP = 17, DOWN = 25, LEFT = 24, SELECT_RIGHT = 9, CLR = 30. A press is released 20 M
# cycles later, so F1 at t is still down when the arrow arrives at t + 5 M. The first press comes
# late: the panel link is not streaming in the first ~200 M cycles (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1200000000

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
LOG="$ENAGA_BUILD/scenario_usersounds.log"

echo "running the emulator for $CYCLES cycles: SOUNDS, save, load, copy, paste, AS WAS ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 23@380000000 \
    --key 19@420000000 --key 25@425000000 \
    --key 23@480000000 \
    --key 17@560000000 --key 23@600000000 \
    --key 25@640000000 --key 23@680000000 \
    --key 19@720000000 --key 17@725000000 \
    --key 9@780000000 \
    --key 24@820000000 --key 25@860000000 --key 23@900000000 \
    --key 24@940000000 --key 23@980000000 \
    --key 30@1020000000 \
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

check("scr SOUNDS" in log, "OK in EDIT opened SOUNDS on the kick track", f"log={log}")
actions = [line for line in log if line.startswith("snd ")]
check(actions == ["snd saved KCK 1", "snd HARD", "snd KCK 1", "snd copy KCK", "snd paste KCK",
                  "snd AS WAS"],
      "saved, loaded HARD and KCK 1, copied, pasted, AS WAS, in order", f"lines={actions}")
check("sounds written" in log, "the bank's file was written to the card", f"log={log}")
check("sounds write failed" not in log, "no write failed", f"log={log}")
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
print("note: no cache model here, and this is the GCC-only build. The unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
