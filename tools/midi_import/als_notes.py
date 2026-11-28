#!/usr/bin/env python3
"""Read an Ableton Live set (.als) into plain data: the .als front end of tools/midi_import.

An .als is gzipped XML. This reads the arrangement (not the session clips) and returns, per
track, the notes of its MIDI clips with the clip loops unrolled onto the timeline, in the same
note-list shape midi_import.py gives a .mid ({start_beats, dur_beats, note, vel, channel}), and
the placements of its audio clips (file, where on the timeline, which part of the file), so a
caller can either quantise the notes (grid.py) or bounce the audio (als_bounce.py).

Stdlib only. Beats are quarter notes from the start of the arrangement; bar n (from 1) starts at
beat 4 * (n - 1) in 4/4.

    python3 tools/midi_import/als_notes.py SET.als            # a summary of every track
    python3 tools/midi_import/als_notes.py SET.als --json     # everything, as JSON
"""
import gzip
import json
import math
import os
import sys
import xml.etree.ElementTree as ET


def _val(elem, path, default=None):
    """The Value attribute of the element at path, or default."""
    if elem is None:
        return default
    found = elem.find(path)
    return found.get('Value') if found is not None else default


def _num(elem, path, default=0.0):
    raw = _val(elem, path)
    return float(raw) if raw is not None else default


def _bool(elem, path, default=False):
    raw = _val(elem, path)
    return raw == 'true' if raw is not None else default


def db(gain):
    return 20.0 * math.log10(gain) if gain > 0 else -math.inf


def _file_ref(elem, set_dir):
    """The absolute path of a SampleRef's file, trying the stored path, then the relative one."""
    ref = elem.find('SampleRef/FileRef') if elem is not None else None
    if ref is None:
        return None
    path = _val(ref, 'Path', '')
    if path and os.path.exists(path):
        return path
    rel = _val(ref, 'RelativePath', '')
    if rel:
        cand = os.path.normpath(os.path.join(set_dir, rel))
        if os.path.exists(cand):
            return cand
    return path or None


def _unroll(clip, content_events):
    """Place a clip's content on the timeline.

    content_events: (time, length, payload) in clip time (beats). The clip shows its content
    from content time start = LoopStart + StartRelative, wrapping from LoopEnd back to LoopStart
    when the loop is on, between CurrentStart and CurrentEnd on the timeline. Returns
    (timeline_start, length, payload) for every event that starts inside the clip, its length
    cut at the clip's end and, for a looped clip, at the loop end.
    """
    t0 = _num(clip, 'CurrentStart')
    t1 = _num(clip, 'CurrentEnd')
    loop_on = _bool(clip, 'Loop/LoopOn')
    ls = _num(clip, 'Loop/LoopStart')
    le = _num(clip, 'Loop/LoopEnd')
    rel = _num(clip, 'Loop/StartRelative')
    out = []
    if not loop_on:
        # Plays content from LoopStart (the start marker) once.
        for (t, length, payload) in content_events:
            pos = t0 + (t - ls)
            if pos < t0 - 1e-9 or pos >= t1 - 1e-9:
                continue
            out.append((pos, min(length, t1 - pos), payload))
        return out
    span = le - ls
    if span <= 0:
        return out
    # First pass starts at ls + rel; later passes at ls.
    pass_start_content = ls + rel
    pos_tl = t0
    while pos_tl < t1 - 1e-9:
        pass_len = le - pass_start_content
        for (t, length, payload) in content_events:
            if t < pass_start_content - 1e-9 or t >= le - 1e-9:
                continue
            pos = pos_tl + (t - pass_start_content)
            if pos >= t1 - 1e-9:
                continue
            out.append((pos, min(length, le - t, t1 - pos), payload))
        pos_tl += pass_len
        pass_start_content = ls
    return out


def _midi_notes(clip):
    """(time, duration, {note, vel}) of a MidiClip's enabled notes, in clip time."""
    events = []
    for kt in clip.findall('Notes/KeyTracks/KeyTrack'):
        key = int(_val(kt, 'MidiKey', '0'))
        for ev in kt.findall('Notes/MidiNoteEvent'):
            if ev.get('IsEnabled', 'true') != 'true':
                continue
            events.append((float(ev.get('Time')), float(ev.get('Duration')),
                           {'note': key, 'vel': int(round(float(ev.get('Velocity'))))}))
    return events


def _audio_clip(clip, set_dir):
    """An audio clip's placement: the timeline span and the file's seconds that fill it."""
    markers = sorted((float(m.get('BeatTime')), float(m.get('SecTime')))
                     for m in clip.findall('WarpMarkers/WarpMarker'))
    return {
        'start': _num(clip, 'CurrentStart'),
        'end': _num(clip, 'CurrentEnd'),
        'loop_on': _bool(clip, 'Loop/LoopOn'),
        'loop_start': _num(clip, 'Loop/LoopStart'),
        'loop_end': _num(clip, 'Loop/LoopEnd'),
        'start_relative': _num(clip, 'Loop/StartRelative'),
        'warped': _bool(clip, 'IsWarped'),
        'warp_markers': markers,          # (beat, second) in the file
        'gain': _num(clip, 'SampleVolume', 1.0),
        'pitch': _num(clip, 'PitchCoarse') + _num(clip, 'PitchFine') / 100.0,
        'disabled': _bool(clip, 'Disabled'),
        'file': _file_ref(clip, set_dir),
        'name': _val(clip, 'Name', ''),
    }


