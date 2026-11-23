# Changelog

All notable changes to STRATA. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org/). Before 1.0, any release may change
the project file format or the controls.

## [Unreleased]

## [0.3.0] - {{RELEASE_DATE}}

An early beta: everything merged since 0.2.0, built for the same Enaga 0.2.0 firmware. The
features below have not been through the unit walkthrough (docs/UNIT_CHECKLIST.md) yet. Release
notes: [docs/release/v0.3.0.md](docs/release/v0.3.0.md). Projects and kits from 0.1.0 and 0.2.0
open in 0.3.0; their VA tracks sound a little different (see Changed).

### Added

- **The performance mixer on VALUE.** Push VALUE and tap STEP 1-15 to pick one of fifteen master
  effects (LP and HP sweeps, HP riser, echo riser, echo freeze, reverb wash, stutter, tape stop,
  bass kill, pump, crush, noise riser, flanger, gate, build); VALUE is then its amount on every
  screen, 0 dry. Push VALUE + LEFT/RIGHT moves to the next effect, push VALUE + CLR turns
  everything off. One effect at a time; nothing of it is saved.
- **A master looper** on push VALUE + STEP 16: it records 1, 2, 4 or 8 bars of the mix (the FUNC
  menu's new LOOP LEN row) from the next bar line, and VALUE crossfades between the live beat and
  the loop. Effects picked afterwards play over the loop. It borrows RESAMPLE's buffer, so
  RESAMPLE and FREEZE wait while a loop is held.
- **A tap on VALUE resets it:** the picked effect's amount, the looper's mix or PERFORM's morph
  goes back to 0; FUNC + tap turns the DJ filter off. A hold of half a second or more resets
  nothing, and the tempo is never reset.
- **WAVE MONO and WAVE POLY**, two new machines: the VA voice with a wavetable oscillator. 16
  built-in tables of 16 frames (CLS, PWM, SYN, FMT, RSW, HRM, FLD, PHD, FM1, TIN, DGT, VOW, GRL,
  EPN, PLK, MLT), POS and MOT (envelope to position), WRP and DET on their OSC page, the ASR
  page, and 8 + 7 factory sounds. WAVE POLY shares the 12-voice POLY budget. A table is built
  the first time a track plays it (until then the voice plays only its sub), so nothing is added
  to the start-up.
- **FM4**, a four-operator FM machine: the eight classic algorithms, operator 4 feedback, an
  index envelope, a detuned second pair and keytracked brightness, for e-pianos, bells, glass,
  brass, organs and FM basses. Six voices with chords, in the shared 12-voice POLY budget, and 8
  factory sounds (EPIANO, BELL, BASS, BRASS, ORGAN, MALLET, PAD, GLASS).
- **The stock SmplTrek folders in the sample browser:** on a card the stock firmware has used,
  the top level adds `POOL/` (`/SmplTrek/Pool/Audio`) and `SMPLTREK/` (the whole tree, stock
  projects included). Read only.
- **The OSC page on VA MONO and VA POLY**, an eighth EDIT page: WAV (saw, square, pulse width),
  OS2 (off, a detuned second saw for chorus and Reese basses, or a hard-synced pulse +0 to +24
  semitones), SUB (sub level) and DRV (drive into the filter). Locks, scenes and macros work on
  all four; kits and scenes save them.
- **More factory sounds**: MNO adds ACIDSQ, FAT, REESE, SYNC and LEAD (12 in all), PLY adds BRASS
  (7). The SOUNDS list scrolls.

### Changed

- **RES works over the whole knob** and self-oscillates near the top (held at a steady level).
  It used to do little for its first three quarters, and LP24 barely resonated at all; LP24 now
  squelches as hard as LP12, so ACID squelches without hunting for the spot.
- **Accent** no longer adds resonance (it made accented notes boomy); it still opens the filter,
  raises the level and brings the noise and (in sync mode) the synced pulse forward.
- **Every MNO and PLY factory sound was remade** for the new voice, and the factory BASS and CHORD
  tracks use the OSC page (saw and sub, no synced pulse): rounder, and cheaper on the CPU.
- **Older projects change sound a little:** their VA tracks load WAV saw, OS2 off, SUB 102,
  DRV 0, without the old always-on synced pulse, and RES's new curve gives more resonance at the
  same knob value. OS2 at 114 (`S+19`) is close to the old bite.
- **Every file that cannot load whole now streams.** Files over 2 MiB are read in 2 MiB chunks
  (which frees 14.5 MiB of heap), and the ones that cannot be held whole, such as a file at
  another rate longer than 30 s, stream from the card instead of some showing `TOO LONG` or
  `NO ROOM`.

### Fixed

- A frozen track or a playing loop could run out of time for up to seven bars after a stop and
  start, a relocate or a tempo change; they now find their place on the next bar line.
- Recording a new looper take no longer cuts the old loop with a click: it plays on until the
  take starts, then fades out.
- A freeze the engine refused could leave FREEZE stuck waiting; it now says `FREEZE REFUSED`.
- WAVE's WRP no longer aliases on high notes.

### Known limits

- Tested on one SmplTrek, with the {{BUILD}} build. {{UNIT_LOAD}} The features above are not yet
  checked on the unit, and the CPU cost of WAVE, FM4, the mixer and the looper there is not
  measured. Every other load figure comes from the emulator, which models no caches.
- WAVE POLY and FM4 share the **12 POLY voices** with VA POLY, across all tracks; that budget and
  the CPU guard's thresholds are provisional.
- The performance mixer and the looper are not saved. The looper borrows RESAMPLE's buffer, so
  RESAMPLE and FREEZE wait while a loop is held, and it has no overdub yet.
- The other 0.2.0 limits stand: streamed samples play forward only, FREEZE is not saved, MIDI
  clock, SYNC and USB MIDI are lightly tested, and on Enaga 0.2.0 unplug USB when driving gear
  from MIDI OUT.

## [0.2.0] - 2026-11-23

An early beta: everything merged since 0.1.0, built for the same Enaga 0.2.0 firmware. It has run
on one unit, but the walkthrough for the new features (docs/UNIT_CHECKLIST.md) has not been done
yet. Release notes: [docs/release/v0.2.0.md](docs/release/v0.2.0.md). Projects and kits from 0.1.0
open in 0.2.0.

### Added

- **Help for the screen you are on.** Holding FUNC alone now shows help for the current screen
  rather than one page for all of them: what A-D do there, named as on the screen and in words
  (`B FMA FM AMOUNT`, `A SRC SOURCE TRACK`), and that screen's gestures (step page, hold-and-press
  LEN, MACRO LEARN, NOTE view, SLC AUTO, rename and delete, KEYS on the pads, the STEP lights).
  Keep holding FUNC and the screen keys (or LEFT/RIGHT) turn its pages; the last three are the
  old page's FUNC + each key and the upper pads, now with FUNC + a knob or pad.
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
- **Factory sounds for one track.** OK in EDIT or MACHINE on a drum, VA MONO or VA POLY track
  opens SOUNDS: 6-7 ready-made sounds per machine (KCK 909, 808, DEEP, PUNCH, DIST, SOFT; MNO
  ROLL, ACID, SUB, PLUCK, GROWL, GLIDE, DRONE; and lists for SNR, HAT, PRC and PLY). OK loads one
  into that track only, with the beat running; AS WAS puts the track's own sound back. Pan, sends,
  lo-fi, LFO, ARP and the fader stay as they were.
