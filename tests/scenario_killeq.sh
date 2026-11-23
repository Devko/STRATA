#!/usr/bin/env bash
# The master kill EQ in the emulator: its MIXER page draws on the C674x, its three kill switches
# reach the engine, and the load with the EQ RUNNING is inside the target.
#
# The emulator presses keys and nothing else (tests/scenario_m7.sh says why), and the kill
# switches are keys: on the MIXER's EQ page a plain push of A, B or C throws that band's switch.
# FUNC + push from other modes needs a held key, which `--key` cannot do; tests/test_ui.cpp owns
# that. All three bands are killed here, so the EQ runs for the rest of the run and the peak
# below includes it -- the figure docs/BUDGET.md asks for ("The master kill EQ"), with cl6x and
# with ENAGA_TI_CGT=none. Compare it with scenario_m7.sh, the same beat with the EQ bypassed.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F7 = 21, SELECT_RIGHT = 9,
# PUSH_A = 1, PUSH_B = 0, PUSH_C = 2. The first press must come LATE (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1200000000

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
LOG="$ENAGA_BUILD/scenario_killeq.log"

echo "running the emulator for $CYCLES cycles: play, MIXER EQ, kill all three bands ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 21@380000000 \
    --key 9@440000000 \
    --key 9@480000000 \
    --key 9@520000000 \
    --key 9@560000000 \
    --key 9@600000000 \
    --key 9@640000000 \
    --key 9@680000000 \
    --key 9@720000000 \
    --key 1@780000000 \
    --key 0@820000000 \
    --key 2@860000000 \
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")

screens = [line for line in log if line.startswith("scr ")]
check(bool(screens), "the screen was logged at all", f"log={log}")

# The EQ page drew, and each push reached the switch it names, in order.
check("scr MIXER EQ" in screens, "drew MIXER EQ", f"screens={screens}")
kills = [line for line in log if line.startswith("KILL ") or line.endswith(" BACK")]
check(kills == ["KILL LOW", "KILL MID", "KILL HI"], "the three pushes killed LOW, MID, HI",
      f"log={kills}")

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
