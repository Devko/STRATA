#!/usr/bin/env bash
# Freeze (GRID's track menu FREEZE, dsp/frozen.hpp) in the emulator, on the factory beat while it
# plays:
#
#   PLAY, DOWN x6                          -> track 7 selected (the CHORD, VA POLY: the dearest)
#   hold OK, UP to FREEZE, OK              -> the track menu, round from GENERATE past CAPTURE
#                                             and RELOAD SAMPLE to FREEZE: 1 bar armed (the menu stays open)
#   ...one bar later                       -> `frz 64`: track 7 plays its loop from RAM
#
# The menu opens on OK held 500 ms, longer than emulate.py's --key holds, so this drives the
# emulator through tools/emu_drive.py and collects the whole log as it goes.
# The emulator cannot read engine memory, so this reads the app's `frz <mask>` log line
# (app/app.hpp, update()). For the load it reads the app's `ld <avg> <max>` lines: four bars,
# one line each, logged only after a track freezes or comes back, from the second whole bar on.
# Four bars are
# one pass of A05, so the frozen and the live run cover the same four chords. The emulator's report has only the
# peak since boot (an earlier version read a last-block figure it does not have: 0.00%).
#
# --baseline does the same freeze and then UNFREEZES the track (OK on the same row again), so its
# windows are track 7 live in the same pattern; compare the two runs. The number that counts is
# still the unit's LOAD (UNIT_CHECKLIST, 13n).
#
# --pad queues A05 PAD (F3, STEP 5) after PLAY, so track 7 is the held VA POLY chord (four to six
# voices all the time) rather than A01's two stabs: the case freezing is for. The freeze is
# armed later there, after A05 has taken over at the end of A01's pass.
#
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=8200000000
BASELINE=0
PAD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --cycles) CYCLES="$2"; shift 2 ;;
        --baseline) BASELINE=1; shift ;;
        --pad) PAD=1; shift ;;
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
LOG="$ENAGA_BUILD/scenario_freeze.log"

echo "running the emulator for $CYCLES cycles: PLAY$([ "$PAD" -eq 1 ] && echo ', A05 PAD'), track 7, track menu FREEZE$([ "$BASELINE" -eq 1 ] && echo ', then unfreeze') ..."
rc=0
python3 -B - "$STAGE" "$CYCLES" "$BASELINE" "$PAD" > "$LOG" 2>&1 <<'PY' || rc=$?
import re, sys
sys.path.insert(0, "tools")
from emu_drive import Drive, M, PLAY, OK, UP, DOWN, F3, step_key, track_menu_up  # noqa: E402

cycles = int(sys.argv[2])
baseline = sys.argv[3] == "1"
pad = sys.argv[4] == "1"
# The take starts on the next bar line and records one bar (about 690 M cycles each at 130 BPM),
# so the track is frozen well before UNF; an OK before then would cancel the take instead. ARM
# is the earliest OK: opening the menu takes about 260 M cycles of the hold.
arm, unf = (1200 * M, 3400 * M) if pad else (840 * M, 2800 * M)

d = Drive(sys.argv[1])
d.go_to(300 * M)
d.tap(PLAY)
for _ in range(6):                   # track 7
    d.tap(DOWN)
    d.go(34 * M)
if pad:                              # queue A05 PAD
    d.go_to(700 * M)
    d.tap(F3)
    d.go(14 * M)
    d.tap(step_key(5))
d.track_menu()
d.check(d.index("TRACK T7") >= 0, "a held OK in GRID opened the track menu on T7",
        f"log={d.log[-12:]}")
for _ in range(track_menu_up("freeze")):   # round past CAPTURE and RELOAD SAMPLE
    d.tap(UP)
d.go_to(arm)
d.tap(OK)
if baseline:
    d.go_to(unf)
    d.tap(OK)
d.go_to(cycles)

log = d.log
frozen = [line for line in log if line.startswith("frz ")]
if baseline:
    d.check("frz 64" in frozen and frozen[-1:] == ["frz 0"], "track 7 froze and came back",
            f"frz={frozen}")
else:
    d.check("frz 64" in frozen, "track 7 froze (frz 64)", f"log={log[-16:]}")
    d.check(bool(frozen) and frozen[-1] == "frz 64", "and stayed frozen", f"last={frozen[-1:]}")
d.common_checks(min_blocks=1000, sounding=False)

# `ld <avg> <max>` lines (app/app.hpp): one a bar, four after each freeze or unfreeze. The ones
# that count follow the last `frz` line: frozen, or live again (--baseline).
windows, seen = [], False
for line in log:
    if line.startswith("frz "):
        seen, windows = True, []
        continue
    m = re.fullmatch(r"ld (\d+\.\d) (\d+\.\d)", line)
    if m:
        windows.append((float(m.group(1)), float(m.group(2))))
windows = windows[-4:]
d.check(seen and len(windows) == 4, "four bars of load after the last frz",
        f"ld lines={len(windows)}")

print()
if windows:
    avg = sum(w[0] for w in windows) / len(windows)
    top = max(w[1] for w in windows)
    label = "track 7 live" if baseline else "track 7 frozen"
    print(f"{label}: avg {avg:.2f}%  max {top:.1f}%  over {len(windows)} bars (avg/max %):")
    print("  " + "  ".join(f"{a:.1f}/{m:.1f}" for a, m in windows))
print("note: compare with the same keys and --baseline; record both in docs/BUDGET.md, Freeze.")
sys.exit(d.finish())
PY

grep -v "^log:" "$LOG" || true

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
