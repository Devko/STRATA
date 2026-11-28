#!/usr/bin/env bash
# The usability review's quick wins in the emulator (2026-11-28; docs/CONTROLS.md): MIXER's UP/DOWN
# jump a group of pages, a held OK in KEYS opens the track menu, and FUNC + REC twice starts and
# stops JAM REC from RESAMPLE. emulate.py's --key holds a key for 20 M cycles (54 ms) and the
# track menu wants a 500 ms hold, so this drives the emulator from Python, as tests/scenario_gen.sh
# does, and collects the log as it goes (the report keeps only its last 16 lines).
#
#   PLAY
#   MIXER, DOWN x2           -> "DCK 1-4", "WID 1-4"
#   UP x3                    -> "DCK 1-4", "TRK 1-4", "MST" (round the top)
#   HOME, HOME               -> KEYS
#   hold OK (700 ms)         -> "TRACK T1"; DOWN is the menu's row; CLR -> "KEYS"
#   FUNC + REC               -> RESAMPLE
#   FUNC + REC               -> "JAM REC JAM <n>"; two seconds of the beat on the card
#   FUNC + REC               -> "JAM STOPPING", then "SAVED JAM <n>"
#
# Keys (third_party/enaga/include/enaga/abi.h): PLAY 22, OK 23, CLR 30, DOWN 25, UP 17, FUNC 19,
# REC (F2) 26, HOME (F4) 27, MIXER (F7) 21.
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
LOG="$ENAGA_BUILD/scenario_shortcuts.log"

echo "running the emulator: MIXER groups, the KEYS track menu, JAM REC on FUNC + REC twice ..."
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
PLAY, OK, CLR, DOWN, UP, FUNC, REC, HOME, MIXER = 22, 23, 30, 25, 17, 19, 26, 27, 21

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

def chord(key):
    # FUNC held, the key pressed inside the hold, both let go.
    emu.key(FUNC, True); go(5 * M); tap(key); emu.key(FUNC, False); go(14 * M)

def starts(prefix, after=-1):
    for i in range(after + 1, len(log)):
        if log[i].startswith(prefix):
            return i
    return -1

go(300 * M)
tap(PLAY)
go(200 * M)

# MIXER: DOWN twice, UP three times; each jump says the page it lands on.
tap(MIXER)
mark = len(log) - 1
for _ in range(2):
    tap(DOWN)
for _ in range(3):
    tap(UP)
names = [l for l in log[mark + 1:] if l in ("TRK 1-4", "DCK 1-4", "WID 1-4", "MST")]
check(names == ["DCK 1-4", "WID 1-4", "DCK 1-4", "TRK 1-4", "MST"],
      "MIXER UP/DOWN jumped a group at a time, round the top", f"names={names} log={log[-12:]}")

# KEYS: HOME twice, then a held OK.
tap(HOME); go(20 * M); tap(HOME)
go(50 * M)
check(index("scr KEYS") >= 0, "HOME twice reached KEYS", f"log={log[-12:]}")
emu.key(OK, True); go(700 * MS); emu.key(OK, False); go(14 * M)
opened = index("TRACK T1")
check(opened >= 0, "a held OK in KEYS opened the track menu", f"log={log[-12:]}")
tap(DOWN)
tap(CLR)
back = index("KEYS", opened)
check(back > opened, "CLR closed it, back on KEYS", f"log={log[-12:]}")

# JAM REC: FUNC + REC is RESAMPLE, again starts the jam, again stops it.
chord(REC)
go(50 * M)
resample = index("scr RESAMPLE")
check(resample >= 0, "FUNC + REC opened RESAMPLE", f"log={log[-12:]}")
chord(REC)
started = starts("JAM REC JAM ", resample)
check(started > resample, "FUNC + REC again started a jam", f"log={log[-12:]}")
go(2000 * MS)
chord(REC)
go(300 * M)
stopping = index("JAM STOPPING", started)
check(stopping > started, "FUNC + REC on RESAMPLE stopped it", f"log={log[-12:]}")
saved = starts("SAVED JAM ", stopping)
check(saved > stopping, "the file closed cleanly", f"log={log[-12:]}")
check(starts("JAM:") < 0, "the card kept up and nothing failed", f"log={log[-12:]}")

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