- **A05 PAD**, a fifth factory pattern: four bars of held VA POLY chords (Cm9, Abmaj7, Ebmaj7,
  Gm7) that swell in and ring into each other, so the ASR page can be heard straight away. The
  pad is in the pattern's locks; the kit and A01-A04 are unchanged. New projects get it; an
  existing project keeps its own bank A.
- **Note length from the STEP keys:** hold a note's STEP and press a later one: its LEN reaches
  that step. The pair is still a range for CLR, copy, paste and A/B.
- **The step page answers A-D with its STEP still held:** C is LEN there, not a lock on CUT.
- **REC records how long a note was held** into its step's LEN, on SAMPLE, GRAIN, the VAs and MID.
- **Editable sidechain duck.** Three new MIXER pages after the faders: DCK 1-4 and DCK 5-8 set
  how far each track ducks (OFF to 100%), DUCK sets the source track, attack, hold and release.
  Saved in the kit; kits and projects from before open with the old fixed duck (track 1, 35%,
  3/40/120 ms). The delay, reverb, master and EQ pages are now three screen-key presses further on.
- **FREEZE a track** (FUNC menu, FREEZE): the track plays a 1-8 bar loop of its own machine from
  RAM, so a held pad or a dense GRAIN costs almost nothing while its insert chain, mix and sends
  stay live. Unfreeze is instant. Not saved: a reload, NEW or kit recall unfreezes.
