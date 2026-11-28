#!/usr/bin/env python3
"""Compare an import's render with the original, and make an A/B file to listen to.

    python tools/midi_import/ab.py ORIGINAL.wav STRATA.wav --bpm 135 [--bars 8] [--ab OUT.wav]
                                   [--pairs 17,49] [--skip SECONDS]

Prints, for every section of --bars bars, the loudness of each in four bands (lows < 150 Hz,
low mids to 1 kHz, high mids to 5 kHz, highs) and the difference, so a track that is too loud or
a band that is missing shows where it is. --ab writes the two alternating section by section,
each matched to the original's overall loudness: original first, then STRATA, then the next
section of the original, and so on, with a short click marking every switch. --pairs 17,49
plays the sections starting at those bars twice each instead: original, then STRATA.

Both files are read through ffmpeg at 48 kHz. Needs numpy (as als_bounce.py).
"""
import argparse
import math
import subprocess
import sys

import numpy as np

RATE = 48000
BANDS = [(20, 150, 'low'), (150, 1000, 'lowmid'), (1000, 5000, 'himid'), (5000, 20000, 'high')]


def read(path):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 'f32le', '-ac', '2', '-ar', str(RATE), '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)


def band_db(x):
    """dB of each band's energy in a stretch, from its spectrum."""
    m = x.mean(axis=1)
    if len(m) < 2048:
        return [-120.0] * len(BANDS), -120.0
    spec = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    freqs = np.fft.rfftfreq(len(m), 1.0 / RATE)
    total = spec.sum() + 1e-20
    out = []
    for lo, hi, _ in BANDS:
        e = spec[(freqs >= lo) & (freqs < hi)].sum()
        out.append(10 * math.log10(e / len(m) + 1e-20))
    return out, 10 * math.log10(total / len(m))


def rms_db(x):
    return 20 * math.log10(float(np.sqrt(np.mean(x ** 2))) + 1e-9)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('original')
    ap.add_argument('strata')
    ap.add_argument('--bpm', type=float, required=True)
    ap.add_argument('--bars', type=int, default=8)
    ap.add_argument('--ab')
    ap.add_argument('--skip', type=float, default=0.0,
                    help='seconds to drop from the original start, when its export began before '
                    'the bar the import starts on (Live exports from the selection)')
    ap.add_argument('--pairs', help='sections by their first bar, e.g. 17,49,65: each played by '
                    'the original and then by STRATA (with --ab), instead of alternating the song')
    args = ap.parse_args()
    a, b = read(args.original), read(args.strata)
    a = a[int(round(args.skip * RATE)):]
    sec = int(round(args.bars * 4 * 60.0 / args.bpm * RATE))
    n = min(len(a), len(b)) // sec
    print(f"original {rms_db(a):6.1f} dB rms, STRATA {rms_db(b):6.1f} dB rms over the whole song")
    gain = 10 ** ((rms_db(a) - rms_db(b)) / 20)
    print(f"{'bars':>9}  " + '  '.join(f"{name:>17}" for _, _, name in BANDS) + "   (original / STRATA matched / diff)")
    for i in range(n):
        xa, xb = a[i * sec:(i + 1) * sec], b[i * sec:(i + 1) * sec] * gain
        ba, _ = band_db(xa)
        bb, _ = band_db(xb)
        cells = '  '.join(f"{p:5.0f} {q:5.0f} {q - p:+5.1f}" for p, q in zip(ba, bb))
        print(f"{i * args.bars + 1:4}-{(i + 1) * args.bars:<4}  {cells}")
    if args.ab:
        out = []
        click = np.zeros((480, 2), dtype=np.float32)
        click[:48] = 0.25
        if args.pairs:
            bar = sec // args.bars
            for first in (int(x) for x in args.pairs.split(',')):
                lo = (first - 1) * bar
                for src in (a, b * gain):
                    out.append(src[lo:lo + sec].copy())
                    out.append(click)
        else:
            for i in range(n):
                src = a if i % 2 == 0 else b * gain
                out.append(src[i * sec:(i + 1) * sec].copy())
                out.append(click)
        y = np.concatenate(out)
        peak = float(np.max(np.abs(y))) or 1.0
        if peak > 0.98:
            y *= 0.98 / peak
        pcm = (np.clip(y, -1, 1) * 32767).astype('<i2')
        import wave
        with wave.open(args.ab, 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(pcm.tobytes())
        how = 'each original, then STRATA' if args.pairs else 'original first, then STRATA, alternating'
        print(f"wrote {args.ab}: sections of {args.bars} bars, {how}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
