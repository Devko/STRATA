#!/usr/bin/env python3
"""Render STRATA's audio output to a WAV, offline, through the SmplTrek emulator.

docs/SPEC.md's testing table asks for "an offline renderer that writes N bars to WAV". This is
it, and it exists because there is no other way to actually LISTEN to the app without a unit:

  - Enaga's web panel (tools/panel.sh) plays live, but the emulator runs at about 0.5-0.6x real
    time on a development host, so its sound breaks up (Enaga's docs/design.md says so).
  - Enaga's tools/emulate.py saves the capture ring at the end of a run, which holds only the
    last 32,767 frames: 0.68 seconds, whatever the run's length.

Rendering offline fixes both. The emulator can take as long as it likes per block, and this
drains the capture ring as it fills instead of reading it once at the end, so the length is
limited by patience rather than by a buffer. The result plays back at the right pitch and tempo
because it is the unit's own 48 kHz output, sample for sample -- the same words the McASP would
hand the codec.

    tools/render.sh --seconds 16 -o /tmp/strata.wav

The default is 16-bit, which every player opens. --bits 32 keeps the capture's own signed 32-bit
words, which is what to use for measurement rather than for listening.
"""
import argparse
import array
import importlib.util
import math
import struct
import sys
import time
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENAGA = ROOT / "third_party" / "enaga"

# 372 MHz: 496,000 cycles per 64-frame block at 48 kHz (docs/SPEC.md, "Platform facts").
CYCLES_PER_SECOND = 372_000_000
PLAY_KEY = 22                   # ENAGA_KEY_F5, which STRATA maps to PLAY
KEY_HOLD_CYCLES = 20_000_000    # what emulate.py uses for a press

# The capture ring holds 32,767 frames, about 0.68 s. Drain well inside that: at 100 M cycles a
# slice (0.27 s) two thirds of the ring is still free when it is read.
SLICE_CYCLES = 100_000_000


