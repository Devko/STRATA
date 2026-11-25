#!/usr/bin/env bash
# M6 emulator scenario: KEYS mode, with REC on, keeps the STEP keys as steps.
#
# M6's other pieces cannot be driven from here. The Euclidean generator is on encoders and the
# emulator core exposes `key_input` and nothing else -- no pads, no encoders (checked: there is
# no pad or encoder entry point in emulator/core/core.h at all). KEYS plays on the pads since
# 2026-11-23, so playing and recording in KEYS are host tests now (tests/test_ui.cpp,
# ui_keys_pads_record_at_their_own_velocity). What is left on keys is D12: in KEYS, REC on and
# the transport running, the STEP keys are the track's steps, not notes to record.
#
# The observable effect is app/app.cpp's `g<16 trig bits in hex> u<undo depth>` line, which it
# logs whenever the selected track's current page changes. The default pattern has the kick on
# steps 1, 5, 9 and 13 of track 1 -- g1111. STEP 1, 3 and 5 toggle bits 0, 2 and 4, whatever
# the playhead does: g1104 and three undo levels. A recorded note would land wherever the
# playhead was, so an exact mask also proves nothing was recorded.
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

# STEP 1, 3 and 5 in KEYS toggled those three steps and recorded nothing (D12): exactly g1104,
# three undo levels. See the header.
if len(grids) >= 2:
    check(grids[-1] == "g1104 u3",
          "STEP 1, 3 and 5 in KEYS toggled their steps (D12)",
          f"{grids[0]} -> {grids[-1]}")
else:
    check(False, "STEP 1, 3 and 5 in KEYS toggled their steps (D12)", f"only {grids}")
    print("        nothing changed. Either KEYS was not reached (HOME twice never took, or the")
    print("        STEP keys are swallowed in KEYS again). The log above says which.")

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
