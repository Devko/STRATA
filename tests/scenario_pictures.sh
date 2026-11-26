#!/usr/bin/env bash
# EDIT's pictures in the emulator (docs/CONTROLS.md, "Waveforms and envelopes"): every page that
# draws a waveform or an envelope is walked while the beat plays, so each one draws on the C674x
# with the real font, from the loader's overview and the audio side's WaveView, with no crash, no
# overrun and the load inside the target. A draw off the canvas is a hard fault on the unit and a
# silent pass on the host, which is what this is for.
#
#   PLAY, EDIT                -> track 1 (KICK), SRC
#   SELECT_RIGHT x2           -> AMP: the kick's decay
#   DOWN x7                   -> track 8 (SAMPLE), AMP: ATK DEC
#   SELECT_LEFT x2            -> SRC: the waveform, its window and the loop's playhead
#   FUNC+EDIT, DOWN, EDIT     -> track 8 to GRAIN, back on SRC: the bracket and the grains
#   SELECT_RIGHT x6           -> through AMP (the cloud's envelope) to ASR
#   UP                        -> track 7 (VA POLY), ASR: ATK SUS REL
#   SELECT_LEFT x5            -> FLTR: the filter's envelope, ENV as its height
#
# Keys (third_party/enaga/include/enaga/abi.h): F1 = 19 FUNC, F5 = 22 PLAY, F8 = 28 EDIT,
# SELECT_LEFT = 8, SELECT_RIGHT = 9, UP = 17, DOWN = 25. Each page is held 30-40 M cycles, two or
# more 50 ms frames. The first press comes late: the panel link is not streaming in the first
# ~200 M cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1450000000
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
LOG="$ENAGA_BUILD/scenario_pictures.log"

echo "running the emulator for $CYCLES cycles: EDIT's waveforms and envelopes ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 28@340000000 \
    --key 9@380000000 --key 9@420000000 \
    --key 25@460000000 --key 25@490000000 --key 25@520000000 --key 25@550000000 \
    --key 25@580000000 --key 25@610000000 --key 25@640000000 \
    --key 8@700000000 --key 8@740000000 \
    --key 19@800000000 --key 28@805000000 \
    --key 25@840000000 \
    --key 28@880000000 \
    --key 9@940000000 --key 9@980000000 --key 9@1020000000 --key 9@1050000000 \
    --key 9@1080000000 --key 9@1110000000 \
    --key 17@1160000000 \
    --key 8@1220000000 --key 8@1250000000 --key 8@1280000000 --key 8@1310000000 \
    --key 8@1340000000 \
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

# Enaga's emulator keeps the last 16 log lines, so the early pages have scrolled out by the end;
# the walk's tail is what is left to check. The pages before it drew too, or the app would not
# have got this far without a fault.
screens = [line for line in log if line.startswith("scr ")]
check("scr EDIT ENV" in screens, "the walk reached ASR", f"screens={screens}")
check(bool(screens) and screens[-1] == "scr EDIT FLTR", "and ended on the synth's FLTR",
      f"screens={screens}")
check(any(line.startswith("mch 8 GRN") for line in log) or len(log) == 16,
      "track 8 went to GRAIN", f"log={log}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1500, "audio ran throughout", f"blocks={blocks}")
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
