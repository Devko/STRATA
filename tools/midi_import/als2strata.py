#!/usr/bin/env python3
"""An Ableton set's arrangement as a STRATA project: samples for the card and a generator table.

A map (JSON) says which of the set's tracks become each of STRATA's eight, and how:

  mono    the notes of MIDI tracks on a mono synth (VA MONO, WAVE MONO): one note a step
  poly    the notes as chords on VA POLY, WAVE POLY or FM4 (grid.poly)
  hits    one-shots on a SAMPLE track: every distinct hit of the source tracks (a Simpler's
          notes, an audio clip, or the hits found in a long consolidated clip) becomes a slice
          of one WAV, and each hit a trig on its slice (note 60 + slice)
  blocks  audio loops on a SAMPLE track: the source tracks mixed and cut into blocks of
          block_bars; every distinct block becomes a slice, and each block a trig at its start

Then the song is cut into 64-step patterns, identical ones kept once, and the order becomes the
chain. Out come the slices' WAVs (for /SAMPLES/<PROJECT>/ on the card) and project.inc, which
import_project.cpp turns into the project files and renders (build_import.sh does both).

    python tools/midi_import/als2strata.py MAP.json OUTDIR

MAP.json (paths may be absolute; "from" names are the set's track names):

    {"set": "D:/.../demo1.als", "project": "DEMO1", "bars": [1, 132],
     "root": 10, "scale": "minor", "swing": 50,
     "tracks": [
       {"name": "KICK", "mode": "hits", "from": ["Kick"], "max_seconds": 0.6},
       {"name": "BASS", "mode": "mono", "from": ["Bassline (Serum)", "Reese Bass"],
        "machine": "va_mono", "preset": "FAT", "transpose": -12, "velocity": [90, 110],
        "params": {"8": 64}, "level": 1.0, "pan": 64, "duck": 60},
       {"name": "LOOPS", "mode": "blocks", "from": ["Open Hat 1", "Clap"], "block_bars": 4},
       ...]}

Optional per track: "params" (slot: value, after the preset), "level" (fader), "pan",
"duck" (percent, under track 1), "sna"/"snb" (sends), "max_length_steps", "tolerance" (dB, how
alike two hits or blocks must be to share a slice; 3 by default).

Needs numpy and ffmpeg for hits and blocks (as als_bounce.py). The set, its samples and
everything written here are the set's owner's: keep OUTDIR under build/, out of git.
"""
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import als_bounce  # noqa: E402
import als_notes  # noqa: E402
import grid  # noqa: E402

RATE = als_bounce.RATE
STEPS_PER_BAR = 16
PATTERN_STEPS = 64
SLICES_MAX = 16
SCALES = {'chromatic': 0, 'minor': 1, 'major': 2, 'dorian': 3, 'phrygian': 4}   # core/scale.hpp Scale ids used here
MACHINES = {'kick': 0, 'snare': 1, 'hat': 2, 'perc': 3, 'sample': 4, 'grain': 5, 'va_mono': 6,
            'va_poly': 7, 'wave_mono': 9, 'wave_poly': 10, 'fm': 11}


# --- sound comparison -------------------------------------------------------

