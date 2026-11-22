#!/usr/bin/env bash
# Kit browser emulator scenario: FUNC+EDIT twice opens the kit library, FUNC+DOWN stores the kit
# into slot 1, and OK twice recalls it -- all on keys, which is all the emulator can press.
#
# Observable through Enaga's log: app/app.hpp logs `scr KIT` when the screen changes, and
# app/storage.cpp logs `kit saved` and `kit loaded` (or `kit save failed <why>`) for each kit
# job. The recall plays through a running transport, so the beat must still be sounding and the
# load must stay inside the target afterwards.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F8 = 28, DOWN = 25, OK = 23.
# A press is released 20 M cycles later, so F1 at t is still down when F8 arrives at t + 5 M.
# The two OKs are 60 M cycles apart, well inside the 2 s (744 M cycles) a second press has.
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
LOG="$ENAGA_BUILD/scenario_kits.log"

echo "running the emulator for $CYCLES cycles: play, MACHINE, KITS, store, recall ..."
python3 -B "$ENAGA_ROOT/tools/emulate.py" "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@380000000 --key 28@385000000 \
    --key 19@460000000 --key 28@465000000 \
    --key 19@560000000 --key 25@565000000 \
    --key 23@660000000 \
    --key 23@720000000 \
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

check("scr MACHINE" in log, "F1 + EDIT opened the machine select", f"log={log}")
check("scr KIT" in log, "F1 + EDIT again opened the kit browser", f"log={log}")
kits = [line for line in log if line.startswith("kit ")]
check("kit saved" in kits, "F1 + DOWN stored the kit in slot 1", f"lines={kits}")
check("kit loaded" in kits, "OK twice recalled it", f"lines={kits}")
if any("failed" in line for line in kits):
    print("        a kit job failed; the reason is on the line. NO CARD or FULL means the")
    print("        emulator's FAT image, not the browser.")

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