def _devices(track):
    out = []
    devs = track.find('DeviceChain/DeviceChain/Devices')
    if devs is None:
        return out
    for d in devs:
        name = d.tag
        if d.tag == 'PluginDevice':
            plug = d.find('.//PluginDesc//PlugName')
            if plug is None:
                plug = d.find('.//PluginDesc//Name')
            name = 'VST:' + (plug.get('Value') if plug is not None else '?')
        out.append({'type': name, 'on': _bool(d, 'On/Manual', True)})
    return out


def _gain_envelopes(track):
    """The track's automated gains: its fader's envelope and every Utility's Gain envelope, each
    a list of (beat, linear gain) breakpoints (Live ramps linearly between them; two at one
    beat are a jump). A fader envelope replaces the fader; a Utility's multiplies."""
    targets = {}
    mixer = track.find('DeviceChain/Mixer')
    vol = mixer.find('Volume/AutomationTarget') if mixer is not None else None
    if vol is not None:
        targets[vol.get('Id')] = 'fader'
    devs = track.find('DeviceChain/DeviceChain/Devices')
    for d in (devs if devs is not None else []):
        if d.tag == 'StereoGain':
            at = d.find('Gain/AutomationTarget')
            on = d.find('On/Manual')
            if at is not None and (on is None or on.get('Value') == 'true'):
                targets[at.get('Id')] = 'utility'
    out = []
    for env in track.findall('AutomationEnvelopes/Envelopes/AutomationEnvelope'):
        pid = _val(env, 'EnvelopeTarget/PointeeId')
        if pid not in targets:
            continue
        pts = []
        for ev in env.findall('Automation/Events/FloatEvent'):
            t = float(ev.get('Time'))
            pts.append((max(t, -1.0), float(ev.get('Value'))))
        if pts:
            out.append({'kind': targets[pid], 'points': pts})
    return out


def gain_at(envelopes, beats, fader):
    """The gain the envelopes give at each of `beats` (a list or an array of beats), fader
    included: 1.0 where nothing is automated."""
    import numpy as np   # only the bounce needs this, and it has numpy
    beats = np.asarray(beats, dtype=np.float64)
    g = np.full(beats.shape, fader, dtype=np.float64)
    for env in envelopes:
        t = np.array([p[0] for p in env['points']])
        v = np.array([p[1] for p in env['points']])
        val = np.interp(beats, t, v)
        if env['kind'] == 'fader':
            g = g / fader * val if fader > 0 else val
        else:
            g = g * val
    return g


def _simpler(track, set_dir):
    """The first Simpler/Sampler's sample and root key, or None."""
    devs = track.find('DeviceChain/DeviceChain/Devices')
    if devs is None:
        return None
    for d in devs:
        if d.tag not in ('OriginalSimpler', 'MultiSampler'):
            continue
        part = d.find('Player/MultiSampleMap/SampleParts/MultiSamplePart')
        if part is None:
            continue
        return {
            'device': d.tag,
            'file': _file_ref(part, set_dir),
            'root_key': int(_val(part, 'RootKey', '60')),
            'detune': _num(part, 'Detune'),
            'transpose': _num(d, 'Pitch/TransposeKey/Manual'),
            'volume': _num(part, 'Volume', 1.0),
            'sample_start': _num(part, 'SampleStart'),
            'sample_end': _num(part, 'SampleEnd'),
            'playback_mode': int(_num(d, 'Globals/PlaybackMode')),   # 0 classic, 1 one-shot, 2 slice
        }
    return None


