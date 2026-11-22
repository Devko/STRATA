#!/usr/bin/env bash
# Sample browser emulator scenario: EDIT on track 8 (the factory LOOP, a SAMPLE track), OK opens
# the browser, CLR climbs to its top level and OK opens SAMPLES again -- all on keys, which is all
# the emulator can press. It proves the screen draws on the C674x with the real font and that
# files::list answers; whether the emulator's card holds a /SAMPLES folder decides only whether
# the log says `browse listed` or `browse failed`, and either is a pass.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F8 = 28, UP = 17, OK = 23, CLR = 30.
# The first press comes late: the panel link is not streaming in the first ~200 M cycles.
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
LOG="$ENAGA_BUILD/scenario_samples.log"

echo "running the emulator for $CYCLES cycles: play, EDIT, track 8, SAMPLES, up, down ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@380000000 \
    --key 17@420000000 \
    --key 23@460000000 \
    --key 30@560000000 \
    --key 30@620000000 \
    --key 23@700000000 \
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

# The EDIT screen logs its page too ("scr EDIT SRC").
check(any(l == "scr EDIT" or l.startswith("scr EDIT ") for l in log), "EDIT opened", f"log={log}")
check("scr SAMPLES" in log, "OK on the SAMPLE track opened the browser", f"log={log}")
lists = [line for line in log if line.startswith("browse ")]
check(len(lists) >= 1, "files::list answered", f"lines={lists}")
if any("failed" in line for line in lists):
    print("        a listing failed: no /SAMPLES on the emulator's FAT image, most likely. That is")
    print("        the card, not the browser, and the error line is what the unit would show.")

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