def features(x):
    """Two 10 ms energy envelopes in dB (the signal, and its first difference for the highs),
    floored at -60. Alike to within a few dB means the same sound even when a sub-sample shift
    or a warp makes the waveforms differ."""
    m = x.mean(axis=1)
    h = np.diff(m, prepend=0.0)
    n = max(len(m) // 480 * 480, 480)
    m = np.pad(m, (0, max(0, n - len(m))))[:n]
    h = np.pad(h, (0, max(0, n - len(h))))[:n]
    e1 = 10 * np.log10(np.mean(m.reshape(-1, 480) ** 2, axis=1) + 1e-9)
    e2 = 10 * np.log10(np.mean(h.reshape(-1, 480) ** 2, axis=1) + 1e-9)
    return np.maximum(np.concatenate([e1, e2]), -60.0)


def distance(fa, fb):
    n = min(len(fa), len(fb))
    return float(np.mean(np.abs(fa[:n] - fb[:n]))) if n else 99.0


def cluster(sounds, tolerance, limit=SLICES_MAX):
    """Group alike sounds. Returns (representative indices, the group of each sound). Widens the
    tolerance until at most `limit` groups remain."""
    feats = [features(s) for s in sounds]
    tol = tolerance
    while True:
        reps, group = [], []
        for i, f in enumerate(feats):
            best, best_d = None, tol
            for g, r in enumerate(reps):
                d = distance(f, feats[r])
                if d < best_d:
                    best, best_d = g, d
            if best is None:
                reps.append(i)
                best = len(reps) - 1
            group.append(best)
        if len(reps) <= limit:
            return reps, group, tol
        tol *= 1.25


def level(x):
    return float(np.sqrt(np.mean(x ** 2))) if len(x) else 0.0


# --- sources ----------------------------------------------------------------

def _source_list(spec):
    out = []
    for s in spec['from']:
        out.append(s if isinstance(s, dict) else {'track': s})
    return out


def _track(data, name):
    for t in data['tracks']:
        if t['name'] == name and t['kind'] in ('midi', 'audio'):
            return t
    raise SystemExit(f"no track named {name!r} in the set")


def audible_bars(data, t, t0, frames, floor_db=-50.0):
    """For a frozen track, the bars (from t0) where its freeze file is heard: the notes of a
    part its automation filtered or faded out are not worth a trig. None for any other track."""
    if not (t['frozen'] and t['audio']) or als_notes.group_gain(data, t) <= 0:
        return None     # nothing to go by: a track switched off may be heard through another
    stem = als_bounce.render_track(data, t, t0, frames)
    if stem is None:
        return set()
    bar = int(round(4 * 60.0 / data['tempo'] * RATE))
    peak = float(np.max(np.abs(stem))) or 1.0
    out = set()
    for b in range(len(stem) // bar + 1):
        x = stem[b * bar:(b + 1) * bar]
        if len(x) and 20 * math.log10(level(x) / peak + 1e-12) > floor_db:
            out.add(b)
    return out


def notes_of(data, spec, t0, t1, frames):
    notes = []
    for src in _source_list(spec):
        t = _track(data, src['track'])
        tr = int(src.get('transpose', 0)) + int(spec.get('transpose', 0))
        heard = audible_bars(data, t, t0, frames) if spec.get('audible_only', True) else None
        for n in t['notes']:
            if not t0 <= n['start_beats'] < t1:
                continue
            if heard is not None and int((n['start_beats'] - t0) // 4) not in heard:
                continue
            notes.append(dict(n, note=min(127, max(0, n['note'] + tr))))
    return notes


def locks_for(spec, first_bar):
    """The map's "locks" as (step, slot, value) for every step of the song they cover: a value,
    or a ramp from "from" to "to" over the bars. Applied to the trigs in those steps."""
    out = {}
    for lk in spec.get('locks', []):
        a, b = lk['bars']
        s0, s1 = (a - first_bar) * STEPS_PER_BAR, (b - first_bar + 1) * STEPS_PER_BAR
        for s in range(max(0, s0), s1):
            if 'value' in lk:
                v = lk['value']
            else:
                v = lk['from'] + (lk['to'] - lk['from']) * (s - s0) / max(1, s1 - s0 - 1)
            out[(s, int(lk['slot']))] = max(0, min(127, int(round(v))))
    return out


def hit_events(data, spec, t0, t1, frames):
    """(beat, sound, velocity scale) for every hit of the sources between beats t0 and t1."""
    spb = 60.0 / data['tempo']
    max_frames = int(spec.get('max_seconds', 2.0) * RATE)
    events = []
    for src in _source_list(spec):
        t = _track(data, src['track'])
        gain = als_notes.group_gain(data, t)
        if gain <= 0:
            continue
        gl, gr = als_bounce.pan_gains(t['pan'])
        if t['simpler'] and t['notes'] and not t['frozen']:
            cache = {}
            # Automated faders and Utilities (the track's and its groups') scale each hit.
            env = als_bounce.automated_gain(data, t, t0, frames)
            static = als_notes.group_gain(data, t)
            for n in t['notes']:
                if not t0 <= n['start_beats'] < t1:
                    continue
                auto = 1.0
                if env is not None:
                    i = min(len(env) - 1, int((n['start_beats'] - t0) * spb * RATE))
                    auto = float(env[i]) / static if static > 0 else 0.0
                if auto < 1e-3:
                    continue
                key = n['note']
                if key not in cache:
                    one = dict(t, notes=[{'start_beats': 0.0, 'dur_beats': 64.0, 'note': key, 'vel': 127}])
                    buf = np.zeros((max_frames, 2), dtype=np.float32)
                    als_bounce.place_simpler(buf, one, data['tempo'], 0.0)
                    buf[:, 0] *= gain * gl
                    buf[:, 1] *= gain * gr
                    cache[key] = _trim(buf)
                events.append((n['start_beats'], cache[key], n['vel'] / 127.0 * auto))
            continue
        # Clips no longer than two hits are hits; anything longer (a loop, a consolidated
        # track) is searched for the hits inside it.
        long_clip = any((a['end'] - a['start']) * spb * RATE > 2 * max_frames for a in t['audio'])
        if not long_clip:
            for a in t['audio']:
                if a['disabled'] or not t0 <= a['start'] < t1:
                    continue
                n = min(max_frames, int((a['end'] - a['start']) * spb * RATE))
                buf = np.zeros((n, 2), dtype=np.float32)
                als_bounce.place_audio(buf, a, data['tempo'], a['start'])
                buf[:, 0] *= gain * gl
                buf[:, 1] *= gain * gr
                if buf.any():
                    events.append((a['start'], _trim(buf), 1.0))
            continue
        stem = als_bounce.render_track(data, t, t0, frames)
        if stem is None:
            continue
        for start, end in onsets(stem, max_frames, int(spb / 4 * RATE * 0.9)):
            beat = t0 + start / RATE / spb
            events.append((beat, _trim(stem[start:end]), 1.0))
    return events


def _trim(x, floor=1e-4):
    """Cut trailing silence."""
    loud = np.nonzero(np.max(np.abs(x), axis=1) > floor)[0]
    return x[:loud[-1] + 1] if len(loud) else x[:0]


def onsets(stem, max_frames, min_gap):
    """(start, end) frames of the hits in a stem: a rise of 15 dB over the last 50 ms to above
    -50 dBFS starts one, at least min_gap frames (about a 16th) after the last, so a kick's body
    after its click is not a hit of its own; it ends at the next, at max_frames or in silence."""
    hop = 240
    m = np.max(np.abs(stem), axis=1)
    n = len(m) // hop
    env = 20 * np.log10(m[:n * hop].reshape(n, hop).max(axis=1) + 1e-9)
    starts = []
    for i in range(n):
        floor = env[max(0, i - 10):i].min() if i else -120.0    # silence before the stem
        if env[i] > -50 and env[i] - floor > 15 and (not starts or (i - starts[-1]) * hop >= min_gap):
            starts.append(i)
    out = []
    for k, i in enumerate(starts):
        s = i * hop
        nxt = starts[k + 1] * hop if k + 1 < len(starts) else len(stem)
        out.append((s, min(nxt, s + max_frames)))
    return out


# --- tracks to steps --------------------------------------------------------

def steps_for(data, spec, t0, t1, frames, outdir, project):
    """The track's StepNotes on the song's timeline, and, for a SAMPLE track, its WAV."""
    mode = spec['mode']
    info = {'slices': 0, 'wav': None, 'gain': 1.0}
    if mode in ('mono', 'poly'):
        notes = grid.as_notes(notes_of(data, spec, t0, t1, frames))
        steps = grid.quantize(notes, offset_beats=t0, max_length_steps=spec.get('max_length_steps'))
        steps = grid.mono(steps, spec.get('pick', 'high')) if mode == 'mono' else grid.poly(steps)
        if 'velocity' in spec:
            lo, hi = spec['velocity']
            steps = [grid.replace(s, vel=int(round(lo + (hi - lo) * (s.vel - 1) / 126))) for s in steps]
        return steps, info
    if mode == 'hits':
        events = hit_events(data, spec, t0, t1, frames)
        if not events:
            return [], info
        sounds = [e[1] for e in events]
        reps, group, tol = cluster(sounds, spec.get('tolerance', 3.0))
        slices = [sounds[r] for r in reps]
        steps = []
        notes = []
        for (beat, snd, vscale), g in zip(events, group):
            rel = level(snd) / max(level(slices[g]), 1e-9)
            vel = max(1, min(127, int(round(127 * vscale * min(rel, 1.0)))))
            notes.append({'start_beats': beat, 'dur_beats': 0.25, 'note': 60 + g, 'vel': vel, 'channel': 0})
        steps = grid.quantize(grid.as_notes(notes), offset_beats=t0)
        # Two hits on one step: keep the louder.
        best = {}
        for s in steps:
            if s.step not in best or s.vel > best[s.step].vel:
                best[s.step] = s
        steps = [best[k] for k in sorted(best)]
        steps = [grid.replace(s, length=8) for s in steps]
        info.update(write_slices(slices, outdir, project, spec['name'], spec.get('max_seconds', 2.0)))
        info['tolerance'] = tol
        return steps, info
    if mode == 'blocks':
        spb = 60.0 / data['tempo']
        bars = int(spec.get('block_bars', 4))
        mix = np.zeros((frames, 2), dtype=np.float32)
        for src in _source_list(spec):
            buf = als_bounce.render_track(data, _track(data, src['track']), t0, frames)
            if buf is not None:
                mix += buf
        block_frames = int(round(bars * 4 * spb * RATE))
        blocks, at = [], []
        for b in range(int(math.ceil(frames / block_frames))):
            x = mix[b * block_frames:(b + 1) * block_frames]
            if level(x) < 1e-3:
                continue
            x = np.pad(x, ((0, block_frames - len(x)), (0, 0)))
            blocks.append(x)
            at.append(b * bars * STEPS_PER_BAR)
        if not blocks:
            return [], info
        reps, group, tol = cluster(blocks, spec.get('tolerance', 3.0))
        steps = [grid.StepNote(step=s, note=60 + g, vel=100, length=8) for s, g in zip(at, group)]
        info.update(write_slices([blocks[r] for r in reps], outdir, project, spec['name'], None))
        info['tolerance'] = tol
        return steps, info
    raise SystemExit(f"unknown mode {mode!r}")


def write_slices(slices, outdir, project, name, max_seconds):
    """One WAV of equal slices, normalised to -1 dBFS; returns its card path and the gain the
    fader has to give back to keep the set's balance."""
    n = max(len(s) for s in slices)
    if max_seconds:
        n = min(n, int(max_seconds * RATE))
    data = np.zeros((n * len(slices), 2), dtype=np.float32)
    fade = min(240, n // 4)
    for i, s in enumerate(slices):
        s = s[:n].copy()
        if len(s) == n and fade:
            s[-fade:] *= np.linspace(1, 0, fade, dtype=np.float32)[:, None]
        data[i * n:i * n + len(s)] = s
    peak = float(np.max(np.abs(data))) or 1.0
    norm = 0.89 / peak
    leaf = f"{name[:8].upper()}.WAV"
    path = os.path.join(outdir, 'samples', leaf)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    als_bounce.write_wav(path, data * norm)
    return {'slices': len(slices), 'wav': path, 'card': f"/SAMPLES/{project}/{leaf}",
            'gain': 1.0 / norm, 'slice_seconds': n / RATE}


# --- patterns and the chain -------------------------------------------------

def cut_patterns(tracks_steps, tracks_locks, total_steps):
    """Dedupe the song's 64-step windows. Returns (the window start of each unique pattern,
    the chain as (pattern, repeats))."""
    windows = []
    for w in range(0, total_steps, PATTERN_STEPS):
        sig = []
        for steps, locks in zip(tracks_steps, tracks_locks):
            sig.append(tuple((s.step - w, s.note, s.vel, s.length, s.micro, s.chord)
                             for s in steps if w <= s.step < w + PATTERN_STEPS))
            sig.append(tuple((st - w, slot, v) for st, slot, v in locks if w <= st < w + PATTERN_STEPS))
        windows.append(tuple(sig))
    unique, starts, order = {}, [], []
    for i, sig in enumerate(windows):
        if sig not in unique:
            unique[sig] = len(starts)
            starts.append(i * PATTERN_STEPS)
        order.append(unique[sig])
    chain = []
    for p in order:
        if chain and chain[-1][0] == p and chain[-1][1] < 99:
            chain[-1][1] += 1
        else:
            chain.append([p, 1])
    return starts, chain


def cpp_str(s):
    return '"' + s.replace('\\', '/').replace('"', '\\"') + '"'


def emit(outdir, cfg, data, specs, results, starts, chain):
    lines = [f"// Generated by tools/midi_import/als2strata.py from {os.path.basename(cfg['set'])}.",
             "// The set's own material: do not commit.", "#pragma once", "namespace {", "",
             "struct ImportLock { int step, slot, value; };",
             "template <size_t N>",
             "void place_locks(Pattern &p, int track, const ImportLock (&rows)[N], int from) {",
             "    for (const ImportLock &r : rows)",
             "        if (r.step >= from && r.step < from + 64)",
             "            pattern_set_lock(p, track, r.step - from, uint8_t(r.slot), uint8_t(r.value));",
             "}", ""]
    tempo = data['tempo']
    lines += [f"constexpr const char *kName = {cpp_str(cfg['project'])};",
              f"constexpr uint16_t kTempoCenti = {int(round(tempo * 100))};",
              f"constexpr uint8_t kRoot = {int(cfg.get('root', 0))};",
              f"constexpr Scale kScale = Scale({SCALES[cfg.get('scale', 'chromatic')]});",
              f"constexpr uint8_t kSwing = {int(cfg.get('swing', 50))};",
              f"constexpr int kPatterns = {len(starts)};",
              f"constexpr int kArrangementBars = {sum(r for _, r in chain) * PATTERN_STEPS // STEPS_PER_BAR};",
              ""]
    for spec, (steps, info) in zip(specs, results):
        lines.append(grid.emit_cpp(spec['name'].lower(), steps, f"{spec['mode']} from {', '.join(s['track'] for s in _source_list(spec))}"))
        rows = info.get('locks') or [(-1, 0, 0)]
        lines.append(f"constexpr ImportLock kLocks_{spec['name'].lower()}[] = {{" +
                     ", ".join(f"{{{a}, {b}, {c}}}" for a, b, c in rows) + "};")
    lines.append("struct ImportTrack {\n    const char *name; int machine; const char *preset; const char *card;\n"
                 "    const char *wav; int slices; float level; int pan; int duck; int sna; int snb;\n};")
    lines.append("const ImportTrack kImportTracks[kTracks] = {")
    for i in range(8):
        if i < len(specs):
            spec, (steps, info) = specs[i], results[i]
            machine = 'sample' if spec['mode'] in ('hits', 'blocks') else spec.get('machine', 'va_mono')
            lvl = float(spec.get('level', 1.0)) * (info['gain'] if info['wav'] else 1.0)
            lines.append(f"    {{{cpp_str(spec['name'][:8].upper())}, {MACHINES[machine]}, "
                         f"{cpp_str(spec['preset']) if spec.get('preset') else 'nullptr'}, "
                         f"{cpp_str(info['card']) if info['wav'] else 'nullptr'}, "
                         f"{cpp_str(os.path.abspath(info['wav'])) if info['wav'] else 'nullptr'}, "
                         f"{info['slices']}, {lvl:.4f}f, {int(spec.get('pan', 64))}, "
                         f"{int(spec.get('duck', 0))}, {int(spec.get('sna', 0))}, {int(spec.get('snb', 0))}}},")
        else:
            lines.append('    {"---", 0, nullptr, nullptr, nullptr, 0, 0.0f, 64, 0, 0, 0},')
    lines.append("};")
    params = []
    for i, spec in enumerate(specs):
        for slot, value in spec.get('params', {}).items():
            params.append(f"    {{{i}, {int(slot)}, {int(value)}}},")
    lines.append("struct ImportParam { int track, slot, value; };")
    lines.append("const ImportParam kImportParams[] = {")
    lines += params or ["    {-1, 0, 0},"]
    lines.append("};")
    lines.append("// Each pattern is the song's 64 steps from this step on.")
    lines.append("const int kPatternFrom[kPatterns] = {" + ", ".join(str(s) for s in starts) + "};")
    lines.append("struct ImportChain { int pattern, repeats; };")
    lines.append(f"constexpr int kChainLength = {len(chain)};")
    lines.append("const ImportChain kImportChain[kChainLength] = {" +
                 ", ".join(f"{{{p}, {r}}}" for p, r in chain) + "};")
    lines.append("")
    lines.append("void place_all(Pattern &p, int from) {")
    for i, spec in enumerate(specs):
        lines.append(f"    place(p, {i}, kImport_{spec['name'].lower()}, from, 64);")
        lines.append(f"    place_locks(p, {i}, kLocks_{spec['name'].lower()}, from);")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace")
    with open(os.path.join(outdir, 'project.inc'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    with open(argv[1], encoding='utf-8') as f:
        cfg = json.load(f)
    outdir = argv[2]
    os.makedirs(outdir, exist_ok=True)
    data = als_notes.read_set(cfg['set'])
    specs = cfg['tracks']
    if len(specs) > 8:
        raise SystemExit("STRATA has 8 tracks")
    first, last = cfg.get('bars', [1, int(math.ceil(als_bounce.song_beats(data) / 4))])
    last = first - 1 + int(math.ceil((last - first + 1) / 4)) * 4     # whole patterns
    t0, t1 = (first - 1) * 4.0, last * 4.0
    frames = int(round((t1 - t0) * 60.0 / data['tempo'] * RATE))
    results = []
    for spec in specs:
        steps, info = steps_for(data, spec, t0, t1, frames, outdir, cfg['project'])
        lk = locks_for(spec, first)
        trigs = {s.step for s in steps}
        info['locks'] = sorted((st, slot, v) for (st, slot), v in lk.items() if st in trigs)
        results.append((steps, info))
        extra = ''
        if info['wav']:
            extra = (f", {info['slices']} slices of {info['slice_seconds']:.2f} s, tolerance "
                     f"{info['tolerance']:.1f} dB, gain {20 * math.log10(info['gain']):+.1f} dB")
        print(f"{spec['name']:8} {spec['mode']:6} {len(steps):5} steps{extra}")
    total = (last - first + 1) * STEPS_PER_BAR
    starts, chain = cut_patterns([r[0] for r in results], [r[1]['locks'] for r in results], total)
    print(f"bars {first}-{last}: {total // PATTERN_STEPS} windows, {len(starts)} patterns, chain of {len(chain)}")
    if len(starts) > 128 or len(chain) > 64:
        raise SystemExit("too many patterns or chain entries: STRATA holds 128 patterns and 64 entries")
    emit(outdir, cfg, data, specs, results, starts, chain)
    samples = [r[1] for r in results if r[1]['wav']]
    mib = sum(os.path.getsize(s['wav']) for s in samples) / 2 ** 20
    print(f"wrote {outdir}/project.inc and {len(samples)} samples, {mib:.1f} MiB (64 MiB sample memory)")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
