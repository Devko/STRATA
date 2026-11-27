#!/usr/bin/env bash
# The worst GENERATE case in the emulator: on the default kit, DENSITY 100, RANGE 3 OCT, LEAP 100,
# and three GEN presses on every one of the eight tracks (the last one plays), the way Roland
# filled a project with generated parts on the unit (2026-11-27). The beat keeps running, with
# no overruns, inside the load target -- and the CPU guard stays at 0.
#
# Roland's unit showed 40-60% on the top line and GUARD1-GUARD4 on that kind of playing. The
# emulator runs this case at a mean of ~20% a block and a peak of ~36% (cl6x), so the guard can
# never trip here whatever its thresholds; what the unit saw is replayed at the unit's speed by
# tests/test_cpuguard.cpp (cpuguard_a_generated_beat_at_the_units_speed_does_not_raise_the_guard).
# This scenario keeps the case itself measured: its peak load and the mean of every block.
#
#   PLAY, hold OK (700 ms)  -> "TRACK T1"
#   DOWN, RIGHT x5          DENSITY 100
#   DOWN, RIGHT x2          RANGE 3 OCT
#   DOWN, RIGHT x5          LEAP 100
#   UP x3, OK x3            -> "GEN T1 BOTH" three times
#   7 x (CLR, DOWN, hold OK, OK x3) -> "GEN T8 BOTH" (the menu keeps its row and settings)
#   CLR, then 3 s of play, every block's cost read
#
# Keys (third_party/enaga/include/enaga/abi.h): PLAY 22, OK 23, CLR 30, DOWN 25, RIGHT 18, UP 17.
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
LOG="$ENAGA_BUILD/scenario_genload.log"

echo "running the emulator: GENERATE at full density on all eight tracks ..."
rc=0
python3 -B - "$STAGE" > "$LOG" 2>&1 <<'PY' || rc=$?
import importlib, re, sys
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "third_party" / "enaga" / "tools"))
import enaga_cached  # noqa: E402
paths = importlib.import_module("paths")
build_app = importlib.import_module("build_app")
enaga_cached.wrap_build_app(build_app, paths)
emulate = importlib.import_module("emulate")
enaga_cached.wrap_firmware(emulate.firmware_build, paths)

M = 1_000_000
MS = emulate.MS
CYCLES_PER_BLOCK = 496_000
PLAY, OK, CLR, DOWN, RIGHT, UP = 22, 23, 30, 25, 18, 17

emu = emulate.Emulation(sys.argv[1])
seen = 0
log = []

def collect():
    global seen
    count = emu.word("log_count")
    lines = emu.cpu.read(emu.names["log_lines"], emulate.LOG_LINES * 40)
    first = max(seen, count - emulate.LOG_LINES)
    for i in range(first, count):
        at = (i % emulate.LOG_LINES) * 40
        log.append(lines[at:at + 40].split(b"\0")[0].decode("ascii", "replace"))
    seen = count

def go(cycles):
    # In slices, so a pass's worth of log lines never outruns the ring.
    left = int(cycles)
    while left > 0:
        step = min(left, 100 * M)
        result = emu.run(step)
        if result["error"]:
            print(f"  FAIL emulator stopped: {result['error']}")
            sys.exit(1)
        collect()
        left -= step

def tap(key, hold=12 * M):
    emu.key(key, True); go(hold); emu.key(key, False); go(14 * M)

failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

def hold_ok():
    emu.key(OK, True); go(700 * MS); emu.key(OK, False); go(14 * M)

PRESSES = 3

go(300 * M)
tap(PLAY)
go(200 * M)

hold_ok()
check("TRACK T1" in log, "a held OK in GRID opened the track menu", f"log={log[-12:]}")
tap(DOWN)
for _ in range(5): tap(RIGHT)
tap(DOWN)
for _ in range(2): tap(RIGHT)
tap(DOWN)
for _ in range(5): tap(RIGHT)
tap(UP); tap(UP); tap(UP)
for _ in range(PRESSES): tap(OK)
for _ in range(7):
    tap(CLR); tap(DOWN); hold_ok()
    for _ in range(PRESSES): tap(OK)
tap(CLR)
for t in range(1, 9):
    n = sum(1 for l in log if l == f"GEN T{t} BOTH")
    check(n == PRESSES, f"GEN ran {PRESSES} times on track {t}", f"count={n}")

# Every block's cost for 3 s: the run in block-sized slices, audio_cycles_last after each.
loads = []
for _ in range(3 * 750):
    result = emu.run(CYCLES_PER_BLOCK)
    if result["error"]:
        print(f"  FAIL emulator stopped: {result['error']}")
        sys.exit(1)
    loads.append(100.0 * emu.word("audio_cycles_last") / CYCLES_PER_BLOCK)
collect()
mean = sum(loads) / len(loads)

report = emu.report()
check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')
check(not any(l.startswith("guard ") and l != "guard 0" for l in log), "the CPU guard stayed at 0",
      f"{[l for l in log if l.startswith('guard')]}")
peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(mean > 12.0, "the generated parts are playing", f"mean {mean:.2f}%")
# The 60% target is set for the cl6x build that ships (CLAUDE.md: the target depends on it).
# GCC-only, this case reads ~64% (2026-11-27), so that build gets its own tripwire: a regression
# guard, not the budget.
import os
gcc_only = os.environ.get("ENAGA_TI_CGT") == "none"
target = 70.0 if gcc_only else 60.0
check(percent < target, f"load is under {target:.0f}% ({'GCC-only' if gcc_only else 'cl6x'})",
      f"{percent:.2f}% peak")

print()
print(f"mean block load after GEN: {mean:.2f}%  (p99 {sorted(loads)[int(0.99 * len(loads))]:.2f}%)")
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles)")
print("log:", log)
sys.exit(1 if failures else 0)
PY

cat "$LOG" | grep -v "^log:" || true
echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
