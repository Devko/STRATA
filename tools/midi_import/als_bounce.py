#!/usr/bin/env python3
"""A dry reference bounce of an Ableton set's arrangement, for an A/B against its STRATA import.

Places every audio clip (and every frozen track's freeze file) on the timeline as Live's warp
markers say, plays each Simpler track's sample on its notes, and sums them at each track's
fader, pan and group faders. No effects, sends, automation or plug-ins: a frozen track brings its
own (the freeze file is the track's sound before its fader), any other track is heard dry. So it
is close to the set for frozen and audio tracks and drier than it for the rest. When Live itself
is at hand, its own export is the better reference.

Also prints, per track, how loud it is in each bar: the map used to cut a set into STRATA
patterns and a chain.

Needs numpy and ffmpeg (on PATH), unlike the rest of tools/midi_import.

    python tools/midi_import/als_bounce.py SET.als OUT.wav [--stems DIR] [--bars A-B] [--map]
"""
import argparse
import math
import os
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import als_notes  # noqa: E402

RATE = 48000
_cache = {}


def decode(path):
    """The file as float32 stereo at RATE, shape (n, 2). Run from the file's folder so a
    path longer than Windows' 260 characters still opens."""
    if path in _cache:
        return _cache[path]
    folder, name = os.path.split(path)
    raw = subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', 'file:' + name, '-f', 'f32le', '-ac', '2', '-ar', str(RATE), '-'],
        cwd=folder or None, capture_output=True, check=True).stdout
    data = np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)
    _cache[path] = data
    return data


def beat_to_sec(markers, beat):
    """Live's warp: seconds in the file for a beat in the clip, piecewise linear through the
    markers and extended past both ends with the nearest segment's slope."""
    b = np.array([m[0] for m in markers])
    s = np.array([m[1] for m in markers])
    if len(b) < 2:
        return s[0] + (beat - b[0]) * 0.5 if len(b) else beat * 0.5
    i = np.clip(np.searchsorted(b, beat, side='right') - 1, 0, len(b) - 2)
    slope = (s[i + 1] - s[i]) / (b[i + 1] - b[i])
    return s[i] + (beat - b[i]) * slope


