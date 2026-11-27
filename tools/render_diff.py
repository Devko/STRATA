#!/usr/bin/env python3
"""Compare two renders (tools/render.py --bits 32, or any 16/32-bit WAV) sample by sample.

    python3 -B tools/render_diff.py a.wav b.wav [c.wav d.wav ...]   pairs: a vs b, c vs d, ...

For each pair it prints whether they are identical and, if not, the SHAPE of the difference,
which is what tells a rounding difference from a broken build:

  first      where the first differing sample is (frame, seconds, channel) and by how much.
             Float reassociation starts at 1-2 LSB of a 16-bit word, or ~2^16 of a 32-bit one.
             A miscompile usually starts large, at the first note of the affected voice.
  windows    the worst difference in each 0.25 s window, as a fraction of full scale, beside
             each file's own peak in that window. A difference that stays tiny is rounding; one
             that jumps to the signal's own size means the two renders play different things.
  rms        each file's RMS over the whole render: a voice gone silent or blown up shows here.
  shifted    when they differ, whether b is a byte-for-byte copy of a moved by whole audio blocks
             (64 frames, up to 64 blocks either way). An emulator capture is the LAST seconds of
             a run, and the scripted keys reach the app through its main loop: a faster audio
             callback leaves that loop more time, so a press lands a few blocks earlier and the
             whole run, capture included, plays that much sooner. Such a pair is the same audio,
             not a different sound (docs/BUDGET.md, "The HALL in passes").
"""
import math
import struct
import sys
import wave


def load(path):
    with wave.open(path, "rb") as w:
        width, channels, rate = w.getsampwidth(), w.getnchannels(), w.getframerate()
        data = w.readframes(w.getnframes())
    fmt = {2: "h", 4: "i"}[width]
    return struct.unpack(f"<{len(data) // width}{fmt}", data), channels, rate, float(2 ** (8 * width - 1))


BLOCK = 64          # ENAGA_BLOCK_FRAMES
MOST_BLOCKS = 64


def block_shift(a, b, ch):
    """The offset in samples (a multiple of a block's frames x channels) at which b equals a
    exactly over their overlap, or None. Overlaps shorter than half the render do not count."""
    n = min(len(a), len(b))
    for k in range(1, MOST_BLOCKS + 1):
        for lag in (k * BLOCK * ch, -k * BLOCK * ch):
            if abs(lag) > n // 2:
                continue
            x, y = (a[lag:n], b[:n - lag]) if lag > 0 else (a[:n + lag], b[-lag:n])
            if x == y:
                return lag
    return None


def compare(pa, pb, label=None):
    a, ch, rate, full = load(pa)
    b, ch_b, _, _ = load(pb)
    print(label or f"{pa}\n  vs {pb}")
    if ch != ch_b:
        print(f"  channel counts differ ({ch} vs {ch_b})\n")
        return
    n = min(len(a), len(b))
    first, count, worst = None, 0, 0
    win = int(rate * 0.25) * ch
    rows = []
    for start in range(0, n, win):
        wd = wa = wb = 0
        for i in range(start, min(n, start + win)):
            x, y = a[i], b[i]
            wa, wb = max(wa, abs(x)), max(wb, abs(y))
            if x != y:
                d = abs(x - y)
                count += 1
                wd = max(wd, d)
                if first is None:
                    first = (i, d)
        worst = max(worst, wd)
        rows.append((start / ch / rate, wd / full, wa / full, wb / full))
    rms = [math.sqrt(sum(float(v) * v for v in s[:n]) / max(1, n)) / full for s in (a, b)]
    if count == 0 and len(a) == len(b):
        print(f"  identical ({n} samples)\n")
        return
    print(f"  DIFFERS: {count} of {n} samples, worst {worst / full:.3e} full scale"
          + ("" if len(a) == len(b) else f", lengths differ ({len(a)} vs {len(b)})"))
    if first:
        i, d = first
        print(f"  first: frame {i // ch} ({i // ch / rate:.4f} s), channel {i % ch}, by {d / full:.3e}"
              f" ({d} LSB)")
    print(f"  rms: {rms[0]:.4f} vs {rms[1]:.4f}")
    lag = block_shift(a, b, ch)
    if lag is not None:
        frames = abs(lag) // ch
        print(f"  shifted: b is a {frames} frames ({frames // BLOCK} blocks) {'later' if lag > 0 else 'earlier'}"
              f" in the music, identical over {n - abs(lag)} samples: the same audio, a different start")
    print("  window   worst diff   peak a   peak b")
    for t, d, pa_, pb_ in rows:
        print(f"  {t:6.2f}s  {d:10.3e}   {pa_:6.3f}   {pb_:6.3f}")
    print()


def main(argv):
    if len(argv) < 2 or len(argv) % 2:
        sys.exit(__doc__)
    for i in range(0, len(argv), 2):
        compare(argv[i], argv[i + 1])


if __name__ == "__main__":
    main(sys.argv[1:])
