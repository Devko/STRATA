#!/usr/bin/env bash
# Locks on screen in the emulator (docs/CONTROLS.md, "Locks on screen"): hold a STEP and turn a
# knob, on GRID and then on EDIT. While the key is down every frame draws the held step's lock
# against the kit value ("S3 CUT 64>90", the bar's hollow tick) and the GRID cells' glyphs. This
# proves that path runs on the DSP without a crash, an overrun or a stall, and that each turn
# wrote a lock (one undo level deeper) without toggling the trig (the step mask stays as STEP 3 left it).
#
#   PLAY                     -> the beat runs
#   STEP 3                   -> a trig on step 3, u1
#   hold STEP 3 + turn A     -> a lock on step 3, u2, mask unchanged (GRID)
#   MACHINE (F8)             -> EDIT
#   hold STEP 3 + turn B     -> a second lock, u3, mask unchanged (EDIT)
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F8 = 28, STEP n = 31 + n. A key is
# released 20 M cycles after its press, so each turn starts 2 M cycles into the hold.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1000000000
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
LOG="$ENAGA_BUILD/scenario_plock.log"

echo "running the emulator for $CYCLES cycles: locks turned on GRID and EDIT ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 34@380000000 \
    --key 34@460000000 --turn 0:3@462000000 \
    --key 28@560000000 \
    --key 34@640000000 --turn 1:2@642000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
python3 - "$LOG" <<'PY' || rc=$?
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
check("run int" in log, "the transport started", f"log={log}")

edits = [l for l in log if l.startswith("g")]
mask = {l.split()[1]: l.split()[0] for l in edits if len(l.split()) == 2}
check("u1" in mask, "STEP 3 wrote a trig (u1)", f"log={log}")
check("u2" in mask and mask["u2"] == mask.get("u1"),
      "hold STEP 3 + turn A on GRID wrote a lock and toggled nothing (u2)", f"edits={edits}")
check(any(l.startswith("scr EDIT") for l in log), "MACHINE reached EDIT", f"log={log}")
check("u3" in mask and mask["u3"] == mask.get("u1"),
      "hold STEP 3 + turn B on EDIT wrote a lock and toggled nothing (u3)", f"edits={edits}")

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

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
