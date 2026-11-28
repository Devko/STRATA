# Showcase projects

Nine projects that show STRATA beyond techno, each written by a small C++ generator with
`core/`'s own types and serializers, so the files are exactly what Storage saves. Factory
machines and factory sounds only; every note and rhythm was written for these projects (no
samples, no borrowed melodies). Each opens with the chain on: load it and press PLAY for the
arrangement. The card builder ships the generated project files from `content/starter/projects/`.

| Generator | Project | Style | Tempo, key | Shows | Emulator peak (cl6x) |
| --- | --- | --- | --- | --- | --- |
| `deephouse.cpp` | DEEP | deep house | 122, A minor | swing 57, FM4 Rhodes min9 chords with a PAN LFO, held pads, a melody, the HALL | 41.2% |
| `dub.cpp` | DUB | dub techno | 118, F minor | one chord into long echoes, a cutoff lock per hit, an 8-bar LFO, throws | 26.0% |
| `drift.cpp` | DRIFT | ambient | 80, D dorian | polymeter (11, 13 and 7 steps at different speeds), probability, LONG delay, HALL shimmer, WAVE pad | 40.1% |
| `electro.cpp` | ELECTRO | electro | 128, E phrygian | 808 kit, broken kick, cowbell, slide bass, sync lead, a lo-fi scene; no POLY voices | 24.5% |
| `breaks.cpp` | BREAKS | breakbeat jam | 134, A minor | a synthesized break with ghost snares, A:B and chance, TRACK PLAY RND4, half time | 25.6% |
| `chaos.cpp` | CHAOS | crazy breakbeat | 150, D phrygian | rolls over LEN to 1/64, RND1/RND2/PING/REV, 12- and 10-step loops, S&H reese | 35.9% |
| `smash.cpp` | SMASH | drum chaos, sparse melody | 155, F minor | six drum tracks, ratchets to 8, rolls, beat repeats; reese and a pluck | 30.6% |
| `night.cpp` | NIGHT | breakbeat x synthwave | 132, A minor | SMASH's drums under Am F C G on a string pad, octave bass, gliding lead | 32.1% |
| `steel.cpp` | STEEL | straight breakbeat | 140, E minor | no swing, micro or FEEL; rolls on the grid, a loud reese, a dark pad, a pluck | 28.8% |

Peaks are the heaviest pattern, 8 s after PLAY, per 496,000-cycle audio block, measured
2026-11-27 on `main` at #211 (`runtest.sh`). The unit reads about 1.2-1.8x the emulator
(docs/BUDGET.md), so DEEP and DRIFT may reach the CPU guard's 70% there; the guard sheds POLY
voices rather than drop out.

Each project: six 64-step patterns in bank A, an arrangement chain of 48-72 bars, two or three
scenes, two or three macros and eight mute states. The comment at the top of each generator says
what each track and pattern does.

## Building

    tools/showcases/build.sh                     # all nine: build/showcases/<name>/card/...
    tools/showcases/build.sh --render dub        # and the renders: build/showcases/dub/audio/*.wav
    tools/showcases/build.sh --starter           # and copy the projects into content/starter/projects/
    tools/showcases/runtest.sh dub 4             # emulator load of A05, after tools/build.sh

All in WSL. `--render` writes every pattern for 8 bars, the stems of the heaviest pattern and
`<PROJECT>_ARRANGEMENT.wav`: the chain with performance moves (fills, macro sweeps, morphs, mute
recalls, beat repeats), each of which returns to rest. A render fails if the mix leans more than
3 dB to one side.

`showcase_head.inc` holds the shared builders (slots, triggers, chords, chain, macros) and
`showcase_tail.inc` the file writing, the read-back check, the renders and `main()`;
`deephouse.cpp` predates them and carries its own. The renders call `Engine::step_waves()` every
block, as App::update does on the unit, or a WAVE voice would play only its sub.

The mp3s of every arrangement are in the project Library (`*_ARRANGEMENT.mp3`), not in git.