def place_audio(out, clip, tempo, t0_beats):
    if clip['disabled'] or not clip['file'] or not os.path.exists(clip['file']):
        return
    src = decode(clip['file'])
    spb = 60.0 / tempo
    start = int(round((clip['start'] - t0_beats) * spb * RATE))
    end = int(round((clip['end'] - t0_beats) * spb * RATE))
    lo, hi = max(start, 0), min(end, len(out))
    if hi <= lo:
        return
    tl_beats = (np.arange(lo, hi) / RATE) / spb + t0_beats - clip['start']   # beats into the clip
    rate = 2.0 ** (clip['pitch'] / 12.0)
    if clip['warped']:
        content = clip['loop_start'] + clip['start_relative'] + tl_beats
        if clip['loop_on']:
            span = clip['loop_end'] - clip['loop_start']
            first = clip['loop_end'] - (clip['loop_start'] + clip['start_relative'])
            content = np.where(tl_beats < first, content,
                               clip['loop_start'] + np.mod(tl_beats - first, span))
        secs = beat_to_sec(clip['warp_markers'], content)
    else:
        # An unwarped clip's markers are seconds; it plays at its own speed.
        secs = clip['loop_start'] + clip['start_relative'] + tl_beats * spb * rate
        if clip['loop_on']:
            span = clip['loop_end'] - clip['loop_start']
            secs = clip['loop_start'] + np.mod(secs - clip['loop_start'], span)
    pos = secs * RATE
    i = np.floor(pos).astype(np.int64)
    frac = (pos - i).astype(np.float32)[:, None]
    ok = (i >= 0) & (i + 1 < len(src))
    i = np.clip(i, 0, len(src) - 2)
    seg = src[i] * (1 - frac) + src[i + 1] * frac
    seg[~ok] = 0
    # 2 ms fades at the clip's edges, as Live's default clip fades.
    n = len(seg)
    f = min(96, n // 2)
    if f > 0:
        ramp = np.linspace(0, 1, f, dtype=np.float32)[:, None]
        if lo == start:
            seg[:f] *= ramp
        if hi == end:
            seg[-f:] *= ramp[::-1]
    out[lo:hi] += seg * clip['gain']


def place_simpler(out, track, tempo, t0_beats):
    s = track['simpler']
    if not s or not s['file'] or not os.path.exists(s['file']):
        return
    src = decode(s['file'])
    a = int(s['sample_start'])
    b = int(s['sample_end']) if s['sample_end'] > s['sample_start'] else len(src)
    src = src[a:b] * s['volume']
    spb = 60.0 / tempo
    for n in track['notes']:
        semis = n['note'] - s['root_key'] + s['transpose'] + s['detune'] / 100.0
        rate = 2.0 ** (semis / 12.0)
        length = len(src) / rate
        if s['playback_mode'] == 0:    # classic: the note's length, with a short release
            length = min(length, n['dur_beats'] * spb * RATE + 0.01 * RATE)
        start = int(round((n['start_beats'] - t0_beats) * spb * RATE))
        m = int(length)
        if m <= 0 or start >= len(out) or start + m <= 0:
            continue
        idx = np.arange(m) * rate
        i = np.minimum(idx.astype(np.int64), len(src) - 2)
        frac = (idx - i).astype(np.float32)[:, None]
        seg = (src[i] * (1 - frac) + src[i + 1] * frac) * (n['vel'] / 127.0)
        fo = min(240, m // 2)
        seg[-fo:] *= np.linspace(1, 0, fo, dtype=np.float32)[:, None]
        lo = max(start, 0)
        hi = min(start + m, len(out))
        out[lo:hi] += seg[lo - start:hi - start]


def pan_gains(pan):
    """Live's pan: -1 left .. 1 right, unity at the centre."""
    return (min(1.0, 1.0 - pan), min(1.0, 1.0 + pan))


def render_track(data, t, t0, frames, use_fader=True):
    """One track as float32 stereo (frames, 2) from beat t0: its clips or its Simpler notes,
    at its fader, pan and group faders (or unity with use_fader False). None when it is silent
    or has nothing this bounce can play (a plug-in that was not frozen)."""
    tempo = data['tempo']
    if t['kind'] not in ('midi', 'audio'):
        return None
    gain = als_notes.group_gain(data, t) if use_fader else 1.0
    if gain <= 0:
        return None
    buf = np.zeros((frames, 2), dtype=np.float32)
    if t['audio'] and (t['kind'] == 'audio' or t['frozen']):
        for a in t['audio']:
            place_audio(buf, a, tempo, t0)
    elif t['simpler']:
        place_simpler(buf, t, tempo, t0)
    else:
        return None
    gl, gr = pan_gains(t['pan']) if use_fader else (1.0, 1.0)
    if use_fader:
        env = automated_gain(data, t, t0, frames)
        if env is not None:
            buf *= env[:, None].astype(np.float32)
            gain = 1.0
    buf[:, 0] *= gain * gl
    buf[:, 1] *= gain * gr
    return buf if buf.any() else None


def automated_gain(data, t, t0, frames):
    """The track's gain over time, its groups' included, when any of them automates a fader or
    a Utility; None when nothing does (then the static fader stands)."""
    by_id = {x['id']: x for x in data['tracks']}
    chain = [t]
    g = t['group']
    while g != -1 and g in by_id:
        chain.append(by_id[g])
        g = by_id[g]['group']
    if not any(x.get('gain_envelopes') for x in chain):
        return None
    step = 64
    beats = t0 + (np.arange(0, frames + step, step) / RATE) * data['tempo'] / 60.0
    gain = np.ones(len(beats))
    for x in chain:
        if not x['on']:
            return np.zeros(frames)
        envs = x.get('gain_envelopes', [])
        if x is t and t['frozen']:
            # The freeze file already went through the track's own devices.
            envs = [e for e in envs if e['kind'] == 'fader']
        gain *= als_notes.gain_at(envs, beats, x['volume'])
    return np.interp(np.arange(frames), np.arange(len(beats)) * step, gain)


def song_beats(data):
    """The beat after the arrangement's last clip or note."""
    last = 0.0
    for t in data['tracks']:
        for a in t['audio']:
            last = max(last, a['end'])
        for n in t['notes']:
            last = max(last, n['start_beats'] + n['dur_beats'])
    return last


def read_wav(path):
    with wave.open(path, 'rb') as w:
        raw = w.readframes(w.getnframes())
        ch = w.getnchannels()
    x = np.frombuffer(raw, dtype='<i2').astype(np.float32) / 32768.0
    return x.reshape(-1, ch)


def write_wav(path, data):
    pcm = (np.clip(data, -1, 1) * 32767).astype('<i2')
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('als')
    ap.add_argument('out')
    ap.add_argument('--stems', help='folder for one wav per track')
    ap.add_argument('--bars', help='A-B, bars from 1 (default: the whole arrangement)')
    ap.add_argument('--map', action='store_true', help='print each track\'s level per bar')
    args = ap.parse_args()

    data = als_notes.read_set(args.als)
    tempo = data['tempo']
    spb = 60.0 / tempo
    last = song_beats(data)
    bar_beats = 4 * data['time_signature'][0] / data['time_signature'][1]
    first_bar, last_bar = 1, int(math.ceil(last / bar_beats))
    if args.bars:
        first_bar, last_bar = (int(x) for x in args.bars.split('-'))
    t0 = (first_bar - 1) * bar_beats
    frames = int(round((last_bar - first_bar + 1) * bar_beats * spb * RATE))
    mix = np.zeros((frames, 2), dtype=np.float32)
    bar_frames = bar_beats * spb * RATE
    if args.stems:
        os.makedirs(args.stems, exist_ok=True)
    levels = []
    for t in data['tracks']:
        buf = render_track(data, t, t0, frames)
        if buf is None:
            continue
        mix += buf
        if args.stems:
            safe = ''.join(c if c.isalnum() or c in ' -_' else '_' for c in t['name'])[:40]
            write_wav(os.path.join(args.stems, f"{t['id']:03d} {safe}.wav"), buf)
        if args.map:
            row = []
            for b in range(last_bar - first_bar + 1):
                seg = buf[int(b * bar_frames):int((b + 1) * bar_frames)]
                rms = float(np.sqrt(np.mean(seg ** 2))) if len(seg) else 0.0
                row.append(rms)
            levels.append((t['name'], row))
    peak = float(np.max(np.abs(mix))) or 1.0
    write_wav(args.out, mix * (0.89 / peak) if peak > 0.89 else mix)
    print(f'{args.out}: bars {first_bar}-{last_bar}, {frames / RATE:.1f} s, '
          f'peak {20 * math.log10(peak):.1f} dBFS before normalising')
    if args.map:
        # One character a bar: ' ' silent, then . : - = # by level.
        marks = ' .:-=#'
        print(f"{'':28} " + ''.join(str((first_bar + b) // 10 % 10) if (first_bar + b) % 10 == 0 else ' '
                                     for b in range(last_bar - first_bar + 1)))
        for name, row in levels:
            line = ''
            for r in row:
                d = 20 * math.log10(r) if r > 1e-6 else -120
                line += marks[0 if d < -60 else 1 if d < -40 else 2 if d < -30 else 3 if d < -22 else 4 if d < -16 else 5]
            print(f'{name[:28]:28} {line}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
