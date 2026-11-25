#!/usr/bin/env bash
# REL ALL and LEN ALL in the emulator (docs/CONTROLS.md, "REL ALL and LEN ALL"): FUNC + turn A
# and FUNC + turn B in PERFORM. Enaga 0.3.0's emulate.py turns encoders (--turn ENC:DETENTS@CYCLE),
# so the gesture runs on the DSP here: each click must reach the amount (the notice, which
# move_all_notice logs) and the turn back must stop on the detent at 0, while the beat runs with
# no crash, overrun or stall.
#
#   PLAY                     -> the beat runs
#   FUNC + MIXER             -> PERFORM
#   hold FUNC + turn A -3    -> REL ALL -2, -4, -6
#   hold FUNC + turn B -3    -> LEN ALL under 100%
#   hold FUNC + turn A +3    -> REL ALL -4, -2, 0 (rest)
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F7 = 21. Encoder events are
# alphabetical, 0 = A, 1 = B. A key is released 20 M cycles after its press and a detent takes
# 12 ms (4.5 M cycles), so each turn starts 2 M cycles into the FUNC hold and its three clicks end
# inside it. The first press must come LATE (tests/scenario_m1.sh).
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
LOG="$ENAGA_BUILD/scenario_moveall.log"

echo "running the emulator for $CYCLES cycles: REL ALL and LEN ALL in PERFORM ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 19@400000000 --key 21@405000000 \
    --key 19@500000000 --turn 0:-3@502000000 \
    --key 19@600000000 --turn 1:-3@602000000 \
    --key 19@700000000 --turn 0:3@702000000 \
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
# Enaga's log ring keeps the last 16 lines, and this scenario logs more than that, so "strata
# ready" can scroll out. A drawn screen comes only after init(), so it proves the same thing.
check("strata ready" in log or (len(log) == 16 and any(l.startswith("scr ") for l in log)),
      "init() ran to completion")
check("run int" in log, "the transport started", f"log={log}")


# The log from PERFORM on, in order: each click logs its notice.
perform = log[log.index("scr PERFORM") + 1:] if "scr PERFORM" in log else []
check("scr PERFORM" in log, "FUNC + MIXER reached PERFORM", f"log={log}")
rel = [l for l in perform if l.startswith("REL ALL ")]
lens = [l for l in perform if l.startswith("LEN ALL ")]
check(rel[:3] == ["REL ALL -2", "REL ALL -4", "REL ALL -6"],
      "FUNC + turn A moved REL ALL two counts a click", f"rel={rel}")
check(len(lens) == 3 and lens[-1] != "LEN ALL 100%",
      "FUNC + turn B moved LEN ALL under 100%", f"len={lens}")
check(rel[3:] == ["REL ALL -4", "REL ALL -2", "REL ALL 0"],
      "the turn back stopped on the detent at 0", f"rel={rel}")
check(not any(l.startswith("scr ") for l in perform), "PERFORM stayed on screen",
      f"perform={perform}")

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

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
