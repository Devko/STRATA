# Changelog

All notable changes to STRATA. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org/). Before 1.0, any release may change
the project file format or the controls.

## [Unreleased]

### Added

- **CPU guard.** When the audio load stays high, STRATA sheds VA POLY voices (two a level, the
  quietest fading out first) and thins GRAIN clouds instead of risking an overrun. The bottom
  line shows `GUARD1` to `GUARD4` while it is up; it steps back down once the load falls.
- **Rename and delete projects** on the PROJECTS page: push B renames the project under the
  cursor, push C and then OK twice deletes it. Both are safe against a pulled card: a rename
  copies the project with `PROJECT.BIN` last before it deletes the old folder, and a delete
  removes `PROJECT.BIN` first. The open project cannot be deleted.
- **LEN on streamed samples:** a file streamed from the card now stops, or loops back, where LEN
  says. START, slices and reverse stay off for streamed files (docs/FEATURES.md says why).
- **Held notes and pads on VA MONO and VA POLY.** A seventh EDIT page, ASR: attack (to 5 s),
  sustain, release (to 8 s) and filter sustain. With SUS up a note holds while its KEYS pad, STEP
  key, track pad or MIDI note is held, or for its step's LEN, up to 64 steps. At 0 (and in every older
  project) the VAs sound exactly as before.
- **A05 PAD**, a fifth factory pattern: four bars of held VA POLY chords (Cm9, Abmaj7, Ebmaj7,
  Gm7) that swell in and ring into each other, so the ASR page can be heard straight away. The
  pad is in the pattern's locks; the kit and A01-A04 are unchanged. New projects get it; an
  existing project keeps its own bank A.
- **REC records how long a note was held** into its step's LEN, on SAMPLE, GRAIN, the VAs and MID.
- **Editable sidechain duck.** Three new MIXER pages after the faders: DCK 1-4 and DCK 5-8 set
  how far each track ducks (OFF to 100%), DUCK sets the source track, attack, hold and release.
  Saved in the kit; kits and projects from before open with the old fixed duck (track 1, 35%,
  3/40/120 ms). The delay, reverb, master and EQ pages are now three screen-key presses further on.

## [0.1.0] - {{RELEASE_DATE}}

The first release: STRATA on a SmplTrek running Enaga 0.2.0. Early, and released so that people
with a unit can play it and report back. Release notes: [docs/release/v0.1.0.md](docs/release/v0.1.0.md).
How to play it: [docs/MANUAL.md](docs/MANUAL.md); how the controls are laid out:
[docs/CONTROLS.md](docs/CONTROLS.md); every control on one page:
[docs/CHEATSHEET.md](docs/CHEATSHEET.md). The manual and the cheatsheet have a labelled diagram
of the panel and screenshots of every screen.

### Requires

- A Sonicware SmplTrek with bootloader BOOT 1.0.17, running **Enaga 0.2.0** exactly. The app is
  built against Enaga's provisional ABI 0, and Enaga's loader checks only that number, so a
  different Enaga release may load STRATA and then misbehave.

### Added

**Controls**

- **One job per button group, in every mode** ([docs/CONTROLS.md](docs/CONTROLS.md)). The arrow
  pad moves the cursor (OK picks, CLR goes back); the two keys under the screen turn the screen's
  pages; the two keys at the ends of the upper pad row choose which sixteen the STEP keys mean
  (step page, octave or bank); knobs A-D turn the four values on screen and push for their switch.
- **FUNC + a function key does what is printed under it:** sampling, pool (the sample browser),
  system (the FUNC menu), ◀◀ restart, ▶▶ reset, fx (PERFORM) and rec src (MACHINE and kits).
  Plain FUNC chords are otherwise kept for moves made while playing.
- **The FUNC menu** (FUNC + HOME) holds the rare setup actions by name: save (FUNC + HOME, OK),
  save as, open, new and reload project, kits, sample reload, track speed, chain on/off, song or
  loop, auto-fill and record quantize.
- **HOME** always goes to GRID; tapped twice, to KEYS.
- **The lights say where you are:** a mode key is green on its screen and red on its FUNC screen,
  and the performance pads are always lit in their own colours. The upper pads are printed
  TRK 1-7, so each names itself on the screen when pressed, and FUNC held alone shows what every
  function key and pad does.

**Sound**

- **Eight tracks**, each one machine plus a fixed insert chain; any track can host any machine.
- **Nine machines:** KICK, SNARE (with a clap mode), HAT, PERC (2-operator FM), SAMPLE, GRAIN,
  VA MONO (accent, slide and glide), VA POLY (a chord per step, 13 chord types with inversions)
  and **MID**, which plays an outboard synth over MIDI and sends its knobs as CCs.
- **SAMPLE:** one-shot or 1-16 slices played by note, loop and reverse modes, and **SLC AUTO**,
  which cuts a played break at the hits it finds instead of evenly.
