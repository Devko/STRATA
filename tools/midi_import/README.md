# MIDI importer

Turns MIDI (and, through a second front end, Ableton clips) into STRATA steps for a project
generator like the ones in `tools/showcases/`. Python 3, stdlib only; runs on Windows or in WSL.

| File | What it is |
| --- | --- |
| `smf.py` | front end: a `.mid` (SMF 0 or 1) to `Note(start, dur, note, vel, channel)` in beats, plus tempo, time signature, track names |
| `grid.py` | the shared core: notes to `StepNote`s. `quantize` (16th grid, the rest kept as micro -11..+11 of 24 ticks, length as core/timing.hpp's index), `mono`, `poly` (chord_pack type and inversion), `drums` (note to track), `transpose`, `scale_velocity`, `emit_cpp`, `show` |
| `midi2strata.py` | the command line: `info`, `show`, `emit` |
| `imported.inc` | the C++ side: `ImportedStep` and `place()`, which writes a window of a table into a `Pattern` |
| `test_midi_import.py` | tests with MIDI built in memory, and checks that grid.py's copies of the length and chord tables still match core/ (CI runs it) |

Another front end only has to produce notes: `grid.as_notes()` takes `smf.Note`s or dicts with
`start_beats`, `dur_beats`, `note`, `vel`, `channel`.

## Use

    python tools/midi_import/midi2strata.py info D:/Sounds/pack/*.mid
    python tools/midi_import/midi2strata.py show bass.mid --mode mono --bars 4
    python tools/midi_import/midi2strata.py show drums.mid --mode drums
    python tools/midi_import/midi2strata.py emit parts.json -o build/myproject/imported.inc

`parts.json` names each part, its file and how to read it (the docstring of `midi2strata.py`
has every key):

    {"parts": [
      {"name": "bass", "file": "D:/.../bass.mid", "mode": "mono", "transpose": -12, "bars": 4},
      {"name": "stabs", "file": "D:/.../chords.mid", "mode": "poly"},
      {"name": "kick", "file": "D:/.../kick.mid", "mode": "drums", "all_to": 0}
    ]}

A generator then includes `showcase_head.inc`, the emitted tables and `imported.inc`, and places
them: `place(p, kBass, kImport_bass, 0, 64)` puts source steps 0-63 on the bass track.

What does not survive the trip: a chord STRATA has no type for keeps its lowest note (`emit`
says at which steps); notes that start together on a mono track keep the highest; timing further
than 11 ticks from a step lands on the nearer step; a drum track plays its own note (60).

**Third-party MIDI stays out of git.** The tool reads the files where they are; neither the MIDI
nor an `.inc` made from it is committed. Put generated tables under `build/`.
