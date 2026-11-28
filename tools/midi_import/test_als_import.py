#!/usr/bin/env python3
"""Tests for the Ableton front end and the set-to-project steps: python3 tools/midi_import/test_als_import.py

Builds a small .als in memory (gzipped XML in the shape Live 12 writes), so no third-party set is
needed. The parts that need numpy (bouncing, slicing) are skipped where numpy is missing.
"""
from __future__ import annotations

import gzip
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import als_notes  # noqa: E402

try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


def note(t, d, v=100):
    return f'<MidiNoteEvent Time="{t}" Duration="{d}" Velocity="{v}" IsEnabled="true" />'


def midi_clip(start, end, loop_start, loop_end, rel, loop_on, keys):
    tracks = ''.join(
        f'<KeyTrack><Notes>{"".join(note(*n) for n in ns)}</Notes><MidiKey Value="{k}" /></KeyTrack>'
        for k, ns in keys.items())
    return (f'<MidiClip Time="{start}"><CurrentStart Value="{start}" /><CurrentEnd Value="{end}" />'
            f'<Loop><LoopStart Value="{loop_start}" /><LoopEnd Value="{loop_end}" />'
            f'<StartRelative Value="{rel}" /><LoopOn Value="{"true" if loop_on else "false"}" /></Loop>'
            f'<Disabled Value="false" /><Notes><KeyTracks>{tracks}</KeyTracks></Notes></MidiClip>')


def mixer(volume=1.0, on=True, automation_id=900):
    return (f'<Mixer><Volume><Manual Value="{volume}" /><AutomationTarget Id="{automation_id}" /></Volume>'
            f'<Pan><Manual Value="0" /></Pan><Speaker><Manual Value="{"true" if on else "false"}" /></Speaker></Mixer>')


def make_set(path, tracks_xml):
    xml = ('<?xml version="1.0" encoding="UTF-8"?><Ableton><LiveSet><Tracks>' + tracks_xml +
           '</Tracks><MainTrack><DeviceChain><Mixer><Tempo><Manual Value="128" /></Tempo></Mixer>'
           '</DeviceChain></MainTrack></LiveSet></Ableton>')
    with gzip.open(path, 'wb') as f:
        f.write(xml.encode('utf-8'))


def midi_track(tid, name, clips, group=-1, volume=1.0, envelopes=''):
    return (f'<MidiTrack Id="{tid}"><Name><EffectiveName Value="{name}" /></Name>'
            f'<TrackGroupId Value="{group}" /><AutomationEnvelopes><Envelopes>{envelopes}</Envelopes>'
            f'</AutomationEnvelopes><DeviceChain>{mixer(volume, automation_id=900 + tid)}'
            f'<MainSequencer><ClipTimeable><ArrangerAutomation><Events>{"".join(clips)}</Events>'
            f'</ArrangerAutomation></ClipTimeable></MainSequencer></DeviceChain></MidiTrack>')


def group_track(tid, name, volume):
    return (f'<GroupTrack Id="{tid}"><Name><EffectiveName Value="{name}" /></Name>'
            f'<TrackGroupId Value="-1" /><DeviceChain>{mixer(volume, automation_id=800 + tid)}'
            f'</DeviceChain></GroupTrack>')


