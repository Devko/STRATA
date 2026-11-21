#!/usr/bin/env bash
# M3 emulator scenario: edit a step, undo it, redo it -- and prove each one landed.
#
# app/ is the one part of STRATA with no host tests: it is the only part that includes Enaga
# headers (CLAUDE.md), so the step gestures, the undo marks and the command ring between them are
# not reachable from `make -C host test`. This scenario is what stands in for that, and it checks
# EFFECTS rather than the absence of a crash.
#
# How it can see an effect at all: app/app.cpp logs a one-line signature of the selected track's
# current page whenever it changes -- `g<16 trig bits in hex> u<undo depth>`. Pattern memory
# cannot be read from outside the app and the screen comes back as a bitmap, so without that line
# a scenario can only prove the app is still running. Enaga's log ring holds sixteen lines, which
# is why the app logs only on a change and why this scenario makes few edits.
#
# The default pattern puts the kick on steps 1, 5, 9 and 13 of track 1, which is bits 0, 4, 8 and
# 12: g1111. Each assertion below is that number changing the way the key press should change it.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, CLR = 30, OK = 23, STEP n = 31+n.
# A press is released 20 M cycles later, so a key pressed inside that window is a chord: F1 at
# 520 M is still down when CLR arrives at 525 M, which is how F1 + CLR (undo) is driven here.
#
# The press must come LATE: Enaga's panel MCU link is not streaming in the first ~200 M cycles and
# a press before that is silently swallowed (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=700000000

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
LOG="$ENAGA_BUILD/scenario_m3.log"

echo "running the emulator for $CYCLES cycles: play, two step edits, undo, redo ..."
python3 -B "$ENAGA_ROOT/tools/emulate.py" "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 36@400000000 \
    --key 33@460000000 \
    --key 19@520000000 --key 30@525000000 \
    --key 19@600000000 --key 23@605000000 \
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
check("run int" in log, "the transport actually started", f"log={log}")

# The edit trail. Each line is the selected track's page as 16 trig bits, plus the undo depth.
#
#   g1111 u0   the default pattern: kick on steps 1, 5, 9, 13
#   g1101 u1   STEP 5 toggled step 5 off, and the edit pushed one undo level
#   g1103 u2   STEP 2 toggled step 2 on
#   g1101 u1   F1 + CLR undid it: the grid is back and one level was consumed
#   g1103 u2   F1 + OK redid it
#
# Taken as a sequence, not as a set: an undo that restored the wrong state, or a redo that did
# nothing, would still produce the right set of lines.
want = ["g1111 u0", "g1101 u1", "g1103 u2", "g1101 u1", "g1103 u2"]
got = [line for line in log if re.fullmatch(r"g[0-9A-F]{4} u\d+", line)]

check(got == want, "the edit trail is exactly right", f"got={got}")

if got != want:
    # Say which part broke, because the three failures mean very different things.
    if len(got) < 2:
        print("        no edit landed at all: the key press or the command ring")
    elif len(got) >= 4 and got[3] != want[3]:
        print("        the undo did not restore the grid: core/undo or the mark gestures")
    elif len(got) >= 5 and got[4] != want[4]:
        print("        the redo did not come back: the redo branch was dropped")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat is actually sounding", f"{percent:.2f}% peak")
check(percent < 40.0, "load has not regressed badly", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: no cache model here -- compare versions, not targets. The unit decides.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
echo "audio: $ENAGA_BUILD/emulate/audio.wav"
