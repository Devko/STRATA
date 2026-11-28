#!/usr/bin/env bash
# Play modes in the emulator: GRID's track menu's PLAY row walks the selected track's play mode
# while the beat plays, every press reaching the pattern (one undo level each), and the project
# then saves with the mode in it. What the modes play is tests/test_timing.cpp's and
# tests/test_scheduler.cpp's; this proves the row draws and the RND path runs on the C674x with
# the real font, under the load target and without an overrun. The menu opens on OK held 500 ms,
# longer than emulate.py's --key holds, so this drives the emulator through tools/emu_drive.py.
#
#   PLAY                        -> the factory beat, track 1 selected, on GRID
#   hold OK, UP x3              -> the track menu, round from GENERATE to PLAY (under SPEED)
#   RIGHT x5                    -> T1 PLAY REV, PING, RND1, RND2, RND4: logged, u1 .. u5
#   CLR, FUNC + HOME, OK        -> a save, the beat still running in RND4
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "SKIP: no emulator at $ENAGA_EMULATOR" >&2
    exit 0
fi

strata_require_toolchain
strata_link_emulator_toolchain
tools/build.sh > /dev/null

STAGE="$ENAGA_BUILD/stage/STRATA"
mkdir -p "$ENAGA_BUILD"
LOG="$ENAGA_BUILD/scenario_playmode.log"

echo "running the emulator: PLAY from the track menu while the beat plays ..."
rc=0
python3 -B - "$STAGE" > "$LOG" 2>&1 <<'PY' || rc=$?
import sys
sys.path.insert(0, "tools")
from emu_drive import Drive, M, PLAY, OK, CLR, UP, RIGHT, F1, F4  # noqa: E402

d = Drive(sys.argv[1])
d.go(300 * M)
d.tap(PLAY)
d.go(100 * M)
d.track_menu()
d.check(d.index("TRACK T1") >= 0, "a held OK in GRID opened the track menu", f"log={d.log[-12:]}")
for _ in range(3):
    d.tap(UP)
for _ in range(5):
    d.tap(RIGHT)
    d.go(24 * M)
d.go(150 * M)
d.tap(CLR)
d.go(60 * M)
d.chord(F1, F4)
d.go(60 * M)
d.tap(OK)
d.go(200 * M)

log = d.log
plays = [l for l in log if l.startswith("T1 PLAY ")]
order = ["T1 PLAY REV", "T1 PLAY PING", "T1 PLAY RND1", "T1 PLAY RND2", "T1 PLAY RND4"]
d.check(plays == order, "RIGHT x5 on PLAY walked REV .. RND4, one a press", f"plays={plays}")
d.check(any(l.startswith("g") and l.endswith(" u5") for l in log),
        "every press was one undo level (u5)", f"log={log}")
saved = [l for l in log if l.startswith(("saved", "autosaved", "save failed"))]
d.check(any(l.startswith("saved") for l in saved), "FUNC + HOME, OK saved the project",
        f"lines={saved}")
d.check("run int" in log, "the transport started")
d.common_checks(min_blocks=1500)
print("note: no cache model here. docs/BUDGET.md records cl6x as worth about 1.7x and the 60%")
print("      target depends on it. The unit decides.")
sys.exit(d.finish())
PY

grep -v "^log:" "$LOG" || true
echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
