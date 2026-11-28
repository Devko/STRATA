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

## Ableton sets

A whole arrangement can come in from an `.als`: the MIDI parts as STRATA synths, the audio as
sliced samples on SAMPLE tracks, the song cut into patterns and a chain.

| File | What it is |
| --- | --- |
| `als_notes.py` | front end: an `.als` (gzipped XML) to tempo, tracks, the arrangement's notes with clip loops unrolled (as `grid.as_notes` takes them), audio clip placements with warp markers, Simpler samples, fader and Utility automation. Stdlib only |
| `als_bounce.py` | a dry bounce of the arrangement from the set's own files and freezes (no effects or plug-ins), its stems, and a map of which track plays in which bar. Also the renderer the two below use |
| `als2strata.py` | a map (JSON) of which Ableton tracks become each of the 8 STRATA tracks and how (`mono`, `poly`, `hits`, `blocks`; the docstring has every key) to `project.inc` and the samples |
| `import_project.cpp`, `build_import.sh` | the generator: `project.inc` to the project files, read back, and a render of the chain through the host Engine with the samples loaded |
| `ab.py` | the render against the original's export, per section and band, and an A/B file alternating the two |
| `test_als_import.py` | tests with an `.als` built in memory (CI runs it; the numpy parts skip without numpy) |

    python tools/midi_import/als_notes.py SET.als                       # what is in the set
    python tools/midi_import/als_bounce.py SET.als build/x/ref.wav --map   # who plays where
    python tools/midi_import/als2strata.py build/x/map.json build/x/out
    tools/midi_import/build_import.sh build/x/out [--stems]              # WSL
    python tools/midi_import/ab.py EXPORT.wav build/x/out/audio/<P>_ARRANGEMENT.wav --bpm 135 --ab build/x/ab.wav

The card folder is `build/x/out/card/`: `ENAGA/DATA/STRATA/PROJECTS/<P>/` and
`SAMPLES/<P>/`, to copy to the card's root.

How a set is fitted into eight tracks is the map's job, by ear: which parts become synths, which
audio becomes a `blocks` track (whole bars of loops, every distinct block a slice, at most 16),
which hits a `hits` track (every distinct one-shot a slice). `als_bounce.py --map` shows where
each track plays; tracks that never play together can share a STRATA track. What the importer
does on its own: a frozen track's notes are kept only in bars where its freeze is heard (its
automation filtered or faded the rest out), automated faders and Utility gains scale hits and
are baked into blocks, and identical 64-step windows become one pattern. Filter sweeps and other
device automation are the map's `locks` (a value or a ramp on a parameter slot over bars).

What does not come across: plug-in sounds (each part is played by the nearest STRATA preset,
tuned in the map), effects and sends beyond STRATA's own, tempo changes, time signatures other
than 4/4, notes held longer than 48 steps.

**The set, its samples, the map and everything in OUTDIR are the set owner's.** Keep them under
`build/` or another ignored folder; only the tool is committed.
