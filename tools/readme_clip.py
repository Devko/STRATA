#!/usr/bin/env python3
"""The README's clip: STRATA's screens on the device build in the emulator, silent, 28 s.

    tools/readme_clip.py capture STAGE CARD_DIR OUT     # WSL: OUT/frames.bin, OUT/marks.json
    tools/readme_clip.py render OUT GIF [FPS]           # Python with numpy and Pillow: the GIF

The capture runs in WSL (tools/env.sh, after tools/build.sh); the render wherever Pillow is, and
squeezes the GIF with ffmpeg when it finds one. CARD_DIR holds a saved project (the TECHNO
showcase card's ENAGA/DATA/STRATA/PROJECTS/TECHNO/); the capture opens it on A01, moves to A05
PEAK and plays it, then walks GRID, the track menu (GENERATE), a PERFORM macro, MIXER's DJ
filter, EDIT's filter and ARPS on a timeline, one OLED frame every 50 ms. It starts and ends on
GRID, so it loops.
Keys and encoders only: the emulator has no pads.
"""
import importlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
M = 1_000_000
FUNC, REC, PROJ, HOME, PLAY, STOP, MIXER, EDIT = 19, 26, 20, 27, 22, 29, 21, 28
SL, SR, UP, DN, LT, RT, OK, CLR = 8, 9, 17, 25, 24, 18, 23, 30
A, B, Cn, D, VALUE = 0, 1, 2, 3, 4
FPS = 20


