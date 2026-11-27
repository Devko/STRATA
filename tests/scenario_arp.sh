#!/usr/bin/env bash
# ARP emulator scenario: the arp's kit settings and ARPS, the factory arps (core/arp.hpp,
# core/arppresets.hpp), reached by keys and knob pushes on the C674x, with the beat playing.
#
# EDIT on track 6 (BASS, VA MONO: DOWN five times), the selection keys to the ARP page, push A
# (VAR CONV) and push D (RATCHET 2:4) -- each logged as `ARP VAR CONV` and `ARP RTC 2:4` --
# then OK opens ARPS (`scr ARPS`), DOWN and OK load TRANCE, two more DOWN and OK load ACID
# (`arp TRANCE`, `arp ACID`), and CLR goes back to the ARP page. The bass then plays the ACID
# arp with its rests and accents to the end of the run, so the load check covers an arp with
# a 16th rhythm on the busiest synth track; the factory beat is otherwise as it boots.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, DOWN = 25,
# SELECT_RIGHT = 9, PUSH_A = 1, PUSH_D = 3, OK = 23, CLR = 30. The first press comes late: the
# panel link is not streaming in the first ~200 M cycles (tests/scenario_m1.sh). Enaga's log ring
# keeps 16 lines; this logs 21, so the first ones are gone by the end.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1500000000

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
LOG="$ENAGA_BUILD/scenario_arp.log"

echo "running the emulator for $CYCLES cycles: play, EDIT on BASS, ARP, VAR, RATCHET, ARPS ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 25@380000000 --key 25@420000000 --key 25@460000000 --key 25@500000000 \
    --key 25@540000000 \
    --key 9@580000000 --key 9@620000000 --key 9@660000000 --key 9@700000000 \
    --key 9@740000000 \
    --key 1@780000000 \
    --key 3@820000000 \
    --key 23@860000000 \
    --key 25@900000000 --key 23@940000000 \
    --key 25@980000000 --key 25@1020000000 --key 23@1060000000 \
    --key 30@1100000000 \
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
screens = [line for line in log if line.startswith("scr ")]

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
# Not "run int" or "scr EDIT SRC": the five track selections log a line each (`g... u0`), and
# Enaga's 16-line ring has lost both by the end. The load check below proves the beat played.
check("scr EDIT ARP" in screens, "the selection keys reached the ARP page", f"screens={screens}")
check("ARP VAR CONV" in log, "push A on the ARP page walked VAR to CONV", f"log={log}")
check("ARP RTC 2:4" in log, "push D on the ARP page walked RATCHET to 2:4", f"log={log}")
check("scr ARPS" in screens, "OK on the ARP page opened ARPS", f"screens={screens}")
loads = [line for line in log if line.startswith("arp ")]
check(loads == ["arp TRANCE", "arp ACID"], "TRANCE, then ACID loaded in order",
      f"lines={loads}")
check(bool(screens) and screens[-1] == "scr EDIT ARP", "CLR went back to the ARP page",
      f"screens={screens}")

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
print("note: no cache model here. The arp is control-rate work: this should read as the factory")
print("      beat does (scenario_factory.sh), within the half point a peak moves between runs.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
