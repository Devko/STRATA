#!/usr/bin/env bash
# Choke groups (CyDrums S4) in the emulator: MIXER's fader page draws, push C and push D put
# tracks 3 and 4 (the default beat's two hats) in group 1, the notices reach the log, and the
# load with the chokes cutting while the beat plays is inside the target.
#
# The choke itself (who cuts whom, the fade, the same-frame rule) is tests/test_choke.cpp's: the
# emulator cannot hear a track. What only the emulator shows is that the gesture reaches the
# engine on the C674x and that the fade costs nothing measurable.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 (PLAY), F7 = 21 (MIXER), PUSH_C = 2,
# PUSH_D = 3. MIXER opens on TRK 1-4. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1300000000

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
LOG="$ENAGA_BUILD/scenario_choke.log"

echo "running the emulator for $CYCLES cycles: play, MIXER, tracks 3 and 4 into choke group 1 ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES"     --key 22@300000000     --key 21@380000000     --key 2@500000000     --key 3@560000000     > "$LOG" 2>&1 || {
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# The fader page drew, and each push put its strip's track in group 1, in order.
check(any(s.startswith("scr MIXER") for s in screens), "drew MIXER", f"screens={screens}")
chokes = [line for line in log if " CHOKE " in line]
check(chokes == ["T3 CHOKE GROUP 1", "T4 CHOKE GROUP 1"], "push C and D put T3 and T4 in group 1",
      f"log={chokes}")

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
print("note: no cache model here. tools/scenarios.sh says which compiler built it; the unit decides.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
