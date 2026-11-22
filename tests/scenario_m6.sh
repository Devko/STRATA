#!/usr/bin/env bash
# M6 emulator scenario: KEYS mode plays, and what it plays records.
#
# M6's other pieces cannot be driven from here. The Euclidean generator is on encoders and the
# emulator core exposes `key_input` and nothing else -- no pads, no encoders (checked: there is
# no pad or encoder entry point in emulator/core/core.h at all). KEYS mode is the one M6 feature
# that is entirely on KEYS, so it is the one that gets a scenario.
#
# What it proves, and why it is worth a scenario rather than a host test: KEYS crosses every
# layer at once. A STEP key becomes a note through core/scale's degree stepping, that note goes
# through the command ring as a trigger, the live-record path stamps it with the clock state and
# posts an edit, and the edit lands in pattern memory on the audio side. None of that is
# reachable from `make -C host test`, because app/ is the one folder that includes Enaga headers.
#
# The observable effect is app/app.cpp's `g<16 trig bits in hex> u<undo depth>` line, which it
# logs whenever the selected track's current page changes. The default pattern has the kick on
# steps 1, 5, 9 and 13 of track 1 -- g1111 -- so a recorded note must ADD a bit. The exact bit
# depends on where the playhead was when the key was pressed, which is a timing the emulator does
# not make predictable, so the assertion is that the mask GREW and that the undo depth rose --
# not that it reached a particular value. An assertion that pretended to know the step would be
# a flaky test dressed up as a precise one.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F2 = 26, F1 = 19, F4 = 27,
# STEP n = 31 + n. A press is released 20 M cycles later. HOME twice within 400 ms (149 M cycles)
# is KEYS (docs/CONTROLS.md): 460 M and 500 M are 0.11 s apart.
#
# The first press must come LATE: Enaga's panel MCU link is not streaming in the first ~200 M
# cycles and a press before that is silently swallowed (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_m6.log"

echo "running the emulator for $CYCLES cycles: play, REC on, KEYS, three keys ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 26@380000000 \
    --key 27@460000000 --key 27@500000000 \
    --key 32@540000000 \
    --key 34@620000000 \
    --key 36@700000000 \
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

grids = [line for line in log if re.fullmatch(r"g[0-9A-F]{4} u\d+", line)]
check(bool(grids), "the grid signature was logged at all", f"grids={grids}")
check(grids and grids[0] == "g1111 u0", "it started on the default beat", f"first={grids[:1]}")

# A key played in KEYS mode, with REC on, must land a note in the pattern -- so the mask gains a
# bit and the undo depth rises. Counted rather than matched: see the header on why the exact step
# is not knowable here.
def bits(line):
    return bin(int(line[1:5], 16)).count("1")

def depth(line):
    return int(line.split("u")[1])

if len(grids) >= 2:
    check(bits(grids[-1]) > bits(grids[0]),
          "a key played in KEYS recorded a note",
          f"{grids[0]} -> {grids[-1]}")
    check(depth(grids[-1]) > depth(grids[0]),
          "and it cost an undo level, so it was a real edit",
          f"{grids[0]} -> {grids[-1]}")
else:
    check(False, "a key played in KEYS recorded a note", f"only {grids}")
    print("        nothing was recorded. Either KEYS did not play (HOME twice never took, or the")
    print("        STEP keys are still going to the editing path), or REC was not on, or")
    print("        core/record refused the hit. The log above says which.")

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
