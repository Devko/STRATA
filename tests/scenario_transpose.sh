#!/usr/bin/env bash
# TRANSPOSE in the emulator (core/transpose.hpp; docs/CONTROLS.md, "TRANSPOSE"): FUNC + the screen
# keys move the melodic tracks a semitone a press, and both screen keys together put them back.
# The emulator holds a key for 20 M cycles, so a FUNC press with a screen key 2 M cycles later is
# FUNC + that key, and a second screen key 4 M cycles in lands while the first is still down: the
# both-at-once reset. The notices are logged (app/ui_perform.hpp, transpose_key). Which tracks
# move and how is tests/test_transpose.cpp's, the gesture's every screen tests/test_ui.cpp's; this
# proves the path runs on the C674x with the default beat playing transposed, under the load
# target, and that the notice names a key in sharps.
#
#   PLAY
#   FUNC + SEL>  three times    -> "TRANSPOSE +1 <key>", "+2", "+3"
#   FUNC + SEL<                 -> "+2"
#   FUNC + SEL< then SEL> held  -> "+1", then "TRANSPOSE 0": both screen keys are rest
#   FUNC + SEL<                 -> "TRANSPOSE -1 <key>", held there to the end: the load
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, SELECT_LEFT = 8,
# SELECT_RIGHT = 9. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1400000000
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
LOG="$ENAGA_BUILD/scenario_transpose.log"

echo "running the emulator: TRANSPOSE up, down and back to rest ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@400000000 --key 9@402000000 \
    --key 19@440000000 --key 9@442000000 \
    --key 19@480000000 --key 9@482000000 \
    --key 19@520000000 --key 8@522000000 \
    --key 19@560000000 --key 8@562000000 --key 9@564000000 \
    --key 19@620000000 --key 8@622000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
python3 - "$LOG" <<'PY' || rc=$?
import json, os, re, sys

CYCLES_PER_BLOCK = 496_000
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

text = open(sys.argv[1]).read()
match = re.search(r'^\{.*?^\}', text, re.S | re.M)
report = json.loads(match.group(0)) if match else {}
check(bool(match), "a JSON report")
log = report.get("log", [])
check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
# TRANSPOSE writes nothing: an edit would log the pattern at u1 or later.
check(not any(re.match(r"g[0-9a-f]+ u[1-9]", l) for l in log), "the pattern was never written",
      f"log={log}")
blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

# Every TRANSPOSE notice, in order: the amount and the key it names.
seen = []
for line in log:
    m = re.fullmatch(r"TRANSPOSE (0|[+-]\d+)(?: ([A-G]#?))?", line)
    if m:
        seen.append((int(m.group(1)), m.group(2)))
amounts = [a for a, _ in seen]
check(amounts == [1, 2, 3, 2, 1, 0, -1], "the presses walked +1 +2 +3 +2 +1, rest, -1",
      f"seen={seen}")
check(all((a == 0) == (k is None) for a, k in seen), "a key is named away from rest only",
      f"seen={seen}")
# One project key under all of them: each named key is that root moved by the amount.
roots = {(NAMES.index(k) - a) % 12 for a, k in seen if k is not None}
check(len(roots) == 1, "every key named is the project's root moved by the amount",
      f"seen={seen}")

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
# The 60% target is set for the cl6x build that ships (CLAUDE.md); GCC-only gets its own
# tripwire, as scenario_energy's does: a regression guard, not the budget.
gcc_only = os.environ.get("ENAGA_TI_CGT") == "none"
target = 70.0 if gcc_only else 60.0
check(percent < target, f"load is under {target:.0f}% ({'GCC-only' if gcc_only else 'cl6x'})",
      f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
