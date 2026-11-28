#!/usr/bin/env bash
# Sample audition with the beat playing: push A once only arms, the second push hears the file.
#
#   PLAY                     -> `run int`: the factory beat runs
#   EDIT, UP to track 8      -> the factory LOOP, a SAMPLE track with no file
#   OK                       -> SAMPLES opens in /SAMPLES, the cursor on TONE.WAV
#   push A                   -> `A AGAIN TO HEAR`, and nothing streams: there is no cue output,
#                               so a single push must not put a file into the room mid-set
#   push A, 0.38 s later     -> `aud play`: now it is heard
#
# Run ends 0.24 s after the second push. scenario_audition.sh checks the heard output itself
# (pitch, length, level) with the beat stopped; this one checks the gate and the load with the
# beat and the stream together.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22 PLAY, F8 = 28 EDIT, UP = 17, OK = 23,
# PUSH_A = 1. The first press comes late: the panel link is not streaming in the first ~200 M
# cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=940000000

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
LOG="$ENAGA_BUILD/scenario_auditionplay.log"
TONE="$ENAGA_BUILD/scenario_auditionplay_tone.wav"

python3 - "$TONE" <<'PY'
import math, struct, sys, wave
rate, seconds, freq = 44100, 0.4, 1000.0
frames = int(rate * seconds)
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(b"".join(struct.pack("<h", int(16384 * math.sin(2 * math.pi * freq * i / rate)))
                           for i in range(frames)))
PY

echo "running the emulator for $CYCLES cycles: PLAY, EDIT, track 8, SAMPLES, push A twice ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --file "/SAMPLES/TONE.WAV=$TONE" \
    --key 22@250000000 \
    --key 28@300000000 \
    --key 17@360000000 \
    --key 23@420000000 \
    --key 1@560000000 \
    --key 1@700000000 \
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
check("strata ready" in log, "init() ran to completion")
check("run int" in log, "PLAY started the beat (run int)", f"log={log}")
check("scr SAMPLES" in log, "OK on the SAMPLE track opened the browser", f"log={log}")
check(log.count("A AGAIN TO HEAR") == 1, "the first push only armed (A AGAIN TO HEAR, once)",
      f"log={log}")
armed = log.index("A AGAIN TO HEAR") if "A AGAIN TO HEAR" in log else -1
check("aud play" in log, "the second push started the audition (aud play)", f"log={log}")
if "aud play" in log and armed >= 0:
    check(log.index("aud play") > armed, "and not before the first push had armed", f"log={log}")
errors = [l for l in log if l.startswith("aud ") and l not in ("aud play", "aud end")]
check(not errors, "the audition was not refused", f"lines={errors}")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')
peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
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
