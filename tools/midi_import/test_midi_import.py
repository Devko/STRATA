#!/usr/bin/env python3
"""Tests for the MIDI importer: python3 tools/midi_import/test_midi_import.py

Builds its MIDI files in memory (no third-party MIDI in the repo) and checks grid.py's mirrored
tables against the headers they copy, so a change in core/ fails here first.
"""
from __future__ import annotations

import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import grid  # noqa: E402
import smf   # noqa: E402


def varlen(n: int) -> bytes:
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def make_smf(events_by_track: list[list[tuple[int, bytes]]], ppq: int = 96, fmt: int = 1) -> bytes:
    """events: (absolute tick, raw event bytes) per track."""
    data = b"MThd" + struct.pack(">IHHH", 6, fmt, len(events_by_track), ppq)
    for events in events_by_track:
        body = b""
        last = 0
        for tick, ev in sorted(events, key=lambda e: e[0]):
            body += varlen(tick - last) + ev
            last = tick
        body += b"\x00\xFF\x2F\x00"
        data += b"MTrk" + struct.pack(">I", len(body)) + body
    return data


def on(note, vel=100, ch=0):
    return bytes([0x90 | ch, note, vel])


def off(note, ch=0):
    return bytes([0x80 | ch, note, 0])


class SmfTest(unittest.TestCase):
    def test_notes_tempo_and_names(self):
        tempo = b"\xFF\x51\x03" + (500000 * 2 // 2 * 1).to_bytes(3, "big")   # 120 BPM
        name = b"\xFF\x03\x04BASS"
        data = make_smf([
            [(0, tempo)],
            [(0, name), (0, on(36, 110)), (48, off(36)), (96, on(39, 90)), (288, off(39))],
        ])
        song = smf.parse(data)
        self.assertAlmostEqual(song.tempo_bpm, 120.0)
        self.assertEqual(song.track_names[1], "BASS")
        self.assertEqual([(n.start, n.dur, n.note, n.vel) for n in song.notes],
                         [(0.0, 0.5, 36, 110), (1.0, 2.0, 39, 90)])

    def test_note_on_velocity_zero_is_off_and_running_status(self):
        # 0x90 then running status: [36 100] [36 0] -- the second is a note off.
        body = [(0, bytes([0x90, 36, 100])), (24, bytes([36, 0]))]
        song = smf.parse(make_smf([body]))
        self.assertEqual(len(song.notes), 1)
        self.assertAlmostEqual(song.notes[0].dur, 0.25)

    def test_overlapping_same_note_pairs_first_in_first_out(self):
        song = smf.parse(make_smf([[(0, on(60)), (12, on(60)), (24, off(60)), (48, off(60))]]))
        self.assertEqual(sorted(n.dur for n in song.notes), [0.25, 0.375])

    def test_refuses_other_files(self):
        with self.assertRaises(smf.SmfError):
            smf.parse(b"RIFF....")


class GridTest(unittest.TestCase):
    def test_quantize_keeps_offset_as_micro(self):
        # A 16th is 0.25 beats = 24 ticks. 5 ticks late -> step 1, micro +5; 10 early -> step 2, -10.
        notes = [smf.Note(0.25 + 5 / 96, 0.25, 60, 100), smf.Note(0.5 - 10 / 96, 0.25, 62, 100)]
        steps = grid.quantize(notes)
        self.assertEqual([(s.step, s.micro) for s in steps], [(1, 5), (2, -10)])
        self.assertEqual([s.micro for s in grid.quantize(notes, micro=False)], [0, 0])

    def test_quantize_window_and_offset(self):
        notes = [smf.Note(b, 0.25, 60, 100) for b in (0, 4, 8, 16)]
        steps = grid.quantize(notes, offset_beats=4, window_steps=32)
        self.assertEqual([s.step for s in steps], [0, 16])

    def test_length_index(self):
        self.assertEqual(grid.length_index(1), 8)
        self.assertEqual(grid.length_index(4), 12)
        self.assertEqual(grid.length_index(16), 16)
        self.assertEqual(grid.length_index(0.5), 6)
        self.assertEqual(grid.length_index(3.4), 11)    # 3 steps, not 4
        self.assertEqual(grid.length_index(1000), len(grid.LENGTH_SIXTEENTHS) - 1)
        self.assertEqual(grid.length_index(0), 1)

    def test_mono_keeps_one_note_per_step(self):
        steps = grid.quantize([smf.Note(0, 1, 48, 80), smf.Note(0, 1, 60, 120), smf.Note(1, 1, 50, 90)])
        self.assertEqual([(s.step, s.note, s.vel) for s in grid.mono(steps)], [(0, 60, 120), (4, 50, 90)])
        self.assertEqual([s.note for s in grid.mono(steps, "low")], [48, 50])

    def test_chords_root_position_and_inversions(self):
        self.assertEqual(grid.detect_chord([60, 63, 67]), (60, 1, 0))       # C minor
        self.assertEqual(grid.detect_chord([63, 67, 72]), (60, 1, 1))       # first inversion
        self.assertEqual(grid.detect_chord([67, 72, 75]), (60, 1, 2))       # second
        self.assertEqual(grid.detect_chord([57, 60, 64, 67]), (57, 6, 0))   # A min7
        self.assertEqual(grid.detect_chord([48, 55]), (48, 11, 0))          # 5TH
        self.assertIsNone(grid.detect_chord([60]))
        self.assertIsNone(grid.detect_chord([60, 61, 62]))
        # Pitch classes match but the voicing is spread: root position under the lowest note.
        self.assertEqual(grid.detect_chord([48, 67, 75]), (48, 1, 0))

    def test_poly_packs_chord_byte(self):
        steps = grid.quantize([smf.Note(0, 4, n, 100) for n in (63, 67, 72)])
        [s] = grid.poly(steps)
        self.assertEqual((s.note, s.chord), (60, 1 | (1 << 4)))
        self.assertEqual(s.length, 16)

    def test_drums_map_and_loudest_wins(self):
        steps = grid.quantize([smf.Note(0, 0.1, 36, 90), smf.Note(0, 0.1, 35, 120),
                               smf.Note(1, 0.1, 42, 70), smf.Note(1, 0.1, 99, 70)])
        hits = grid.drums(steps, {36: 0, 35: 0, 42: 2})
        self.assertEqual([(h.track, h.step, h.vel, h.note) for h in hits], [(0, 0, 120, 60), (2, 4, 70, 60)])

    def test_as_notes_accepts_front_end_dicts(self):
        [n] = grid.as_notes([{"start_beats": 1.5, "dur_beats": 0.5, "note": 40, "vel": 99, "channel": 2}])
        self.assertEqual((n.start, n.dur, n.note, n.vel, n.channel), (1.5, 0.5, 40, 99, 2))

    def test_emit_cpp_rows(self):
        text = grid.emit_cpp("x", [grid.StepNote(3, 48, 100, 8, -2, 0)])
        self.assertIn("kImport_x[]", text)
        self.assertIn("{3, 48, 100, 8, -2, 0, -1},", text)
        self.assertIn("-2}", grid.emit_cpp("empty", []))


class MirrorTest(unittest.TestCase):
    """grid.py copies tables from core/; these fail when core/ changes them."""

    def read(self, rel):
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            return f.read()

    def test_length_table_matches_timing_hpp(self):
        src = self.read("core/timing.hpp")
        body = src[src.index("kLengthSixteenthsOfStep[kLengthTableSize] = {"):]
        body = body[body.index("{") + 1:body.index("};")]
        values = [int(m) for m in re.findall(r"^\s*(\d+),", body, re.M)]
        self.assertEqual(values, grid.LENGTH_SIXTEENTHS)

    def test_chords_match_chord_hpp(self):
        src = self.read("core/chord.hpp")
        enum = src[src.index("enum class ChordType"):]
        names = re.findall(r"^\s*(\w+)(?:\s*=\s*0)?,", enum[:enum.index("};")], re.M)
        table = dict(re.findall(r"case ChordType::(\w+):\s*return \{\{([^}]*)\}", src))
        for type_id, (_, intervals) in grid.CHORDS.items():
            name = names[type_id]
            self.assertIn(name, table, name)
            self.assertEqual(tuple(int(x) for x in table[name].split(",")), intervals, name)
        self.assertEqual(len(grid.CHORDS), len(names) - 1)   # `none` is not a chord

    def test_micro_and_ticks_match(self):
        src = self.read("core/pattern.hpp")
        self.assertIn("-11..+11 ticks of the 24 in a 16th", src)
        self.assertEqual(grid.MICRO_MAX, 11)
        self.assertEqual(grid.TICKS_PER_STEP, 24)


if __name__ == "__main__":
    unittest.main()
