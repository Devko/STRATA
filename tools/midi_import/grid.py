"""The shared core of the importer: a note list in beats to STRATA steps.

Front ends (smf.py for .mid, als_notes.py for Ableton clips) produce `smf.Note`s or plain dicts
with start_beats/dur_beats/note/vel/channel; everything here works on those and knows the
STRATA step format (core/pattern.hpp `Step`, core/timing.hpp, core/chord.hpp):

    steps = quantize(notes)                    # 16th grid, micro-timing, length index
    line  = mono(steps)                        # VA MONO / WAVE MONO: one note per step
    stabs = poly(steps)                        # VA POLY / FM4: chords as chord_pack bytes
    hits  = drums(steps, {36: 0, 38: 1, 42: 2})  # GM note -> STRATA track, note D (60)
    text  = emit_cpp("bass_a", line)           # a C++ table for a generator .cpp

The tables the C++ side reads are mirrored here, each with the header it copies, and
test_midi_import.py checks them against those headers so a change there fails here.
"""
from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Iterable, Mapping

from smf import Note

# core/timing.hpp: 24 ticks in a 16th (96 PPQN), micro -11..+11 of them.
TICKS_PER_STEP = 24
MICRO_MAX = 11

# core/timing.hpp kLengthSixteenthsOfStep, index 1.. (0 is invalid). Sixteenths of a step.
LENGTH_SIXTEENTHS = [0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768]

# core/chord.hpp ChordType ids and chord_intervals(). Order is the preference when two types
# give the same pitch classes (none here do), and the id is what chord_pack stores.
CHORDS = {
    1: ("MIN", (0, 3, 7)),
    2: ("MAJ", (0, 4, 7)),
    3: ("SUS2", (0, 2, 7)),
    4: ("SUS4", (0, 5, 7)),
    5: ("DIM", (0, 3, 6)),
    6: ("MIN7", (0, 3, 7, 10)),
    7: ("DOM7", (0, 4, 7, 10)),
    8: ("MAJ7", (0, 4, 7, 11)),
    9: ("MIN9", (0, 3, 7, 10, 14)),
    10: ("MIN11", (0, 3, 7, 10, 14, 17)),
    11: ("5TH", (0, 7)),
    12: ("OCT", (0, 12)),
    13: ("5TH OCT", (0, 7, 12)),
}
CHORD_INVERSION_MAX = 3
DRUM_NOTE = 60   # the drums' neutral note (tools/showcases/showcase_head.inc `D`)


@dataclass(frozen=True)
class StepNote:
    step: int          # 16th index from the pattern's start
    note: int          # 0-127 (the chord's root for a chord)
    vel: int           # 1-127
    length: int        # core/timing.hpp length index (8 = one step)
    micro: int = 0     # -11..+11 ticks
    chord: int = 0     # chord_pack byte: type | inversion << 4
    track: int = -1    # drums(): the STRATA track; -1 elsewhere


def as_notes(items: Iterable) -> list[Note]:
    """Accept smf.Notes or dicts from another front end (start_beats/dur_beats or start/dur)."""
    out = []
    for n in items:
        if isinstance(n, Note):
            out.append(n)
            continue
        out.append(Note(float(n.get("start_beats", n.get("start"))),
                        float(n.get("dur_beats", n.get("dur"))),
                        int(n["note"]), int(n.get("vel", n.get("velocity", 100))),
                        int(n.get("channel", 0))))
    return out


def length_index(steps: float) -> int:
    """The table entry nearest `steps` (in steps), compared on a log scale: a 3.4-step note is
    a 3-step note, not a 4-step one, and the short end does not swallow everything."""
    import math
    want = max(steps * 16.0, 1.0)
    best, err = 1, float("inf")
    for i in range(1, len(LENGTH_SIXTEENTHS)):
        e = abs(math.log(LENGTH_SIXTEENTHS[i] / want))
        if e < err:
            best, err = i, e
    return best


