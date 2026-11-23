#!/usr/bin/env bash
# Freeze (FUNC menu FREEZE, dsp/frozen.hpp) in the emulator, on the factory beat while it plays:
#
#   PLAY, DOWN x6                          -> track 7 selected (the CHORD, VA POLY: the dearest)
#   FUNC + HOME, UP, OK                    -> FREEZE is the menu's last row: 1 bar armed
#   ...one bar later                       -> `frz 64`: track 7 plays its loop from RAM
#
# The emulator cannot read engine memory, so this reads the app's `frz <mask>` log line
# (app/app.hpp, update()). For the load it reads the app's `ld <avg> <max>` lines: eight
# half-second windows, logged only after a track freezes or comes back (Enaga's log ring keeps
# sixteen lines and the other scenarios check early ones). The emulator's report has only the
# peak since boot (an earlier version read a last-block figure it does not have: 0.00%).
#
# --baseline does the same freeze and then UNFREEZES the track (the same menu keys again), so its
# windows are track 7 live in the same pattern; compare the two runs. The number that counts is
# still the unit's LOAD (UNIT_CHECKLIST, 13n).
#
# --pad queues A05 PAD (F3, STEP 5) after PLAY, so track 7 is the held VA POLY chord (four to six
# voices all the time) rather than A01's two stabs: the case freezing is for. The freeze is
# armed later there, after A05 has taken over at the end of A01's pass.
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19, F4 = 27, F5 = 22, OK = 23, UP = 17,
# DOWN = 25, F3 = 20, STEP 5 = 36. A chord is FUNC pressed and the second key 5 M cycles later (tests/scenario_m6.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=5600000000
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

KEYS=(--key 22@300000000
      --key 25@360000000 --key 25@420000000 --key 25@480000000
      --key 25@540000000 --key 25@600000000 --key 25@660000000)
# The take starts on the next bar line and records one bar (about 690 M cycles each at 130 BPM),
# so the track is frozen well before UNF; an OK before then would cancel the take instead.
ARM=840000000
UNF=2800000000
if [ "$PAD" -eq 1 ]; then
    KEYS+=(--key 20@700000000 --key 36@740000000)
    ARM=1200000000
    UNF=3400000000
fi
menu_ok() {   # FUNC + HOME opens the menu on its first row, UP wraps to FREEZE, OK at $1
    KEYS+=(--key 19@$(($1 - 120000000)) --key 27@$(($1 - 115000000))
           --key 17@$(($1 - 60000000))
           --key 23@$1)
}
menu_ok "$ARM"
if [ "$BASELINE" -eq 1 ]; then
    menu_ok "$UNF"
fi

echo "running the emulator for $CYCLES cycles: PLAY$([ "$PAD" -eq 1 ] && echo ', A05 PAD'), track 7, FUNC menu FREEZE$([ "$BASELINE" -eq 1 ] && echo ', then unfreeze') ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" "${KEYS[@]}" \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

python3 - "$LOG" "$BASELINE" "$PAD" <<'PY'
import json, re, sys

CYCLES_PER_BLOCK = 496_000
baseline = sys.argv[2] == "1"
pad = sys.argv[3] == "1"

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
frozen = [line for line in log if line.startswith("frz ")]
if baseline:
    # `frz 64` itself may have scrolled out of the ring by now; `frz 0` is only logged after it.
    check(frozen[-1:] == ["frz 0"], "track 7 froze and came back (frz 0)", f"log={log}")
else:
    check("frz 64" in frozen, "track 7 froze (frz 64)", f"log={log}")
    check(frozen and frozen[-1] == "frz 64", "and stayed frozen", f"last={frozen[-1:]}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak since boot")

# `ld <avg> <max>` windows (app/app.hpp): half a second each, eight after each freeze or
# unfreeze. The ones that count follow the last `frz` line: frozen, or live again (--baseline).
windows, seen = [], False
for line in log:
    if line.startswith("frz "):
        seen, windows = True, []
        continue
    m = re.fullmatch(r"ld (\d+\.\d) (\d+\.\d)", line)
    if m:
        windows.append((float(m.group(1)), float(m.group(2))))
windows = windows[-8:]
check(seen and len(windows) == 8, "eight load windows after the last frz",
      f"ld lines={len(windows)}")

print()
if windows:
    avg = sum(w[0] for w in windows) / len(windows)
    top = max(w[1] for w in windows)
    label = "track 7 live" if baseline else "track 7 frozen"
    print(f"{label}: avg {avg:.2f}%  max {top:.1f}%  over {len(windows)} windows (avg/max %):")
    print("  " + "  ".join(f"{a:.1f}/{m:.1f}" for a, m in windows))
print("note: compare with the same keys and --baseline; record both in docs/BUDGET.md, Freeze.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
