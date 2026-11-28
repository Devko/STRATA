#!/usr/bin/env bash
# Everything a jam can turn up at once, in the emulator: the worst case of 2026-11-28's features
# stacked, where each had only been measured alone (docs/BUDGET.md, "Everything turned up").
# genload's generated parts on all eight tracks, then the line input on, REL ALL +64, LEN ALL
# well up (400% at its top), ENERGY +64, VariSpeed +12.0 (the beat at twice the tempo), TRANSPOSE +3 and JAM REC
# running, with PERFORM on screen. Every block's cost is read for 3 s, as genload does.
#
# The 60% target is the cl6x build's (CLAUDE.md). This scenario fails over it with cl6x; GCC-only
# it reports the load and checks only that the app runs and the moves landed, since that build is
# over 60 on genload alone and is not what ships. The CPU guard may rise here: its levels are
# printed, and a rise is reported, not failed, since shedding voices is what it is for (the peak
# then includes the blocks before it acted).
#
#   PLAY, the GEN presses of tests/scenario_genload.sh  -> "GEN T1 BOTH" .. "GEN T8 BOTH"
#   CLR, MIXER, SEL< x4, push A                         -> "IN ON"
#   FUNC + MIXER                                        -> PERFORM
#   FUNC + turn A +40, B +32, C +40                     -> "REL ALL +64", "LEN ALL" past 100%,
#                                                          "ENERGY +64"
#   FUNC + turn VALUE +150                              -> "VARISPEED +12.0 ..."
#   (A, C and VALUE turn past their ends, which hold: over the generated parts some quick
#   detents do not land, about 1 in 6 on C and 1 in 20 on VALUE in the first cl6x run, and 3 in
#   16 on A GCC-only.)
#   FUNC + SEL> x3                                      -> "TRANSPOSE +3 IN KEY"
#   FUNC + HOME, DOWN to JAM REC, OK                    -> "JAM REC JAM <n>"
#   3 s of play, every block's cost read
#
# Keys (third_party/enaga/include/enaga/abi.h): PLAY (F5) 22, OK 23, CLR 30, DOWN 25, RIGHT 18,
# UP 17, FUNC (F1) 19, HOME (F4) 27, MIXER (F7) 21, SEL< 8, SEL> 9, push A 1. Encoders are
# alphabetical, 0-3 = A-D, 4 = VALUE. A screen key comes 5 M cycles after FUNC, not closer
# (tests/scenario_transpose.sh says why).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

# For tools/scenarios.sh's longest-first order (genload's presses plus the moves and 3 s).
CYCLES=6000000000
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
LOG="$ENAGA_BUILD/scenario_stack.log"

echo "running the emulator: generated parts with every live move turned up ..."
rc=0
python3 -B - "$STAGE" > "$LOG" 2>&1 <<'PY' || rc=$?
import importlib, os, re, sys
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
FUNC, HOME, MIXER, SEL_L, SEL_R, PUSH_A = 19, 27, 21, 8, 9, 1
ENC_A, ENC_B, ENC_C, ENC_VALUE = 0, 1, 2, 4

# A larger card than the default 4 MiB: the jam writes about 290 KB a second beside the app.
emu = emulate.Emulation(sys.argv[1], card_mib=16)
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

def hold_ok():
    emu.key(OK, True); go(700 * MS); emu.key(OK, False); go(14 * M)

def func_key(key):
    # FUNC down, the key 5 M cycles later, both let go together.
    emu.key(FUNC, True); go(5 * M)
    emu.key(key, True); go(12 * M)
    emu.key(key, False); emu.key(FUNC, False); go(20 * M)

def func_turn(encoder, detents):
    # FUNC held through the whole turn (12 ms a detent); emu.turn runs the emulator meanwhile.
    emu.key(FUNC, True); go(2 * M)
    emu.turn(encoder, detents)
    collect()
    emu.key(FUNC, False); go(20 * M)

failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

def has(prefix):
    return any(l.startswith(prefix) for l in log)

gcc_only = os.environ.get("ENAGA_TI_CGT") == "none"

# genload's generated parts.
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

# The line input on: MIXER's IN page (going round: EQ, MST, IN+, IN), push A.
tap(MIXER)
for _ in range(4): tap(SEL_L)
tap(PUSH_A)
check("IN ON" in log, "push A turned the line input on", f"log={log[-12:]}")

# PERFORM, and its FUNC layer turned up.
func_key(MIXER)
check("scr PERFORM" in log, "FUNC + MIXER reached PERFORM", f"log={log[-12:]}")
func_turn(ENC_A, 40)
check("REL ALL +64" in log, "FUNC + turn A took REL ALL to +64", f"log={log[-6:]}")
func_turn(ENC_B, 32)
len_all = [l for l in log if l.startswith("LEN ALL ")]
check(bool(len_all) and len_all[-1] != "LEN ALL 100%" and
      int(len_all[-1][8:-1]) > 100, "FUNC + turn B took LEN ALL past 100%", f"{len_all[-1:]}")
func_turn(ENC_C, 40)
check("ENERGY +64" in log, "FUNC + turn C took ENERGY to +64", f"log={log[-6:]}")
func_turn(ENC_VALUE, 150)
check(has("VARISPEED +12.0 "), "FUNC + turn VALUE took VariSpeed to +12.0", f"log={log[-6:]}")
for _ in range(3): func_key(SEL_R)
check("TRANSPOSE +3 IN KEY" in log, "FUNC + SEL> took TRANSPOSE to +3", f"log={log[-6:]}")

# JAM REC: the FUNC menu's row, walked by name (tools/menu_rows.py), not by a press count.
import subprocess
jam_downs = int(subprocess.check_output(
    [sys.executable, "-B", "tools/menu_rows.py", "app/ui_perfmix.hpp", "MenuItem", "jam"]))
func_key(HOME)
go(40 * M)
for _ in range(jam_downs):
    tap(DOWN)
    go(40 * M)
tap(OK)
go(100 * M)
check(has("JAM REC JAM "), "the FUNC menu started JAM REC", f"log={log[-8:]}")

# Every block's cost for 3 s.
loads = []
for _ in range(3 * 750):
    result = emu.run(CYCLES_PER_BLOCK)
    if result["error"]:
        print(f"  FAIL emulator stopped: {result['error']}")
        sys.exit(1)
    loads.append(100.0 * emu.word("audio_cycles_last") / CYCLES_PER_BLOCK)
collect()
mean = sum(loads) / len(loads)
p99 = sorted(loads)[int(0.99 * len(loads))]

report = emu.report()
check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check(not has("JAM:"), "the jam kept recording", f"{[l for l in log if l.startswith('JAM')]}")
check(mean > 12.0, "the stacked beat is playing", f"mean {mean:.2f}%")

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
overruns = report.get("audio_overruns", -1)
guards = [l for l in log if l.startswith("guard ")]
if gcc_only:
    print(f"  --   GCC-only: load not checked against the target ({percent:.2f}% peak, "
          f"{overruns} overruns)")
else:
    check(overruns == 0, "no audio overruns", f"overruns={overruns}")
    check(percent < 60.0, "load is under 60% (cl6x)", f"{percent:.2f}% peak")

print()
print(f"guard levels logged: {guards if guards else 'none (stayed at 0)'}")
print(f"mean block load, everything up: {mean:.2f}%  (p99 {p99:.2f}%, max {max(loads):.2f}%)")
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles)")
print("log:", log)
sys.exit(1 if failures else 0)
PY

grep -v "^log:" "$LOG" || true
echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
