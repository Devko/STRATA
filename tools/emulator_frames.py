#!/usr/bin/env python3
"""Record the app's screen in the emulator, frame by frame, to see an animation as the unit draws it.

    ENAGA_EMULATOR=... python3 -B tools/emulator_frames.py STAGE_DIR OUT_DIR --from CYCLES --frames N \\
        [--key KEY@CYCLE ...] [--turn ENC:DETENTS@CYCLE ...]

STAGE_DIR is the built app (tools/build.sh's $ENAGA_BUILD/stage/STRATA). The keys and turns are
emulate.py's. From cycle --from on, the screen is saved every 50 ms (18.6 M cycles, the rate
Enaga draws at) as OUT_DIR/frame_NNN.pgm, greys 0-255, N frames. tools/visuals_mockup.py's
counterpart for the real app: the live views' motion, not a still. Assembling a GIF is left to
whatever is at hand (Pillow, ImageMagick).
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "third_party" / "enaga" / "tools"))

import emulate  # noqa: E402

FRAME_CYCLES = 50 * emulate.MS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stage")
    ap.add_argument("out")
    ap.add_argument("--from", dest="start", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--key", action="append", default=[])
    ap.add_argument("--turn", action="append", default=[])
    args = ap.parse_args()

    events = []
    for item in args.key:
        key, at = item.split("@")
        events += [(int(at, 0), "key", (int(key, 0), True)),
                   (int(at, 0) + 20_000_000, "key", (int(key, 0), False))]
    for item in args.turn:
        what, at = item.split("@")
        encoder, detents = what.split(":")
        events.append((int(at, 0), "turn", (int(encoder), int(detents))))
    shots = [args.start + i * FRAME_CYCLES for i in range(args.frames)]
    events += [(at, "shot", (i,)) for i, at in enumerate(shots)]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with emulate.Emulation(args.stage) as emu:
        from display import pixels  # the emulator's own: Emulation puts it on the path
        now = 0
        for at, kind, data in sorted(events, key=lambda e: e[0]):
            if at > now:
                result = emu.run(at - now)
                now = at
                if result["error"]:
                    sys.exit(f"emulator stopped: {result['error']}")
            if kind == "key":
                emu.key(*data)
            elif kind == "turn":
                emu.turn(*data)
                now = emu.cpu.state()["cycles"]
            elif kind == "shot":
                frame, _ = emu.cpu.oled_frame()
                grey = pixels(frame)
                (out / f"frame_{data[0]:03d}.pgm").write_bytes(b"P5\n128 128\n255\n" + bytes(grey))
    print(f"{len(shots)} frames in {out}")


if __name__ == "__main__":
    main()
