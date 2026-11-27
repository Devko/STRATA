#!/usr/bin/env python3
"""Measure the sound probe's renders (host/probe.cpp; `make -C host probe`).

    python3 tools/sound_probe.py build/host/probe [--filter vamono_saw]

Needs numpy. For every render in manifest.tsv it prints what its kind is about:

  tone   tuning (cents off equal temperament, A = 440 Hz), the loudest component that is NOT a
         harmonic of the note, in dB below the loudest harmonic ("alias"), and the same limited
         to below 5 kHz, where an alias is easiest to hear ("alias<5k"). A clean, band-limited
         oscillator reads below about -80; a naive one -20 to -40 on a high note.
  reso   the frequency the filter rings at, against the cutoff CUT asks for.
  click  the largest jump from one sample to the next, against the largest the steady note
         makes: a click reads as a jump several times the steady one.
  dc     the mean of the note, and what is left once it has gone (DC and denormals).
  drum   peak, DC, and the tail.
"""
import argparse
import math
import struct
import sys
import wave
from pathlib import Path

import numpy as np

RATE = 48000


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    # The probe writes a 44-byte header and 32-bit float samples.
    return np.frombuffer(data[44:], dtype="<f4").astype(np.float64)


def note_hz(note):
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


def bh4(n):
    k = np.arange(n) / (n - 1)
    return (0.35875 - 0.48829 * np.cos(2 * np.pi * k) + 0.14128 * np.cos(4 * np.pi * k)
            - 0.01168 * np.cos(6 * np.pi * k))


def spectrum(x):
    w = bh4(len(x))
    s = np.abs(np.fft.rfft(x * w))
    f = np.fft.rfftfreq(len(x), 1.0 / RATE)
    return f, s


def peak_freq(f, s, lo, hi):
    """The strongest bin between lo and hi Hz, refined by a parabola through its neighbours."""
    i0 = np.searchsorted(f, lo)
    i1 = np.searchsorted(f, hi)
    i = i0 + int(np.argmax(s[i0:i1]))
    a, b, c = np.log(s[i - 1] + 1e-30), np.log(s[i] + 1e-30), np.log(s[i + 1] + 1e-30)
    d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return (i + d) * (f[1] - f[0]), s[i]


def tone(x, note):
    seg = x[int(0.25 * RATE): int(0.25 * RATE) + 32768] if len(x) > 0.25 * RATE + 32768 else x[int(0.2 * RATE):]
    f, s = spectrum(seg)
    want = note_hz(note)
    got, _ = peak_freq(f, s, want * 0.9, want * 1.1)
    cents = 1200 * math.log2(got / want)
    df = f[1] - f[0]
    harm = np.zeros(len(s), bool)
    k = 1
    while k * got < RATE / 2:
        c = k * got
        harm |= np.abs(f - c) <= 6 * df
        k += 1
    harm |= f < 20.0
    top = s[harm].max()
    inh = np.where(harm, 0.0, s)
    alias = 20 * np.log10(inh.max() / top + 1e-12)
    low = f < 5000.0
    alias5 = 20 * np.log10(inh[low].max() / top + 1e-12)
    worst_f = f[int(np.argmax(inh))]
    rms = 20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-12)
    # Brightness: the spectral centroid of the harmonics alone, in multiples of the note.
    p2 = np.where(harm & (f > 20.0), s, 0.0) ** 2
    bright = (f * p2).sum() / (p2.sum() + 1e-30) / got
    return f"bright {bright:5.1f}x  f0 {got:8.2f} Hz {cents:+6.2f} c  alias {alias:6.1f} dB (at {worst_f:6.0f} Hz)  alias<5k {alias5:6.1f}  rms {rms:5.1f} dBFS"


def cut_hz(cut):
    return 35.0 * 2.0 ** (8.4214 * cut / 127.0)


def reso(x, note, what):
    cut = int(what.split()[0].split("=")[1])
    seg = x[int(0.5 * RATE): int(0.5 * RATE) + 32768]
    f, s = spectrum(seg)
    want = cut_hz(cut)
    got, mag = peak_freq(f, s, 30, 20000)
    ratio = 1200 * math.log2(got / want)
    pk = 20 * np.log10(np.abs(seg).max() + 1e-12)
    return f"cutoff {want:7.0f} Hz  rings at {got:7.0f} Hz ({ratio:+6.0f} c)  peak {pk:5.1f} dBFS"


def click(x):
    d = np.abs(np.diff(x))
    pk = np.abs(x).max()
    i = int(np.argmax(d))
    # The steady jump: the 99.9th percentile of the jumps while sounding.
    steady = np.percentile(d[np.abs(x[1:]) > pk * 0.05], 99.9)
    return f"largest jump {d.max():.4f} at {i / RATE:6.3f} s = {d.max() / steady:4.1f}x the 99.9th pct  peak {20*np.log10(pk+1e-12):5.1f} dBFS"


def dc(x):
    held = x[int(0.2 * RATE): int(0.9 * RATE)]
    tail = x[int(2.0 * RATE):]
    nz = np.abs(tail[tail != 0])
    small = nz.min() if len(nz) else 0.0
    return (f"mean while held {held.mean():+.5f} ({20*np.log10(abs(held.mean())/np.sqrt(np.mean(held**2))+1e-12):5.1f} dB of rms)"
            f"  tail max {np.abs(tail).max():.2e}  smallest tail value {small:.1e}"
            + ("  DENORMAL" if 0 < small < 1.2e-38 else ""))


def drum(x):
    pk = np.abs(x).max()
    tail = x[-4800:]
    return f"peak {20*np.log10(pk+1e-12):5.1f} dBFS  mean {x.mean():+.5f}  tail max {np.abs(tail).max():.1e}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--filter", default="")
    a = ap.parse_args()
    d = Path(a.dir)
    for line in (d / "manifest.tsv").read_text().splitlines():
        name, kind, note, what = line.split("\t")
        if a.filter not in name:
            continue
        x = read(d / f"{name}.wav")
        note = int(note)
        if kind == "tone":
            r = tone(x, note)
        elif kind == "reso":
            r = reso(x, note, what)
        elif kind == "click":
            r = click(x)
        elif kind == "dc":
            r = dc(x)
        elif kind == "riff":
            r = f"peak {20*np.log10(np.abs(x).max()+1e-12):5.1f} dBFS (for listening)"
        else:
            r = drum(x)
        print(f"{name:22s} {what:26s} {r}")


if __name__ == "__main__":
    main()