- **Bounce one track:** in RESAMPLE, B picks `TRK 1`-`TRK 8` as the source, and the take is that
  track alone, mono, before its duck, fader, pan and sends.
- **64 MiB of sample memory for every project**, older ones included (was 32 MiB), so fewer files
  need to stream from the card.
- **KEYS on the pads:** the two pad rows play as a piano keyboard, with velocity, held notes and
  octave pads.
- **CLEAR PATTERN** in the FUNC menu: every track of the pattern in one press (OK twice), one
  undo. Sounds, lengths, speeds and swing stay.
- **NOTE view:** TUN and PIT read as a note name and turn a semitone a click; push the knob for
  the plain 0-127 value.
- **MACRO LEARN:** assign a parameter to a macro by pushing its knob in EDIT. The macro page says
  `FULL` and `n/4`, pressing the same STEP again removes a parameter, and EDIT shows
  `HELD BY MACRO` on a parameter a macro is holding.
- **STEP lights:** a trig's green shows its velocity, and a step with a condition blinks.
- **Track pads hold their note** while pressed when a VA's SUS is up.

### Changed

- **MACHINE: LEFT/RIGHT now move over the machine grid** (a column of three), the way UP/DOWN do.
  They used to change the track, so turning RIGHT for the next machine changed another track's
  machine. The track is on the pads and the screen keys, which now say `TRACK 2` and so on.
- **Morph at rest leaves the sound to the EDIT knobs.** After OK captured a scene, the morph held
  the capture over the kit even at position 0, so an EDIT knob moved the screen but not the
  sound. Now position 0 writes nothing, which also means parking the morph at 0 no longer
  recalls scene A.
- **The VA voice runs its three passes unfused** for now: cl6x rendered the fused loop slightly
  differently from GCC, so it is off until that is understood. It costs about 1.7 points of load.

### Fixed

- OK in RESAMPLE no longer retargets a take that is still being written.
- The FUNC menu no longer shows a MUTATE verdict that OK would not give.
- MACRO LEARN is easy to leave, and leaving it never clears the track.

### Known limits

- Tested on one SmplTrek, with the cl6x build. On the 2026-11-22 build the load stayed under 30% on the default beat, and MAX peaked at 84.3% in long, heavy play with no clicks or dropouts. The features above are not yet
  checked on the unit. Every other load figure comes from the emulator, which models no caches.
- The **CPU guard's** thresholds (70% average, 90% in one block, back below 55%) are provisional;
  nothing in the emulator reaches them.
- **12 VA POLY voices** are still shared by all tracks, and held ASR notes count against them for as
  long as they hold.
- A streamed sample honours LEN, but still plays forward only: no START, slices or reverse
  (docs/FEATURES.md, "Why streamed samples play forward only").
- FREEZE is not saved with the project.
- The other 0.1.0 limits stand: MIDI clock, SYNC and USB MIDI are lightly tested, and on Enaga 0.2.0
  unplug USB when driving gear from MIDI OUT.

## [0.1.0] - 2026-11-22

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

- Tested on one SmplTrek on 2026-11-22, with the cl6x build. The audio load stayed under 30% on the default beat, and MAX, the highest load since PLAY, peaked at 84.3% during long, heavy play with big effects and a lot going on, with no clicks or dropouts. Every other
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