def load_emulate():
    """Enaga's tools/emulate.py as a module, with its own tools/ directory importable."""
    sys.path.insert(0, str(ENAGA / "tools"))
    spec = importlib.util.spec_from_file_location("enaga_emulate", ENAGA / "tools" / "emulate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Capture:
    """Reads new words out of the emulator's rolling capture ring, keeping channel phase.

    The ring reports how many words have been transmitted in total and how many it still holds,
    so the new ones are the last (total - already_taken) of what it holds. If that is more than
    it holds, the difference was overwritten before we got to it -- a dropped sample, which is
    reported rather than silently papered over.

    Channel phase matters and only has to be settled once: the two WAV channels are McASP slots 0
    and 1, so a capture that starts on slot 1 is a frame out and would swap left and right for the
    whole file. After the first aligned read the words are consecutive, so appending keeps it.
    """

    def __init__(self):
        self.words = bytearray()
        self.taken = 0
        self.lost = 0
        self.aligned = False

    def drain(self, cpu):
        raw, info = cpu.audio_output()
        total = info["transmitted_words"]
        held = info["captured_words"]
        new = total - self.taken
        if new <= 0:
            return 0

        if new > held:
            self.lost += new - held
            new = held

        chunk = raw[len(raw) - 4 * new:]

        if not self.aligned:
            # The slot of the oldest word still held is first_capture_slot; the word we are about
            # to take first sits (held - new) words after it.
            if (info["first_capture_slot"] + held - new) & 1:
                chunk = chunk[4:]
            self.aligned = True

        self.words.extend(chunk)
        self.taken = total
        return new

    def frames(self):
        return len(self.words) // 8


def write_wav(path, words, bits):
    count = len(words) // 4
    values = struct.unpack(f"<{count}i", bytes(words[:count * 4]))

    if bits == 32:
        payload = bytes(words[:count * 4])
        width = 4
    else:
        # The captured words are full-scale signed 32-bit (a sample held at the limiter's ceiling
        # reads as exactly -2^31), so the top 16 bits are the sample. Shift rather than scale, and
        # clamp, because 2^31 - 1 would otherwise round past 32,767.
        shifted = array.array("h", (max(-32768, min(32767, v >> 16)) for v in values))
        payload = shifted.tobytes()
        width = 2

    frames = count // 2
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(width)
        wav.setframerate(48000)
        wav.writeframes(payload[:frames * 2 * width])

    peak = max((abs(v) for v in values), default=0) / float(2 ** 31)
    rms = (sum((v / 2.0 ** 31) ** 2 for v in values) / len(values)) ** 0.5 if values else 0.0
    return dict(frames=frames, seconds=frames / 48000.0, peak=peak, rms=rms)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app_dir", type=Path, help="the staged flat app directory (tools/build.sh)")
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--seconds", type=float, default=16.0, help="of audio to render (default 16)")
    ap.add_argument("--bits", type=int, choices=(16, 32), default=16)
    ap.add_argument("--play-at", type=int, default=300_000_000,
                    help="cycle to press PLAY. Must be late: Enaga's panel link is not streaming "
                         "in the first ~200 M cycles and an earlier press is swallowed")
    ap.add_argument("--key", action="append", default=[], metavar="KEY@CYCLE",
                    help="an extra key press, released 20 M cycles later, as emulate.py takes it")
    ap.add_argument("--no-play", action="store_true", help="render without starting the transport")
    args = ap.parse_args(argv)

    emulate = load_emulate()

    events = []
    if not args.no_play:
        events += [(args.play_at, PLAY_KEY, True),
                   (args.play_at + KEY_HOLD_CYCLES, PLAY_KEY, False)]
    for item in args.key:
        key, at = item.split("@")
        events += [(int(at, 0), int(key, 0), True),
                   (int(at, 0) + KEY_HOLD_CYCLES, int(key, 0), False)]
    events.sort()

    want_frames = int(args.seconds * 48000)
    # Enough cycles for the warm-up, the key presses, and then the audio itself.
    latest_event = max([args.play_at] + [at for at, _, _ in events])
    end = latest_event + KEY_HOLD_CYCLES + int(args.seconds * CYCLES_PER_SECOND)

    print(f"rendering {args.seconds:.1f} s ({want_frames} frames) from {args.app_dir}")
    capture = Capture()
    started = time.perf_counter()

    with emulate.Emulation(args.app_dir) as emu:
        now = 0
        for at, key, down in events:
            if at > now:
                emu.run(at - now)
                now = at
            emu.key(key, down)

        # From here it is just blocks. Drain every slice so the ring never has the chance to
        # overwrite what it holds.
        last_report = 0.0
        while now < end and capture.frames() < want_frames:
            step = min(SLICE_CYCLES, end - now)
            result = emu.run(step)
            now += step
            capture.drain(emu.cpu)
            if result and result["error"]:
                print(f"  emulator stopped: {result['error']}", file=sys.stderr)
                break
            seconds = capture.frames() / 48000.0
            if seconds - last_report >= 1.0:
                last_report = seconds
                elapsed = time.perf_counter() - started
                print(f"  {seconds:5.1f} s of audio  ({elapsed:5.1f} s elapsed, "
                      f"{seconds / elapsed:.2f}x real time)")

        report = emu.report()

    stats = write_wav(args.out, capture.words, args.bits)

    print()
    print(f"wrote {args.out}: {stats['frames']} frames, {stats['seconds']:.2f} s, "
          f"{args.bits}-bit stereo 48 kHz")
    if stats["frames"]:
        print(f"peak {20 * math.log10(max(stats['peak'], 1e-9)):+.2f} dBFS, "
              f"rms {20 * math.log10(max(stats['rms'], 1e-9)):+.2f} dBFS")
    if capture.lost:
        print(f"WARNING: {capture.lost} words were overwritten before they could be read; "
              f"lower SLICE_CYCLES", file=sys.stderr)

    # The same health check the scenarios make, because a render is also a run: a dropout or a
    # crash would otherwise be something you notice only by ear, or not at all.
    print(f"app_status={report.get('app_status')!r} overruns={report.get('audio_overruns')} "
          f"blocks={report.get('audio_blocks')} peak_cycles={report.get('audio_cycles_peak')}")
    if report.get("crash_reason"):
        print(f"CRASH: {report['crash_reason']}", file=sys.stderr)
        return 1
    if report.get("audio_overruns"):
        print("WARNING: audio overruns in this run; the sound is not what the unit would make",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