def read_set(path):
    """The whole set as a dict: tempo, time signature and every track (see the module doc)."""
    with gzip.open(path) as f:
        root = ET.parse(f).getroot()
    live = root.find('LiveSet')
    set_dir = os.path.dirname(os.path.abspath(path))
    main = live.find('MainTrack')
    if main is None:
        main = live.find('MasterTrack')
    tempo = _num(main, 'DeviceChain/Mixer/Tempo/Manual', 120.0)
    sig_num, sig_den = 4, 4
    sig = main.find('.//TimeSignature/TimeSignatures/RemoteableTimeSignature') if main is not None else None
    if sig is not None:
        sig_num = int(_val(sig, 'Numerator', '4'))
        sig_den = int(_val(sig, 'Denominator', '4'))

    tracks = []
    for t in list(live.find('Tracks')) + list(live.find('ReturnTracks') if live.find('ReturnTracks') is not None else []):
        kind = {'MidiTrack': 'midi', 'AudioTrack': 'audio', 'GroupTrack': 'group',
                'ReturnTrack': 'return'}.get(t.tag)
        if kind is None:
            continue
        mixer = t.find('DeviceChain/Mixer')
        entry = {
            'id': int(t.get('Id')),
            'name': _val(t, 'Name/EffectiveName', ''),
            'kind': kind,
            'group': int(_val(t, 'TrackGroupId', '-1')),
            'on': _bool(mixer, 'Speaker/Manual', True),
            'volume': _num(mixer, 'Volume/Manual', 1.0),
            'pan': _num(mixer, 'Pan/Manual', 0.0),
            'sends': [_num(s, 'Send/Manual') for s in mixer.findall('Sends/TrackSendHolder')] if mixer is not None else [],
            'frozen': _bool(t, 'Freeze'),
            'gain_envelopes': _gain_envelopes(t),
            'devices': _devices(t),
            'simpler': _simpler(t, set_dir) if kind == 'midi' else None,
            'notes': [],
            'audio': [],
        }
        if kind == 'midi':
            seq = t.find('DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events')
            notes = []
            for clip in (seq if seq is not None else []):
                if clip.tag != 'MidiClip' or _bool(clip, 'Disabled'):
                    continue
                for (pos, length, p) in _unroll(clip, _midi_notes(clip)):
                    notes.append({'start_beats': round(pos, 6), 'dur_beats': round(length, 6),
                                  'note': p['note'], 'vel': p['vel'], 'channel': 0})
            notes.sort(key=lambda n: (n['start_beats'], n['note']))
            entry['notes'] = notes
            fz = t.find('DeviceChain/FreezeSequencer/Sample/ArrangerAutomation/Events')
            if fz is None:
                fz = t.find('DeviceChain/FreezeSequencer/ClipTimeable/ArrangerAutomation/Events')
            if entry['frozen'] and fz is not None:
                entry['audio'] = [_audio_clip(c, set_dir) for c in fz if c.tag == 'AudioClip']
        elif kind == 'audio':
            seq = t.find('DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events')
            entry['audio'] = [_audio_clip(c, set_dir) for c in (seq if seq is not None else [])
                              if c.tag == 'AudioClip']
        tracks.append(entry)
    _resolve_missing(tracks, set_dir)
    return {'tempo': tempo, 'time_signature': [sig_num, sig_den], 'tracks': tracks}


def _resolve_missing(tracks, set_dir):
    """Find files whose stored path does not open: a set saved elsewhere keeps its samples in
    the original project's Samples/ folder, which the files that did resolve point at. Also
    tries the name without leading or trailing spaces, which Windows drops."""
    roots = [set_dir]
    refs = [t['simpler'] for t in tracks if t['simpler']] + [a for t in tracks for a in t['audio']]
    for r in refs:
        f = r['file']
        if f and os.path.exists(f):
            norm = f.replace('\\', '/')
            cut = norm.find('/Samples/')
            if cut > 0 and norm[:cut] not in roots:
                roots.append(norm[:cut])
    for r in refs:
        f = r['file']
        if not f or os.path.exists(f):
            continue
        rel = f.replace('\\', '/')
        cut = rel.find('Samples/')
        rel = rel[cut:] if cut >= 0 else os.path.basename(rel)
        head, name = os.path.split(rel)
        for root in roots:
            for cand in (os.path.join(root, rel), os.path.join(root, head, name.strip())):
                if os.path.exists(cand):
                    r['file'] = cand
                    break
            if os.path.exists(r['file']):
                break


def group_gain(set_data, track):
    """The track's fader times every enclosing group's fader (linear)."""
    by_id = {t['id']: t for t in set_data['tracks']}
    gain = track['volume']
    g = track['group']
    while g != -1 and g in by_id:
        gain *= by_id[g]['volume']
        if not by_id[g]['on']:
            gain = 0.0
        g = by_id[g]['group']
    return gain if track['on'] else 0.0


def summary(set_data, out=sys.stdout):
    out.write(f"tempo {set_data['tempo']:g}, {set_data['time_signature'][0]}/"
              f"{set_data['time_signature'][1]}\n")
    for t in set_data['tracks']:
        if t['kind'] == 'group':
            continue
        notes = t['notes']
        span = ''
        if notes:
            span = (f" bars {notes[0]['start_beats'] / 4 + 1:g}-"
                    f"{(notes[-1]['start_beats'] + notes[-1]['dur_beats']) / 4 + 1:g}")
            keys = sorted({n['note'] for n in notes})
            span += f" keys {keys[0]}-{keys[-1]} ({len(keys)})"
        files = sorted({os.path.basename(a['file'] or '?') for a in t['audio']})
        out.write(f"{t['kind']:6} {t['name'][:30]:30} {'on ' if t['on'] else 'off'} "
                  f"{db(group_gain(set_data, t)):6.1f} dB  notes {len(notes):4}{span}"
                  f"{'  frozen' if t['frozen'] else ''}"
                  f"{'  audio ' + str(len(t['audio'])) + ' ' + ', '.join(files)[:60] if files else ''}\n")


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    data = read_set(argv[1])
    if '--json' in argv:
        json.dump(data, sys.stdout, indent=1)
        sys.stdout.write('\n')
    else:
        summary(data)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
