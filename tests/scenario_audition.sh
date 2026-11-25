#!/usr/bin/env bash
# Sample audition in the emulator: push A in SAMPLES hears a file without loading it.
#
#   (the beat stays stopped, so the output is the audition alone)
#   EDIT, UP to track 8      -> the factory LOOP, a SAMPLE track with no file
#   OK                       -> SAMPLES opens in /SAMPLES, the cursor on TONE.WAV, its one file
#   push A                   -> `aud play`: the file streams from the card to the output
#   ...0.4 s later           -> `aud end`: played whole, the stream closed
#
# TONE.WAV is made here: 0.4 s of a 1 kHz sine at -6 dBFS, 44.1 kHz mono 16-bit, so the
# file plays at its own rate rather than the engine's. The emulator's capture (audio.wav) is then
# read back: a 1 kHz tone, 0.4 s long, with silence before the push and after its end. That
# is the whole path on the C674x and Enaga's real file layer: the header read, files::Stream,
# the audio side's conversion and Catmull-Rom read, and the mix.
#
# Keys (third_party/enaga/include/enaga/abi.h): F8 = 28 EDIT, UP = 17, OK = 23, PUSH_A = 1.
# The first press comes late: the panel link is not streaming in the first ~200 M cycles.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

# The capture holds only the run's last 65,536 words (0.68 s), so the run ends 0.24 s after the
# push: the window then holds the silence before it, the whole tone and the silence after.
CYCLES=800000000

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
LOG="$ENAGA_BUILD/scenario_audition.log"
TONE="$ENAGA_BUILD/scenario_audition_tone.wav"
CAPTURE="$ENAGA_BUILD/emulate/audio.wav"
rm -f "$CAPTURE"

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

echo "running the emulator for $CYCLES cycles: EDIT, track 8, SAMPLES, push A ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" \
    --file "/SAMPLES/TONE.WAV=$TONE" \
    --key 28@300000000 \
    --key 17@360000000 \
    --key 23@420000000 \
    --key 1@560000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
python3 - "$LOG" "$CAPTURE" <<'PY' || rc=$?
import json, re, struct, sys, wave

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
check("scr SAMPLES" in log, "OK on the SAMPLE track opened the browser", f"log={log}")
check(any(l.startswith("browse listed") for l in log), "/SAMPLES was listed", f"log={log}")
errors = [l for l in log if l.startswith("aud ") and l not in ("aud play", "aud end")]
check(not errors, "the audition was not refused", f"lines={errors}")
check("aud play" in log, "push A started the audition (aud play)", f"log={log}")
check("aud end" in log and log.index("aud end") > log.index("aud play") if "aud play" in log else False,
      "it ended by itself (aud end)", f"log={log}")
check("run int" not in log, "the transport stayed stopped: what is heard is the audition")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')
peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")

# The capture: 48 kHz stereo, 32-bit words. Find where it is loud, how long for, and at what
# pitch (zero crossings of the left side).
try:
    with wave.open(sys.argv[2], "rb") as w:
        rate, width, channels = w.getframerate(), w.getsampwidth(), w.getnchannels()
        raw = w.readframes(w.getnframes())
    fmt = {2: "h", 4: "i"}[width]
    samples = struct.unpack("<" + fmt * (len(raw) // width), raw)
    left = samples[0::channels]
    right = samples[1::channels] if channels > 1 else left
    full = float(2 ** (8 * width - 1))
    window = rate // 100                                      # 10 ms
    levels = [max(abs(x) for x in left[i:i + window]) / full
              for i in range(0, len(left) - window, window)]
    loudest = max(levels) if levels else 0.0
    loud = [i for i, v in enumerate(levels) if v > loudest * 0.5] if loudest > 0 else []
    span_s = (loud[-1] - loud[0] + 1) * window / rate if loud else 0.0
    print(f"        capture: {len(left) / rate:.2f} s, loudest {loudest:.3f} of full scale, "
          f"loud for {span_s:.2f} s")
    check(loudest > 0.05, "the tone was heard", f"loudest={loudest:.3f}")
    check(0.35 <= span_s <= 0.45, "for its whole 0.4 s, at its own rate", f"{span_s:.2f} s")
    if loud:
        a, b = loud[0] * window + window, (loud[-1]) * window
        seg = left[a:b]
        crossings = sum(1 for x, y in zip(seg, seg[1:]) if (x < 0) != (y < 0))
        freq = crossings / 2 / (len(seg) / rate) if seg else 0.0
        check(950 <= freq <= 1050, "at 1 kHz", f"{freq:.0f} Hz")
        # The capture's two slots come one word apart, so R may be L a frame later.
        same = (all(x == y for x, y in zip(left[a:b], right[a:b])) or
                all(x == y for x, y in zip(left[a + 1:b], right[a:b - 1])))
        check(same, "a mono file on both sides alike")
        check(0.2 <= loudest <= 0.3, "at -6 dB: the file's 0.5 heard at 0.25", f"{loudest:.3f}")
        before = levels[:max(loud[0] - 2, 0)]
        after = levels[loud[-1] + 2:]
        quiet = max(before) if before else 1.0
        quiet_after = max(after) if after else 1.0
        check(quiet < loudest * 0.05, "silence before the push", f"{quiet:.4f}")
        check(quiet_after < loudest * 0.05, "silence after its end", f"{quiet_after:.4f}")
except FileNotFoundError:
    check(False, "the emulator wrote a capture", sys.argv[2])

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
