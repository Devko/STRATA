#!/usr/bin/env python3
"""Walk STRATA's screens in the emulator and turn the frames into the docs' screenshots.

    tools/emulator_screens.py walk STAGE OUT PART     # one part of the walk: a, b, c, d or e
    tools/emulator_screens.py pick WALK HOSTDUMPS DOCS # WALK's frames -> DOCS/NAME.png, overview.png

tools/emulator_screens.sh runs both: the five parts side by side (each boots its own emulator),
then the pick. The walk drives the built app (tools/build.sh's $ENAGA_BUILD/stage/STRATA) with
keys and encoder turns only, the factory project playing, a two-bar drum loop on the card as
/SAMPLES/LOOP.WAV. Each frame is what the unit's OLED shows at that cycle, greys 0-255.

The emulator cannot press pads, so MUTATE and SCENE come from the host build's draw of the same
screens (HOSTDUMPS, the ui_screenshots test's NAME.pgm/NAME.txt; tools/screenshots.sh's route).

EDIT's page index is global and skips the pages a machine lacks, so where a page lands depends
on the walk before it: PICKS names the frame each screenshot comes from. After a change to the
screens or the walk, look at the frames (a contact sheet) before trusting PICKS.
"""
import importlib
import math
import os
import random
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Screenshot name -> walk frame. Burst frames end _fNN; the first of a burst is used.
PICKS = {
    "grid": "grid_f00", "step": "step", "keys": "keys_f00", "help": "help",
    "help_keys": "helpp_06", "menu": "menu", "perform": "perform_f00", "resample": "resample",
    "project": "project", "chain": "chain", "pattern": "pattern", "setup": "setup",
    "projects": "projects_saved", "kit": "kit_saved", "machine": "machine", "sounds": "sounds",
    "samples": "samples_loaded",
    "edit": "edit_t1_p2_f00", "edit_fltr": "edit_t1_p3_f00", "edit_amp": "edit_t1_p4_f00",
    "edit_fx": "edit_t1_p5_f00", "edit_lfo": "lfo_on_f00", "edit_arp": "edit_t1_p1_f00",
    "edit_vfltr": "edit_t6_p7_f00", "edit_env": "edit_t7_p4_f00", "edit_osc": "edit_t7_p5_f00",
    "edit_wave": "smp_p1_f00", "edit_wave_amp": "smp_p3_f00", "edit_grain": "grn_p0_f00",
    "mixer": "mixer_02_f00", "mixer_dck": "mixer_04_f00", "mixer_duck": "mixer_06_f00",
    "mixer_dly": "mixer_07_f00", "mixer_mst": "mixer_12_f00", "mixer_eq": "mixer_13_f00",
}
HOST = ["mutate", "scene"]
ORDER = ["grid", "step", "edit", "mixer", "perform", "keys", "machine", "kit", "project"]

M = 1_000_000
FUNC, REC, PROJ, HOME, PLAY, STOP, MIXER, EDIT = 19, 26, 20, 27, 22, 29, 21, 28
SL, SR, UP, DN, LT, RT, OK, CLR = 8, 9, 17, 25, 24, 18, 23, 30
STEP1 = 32
TOAST = 1000 * M                                     # 2.7 s: a screen's name toast is gone


def drum_loop(path, bpm=130.0, bars=2, rate=48000):
    """A two-bar kick, snare and hat loop, mono 16-bit: something for SAMPLE and GRAIN to show."""
    beat = 60.0 / bpm
    n = int(rate * beat * 4 * bars)
    out = [0.0] * n
    rnd = random.Random(7)
    for step in range(16 * bars):
        at = int(step * beat / 4 * rate)
        if step % 4 == 0:                                    # kick
            for i in range(int(0.3 * rate)):
                t = i / rate
                if at + i < n:
                    out[at + i] += 0.9 * math.exp(-t * 9) * math.sin(2 * math.pi * (50 * t + 90 * (1 - math.exp(-t * 30)) / 30))
        if step % 8 == 4:                                    # snare
            for i in range(int(0.2 * rate)):
                t = i / rate
                if at + i < n:
                    out[at + i] += math.exp(-t * 18) * (0.5 * rnd.uniform(-1, 1) + 0.3 * math.sin(2 * math.pi * 190 * t))
        if step % 2 == 1:                                    # hat
            for i in range(int(0.05 * rate)):
                if at + i < n:
                    out[at + i] += 0.25 * math.exp(-i / rate * 70) * rnd.uniform(-1, 1)
    data = b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 26000)))) for v in out)
    header = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
              struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data)))
    Path(path).write_bytes(header + data)


