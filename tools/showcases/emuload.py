#!/usr/bin/env python3
"""Load the DEEP card in the emulator, play one pattern, report the audio peak load.

    emuload.py STAGE CARD_DIR SLOT SECONDS [WAV]
"""
import importlib
import json
import sys
import time
import wave
from pathlib import Path

import os
ROOT = Path(os.environ.get("STRATA_ROOT", "/mnt/d/DEV/STRATA"))   # tools/env.sh sets it
M = 1_000_000
PROJ, PLAY, STOP, OK, SR, RT, HOME = 20, 22, 29, 23, 9, 18, 27
CYCLES_PER_BLOCK = 496_000

sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "third_party" / "enaga" / "tools"))
import enaga_cached  # noqa: E402
paths = importlib.import_module("paths")
build_app = importlib.import_module("build_app")
enaga_cached.wrap_build_app(build_app, paths)
emulate = importlib.import_module("emulate")
enaga_cached.wrap_firmware(emulate.firmware_build, paths)

stage, card, slot, seconds = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
wav_out = sys.argv[5] if len(sys.argv) > 5 else None
files = {"/" + str(f.relative_to(card)).replace("\\", "/"): f for f in card.rglob("*") if f.is_file()}
emu = emulate.Emulation(stage, card_files=files, card_mib=16, keep_audio=bool(wav_out))
wall = time.time()


def go(c):
    r = emu.run(int(c))
    if r["error"]:
        sys.exit("emulator stopped: " + r["error"])


def tap(k):
    emu.key(k, True); go(12 * M); emu.key(k, False); go(14 * M)


go(320 * M)
tap(PROJ); go(150 * M)
for _ in range(4):
    tap(SR)
go(300 * M)
tap(OK); go(100 * M); tap(OK)          # PROJECTS: load DEEP (the only project on the card)
go(2500 * M)
tap(PROJ); go(300 * M)
for _ in range(slot):
    tap(RT); go(60 * M)
tap(OK); go(150 * M)
tap(HOME); go(100 * M)
before = emu.word("audio_cycles_peak")
emu.audio = bytearray(); emu.audio_slot = None
tap(PLAY)
cycles = int(seconds * 372 * M)
done = 0
lasts = []
while done < cycles:
    go(2 * M)
    done += 2 * M
    lasts.append(emu.word("audio_cycles_last"))
lasts.sort()
avg = sum(lasts) / len(lasts)
report = emu.report()
log = report.get("log", [])
peak = report.get("audio_cycles_peak", 0)
print(json.dumps(dict(slot=slot, seconds=seconds, before_play_pct=round(100 * before / CYCLES_PER_BLOCK, 2),
                      peak_pct=round(100 * peak / CYCLES_PER_BLOCK, 2), avg_pct=round(100 * avg / CYCLES_PER_BLOCK, 2), p99_pct=round(100 * lasts[int(len(lasts) * 0.99)] / CYCLES_PER_BLOCK, 2),
                      overruns=report.get("audio_overruns"), app=report.get("app_status"),
                      crash=report.get("crash_reason"), blocks=report.get("audio_blocks"),
                      wall_s=round(time.time() - wall), log_tail=log[-12:])))
if wav_out:
    data, info = emu.audio_wav()
    if data:
        open(wav_out, "wb").write(data)
