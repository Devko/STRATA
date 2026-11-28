"""Standard MIDI File reader: a .mid (format 0 or 1) to a plain note list.

The front end of the importer. Everything after it (grid.py) works on `Note`s in beats and
never sees a MIDI byte, so another front end (an Ableton .als clip, a hand-written list) only
has to produce the same list.

Stdlib only. Reads note on/off (a note on with velocity 0 is an off), track names, the first
tempo and time signature; every other event is skipped by its length.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class Note:
    start: float      # beats (quarter notes) from the file's start
    dur: float        # beats
    note: int         # 0-127
    vel: int          # 1-127
    channel: int = 0  # 0-15
    track: int = 0    # the SMF track it came from


@dataclass
class Song:
    notes: list[Note] = field(default_factory=list)
    tempo_bpm: float = 120.0
    numerator: int = 4
    denominator: int = 4
    track_names: dict[int, str] = field(default_factory=dict)

    def length_beats(self) -> float:
        return max((n.start + n.dur for n in self.notes), default=0.0)


class SmfError(ValueError):
    pass


def _varlen(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    for _ in range(4):
        if pos >= len(data):
            raise SmfError("truncated variable-length number")
        b = data[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not b & 0x80:
            return value, pos
    raise SmfError("variable-length number longer than four bytes")


def parse(data: bytes) -> Song:
    if data[:4] != b"MThd":
        raise SmfError("not a MIDI file (no MThd)")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntracks, division = struct.unpack(">HHH", data[8:14])
    if fmt > 1:
        raise SmfError(f"SMF format {fmt} is not supported (0 and 1 are)")
    if division & 0x8000:
        raise SmfError("SMPTE time division is not supported")
    ppq = division
    song = Song()
    tempo_seen = sig_seen = False
    pos = 8 + hlen
    for track in range(ntracks):
        if data[pos:pos + 4] != b"MTrk":
            raise SmfError(f"track {track}: no MTrk")
        tlen = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        chunk = data[pos + 8:pos + 8 + tlen]
        pos += 8 + tlen
        tick = 0
        p = 0
        status = 0
        held: dict[tuple[int, int], list[tuple[int, int]]] = {}   # (ch, note) -> [(tick, vel)]
        while p < len(chunk):
            delta, p = _varlen(chunk, p)
            tick += delta
            b = chunk[p]
            if b & 0x80:
                status = b
                p += 1
            elif status == 0:
                raise SmfError(f"track {track}: running status before any status byte")
            if status == 0xFF:
                kind = chunk[p]
                n, p = _varlen(chunk, p + 1)
                body = chunk[p:p + n]
                p += n
                if kind == 0x03 and track not in song.track_names:
                    song.track_names[track] = body.decode("latin-1").strip()
                elif kind == 0x51 and not tempo_seen and n == 3:
                    song.tempo_bpm = 60_000_000 / int.from_bytes(body, "big")
                    tempo_seen = True
                elif kind == 0x58 and not sig_seen and n >= 2:
                    song.numerator, song.denominator = body[0], 1 << body[1]
                    sig_seen = True
                status = 0    # meta and sysex cancel running status
                continue
            if status in (0xF0, 0xF7):
                n, p = _varlen(chunk, p)
                p += n
                status = 0
                continue
            kind, ch = status & 0xF0, status & 0x0F
            if kind in (0xC0, 0xD0):
                p += 1
                continue
            d1, d2 = chunk[p], chunk[p + 1]
            p += 2
            if kind == 0x90 and d2 > 0:
                held.setdefault((ch, d1), []).append((tick, d2))
            elif kind == 0x80 or (kind == 0x90 and d2 == 0):
                stack = held.get((ch, d1))
                if stack:
                    on, vel = stack.pop(0)
                    song.notes.append(Note(on / ppq, (tick - on) / ppq, d1, vel, ch, track))
        # A note never released ends with its track.
        for (ch, d1), stack in held.items():
            for on, vel in stack:
                song.notes.append(Note(on / ppq, max(tick - on, 1) / ppq, d1, vel, ch, track))
    song.notes.sort(key=lambda n: (n.start, n.note))
    return song


def read(path: str) -> Song:
    with open(path, "rb") as f:
        return parse(f.read())
