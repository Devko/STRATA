#!/usr/bin/env bash
# 16th-note triplets in the emulator: GRID's track menu's SPEED row walks track 1 from 1 to the
# new 3/2 (a step every 16 ticks) and on to 2 and back while the beat plays, every press reaching
# the pattern (one undo level each), and the project then saves with the triplet speed in it.
# Where the steps land is tests/test_timing.cpp's (triplet_speed_puts_24_steps_in_a_bar_...);
# this proves the row walks there on the C674x and the beat plays on at 3/2, under the load target
# and without an overrun. The menu opens on OK held 500 ms, longer than emulate.py's --key holds,
# so this drives the emulator through tools/emu_drive.py.
#
#   PLAY                        -> the factory beat, track 1 selected, on GRID
#   hold OK, UP x4              -> the track menu, round from GENERATE to SPEED
#   LEFT, LEFT, RIGHT           -> T1 SPEED 3/2, 2, 3/2: logged, u1 .. u3
#   CLR, FUNC + HOME, OK        -> a save, the kick still running in triplets
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
LOG="$ENAGA_BUILD/scenario_triplets.log"

echo "running the emulator: SPEED to 3/2 from the track menu while the beat plays ..."
rc=0
python3 -B - "$STAGE" > "$LOG" 2>&1 <<'PY' || rc=$?
import sys
sys.path.insert(0, "tools")
from emu_drive import Drive, M, PLAY, OK, CLR, UP, LEFT, RIGHT, F1, F4  # noqa: E402

d = Drive(sys.argv[1])
d.go(300 * M)
d.tap(PLAY)
d.go(100 * M)
d.track_menu()
d.check(d.index("TRACK T1") >= 0, "a held OK in GRID opened the track menu", f"log={d.log[-12:]}")
for _ in range(4):
    d.tap(UP)
d.tap(LEFT)
d.go(30 * M)
d.tap(LEFT)
d.go(30 * M)
d.tap(RIGHT)
d.go(150 * M)
d.tap(CLR)
d.go(60 * M)
d.chord(F1, F4)
d.go(60 * M)
d.tap(OK)
d.go(200 * M)

log = d.log
speeds = [l for l in log if l.startswith("T1 SPEED ")]
d.check(speeds == ["T1 SPEED 3/2", "T1 SPEED 2", "T1 SPEED 3/2"],
        "LEFT, LEFT, RIGHT on SPEED: 3/2, 2, 3/2, one a press", f"speeds={speeds}")
d.check(any(l.startswith("g") and l.endswith(" u3") for l in log),
        "every press was one undo level (u3)", f"log={log}")
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
