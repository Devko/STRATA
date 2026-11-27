#!/usr/bin/env bash
# GEN and EVOLVE in the emulator (core/generate.hpp, core/evolve.hpp; app/ui_trackmenu.hpp): hold
# OK in GRID for the track menu, GENERATE a part on track 1, set EVOLVE to every loop and let it
# turn a few passes, then HOME. emulate.py's --key holds a key for 20 M cycles (54 ms) and the
# menu wants a 500 ms hold, so this drives the emulator from Python, as tools/emulator_screens.py
# does, and collects the log as it goes (the report keeps only its last 16 lines).
#
#   PLAY
#   hold OK (700 ms)        -> "TRACK T1"
#   OK on GENERATE          -> "GEN T1 BOTH", and the pattern is written (g... u1)
#   DOWN x5, RIGHT          -> "EVOLVE T1 1 LOOP"
#   four passes             -> "EVOLVE TURN" at least three times
#   DOWN x5, OK on HOME     -> "HOME: EVOLVE OFF"
#   two more passes         -> no more turns
#   CLR                     -> "GRID"
#
# Keys (third_party/enaga/include/enaga/abi.h): PLAY 22, OK 23, CLR 30, DOWN 25, RIGHT 18.
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
LOG="$ENAGA_BUILD/scenario_gen.log"

echo "running the emulator: GEN and EVOLVE from the GRID track menu ..."
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
PLAY, OK, CLR, DOWN, RIGHT = 22, 23, 30, 25, 18

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

def index(line, after=-1):
    for i in range(after + 1, len(log)):
        if log[i] == line:
            return i
    return -1

PASS = 2000 * MS                     # one pass of the boot pattern: 16 steps at 120 BPM

go(300 * M)
tap(PLAY)
go(200 * M)

emu.key(OK, True); go(700 * MS); emu.key(OK, False); go(14 * M)
opened = index("TRACK T1")
check(opened >= 0, "a held OK in GRID opened the track menu", f"log={log[-12:]}")

edits_before = len([l for l in log if re.match(r"g[0-9a-fA-F]+ u[1-9]", l)])
tap(OK)
gen = index("GEN T1 BOTH", opened)
check(gen > opened, "OK on GENERATE generated track 1", f"log={log[-12:]}")
go(100 * M)
edits = [l for l in log if re.match(r"g[0-9a-fA-F]+ u[1-9]", l)]
check(len(edits) > edits_before, "the pattern was written, one undo level", f"edits={edits}")

for _ in range(5):
    tap(DOWN)
tap(RIGHT)
evolve = index("EVOLVE T1 1 LOOP", gen)
check(evolve > gen, "RIGHT on EVOLVE set every loop", f"log={log[-12:]}")

go(4 * PASS)
turns = [i for i in range(evolve + 1, len(log)) if log[i] == "EVOLVE TURN"]
check(len(turns) >= 3, "EVOLVE turned once a pass", f"turns={len(turns)}")

for _ in range(5):
    tap(DOWN)
tap(OK)
home = index("HOME: EVOLVE OFF", evolve)
check(home > evolve, "OK on HOME went back and stopped", f"log={log[-12:]}")
after_home = len([i for i in range(home + 1, len(log)) if log[i] == "EVOLVE TURN"])
go(2 * PASS)
later = len([i for i in range(home + 1, len(log)) if log[i] == "EVOLVE TURN"])
# HOME itself reports one change; after it nothing turns.
check(later == after_home and after_home <= 1, "no turn after HOME",
      f"at home={after_home} later={later}")

tap(CLR)
check(index("GRID", home) > home, "CLR closed the menu", f"log={log[-8:]}")
go(200 * M)

report = emu.report()
check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check("run int" in log, "the transport started")
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