def quantize(notes: Iterable, *, steps_per_beat: int = 4, offset_beats: float = 0.0,
             window_steps: int | None = None, micro: bool = True,
             max_length_steps: float | None = None) -> list[StepNote]:
    """Snap each note to the nearest step. What the snap moved becomes micro-timing (clamped to
    +-11 ticks) unless `micro` is False. `offset_beats` is where the pattern starts in the
    source; `window_steps` drops notes at or past that many steps (a 64-step pattern)."""
    out = []
    tick_per_beat = TICKS_PER_STEP * steps_per_beat
    for n in as_notes(notes):
        ticks = round((n.start - offset_beats) * tick_per_beat)
        step = (ticks + TICKS_PER_STEP // 2) // TICKS_PER_STEP
        if ticks < -TICKS_PER_STEP // 2:
            continue
        if window_steps is not None and step >= window_steps:
            continue
        m = ticks - step * TICKS_PER_STEP
        m = max(-MICRO_MAX, min(MICRO_MAX, m)) if micro else 0
        dur = n.dur * steps_per_beat
        if max_length_steps is not None:
            dur = min(dur, max_length_steps)
        out.append(StepNote(step, n.note, max(1, min(127, n.vel)), length_index(dur), m))
    out.sort(key=lambda s: (s.step, s.note))
    return out


def _by_step(steps: Iterable[StepNote]) -> dict[int, list[StepNote]]:
    groups: dict[int, list[StepNote]] = {}
    for s in steps:
        groups.setdefault(s.step, []).append(s)
    return groups


def mono(steps: Iterable[StepNote], pick: str = "high") -> list[StepNote]:
    """One note per step, for a mono machine: the highest ("high") or lowest ("low") of
    the notes that start on it, keeping the loudest velocity of the group."""
    out = []
    for step, group in sorted(_by_step(steps).items()):
        s = max(group, key=lambda x: x.note) if pick == "high" else min(group, key=lambda x: x.note)
        out.append(replace(s, vel=max(g.vel for g in group)))
    return out


def chord_pack(type_id: int, inversion: int) -> int:
    count = len(CHORDS[type_id][1])
    deepest = min(count - 1, CHORD_INVERSION_MAX)
    return type_id | (min(inversion, deepest) << 4)


def _voicing(root: int, intervals: tuple[int, ...], inversion: int) -> list[int]:
    """core/chord.cpp chord_invert: the lowest note up an octave, `inversion` times."""
    notes = sorted(root + i for i in intervals)
    for _ in range(inversion):
        notes = sorted(notes[1:] + [notes[0] + 12])
    return notes


def detect_chord(pitches: list[int]) -> tuple[int, int, int] | None:
    """(root note, chord type id, inversion) that plays exactly `pitches`, else the closest
    pitch-class match in root position an octave at or below the lowest note, else None."""
    ps = sorted(set(pitches))
    if len(ps) < 2:
        return None
    classes = {p % 12 for p in ps}
    loose = None
    for type_id, (_, iv) in CHORDS.items():
        for inv in range(min(len(iv) - 1, CHORD_INVERSION_MAX) + 1):
            for root in range(ps[0] - 24, ps[0] + 1):
                if _voicing(root, iv, inv) == ps:
                    return root, type_id, inv
        if loose is None:
            for rc in classes:
                if {(rc + i) % 12 for i in iv} == classes:
                    root = ps[0] - ((ps[0] - rc) % 12)
                    loose = (root, type_id, 0)
                    break
    return loose


def poly(steps: Iterable[StepNote]) -> list[StepNote]:
    """Group the notes that start on one step into one chord step. A group that is no STRATA
    chord keeps its lowest note (the bass of the voicing) and says so in `unmatched`."""
    out = []
    for step, group in sorted(_by_step(steps).items()):
        low = min(group, key=lambda x: x.note)
        vel = max(g.vel for g in group)
        length = max(g.length for g in group)
        found = detect_chord([g.note for g in group])
        if found is None:
            out.append(replace(low, vel=vel, length=length))
        else:
            root, type_id, inv = found
            out.append(replace(low, note=root, vel=vel, length=length, chord=chord_pack(type_id, inv)))
    return out


def unmatched(steps: Iterable[StepNote]) -> list[int]:
    """Steps where poly() had several notes but found no chord (for a report)."""
    return [step for step, g in sorted(_by_step(steps).items())
            if len({x.note for x in g}) > 1 and detect_chord([x.note for x in g]) is None]


# General MIDI percussion, to a role name. A caller maps roles (or notes) to its tracks.
GM_DRUMS = {
    35: "kick", 36: "kick", 37: "rim", 38: "snare", 39: "clap", 40: "snare",
    41: "tom", 43: "tom", 45: "tom", 47: "tom", 48: "tom", 50: "tom",
    42: "closed", 44: "closed", 46: "open", 49: "crash", 51: "ride", 53: "ride", 57: "crash",
    54: "perc", 56: "perc", 60: "perc", 61: "perc", 62: "perc", 63: "perc", 64: "perc",
    69: "shaker", 70: "shaker", 75: "perc", 76: "perc", 77: "perc",
}


def drums(steps: Iterable[StepNote], track_of: Mapping[int, int], length: int = 8) -> list[StepNote]:
    """Drum hits: each note in `track_of` goes to that STRATA track at the neutral note, one hit
    per track and step (the loudest). Notes not in the map are dropped."""
    best: dict[tuple[int, int], StepNote] = {}
    for s in steps:
        t = track_of.get(s.note)
        if t is None:
            continue
        key = (t, s.step)
        hit = replace(s, note=DRUM_NOTE, length=length, track=t)
        if key not in best or hit.vel > best[key].vel:
            best[key] = hit
    return sorted(best.values(), key=lambda s: (s.track, s.step))


def transpose(steps: Iterable[StepNote], semitones: int) -> list[StepNote]:
    return [replace(s, note=max(0, min(127, s.note + semitones))) for s in steps]


def scale_velocity(steps: Iterable[StepNote], lo: int = 40, hi: int = 127) -> list[StepNote]:
    """Stretch velocities into lo..hi (a MIDI loop at a flat 100 keeps a flat value)."""
    steps = list(steps)
    if not steps:
        return steps
    vmin, vmax = min(s.vel for s in steps), max(s.vel for s in steps)
    if vmax == vmin:
        return steps
    return [replace(s, vel=round(lo + (s.vel - vmin) * (hi - lo) / (vmax - vmin))) for s in steps]


def emit_cpp(name: str, steps: Iterable[StepNote], comment: str = "") -> str:
    """A C++ table `kImport_<name>` of ImportedStep (tools/midi_import/imported.inc) rows:
    {step, note, vel, length, micro, chord, track}."""
    rows = list(steps)
    lines = []
    if comment:
        lines += [f"// {c}" for c in comment.splitlines()]
    lines.append(f"constexpr ImportedStep kImport_{name}[] = {{")
    for s in rows:
        lines.append(f"    {{{s.step}, {s.note}, {s.vel}, {s.length}, {s.micro}, {s.chord}, {s.track}}},")
    if not rows:
        lines.append("    {0, 0, 0, 0, 0, 0, -2},   // empty: place() skips track -2")
    lines.append("};")
    return "\n".join(lines) + "\n"


def show(steps: Iterable[StepNote], width: int = 16) -> str:
    """A text grid for choosing loops by eye: one row per bar, one cell per step."""
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    cells: dict[int, str] = {}
    for s in steps:
        label = f"{names[s.note % 12]}{s.note // 12 - 1}"
        if s.chord:
            label += "*"
        cells[s.step] = label
    last = max(cells, default=-1)
    out = []
    for bar in range(last // width + 1):
        row = [cells.get(bar * width + i, ".").ljust(4) for i in range(width)]
        out.append(f"{bar + 1:3} | " + "".join(row))
    return "\n".join(out)