- **Inserts per track:** drive, multimode filter (LP, BP, HP, 12 or 24 dB), lo-fi, amp envelope,
  sidechain duck, pan and level.
- **One LFO per track:** six shapes, free (0.06-32 Hz) or synced (8 bars to 1/32, dotted and
  triplet), on any of the track's 16 parameters, and lockable, scene-able and macro-able like them.
- **An arpeggiator per track:** UP, DOWN, UP/DOWN, ORDER and RANDOM, 1/4 to 1/32T, 1-4 octaves and
  a gate length. It runs on sequenced steps (on VA POLY, through the step's chord) and on held
  KEYS keys.
- **Sends:** a tempo-synced ping-pong delay and a reverb, each with a second mode for ambient
  work: the **LONG** delay (up to 12 s, tape wow, diffusion, low cut) and the **HALL** reverb (a
  large, slowly moving tail with an octave-up shimmer).
- **Master:** a one-knob DJ filter, a three-band **kill EQ** (an isolator with kill switches,
  scene-able), a glue compressor and a limiter.

**Sequencer**

- Up to 64 steps per track, per-track length and speed (polymeter), pattern swing and **per-track
  swing**, micro-timing.
- Parameter locks (8 per step), trig conditions, probability, ratchets, note length, accent and
  slide.
- Live and motion recording with record quantize, a Euclidean generator, KEYS mode for playing in
  a scale, copy and paste, and 16 levels of undo.
- Banks A-H of 16 patterns, queued pattern changes, and a **chain** of up to 64 entries with
  repeats. **Song mode:** each chain entry can recall a mute state, and SONG plays the chain once
  and stops at the end.

**Performance**

- Seven performance pads: FILL, REPEAT (beat repeat), MUTE, SCENE A, SCENE B, THROW (delay or
  reverb) and MUTATE (seeded, keep or revert).
- 16 scenes with a morph between two of them, four macros of up to four targets each, eight mute
  states, and auto-fill.
- **Note repeat** on the pads, with pressure as velocity.
- **Tape stop** and **delay freeze**, both held.
- Pad pressure bends macro A.

**Samples, projects and the card**

- A **sample browser** on the unit for SAMPLE and GRAIN tracks: pick a WAV from `/SAMPLES` or
  `/RECORD` while the beat plays.
- WAV files of 8, 16 or 24 bits or 32-bit float, mono or stereo, at any sample rate. At 48 kHz a
  sample can be minutes long (up to 349 s, within the 32 MiB sample memory); at other rates up to
  30 s at 96 kHz. A file too big for memory **streams from the card** (up to four tracks at once).
- **Resampling:** record 1-8 bars of the master, or of an audio input (line, mic, Hi-Z), to a file
  or straight onto a track.
- **Projects:** autosave, background sample loading, and a **project browser** to open another
  project or save this one under a new name. A **kit library** of 16 kits on the card, shared by
  all projects.
- Everything lives on the SD card under `/ENAGA/DATA/STRATA/`. STRATA never writes the unit's flash.

**MIDI and sync**

- **MIDI out per track** on its own channel: steps, chords, ratchets, arp, pads, KEYS and MIDI in,
  each with its note-off; a second STOP sends All Notes Off.
- **MIDI in:** notes play the tracks and are held until their note-off, CC 16-31 set the track's
  parameters, and a program change queues a pattern.
- Follows the unit's clock: internal, MIDI clock or SYNC IN, chosen in the Enaga menu. Under an
  external clock STRATA refuses tempo edits and follows the transport.

**Factory content and display**

- **The factory project**, on first start and for every new project: a techno kit at 130 BPM in
  C minor, patterns A01 GROOVE, A02 GROOVE+, A03 BREAK and A04 PEAK, a chain that walks them
  (shipped off), scenes DUB, DARK and TENSION, three macros (B ACID, C DUB, D DRIVE; A is left
  free for pad pressure) and eight mute states.
- The audio load on the top line, and the highest load since PLAY (**MAX**) on the bottom line.

### Known limits

- Tested on one SmplTrek on {{RELEASE_DATE}}, with the {{BUILD}} build. {{UNIT_LOAD}} Every other
  load figure comes from the
  emulator, which models no caches.
- VA POLY is the expensive machine: all tracks together share **12 VA POLY voices**. Past that,
  the quietest sounding voice is cut with a short fade. The number is provisional.
- MIDI clock, SYNC in and out, and USB MIDI are Enaga features that STRATA may be the first app to
  use on a unit. Expect rough edges there.
- MIDI out on Enaga 0.2.0: with USB connected to a computer that is not reading the SmplTrek's
  MIDI port, the MIDI OUT jack stops too. Enaga fixed it after 0.2.0 (`192f03f`); until a firmware
  release has it, unplug USB when driving gear from the jack.
- A streamed sample plays forward from its start on one voice: no start, length, slices or reverse.
- There is no delete or rename for projects on the unit: do it on a computer.
- The Lofi-12 XT and CyDrums are the same board, but STRATA has not run on either.