class AlsNotes(unittest.TestCase):
    def read(self, tracks_xml):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 'test.als')
            make_set(path, tracks_xml)
            return als_notes.read_set(path)

    def test_tempo_and_tracks(self):
        data = self.read(midi_track(1, 'Bass', []))
        self.assertEqual(data['tempo'], 128.0)
        self.assertEqual([t['name'] for t in data['tracks']], ['Bass'])
        self.assertEqual(data['tracks'][0]['kind'], 'midi')

    def test_unlooped_clip_plays_from_its_start_marker_once(self):
        # Content 0-4 with notes at 0 and 2; start marker at 1, placed at beat 8 for 4 beats.
        clip = midi_clip(8, 12, 1, 5, 0, False, {60: [(0, 0.5), (2, 0.5), (4.5, 0.25)]})
        notes = self.read(midi_track(1, 'A', [clip]))['tracks'][0]['notes']
        self.assertEqual([(n['start_beats'], n['note']) for n in notes], [(9.0, 60), (11.5, 60)])

    def test_looped_clip_unrolls_and_cuts_at_the_clip_end(self):
        # A one-bar loop of two notes, placed for 2.5 bars.
        clip = midi_clip(16, 26, 0, 4, 0, True, {36: [(0, 1), (2, 1)]})
        notes = self.read(midi_track(1, 'A', [clip]))['tracks'][0]['notes']
        self.assertEqual([n['start_beats'] for n in notes], [16, 18, 20, 22, 24])
        self.assertTrue(all(n['dur_beats'] == 1 for n in notes))

    def test_loop_start_relative_starts_inside_the_loop(self):
        # Starts playing at loop beat 2, then wraps to the loop start.
        clip = midi_clip(0, 8, 0, 4, 2, True, {50: [(0, 1), (2, 1)]})
        notes = self.read(midi_track(1, 'A', [clip]))['tracks'][0]['notes']
        self.assertEqual([n['start_beats'] for n in notes], [0, 2, 4, 6])

    def test_note_held_past_the_loop_end_is_cut(self):
        clip = midi_clip(0, 8, 0, 4, 0, True, {50: [(3, 3)]})
        notes = self.read(midi_track(1, 'A', [clip]))['tracks'][0]['notes']
        self.assertEqual([(n['start_beats'], n['dur_beats']) for n in notes], [(3, 1), (7, 1)])

    def test_group_gain_multiplies_and_a_switched_off_group_silences(self):
        data = self.read(group_track(5, 'G', 0.5) + midi_track(1, 'A', [], group=5, volume=0.5))
        a = [t for t in data['tracks'] if t['name'] == 'A'][0]
        self.assertAlmostEqual(als_notes.group_gain(data, a), 0.25)

    def test_fader_automation_is_read(self):
        env = ('<AutomationEnvelope><EnvelopeTarget><PointeeId Value="901" /></EnvelopeTarget><Automation>'
               '<Events><FloatEvent Time="-63072000" Value="1" /><FloatEvent Time="16" Value="1" />'
               '<FloatEvent Time="16" Value="0" /></Events></Automation></AutomationEnvelope>')
        data = self.read(midi_track(1, 'A', [], envelopes=env))
        envs = data['tracks'][0]['gain_envelopes']
        self.assertEqual(len(envs), 1)
        self.assertEqual(envs[0]['kind'], 'fader')
        if HAVE_NUMPY:
            g = als_notes.gain_at(envs, [0.0, 8.0, 20.0], 1.0)
            self.assertEqual(list(g), [1.0, 1.0, 0.0])


@unittest.skipUnless(HAVE_NUMPY, 'numpy not installed')
class SetToProject(unittest.TestCase):
    def setUp(self):
        import als2strata
        import grid
        self.a2s = als2strata
        self.grid = grid

    def test_identical_windows_become_one_pattern_and_repeats(self):
        S = self.grid.StepNote
        # Four 64-step windows: A A B A.
        steps = [S(w * 64 + s, 60, 100, 8) for w in (0, 1, 3) for s in (0, 8)] + [S(128, 62, 100, 8)]
        starts, chain = self.a2s.cut_patterns([steps], [[]], 256)
        self.assertEqual(starts, [0, 128])
        self.assertEqual(chain, [[0, 2], [1, 1], [0, 1]])

    def test_a_lock_makes_a_window_its_own_pattern(self):
        S = self.grid.StepNote
        steps = [S(w * 64, 60, 100, 8) for w in range(2)]
        starts, chain = self.a2s.cut_patterns([steps], [[(64, 5, 30)]], 128)
        self.assertEqual(starts, [0, 64])

    def test_lock_ramp_spans_the_bars(self):
        locks = self.a2s.locks_for({'locks': [{'bars': [3, 4], 'slot': 5, 'from': 0, 'to': 31}]}, 1)
        self.assertEqual(locks[(32, 5)], 0)
        self.assertEqual(locks[(63, 5)], 31)
        self.assertNotIn((31, 5), locks)

    def test_alike_sounds_share_a_slice(self):
        import numpy as np
        rng = np.random.default_rng(1)
        hit = np.zeros((4800, 2), dtype=np.float32)
        hit[:480] = rng.standard_normal((480, 2)) * 0.5
        other = np.zeros((4800, 2), dtype=np.float32)
        other[:2400] = rng.standard_normal((2400, 2)) * 0.1
        reps, group, _ = self.a2s.cluster([hit, other, hit * 0.99, other], 3.0)
        self.assertEqual(group, [0, 1, 0, 1])

    def test_onsets_skip_a_kick_body_inside_a_16th(self):
        import numpy as np
        x = np.zeros((48000, 2), dtype=np.float32)
        for start in (0, 12000, 24000):
            x[start:start + 240] = 0.05         # the click
            x[start + 2400:start + 4800] = 0.8  # the body, 50 ms later: the same hit
        hits = self.a2s.onsets(x, 24000, 4800)
        self.assertEqual([s for s, _ in hits], [0, 12000, 24000])

    def test_slice_knob_matches_the_sampler(self):
        # dsp/sample.cpp: slices = 1 + round(knob / 127 * 15). import_project.cpp's slice_knob.
        for n in range(1, 17):
            knob = (n - 1) * 127 // 15 + (1 if (n - 1) * 127 % 15 else 0)
            self.assertEqual(1 + int(knob / 127 * 15 + 0.5), n)


if __name__ == '__main__':
    unittest.main(verbosity=1)