def capture(stage, card, out):
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(ROOT / "third_party" / "enaga" / "tools"))
    import enaga_cached
    paths = importlib.import_module("paths")
    build_app = importlib.import_module("build_app")
    enaga_cached.wrap_build_app(build_app, paths)
    emulate = importlib.import_module("emulate")
    enaga_cached.wrap_firmware(emulate.firmware_build, paths)

    card, out = Path(card), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    files = {"/" + str(f.relative_to(card)).replace("\\", "/"): f for f in card.rglob("*") if f.is_file()}
    emu = emulate.Emulation(stage, card_files=files, card_mib=16)
    cpu = emu.cpu
    from display import pixels  # the emulator's, on the path once it runs
    frame_cycles = 1000 * emulate.MS // FPS
    st = dict(now=0, capturing=False, t0=0, next=0, n=0)
    fout = open(out / "frames.bin", "wb")
    marks = []
    wall = time.time()

    def shoot():
        frame, _ = cpu.oled_frame()
        fout.write(bytes(pixels(frame)))
        st["n"] += 1
        st["next"] += frame_cycles

    def run_to(target):
        while st["now"] < target:
            stop = min(target, st["next"]) if st["capturing"] else target
            if stop > st["now"]:
                r = emu.run(stop - st["now"])
                if r["error"]:
                    sys.exit("emulator stopped: " + r["error"])
                st["now"] = stop
            if st["capturing"] and st["now"] >= st["next"]:
                shoot()

    def go(c):
        run_to(st["now"] + int(c))

    # Shorter taps lose presses: 12 M cycles down, 14 M up.
    def tap(k, hold=12 * M):
        emu.key(k, True); go(hold); emu.key(k, False); go(14 * M)

    def chord(mod, k):
        emu.key(mod, True); go(3 * M); tap(k); emu.key(mod, False); go(4 * M)

    def turn(enc, det):
        # Two detents at a time: emu.turn runs the clock itself, so frames keep coming.
        step = 2 if det > 0 else -2
        left = det
        while left:
            d = step if abs(left) >= 2 else left
            emu.turn(enc, d)
            left -= d
            st["now"] = cpu.state()["cycles"]
            while st["capturing"] and st["next"] <= st["now"]:
                shoot()

    def at(sec):
        run_to(st["t0"] + int(sec * 1000 * emulate.MS))

    def sweep(enc, det, start, end):
        n = max(1, abs(det) // 2)
        s = 2 if det > 0 else -2
        for i in range(n):
            at(start + (end - start) * i / n)
            turn(enc, s if i < n - 1 else det - (n - 1) * s)

    def mark(name):
        marks.append(dict(frame=st["n"], name=name))
        print(f"{st['n']:4d} {st['n'] / FPS:5.1f}s {name}  ({time.time() - wall:.0f} s wall)", flush=True)

    # --- set up, not filmed: TECHNO on A05 PEAK, EDIT on FLTR, GRID on the hat track ----------
    go(320 * M)
    tap(PROJ); go(150 * M)
    for _ in range(4):
        tap(SR)
    go(300 * M)
    tap(OK); go(100 * M); tap(OK)                      # PROJECTS: load TECHNO
    go(2500 * M)
    tap(PROJ); go(300 * M)
    for _ in range(4):
        tap(RT); go(60 * M)
    tap(OK); go(150 * M)                               # A05 PEAK
    tap(EDIT); go(150 * M)
    for _ in range(8):
        tap(SL)
    tap(SR)                                            # the page index wraps: this is ARP
    for _ in range(4):
        tap(SL)                                        # LFO FX AMP FLTR
    go(100 * M)
    tap(HOME); go(100 * M)
    tap(DN); tap(DN)                                    # GRID: track 3
    tap(PLAY)
    go(3000 * M)                                       # into the pattern, the toasts gone

    # --- the clip -------------------------------------------------------------------------------
    st["capturing"] = True
    st["t0"] = st["now"]
    st["next"] = st["now"]
    mark("GRID")
    at(2.8)
    emu.key(OK, True); go(250 * M); emu.key(OK, False); go(14 * M)
    mark("TRACK MENU")
    at(4.0); tap(OK)
    mark("GENERATE")
    at(4.8); tap(OK)
    at(5.6); tap(CLR)
    mark("GRID, NEW PART")
    at(7.2); chord(FUNC, MIXER)
    mark("PERFORM")
    sweep(B, 110, 8.0, 10.6)
    sweep(B, -110, 11.0, 12.4)
    at(12.8); tap(MIXER); tap(SL); tap(SL)
    mark("MIXER MST")
    sweep(A, -28, 13.6, 15.2)
    sweep(A, 28, 15.4, 16.8)
    at(17.2); tap(EDIT)
    tap(DN); tap(DN); tap(DN)                          # track 6, the bass
    mark("EDIT FLTR")
    sweep(A, 56, 18.0, 19.6)
    sweep(A, -56, 19.8, 21.2)
    at(21.4)
    for _ in range(4):
        tap(SR)                                        # AMP FX LFO ARP
    at(22.0); tap(OK)
    mark("ARPS")
    at(23.0); tap(DN)
    at(23.5); tap(DN)
    at(24.0); tap(DN)
    at(24.8); tap(CLR); tap(HOME); tap(UP); tap(UP); tap(UP)
    mark("GRID")
    at(27.6)
    st["capturing"] = False
    fout.close()
    (out / "marks.json").write_text(json.dumps(dict(fps=FPS, frames=st["n"], marks=marks), indent=1))
    print("done", st["n"], "frames", round(time.time() - wall), "s wall")
    emu.close()


def colour(grey):
    """The docs' screenshot look (tools/screenshots.py): a cool white OLED."""
    if grey == 0:
        return (10, 12, 16)
    t = grey / 15.0
    return (int(30 + 215 * t), int(36 + 214 * t), int(44 + 211 * t))


def render(cap, gif, fps=FPS, scale=4, border=6):
    import numpy as np
    from PIL import Image
    cap = Path(cap)
    data = np.frombuffer((cap / "frames.bin").read_bytes(), np.uint8)
    n = len(data) // (128 * 128)
    step = max(1, round(FPS / int(fps)))
    # One palette for every frame: the bezel, 16 greys, and each grey at 3/4 for the faint
    # OLED grid on the last row and column of every block, as in docs/screens.
    pal = [(24, 26, 30)] + [colour(g) for g in range(16)] + \
          [tuple(c * 3 // 4 for c in colour(g)) for g in range(16)]
    flat = [v for c in pal for v in c] + [0] * (768 - 3 * len(pal))
    size = (128 + 2 * border) * scale
    lines = np.zeros((scale, scale), np.uint8)
    lines[-1, :] = lines[:, -1] = 16
    grid = np.tile(lines, (128, 128))
    o = border * scale
    frames = []
    for i in range(0, n, step):
        grey = 1 + np.minimum(15, (data[i * 16384:(i + 1) * 16384].astype(np.int32) + 8) // 17)
        big = np.kron(grey.reshape(128, 128), np.ones((scale, scale), np.int32)) + grid
        canvas = np.zeros((size, size), np.uint8)
        canvas[o:o + 128 * scale, o:o + 128 * scale] = big
        img = Image.fromarray(canvas, "P")
        img.putpalette(flat)
        frames.append(img)
    frames[0].save(gif, save_all=True, append_images=frames[1:], duration=int(1000 * step / FPS),
                   loop=0, optimize=False, disposal=1)
    # ffmpeg's encoder stores only the rectangle that changed and makes the rest transparent:
    # a third of Pillow's size, the same pixels (no dither: the palette is exact).
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        tmp = Path(str(gif) + ".tmp.gif")
        subprocess.run([ffmpeg, "-v", "error", "-y", "-i", str(gif), "-filter_complex",
                        "split[a][b];[a]palettegen=max_colors=40:stats_mode=full[p];"
                        "[b][p]paletteuse=dither=none:diff_mode=rectangle",
                        "-gifflags", "+transdiff+offsetting", str(tmp)], check=True)
        tmp.replace(gif)
    print(f"{gif}: {len(frames)} frames at {FPS / step:.0f} fps, {Path(gif).stat().st_size / 1e6:.2f} MB")


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "capture":
        capture(*sys.argv[2:])
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "render":
        render(*sys.argv[2:])
    else:
        sys.exit(__doc__)