def walk(stage, out, part):
    sys.path.insert(0, str(ROOT / "tools"))
    sys.path.insert(0, str(ROOT / "third_party" / "enaga" / "tools"))
    import enaga_cached
    paths = importlib.import_module("paths")
    build_app = importlib.import_module("build_app")
    enaga_cached.wrap_build_app(build_app, paths)
    emulate = importlib.import_module("emulate")
    enaga_cached.wrap_firmware(emulate.firmware_build, paths)

    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    loop = out / f"loop_{part}.wav"
    drum_loop(loop)
    t0 = time.time()
    emu = emulate.Emulation(stage, card_files={"/SAMPLES/LOOP.WAV": loop}, card_mib=16)
    from display import pixels

    def go(c):
        r = emu.run(int(c))
        if r["error"]:
            sys.exit(f"emulator stopped: {r['error']}")

    # Shorter taps lose presses: 12 M cycles down, 14 M up.
    def tap(k, hold=12 * M):
        emu.key(k, True); go(hold); emu.key(k, False); go(14 * M)

    def chord(mod, k):
        emu.key(mod, True); go(3 * M); tap(k); emu.key(mod, False); go(4 * M)

    def turn(enc, detents):
        emu.turn(enc, detents); go(60 * M)

    # A screen opens with its name as a toast for a moment: wait it out.
    def shot(name, wait=40 * M, burst=1):
        go(wait)
        for i in range(burst):
            if i:
                go(18_600_000)                               # one 50 ms draw later
            frame, _ = emu.cpu.oled_frame()
            nm = name if burst == 1 else f"{name}_f{i:02d}"
            (out / f"{nm}.pgm").write_bytes(b"P5\n128 128\n255\n" + bytes(pixels(frame)))
        print(f"{time.time() - t0:7.1f}s {name}", flush=True)

    def to_first_page():
        tap(EDIT)
        for _ in range(8):
            tap(SL)

    go(320 * M)                                              # boot
    tap(PLAY)
    go(600 * M)

    if part == "a":                                          # GRID, step page, KEYS, menus, PROJECT
        tap(HOME); shot("grid", burst=4)
        emu.key(STEP1, True); go(5 * M); tap(OK); emu.key(STEP1, False); go(5 * M)
        shot("step")
        tap(HOME)
        tap(HOME); go(30 * M); tap(HOME, hold=4 * M); go(2 * M); tap(HOME, hold=4 * M)
        shot("keys", wait=TOAST, burst=2)
        tap(HOME); go(30 * M)
        emu.key(FUNC, True); go(320 * M); shot("help", wait=10 * M)
        emu.key(FUNC, False); go(30 * M)
        chord(FUNC, HOME); shot("menu", wait=TOAST)
        tap(CLR); go(30 * M)
        chord(FUNC, MIXER); shot("perform", burst=4)
        tap(HOME)
        chord(FUNC, REC); shot("resample", wait=TOAST)
        tap(CLR); tap(HOME)
        tap(PROJ); shot("project", wait=TOAST)
        for nm in ("chain", "pattern", "setup", "projects"):
            tap(SR); shot(nm, wait=TOAST)
        tap(CLR); tap(HOME)

    if part == "b":                                          # every EDIT page of every track
        to_first_page()
        for t in range(8):
            for p in range(8):
                shot(f"edit_t{t + 1}_p{p}", wait=30 * M, burst=2)
                tap(SR)
            tap(DN)
        tap(OK); shot("sounds", wait=TOAST); tap(CLR)
        chord(FUNC, EDIT); shot("machine", wait=TOAST)
        tap(CLR); tap(HOME)

    if part == "c":                                          # every MIXER page
        tap(MIXER)
        for _ in range(14):
            tap(SL)
        for p in range(14):
            shot(f"mixer_{p:02d}", wait=40 * M, burst=2)
            tap(SR)

    if part == "d":                                          # LOOP.WAV on track 8: SAMPLE, then GRAIN
        tap(EDIT)
        for _ in range(7):
            tap(DN)
        chord(FUNC, PROJ); shot("samples", wait=TOAST)
        tap(OK); go(800 * M); shot("samples_loaded", wait=TOAST)
        tap(CLR); tap(HOME)
        to_first_page()
        for p in range(8):
            shot(f"smp_p{p}", wait=60 * M, burst=2)
            tap(SR)
        chord(FUNC, EDIT); go(60 * M); tap(DN); shot("machine_grn", wait=60 * M)
        go(300 * M)
        to_first_page()
        for p in range(8):
            shot(f"grn_p{p}", wait=80 * M, burst=2)
            tap(SR)

    if part == "e":                                          # LFO running, kits, a saved project, help
        to_first_page()                                      # lands on track 1's LFO page
        turn(2, 40); turn(3, 6)
        shot("lfo_on", wait=300 * M, burst=4)
        tap(HOME)
        chord(FUNC, EDIT); chord(FUNC, EDIT); go(400 * M)
        for _ in range(3):
            chord(FUNC, DN); go(500 * M); tap(DN)
        tap(UP); tap(UP)
        shot("kit_saved", wait=TOAST)
        tap(CLR); tap(HOME)
        chord(FUNC, HOME); tap(OK); go(900 * M)                # the menu's SAVE
        tap(CLR); tap(HOME)
        tap(PROJ); go(150 * M)
        for _ in range(4):
            tap(SR)
        shot("projects_saved", wait=TOAST)
        tap(CLR); tap(HOME); go(30 * M)
        emu.key(FUNC, True); go(320 * M)
        for i in range(11):
            shot(f"helpp_{i:02d}", wait=10 * M)
            tap(SR)
        emu.key(FUNC, False); go(30 * M)

    print("done", round(time.time() - t0, 1))
    emu.close()


def read_pgm(path):
    data = Path(path).read_bytes()
    return list(data[-128 * 128:])


def pick(walk_dir, host_dir, docs):
    sys.path.insert(0, str(ROOT / "tools"))
    import screenshots as shots
    os.makedirs(docs, exist_ok=True)
    images = {}
    for name, frame in PICKS.items():
        grey = [min(15, (v + 8) // 17) for v in read_pgm(Path(walk_dir) / f"{frame}.pgm")]
        images[name] = shots.render(grey)
    font, unknown = shots.load_font()
    for name in HOST:
        images[name] = shots.render(shots.load_screen(host_dir, name, font, unknown))
    for name, image in images.items():
        shots.write_png(os.path.join(docs, name + ".png"), *image)
    shots.write_png(os.path.join(docs, "overview.png"), *shots.tile([images[n] for n in ORDER], 3, 24))
    print(f"wrote {len(images)} screens and overview.png to {docs}")


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "walk":
        walk(*sys.argv[2:])
    elif len(sys.argv) == 5 and sys.argv[1] == "pick":
        pick(*sys.argv[2:])
    else:
        sys.exit(__doc__)
