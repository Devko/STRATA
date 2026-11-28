# STRATA at a glance

What STRATA can do, in one page. How to play each part is in [MANUAL.md](MANUAL.md); which
parts have been checked on the unit is in [FEATURES.md](FEATURES.md).

## Design principles

From [SPEC.md](SPEC.md):

- **Performance first.** Every edit works while the transport runs and lands on the beat.
- **Two keys at most.** The common actions take one key, or FUNC plus one key.
- **Timing is sacred.** Steps are scheduled inside the audio callback against Enaga's clock and
  land on an exact sample. Screen or card activity never moves a note.
- **Controlled accidents.** Randomness is seeded, bounded and undoable.
- **Always restorable.** STRATA never writes the SPI flash. Everything lives on the SD card, so
  one official update restores the unit.

## Sound

Eight tracks. Each holds one machine followed by the same insert chain, and any track can host
any machine. The factory kit:

| Track | Machine | Role |
| :---: | --- | --- |
| 1 | **KICK** | 909-style kick, tuned to the key |
| 2 | **SNARE** | Clap (the snare's clap mode) |
| 3 | **HAT** | Closed hat; it chokes track 4 |
| 4 | **HAT** | Open hat |
| 5 | **PERC** | Rimshot (2-operator FM) |
| 6 | **VA MONO** | Rolling bass, with accent and slide |
| 7 | **VA POLY** | Dub chord stabs, up to six notes a step |
| 8 | **SAMPLE** | A loop cut into slices |

- **Eleven machines:** KICK, SNARE, HAT, PERC, SAMPLE, GRAIN, VA MONO, VA POLY, WAVE MONO,
  WAVE POLY and FM4, plus **MID**, which plays an outboard synth over MIDI. The drums follow
  Mutable Instruments Plaits' models, reimplemented ([SOUND.md](SOUND.md)). Switching a track's
  machine loads that machine's default sound.
- **VA MONO and VA POLY:** saw or pulse, a second oscillator detuned or hard-synced, sub, drive,
  a resonant filter that self-oscillates, with its own envelope decay and opened by velocity, and
  an attack-sustain-release page for pads and drones.
- **WAVE MONO and WAVE POLY:** the same voice with 16 built-in wavetables (classic shapes, sync,
  formants, FM, e-piano, vowels, mallets and more), a position the envelope can sweep, and warp.
- **FM4:** four-operator FM in eight algorithms, with feedback and an index envelope, for
  e-pianos, bells, glass, brass, organs and FM basses; chords of up to six notes.
- **SAMPLE:** one-shot or slices by note, with SLC AUTO cutting a break at its hits.
- **GRAIN:** granular clouds from a sample, with its own envelope, a scan that time-stretches,
  and a pitch spray up to an octave shimmer.
- **Factory sounds:** 116 ready-made sounds for the drums, the VAs, WAVE, FM4 and GRAIN, loaded
  into one track at a time while the beat plays.
- **Per track:** drive, multimode filter (LP, BP, HP, 12 or 24 dB; one TON knob on the VA and
  WAVE machines, which have a filter of their own), lo-fi, amp envelope, sidechain duck, pan,
  level, an **LFO** (six shapes, free or synced), two **MOD slots**, a **choke group** and an
  **arpeggiator** with rhythms, accents, ratchets and fourteen factory arps.
- **Stereo width** on VA POLY, WAVE POLY and FM4: chord notes spread across the field, or each
  note in unison, two detuned voices panned hard left and right.
- **Sends and master:** a tempo-synced ping-pong delay with a LONG mode (up to 12 s, tape wow), a
  reverb with a HALL mode (shimmer), a stereo chorus, a DJ filter, a three-band kill EQ, a glue
  compressor and a limiter.

## Sequencer

- Up to 64 steps per track, with per-track length, speed (16th-note triplets among them), swing
  (polymeter) and play mode
  (forward, reverse, ping-pong, or random steps or blocks, seeded), and micro-timing
- Parameter locks, trig conditions, probability, ratchets and rolls over a step's length,
  accents, slides and chords
- Key and scale per pattern (15 scales); notes on the VA, WAVE, FM4, GRAIN and unsliced SAMPLE
  tracks fold into the scale
- Live and motion recording (chords note for note, note-repeat rolls, a click and a count-in),
  a Euclidean generator, and KEYS: both pad rows as a
  pressure-sensitive keyboard, with the pattern's scale lit, while the STEP keys stay steps
- 8 banks of 16 patterns with queued changes, a pattern menu (copy, paste, clear, double), and
  a chain that becomes a song: each entry can recall a mute state, and SONG mode stops at the end
- **GENERATE** a drum or melodic part in the key, and **EVOLVE** it every few loops, orbiting
  the part or drifting away from it (hold OK in GRID)
- 16 levels of undo

## Performance

- Seven performance pads on the upper row: **FILL**, **REPEAT**, **MUTE**, **SCENE A**,
  **SCENE B**, **THROW** and **MUTATE**
- **A performance mixer on VALUE:** push VALUE and tap a STEP key to pick one of fourteen
  effects (filter sweeps, risers, echo freeze, reverb wash, mute bus, tape stop, bass kill, pump,
  crush, noise riser, gate, build), then turn VALUE to bring it in. STEP 13 puts the A/B scene
  morph on VALUE from any screen, and STEP 16 is a **looper** that records 1 to 8 bars of the
  mix and crossfades it against the live beat.
- Scenes with an A/B morph, four macros you set up by turning knobs (B, C and D ready in the
  demo project) and stored mute states
- Beat repeat, note repeat, **step stutter** (hold REPEAT and a STEP), delay and reverb throws,
  kill-EQ switches, and seeded mutation you keep or revert
- **ENERGY:** one knob from a breakdown to the peak: turned down, every track drops its weakest
  hits, hats first and kick last; turned up, quiet ghosts fill the empty off-beats. The pattern
  itself never changes
- **TRANSPOSE:** FUNC + the screen keys walk the melodic tracks through the key a step at a time
  (or by semitones), while the drums and sliced loops stay put
- **LOCK:** store the live set of a pattern, change everything, and jump back in one tap; tap
  again to swap back
- **Freeze** a track to a loop in memory to save CPU, **resample** the master or an input, or
  **bounce** one track to a file or another track
- **JAM REC:** the finished master to a WAV on the card for as long as the set runs (up to
  3 h 51 min), started and stopped from the FUNC menu

## Samples, projects and MIDI

- A sample browser on the unit that also reaches the stock firmware's sample pool and project
  folders; WAVs at any rate and depth; long files loaded in the background, and files too big
  for memory streamed from the card
- Projects with autosave, a project browser (open, save as, rename, delete), and a kit library
  shared by all projects
- **Starter content** from the card builder: ten starter projects from techno to ambient and
  breakbeat, and a sample pack of 105 files rendered by STRATA's own machines
- MIDI out per track; MIDI in for notes, CCs and program changes; follows the unit's clock
  (internal, MIDI clock or SYNC IN)
- **Help on every screen:** hold FUNC for what the knobs and keys do where you are
- **A screen that shows the music:** bar.beat and a progress line on every screen, a strip of
  what is off rest, a master meter, the sample and its playheads under SAMPLE's and GRAIN's knobs,
  the envelopes their knobs shape, and every lock drawn against the kit value
