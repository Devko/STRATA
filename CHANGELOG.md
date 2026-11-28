# Changelog

All notable changes to STRATA. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org/). Before 1.0, any release may change
the project file format or the controls.

## [Unreleased]

### Added

- **Two reminders on the screen.** A quick tap of FUNC alone says `HOLD FUNC: HELP`, and a tap of
  OK in GRID says `HOLD OK: TRACK MENU`. Both keys did nothing visible on their own, and nothing
  on the panel pointed at the FUNC help or the GEN and EVOLVE menu. Neither covers another notice.
- **JAM REC: the whole jam to the card.** The FUNC menu's last row (FUNC+HOME, UP, OK) records
  everything STRATA plays to `/RECORD/JAM_<n>.WAV` until OK again: 48 kHz 24-bit stereo, about
  1 GB an hour, up to 3 h 51 min. The finished master as RESAMPLE hears it, never the click or
  the sample audition. A dot blinks in the top bar while it runs and the row counts the time. A
  full card or the length limit closes the file cleanly and says so; switching off closes it
  too. RESAMPLE, freeze and the looper still work meanwhile. Every FUNC menu row counted
  upwards from SAVE is one further on (LOAD DEMO is UP twice now). New emulator scenario
  `jamrec`.
- **ENERGY, one knob from breakdown to peak.** In PERFORM, FUNC + turn C (-64 .. +64, rest at 0
  with a detent). Down, every track drops its weakest hits first, in an order: hats at once, then
  percussion, snare and samples, the lines, the pads (which keep their strongest hits), and the
  kick last. Up, empty off-beats get quiet ghosts with the sound of the hit before them (hats most,
  the kick only near the top, pads never; one note on an arp track, lighting no step or glow) and
  probabilities lift towards 100. Each position plays the same groove every bar; the pattern is
  never written, and a LOCK holds it. New emulator
  scenario `energy`.
- **Starter content in the card builder.** strata.einblick.dev can now put something to play on
  the card with STRATA: ten starter projects to open and jam on (TECHNO, DEEP, DUB, DRIFT,
  ELECTRO, NIGHT, BREAKS, STEEL, SMASH, CHAOS; for 0.5.0 and newer), a sample pack of 105
  one-shots and chords rendered by STRATA's own machines in `/SAMPLES/STRATA`, and a one-page
  quick start, `STRATA-QUICKSTART.txt`, all behind one tick box; nothing in the app changed. The
  projects live in `content/starter/` and a host test opens every one on each build.
- **Nine showcase projects beyond techno** (`tools/showcases/`): DEEP (deep house), DUB (dub
  techno), DRIFT (ambient), ELECTRO, BREAKS, CHAOS, SMASH, NIGHT (breakbeat over synthwave) and
  STEEL (straight breakbeat), each written by a generator with core/'s own serializers, factory
  sounds only, opening with the chain on. `tools/showcases/build.sh` makes the cards (and with
  `--render` the arrangement renders); `--starter` copies them into `content/starter/projects/`
  for the card builder.
- **16th-note triplets as a track speed.** TRACK SPEED (FUNC menu) has a new **3/2** between 2
  and 1: a step every 16 ticks, six to the beat, so 24 steps make a bar. Set LEN 24 for a one-bar
  triplet hat. The arp, rolls over LEN and REPEAT already had 1/16T. No file change: a build from
  before this one plays a 3/2 track at 1x. New emulator scenario `triplets`.
- **Stereo width on the POLY machines.** VA POLY, WAVE POLY and FM4 can spread their chords
  across the stereo field or play each note in unison: two voices panned hard left and right,
  detuned 6 to 40 cents apart. One knob per track on two new MIXER pages, WID 1-4 and WID 5-8
  (after DUCK): 0 mono, 1-63 spread, 64-127 unison; a push steps OFF, spread, unison. Unison
  holds three notes a track and counts two voices a note against the shared 12. Saved in the kit,
  in a new optional KIT.BIN section; older kits open mono. MIXER's DLY to EQ pages are two
  further on. Turning WID up from 0 under a ringing filter does not click, and RESAMPLE from a
  wide track records it before the duck, as from a mono one. New emulator scenario `width`.
- **A chorus, the third send.** MIXER has three new pages after ROUTE: CHO 1-4 and CHO 5-8 set
  how much of each track goes to the chorus (push a knob for OFF or half), and CHO sets its sweep
  rate, depth, return and how much of it feeds the reverb. One stereo chorus serves every track
  and costs nothing while no track sends to it. The sends are saved in the kit, in a new optional
  KIT.BIN section; the four knobs at the end of PROJECT.BIN's mixer section. Older projects and
  kits open with no track sending; an older build ignores both. MST and EQ are now three pages
  further on (one SELECT_LEFT from TRK 1-4 still reaches EQ). New emulator scenario `chorus`.

### Fixed

- **The CPU guard no longer thins chords that were never in danger.** With generated parts on
  every track the unit read 40-60% and showed `GUARD1`-`GUARD4`, shedding POLY voices and grains
  with no block anywhere near an overrun. The guard averaged the load over 11 ms, which follows
  each step's burst of note starts to its top; it now averages over 85 ms, and one busy block no
  longer restarts the second it waits before stepping down. A single block at 90% still raises
  it at once. New emulator scenario `genload`: GENERATE at full density on all eight tracks.

### Changed

- **Sample audition with the beat playing takes two pushes.** In SAMPLES, push A while the
  transport runs only says `A AGAIN TO HEAR`; a second push on the same file within 2 s plays
  it. The SmplTrek has no cue output, so the audition goes out with the beat, and one bump of
  the knob in a set should not put a file into the room. Stopped, one push hears it as before.
- **The clap sounds like a clap.** Its noise moved from 5-10 kHz down to about 2-3 kHz, and each
  of its bursts now rings until the next one strikes instead of dying after a third of the gap,
  so the flutter is as loud as the tail rather than 10-18 dB under it. The factory kit's CLAP and
  the CLAP, SHORT and CLAP4 sounds have the new band; CLAP4's bursts are 14 ms apart, not 40.
  A clap in a saved kit keeps its knobs, so it gets the livelier bursts but not the new band.
- **The factory kit is rebalanced.** The bass sat 9.5 dB and the chord 7 dB under the kick while
  the hats were nearly as loud as it. New faders: KICK 0.8, CLAP 0.85, both hats 0.7, PERC 1.2,
  BASS 1.5, CHORD 1.25. The bass now sits about 6 dB under the kick and the chord about 3, and
  the share of the mix between 250 Hz and 4 kHz goes from 14% to 22%. A new project starts
  with these; a saved project keeps its own faders.
- **Flipping through SOUNDS no longer jumps in level.** Within one machine the presets were up
  to 14 dB apart (VA MONO LEAD 12 dB louder than GROWL, PERC CHIME 10 dB louder than CLAVE).
  28 presets have a new LVL, which brings most machines to within 3-6 dB. VA MONO GROWL, WAVE
  MONO RESO and GRAIN STUTTR, CHOPS and GLITCH stay quieter: their LVL was already at the top.
- **Accented notes on VA MONO, VA POLY and the WAVE machines no longer tick.** An accent opened
  the filter by half as much again plus 10 semitones, which on a wide, resonant patch such as
  ACID swept it to about 18 kHz for a few milliseconds. It now opens at most 18 semitones past
  the plain note (ACID's accent peaks near 6.5 kHz), and its attack is 1.5 ms instead of 0.6.
  Accents on patches with a modest ENV sound as before.
- **FM4 EPIANO has a strike.** It was a mellow, organ-like tone with almost nothing above 1 kHz
  even in its attack; now it is bright as it is struck, barks when hit hard, and mellows as it
  rings (IDX 95, IEN 127, VEL 120, FBK 10). It is also what a track switched to FM4 starts with.

## [0.5.0] - 2026-11-27

The release candidate for 1.0: everything merged since 0.4.0 (#180 to #208). From here to 1.0
come fixes, measurements and docs, not features. It is the first release for **Enaga 0.4.0**, so
updating from 0.4.0 means new firmware as well as the app, and the first whose files 0.4.0 cannot
fully read: a project saved by 0.5.0 plays its LFOs wrong on 0.4.0 (see Upgrading from 0.4.0).
The features below have not been through the unit walkthrough (docs/UNIT_CHECKLIST.md) yet.
Release notes: [docs/release/v0.5.0.md](docs/release/v0.5.0.md).

### Upgrading from 0.4.0

- **Firmware: Enaga 0.4.0.** Install `SmplTrek.bin` from
  [Enaga's v0.4.0 release](https://github.com/seawaffle/enaga/releases/tag/v0.4.0), then replace
  `/ENAGA/APPS/STRATA/APP.ENA` (the card builder fetches the matching Enaga). 0.4.0 follows MIDI
  clock over USB MIDI too, keeps the right tempo after a long pause in the clock, spends less of
  each audio block on itself, writes FAT32's free-space record correctly and reads and writes the
  card faster. Nothing in STRATA's controls changes with it.
- **Your projects and kits open as they are** and sound as before, with three exceptions: FM4 at
  high IDX on high notes is a little less bright (it no longer aliases), a track with DRIVE up
  loses a DC offset it should never have had (the factory kicks peak about 0.6 dB lower), and
  the arp's RND, with RATCHET's RND2 and RND3, draws new notes on every pass instead of repeating
  one fixed pattern. A choked or DRV-locked track no longer thumps or clicks (#197).
- **Older projects are converted once, on load.** The LFO's WAV and DST are now stored as
  numbers (format decision C, #205): projects, kits and patterns from 0.4.0 and earlier,
  their scenes, macros and step locks included, are converted when they open and sound the same.
  New settings went where older builds do not look: choke groups and the arp's VAR, RHYTHM,
  ACCENT and RATCHET in two new optional KIT.BIN sections (11 and 12), REC MODE in PROJECT.BIN's
  unused swing byte, a recorded chord's extra notes in the step's locks, a roll over LEN in the
  ratchet byte, EVOLVE in a new optional PROJECT.BIN section (7).
- **A project saved by 0.5.0 plays its LFOs wrong on 0.4.0** and earlier: they read the new
  numbers as the old bands, so a shape or a destination comes out different (mostly a sine
  going nowhere). Keep a copy of any project you may want to open in 0.4.0 again before you save
  it in 0.5.0. Apart from the LFOs, 0.4.0 opens 0.5.0's projects and loses only the new settings:
  no choke groups, the arps without VAR, RHYTHM, ACCENT and RATCHET, a recorded chord as its
  first note, a roll over LEN as a one-step ratchet, and nothing evolving.
- **No gesture moved.** Everything 0.4.0 did, it still does the same way. Holding OK in GRID,
  which did nothing, now opens the track menu (GEN and EVOLVE); a tap still does nothing.

### Added

- **MIX, and EVOLVE is saved**: MIX, after NOTES in the MUTATE setting, draws a different
  mutation each EVOLVE turn (and each MUTATE press). A project now saves each track's EVOLVE, the
  mode, the mutation and the amount, in PROJECT.BIN's new optional section 7, and opens evolving
  from the pattern as saved; an older project opens with nothing evolving (#200).
- **GEN and EVOLVE**: hold OK in GRID for the track menu. **GENERATE** writes a new part on the
  selected track: a rhythm weighted to the beat with accents and ghosts, and on a melodic track a
  line in the pattern's key over 1-3 octaves (DENSITY, RANGE, LEAP; WHAT keeps the rhythm or the
  notes). **EVOLVE** mutates the track on its own every 1, 2, 4 or 8 loops with PERFORM's MUTATE
  setting: **ORBIT** varies around the part as it was, **DRIFT** lets the changes add up; KEEP and
  HOME stop it. GEN is one undo level (#195); since #200 a project saves its EVOLVE settings.
- **Chords recorded note for note.** On VA POLY, WAVE POLY, FM4 and MID, every note played into
  one step while recording is kept: the step's note plus up to five more, stored as locks 32-36
  in the pattern's pool, so no file change and an older build plays the step's own note. Until
  now only the last key pressed was kept. A new **REC MODE** row in the FUNC menu, after
  QUANTIZE, says what a later pass does: **REPLACE** (the default: the first note of a pass on a
  step takes it over, chord type and all) or **ADD** (notes join the step's chord). The step
  page shows `NOTE C4 +2`; transpose, mutate and NOTE move the whole chord; the last key up of a
  chord sets its LEN. Saved in PROJECT.BIN's old swing byte. Mono and drum tracks are unchanged
  (#183).
- **CLICK and COUNT-IN** in the FUNC menu: a metronome on every beat (higher on a bar's
  first), and a count-in of 1 or 2 bars before a PLAY with REC on, so a take starts on step 1
  after the clicks. Neither is recorded nor saved; under an outside clock there is no count-in
  (#182).
- **Choke groups** for any track: push A-D on MIXER's TRK 1-4 / TRK 5-8 pages puts that track in
  group 1-4 (`C1`-`C4` under its strip), and a note on a track cuts the other tracks of its group
  with a short fade. Saved in the kit; older kits open with no groups (#181).
- **Ratchets over LEN**: on the step page, RTCH goes on past 8 into rolls, `ROLL/64` to `ROLL/4`:
  the step rolls at that rate for its whole LEN (1/32 over 16 steps is 32 hits), with RAMP across
  the roll for build-ups. No file change; an older build plays such a step as a one-step ratchet
  (#185).
- **Note repeat is recordable**: with REC on, a held NOTE RPT pad records one step, a roll over
  LEN at the repeat rate, its LEN from the hold (1/4T records a plain hit) (#187).
- **Arp depth**: push A-D on the ARP page for VAR (converge, diverge, thumb and pinky pedals),
  RHYTHM (eleven rest patterns, Euclidean ones among them; a rest holds the note, so the run turns
  against the bar), ACCENT (seven patterns and CYC, each run's first note) and RATCHET (2-4
  repeats on fixed slots or by chance). Kit settings in KIT.BIN's new optional section 12; an
  older kit plays its arps as before. The ARP picture draws rests, accents and ratchets (#184).
- **ARPS**: OK on the ARP page opens fourteen factory arps (TRANCE, ROLLER, ACID, BLEEP, DUB,
  OFFBT, THUMB, PINKY, CONVRG, RATCHT, FILL, EUCLID, AMBNT, DRIFT), loaded into one track with the
  beat playing, AS WAS first (#184).
- **40 more factory sounds**, techno and ambient, on every machine: 116 in all (docs/MANUAL.md
  has the table) (#184).
- **The card builder**, [strata.einblick.dev](https://strata.einblick.dev): pick a release and it
  builds one SD card zip with STRATA and the Enaga firmware it needs, both fetched in your
  browser from their official releases and checked against their SHA-256. Source in `web/`
  (#180).
- **A one-hour smoke test** (S1-S20) at the top of `docs/UNIT_CHECKLIST.md`: boot, the small
  font on the OLED, load on A01 and A04, LOCK, the MOD slots, one RESAMPLE take and a save that
  survives a power cycle, before the two-hour walkthrough (#186).
- **A clip at the top of the README**: 28 silent seconds of the device build in the emulator
  playing the TECHNO project, recorded by `tools/readme_clip.py` into `docs/media/strata.gif` (#207).

### Changed

- **KICK costs less**: its click layer stops once it has decayed under -110 dB instead of running
  for the whole note. The output moves by at most -129 dB (inaudible; not bit-exact). The
  default beat 24.1% -> 23.2% on the emulator (#208).
- **Hats and WAVE tracks cost less**: the hat's two filters and its VCA run as one loop, and WAVE
  finds its table frames without a run-time recursion; the same sound to the last bit. The
  default beat 24.3% -> 24.1%, a WAVE POLY pattern 27.9% -> 26.6% on the emulator (Enaga 0.4.0) (#206).
- **File format: the LFO's WAV and DST are numbers** (format decision C): a shape or a
  destination is stored as its number rather than as a band of 0-127, so new shapes and
  destinations can come later without changing what any saved project sounds like. Projects,
  kits and patterns from earlier versions are converted on load, scenes, macros and step locks
  included, and sound the same. **A project saved by this version and opened on an earlier one
  plays its LFOs wrong there** (mostly a sine going nowhere). The lock and scene masks behind
  every parameter are 64 bits now, room for a page past the 32 bytes a track has (#205).
- **VA voices cost less**: the filter, envelope and output of every VA MONO and VA POLY voice
  run as one loop the TI compiler can pipeline, the same sound to the last bit. About 1.5 points
  off every pattern that plays them (the default beat 26.6% -> 25.1% on the emulator) (#203).
- **The LONG delay and the HALL reverb cost less**: their per-sample loops now run as passes the
  TI compiler can pipeline, the same sound to the last bit. The worst-case scenario (both sends
  in their ambient modes) drops from 32.3% to 28.6% peak load on the emulator (#196, #202).
- **FM4 no longer aliases at high IDX.** The brightness cap now sees the whole operator chain:
  in a stack the sidebands multiply, so ALG 1 at IDX 90 on middle C and above folded harsh,
  out-of-tune tones back into the audible range, about 10 dB under the note. Now they are 45-56 dB
  down under 5 kHz. Low notes sound as before; high notes at high IDX are somewhat less bright.
  `make -C host probe` and `tools/sound_probe.py` measure every synth for aliasing, tuning,
  resonance, DC and clicks (#189).
- **DRIVE no longer pushes the sound off centre.** The insert drive clips the two halves of the
  wave differently (that is its even-harmonic grit), which also shifted a held note off zero, by
  up to 7% of full scale. A DC blocker (an 8 Hz cut) now follows it: more headroom, no thump when
  a driven track is muted or choked, and nothing off-centre fed into the sends. The factory kicks'
  peaks drop from -3.2 to -3.8 dBFS; what changed is almost all below 20 Hz (#191).
- **The arp's RND is random on every pass** (and RATCHET's RND2 and RND3 with it): each run draws
  a new seed. It used to play one fixed pattern for a given track and step (#190).
- Internal: the engine's command handling is split into twelve handlers, one per family of
  commands, with no change in behaviour; the host tests no longer depend on the order they run in,
  and `tools/test_shuffle.sh` runs them in shuffled orders to keep it so (#186).
- Internal: the emulator scenarios take 638 s instead of 918 s (a build cache, a decode cache,
  the longest first); `make -C host warn` compiles everything at -O1, where CI's sanitizer build
  finds warnings, and CI caches its compiles (#192, #194).
- Internal: in a thread's worktree the scenarios find the main checkout's emulator, and the
  runner refuses to run without one instead of counting skips as passes; the release script
  and the Enaga pin check work there through Windows' git and gh; CI parses every script in
  tools/ and tests/ (#201).

### Fixed

- **A card pulled while a project opens** no longer leaves half of one project and half of the
  other filed under the new name, where the next save wrote the mix over it: the name goes
  back, and the open runs again from its first file when the card returns. Leaving the app
  during a load, or before the boot's load has run, no longer autosaves over the project the
  next start opens (#188).
- **A kit recalled from the library** brings back its SLC AUTO, LFO TRIG, MOD slots, choke
  group and arp settings; only the knobs, machine, fader and duck came back (#184, #188).
- **DOUBLE or undo on the playing pattern** moves the end of its pass too: a chain entry or a
  song ended at the old length (#188).
- **Recording after a chain step or a queued pattern change** puts the note on the step that
  was playing, not one counted from the song's start (#188).
- **A STEP range** ends when you change track or screen. Before, a plain CLR, copy or turn on
  another track (or in EDIT, where `SEL` is not shown) acted on that range there (#188).
- **SAMPLES read again (FUNC+UP)** keeps the cursor on the list when it comes back shorter
  (#188).
- **A step pulled before tick 0** on a one- or two-step track with micro and FEEL no longer
  fires twice there (#188).
- **The UI tests' screenshot walk** opens SAMPLES from EDIT's SRC page again; CI had been red
  since #184 (#193).
- **A choked track** no longer replays its filter's ringing and its drive's DC as a thump on its
  next note: a choke clears the track's insert chain as every other cut does (#197).
- **DRV back on after a DRV lock of 0** (or a turn through 0) starts its DC blocker from rest; it
  carried over the sample from where the drive last ran, a click at the step (#197).
- **REC MODE REPLACE after a pattern change**: the first note recorded onto a chord step of the
  new pattern takes the step over. A note recorded on the same step in the old pattern's same
  pass made it join the chord instead (#197).
- **COUNT-IN**: a downbeat played up to half a step early, before the last click ends, records
  on step 1. It was dropped (#197).
- **COUNT-IN**: a hit in the moment between PLAY and the first audio block no longer records
  onto step 1 in the middle of the count-in (#197).
- **A held NOTE RPT pad joining a chord step** (REC MODE ADD, or a later note of the same pass)
  makes the step roll; the roll was dropped and only the note kept (#197).
- **CLICK over a loud mix** is held to full scale, as the sample audition is (#197).

### Known limits

- Tested on one SmplTrek, with the cl6x build. On the 0.2.0 build (2026-11-22) the load stayed under 30% on the default beat, and MAX peaked at 84.3% in long, heavy play with no clicks or dropouts; builds from main run on the unit (2026-11-27), but 0.5.0's load is not measured on the unit yet. Nothing added since 0.4.0 is
  checked on the unit yet, and the release candidate's unit test (the smoke test and the
  walkthrough) is what decides 1.0. Every other load figure comes from the emulator, which
  models no caches.
- **Enaga 0.4.0 exactly.** Enaga's loader checks only the ABI number, which 0.2.0, 0.3.0 and
  0.4.0 share, so 0.5.0 on older firmware loads and may misbehave.
- **Projects saved by 0.5.0 play their LFOs wrong on 0.4.0** and earlier (see Upgrading from
  0.4.0). From 1.0 on, every build reads every 1.0 file.
- The 12 POLY voices shared by VA POLY, WAVE POLY and FM4 and the CPU guard's thresholds are
  provisional until the unit's readings; the performance mixer, the looper, LOCK and LIVE RND
  are not saved; the looper borrows RESAMPLE's buffer; streamed samples play forward only;
  FREEZE is not saved; MIDI clock, SYNC and USB MIDI are lightly tested.

## [0.4.0] - 2026-11-26

An early beta: everything merged since 0.3.1 (#137 to #177), and the first release for **Enaga
0.3.0** firmware, which updating from 0.3.1 now needs (see Requires). The features below have not
been through the unit walkthrough (docs/UNIT_CHECKLIST.md) yet. Release notes:
[docs/release/v0.4.0.md](docs/release/v0.4.0.md). Projects and kits from 0.1.0 to 0.3.1 open in
0.4.0; their VA and WAVE tracks load their insert filter as the nearest TON, and some gestures
moved (see Gestures that moved).

### Requires

- **Enaga 0.3.0 firmware.** STRATA is now built against Enaga v0.3.0, whose host table has one
  more entry than 0.2.0's, so install the 0.3.0 firmware with this build. Enaga's loader checks
  only the ABI number, which did not change, so an older firmware would load it and misbehave.
  With 0.3.0, MIDI OUT on the DIN jack no longer stalls while USB goes to a computer that is not
  reading.
- **Enaga is no longer in the release zip.** The zip holds the STRATA app only. Install Enaga
  0.3.0 from Enaga's own release, <https://github.com/seawaffle/enaga/releases/tag/v0.3.0>
  (the SmplTrek zip, `SmplTrek.bin`, HOME held at power-on, SYSTEM UPDATE); the zip's
  README.txt has the steps. Earlier zips carried a copy of Enaga 0.2.0's firmware.

### Added

- **SAMPLE holds notes.** SAMPLE reads SUS and REL on the ENV page, as the synths do: with SUS
  up a sample holds for the step's LEN or while its pad is held (a looped sample becomes a pad), and
  REL sets its tail from the old 30 ms declick up to 8 s. ATK stays on AMP. At SUS and REL 0 a
  track sounds exactly as before.
- **Hear a sample before you load it.** In the sample browser (SAMPLES), **push A** plays the
  file under the cursor, whole and from its start, straight to the output at about -6 dB; push A
  again stops it. While one plays, UP/DOWN (or turning A) hears the next file the cursor lands
  on, so a folder can be listened through before anything changes in the track. OK still loads,
  and stops the audition; CLR, another folder or leaving SAMPLES stops it too. The file is
  streamed from the card, so a long stem starts as quickly as a hi-hat and takes no sample
  memory; it plays at its own rate (44.1 kHz stays in tune), is never recorded by RESAMPLE, the
  looper or beat repeat, and works with the beat playing or stopped. The line under the title
  says `HEARING 3/12S`, and a line under the file grows as it plays. 0.28 MiB of RAM.
- **Two MOD slots per track.** Each track has two slots, each a source moving one parameter by a
  depth: **LFO** (the track's LFO, which can stay at DEP 0 and move only the slots), **ENV** (the
  LFO's shape and RAT once per note), **VEL**, **PRS** (the pad's pressure) and **RND** (a new
  value per note). Learnt like the macros: on the LFO page push C (slot 1) or D (slot 2), push
  again for the next source, turn the knob you want moved by as much as you want it moved, OK.
  The knob goes back and the slot keeps the difference. The LFO page lists the slots; on EDIT a
  dim line under a bar shows a slot's range. A cutoff stops at the ~100 Hz floor. Stored in the
  kit (KIT.BIN grows to 1,389 bytes); older projects load with both slots off.
- **Random lock ranges and parameter slide.** Hold a STEP and FUNC and turn a knob: that
  parameter gets a random range on the step, and each pass plays a new value within it (`R20` on
  the lock line, a dim line under the bar). Hold a STEP and FUNC and push a knob: the step's lock
  glides to whatever the next step that plays holds, arriving as that step starts (`SLIDE ON`,
  `/` on the lock line). A plain push with the STEP held still clears the parameter, range and
  slide included. Both are stored in the lock itself, so patterns keep their format; an older
  build ignores a range and a sliding lock.
- **LIVE RND (P1, the live half).** In PERFORM, FUNC + turn D shuffles every armed track's steps
  in random blocks of 1, 2, 4, 8 or 16, a new pick every pass (`LIVE RND4 8 TRK`); FUNC + push D
  takes the selected track out or back in, so the kick can hold while the rest jumps. The same
  draw as TRACK PLAY's RND modes, over each track's own mode while on; the pattern is never
  written, so OFF is the programmed beat again at once. Not saved, held by a LOCK, shown as
  `LIVE RND4 FUNC+D` on PERFORM, a dot under each armed track's meter and `RN4` on line 2.
- **The screens rebuilt to the mockups.** The frame: a lit pattern badge and track badge, the
  tempo, load and LOCK in the small font, notices as a lit toast, the live strip only while
  something is off rest, and page tabs along the bottom (TRK, MAX and the device moved to SETUP;
  DROP and GUARD still show at the right). PERFORM: four gauges named by what they move, a big
  OUT scope in an afterglow, tall track columns (scenes and MUT say themselves in toasts). GRID:
  taller rows with each track's number and level, the playhead's column lit, the knobs in the
  small font. EDIT: the knobs two to a line so the graph gets the screen; the drums' FLTR draws
  the filter's response over the output's spectrum, the LFO's dot leaves a trail, WAVE shows a
  LIVE frame under its waterfall, GRAIN a cloud panel. MIXER MST and EQ: a big spectrum, a
  vectorscope, the compressor and limiter as bars.
- **Live screens, part 6: the master's spectrum and a vectorscope.** On MIXER's MST and EQ pages,
  while the beat plays, the output's spectrum in 24 bands with peak dots and a small vectorscope;
  on the fader pages a live meter beside each fader.
- **Live screens, part 5: WAVE's table as a waterfall, GRAIN's grains as sparks.** On WAVE's
  SRC page the table's sixteen frames are stacked like CyDrums' OSC page, the frame POS plays
  brightest. On GRAIN's SRC page each grain also leaves a fading spark over the sample.
- **Live screens, part 4: the LFO page shows its LFO.** While an LFO runs, EDIT's LFO page draws
  one cycle of its shape with a dot riding it at the LFO's phase, and a strip of what it put out
  over the last two seconds. Under every envelope (AMP, ASR, FLTR) a dim fill, lit under the part
  the playing note has travelled.
- **Live screens, part 3: GRID and PERFORM move with the music.** On GRID each trig that
  plays glows round its cell and fades, a trail behind the playhead. On PERFORM the macros are
  round gauges over a dim, glowing scope of the output, and a row of eight track meters runs
  under them while the beat plays. Stopped, both screens hold still.
- **Live screens, part 2: the audio taps (nothing new on screen yet).** Every block the audio
  side now publishes each track's level, each LFO's phase and the finished output (every second
  frame, into a ring the screen drains) for the scopes, meters and moving dots of the next parts.
  About +0.3 points of load (docs/BUDGET.md).
- **Live screens, part 1: the drawing kit and a livelier frame.** The master meter reads in
  decibels (the top 48 dB) and in segments, brightest in the top 6 dB, so a quiet mix moves it;
  the progress line marks the beats and bars still to come and lights its head. SETUP shows what
  a frame costs to draw (`DRW 0.85MS`). Underneath, `app/paint.hpp`: anti-aliased lines, arcs,
  soft dots, an afterglow and a 3 x 5 font, which the next parts use for scopes, spectra and
  moving envelopes.
- **LFO TRIG (S2).** Push RAT's knob (B) on the LFO page for **FREE** (as before), **RETRIG**
  (every note on the track starts the cycle again) or **ONE-SHOT** (every note runs one cycle and
  the LFO stops: a second envelope you can point at any parameter). In ONE-SHOT the shapes are
  envelopes that start and end at the knob: TRI rises and falls, SAW decays from DEP, RMP rises
  and holds, SQR is a gate for half the cycle, SIN one wobble, S+H a new random value per note.
  RAT is the time (free) or the note length (synced). The page's last line ends `RETRIG` or
  `ONE-SHOT`. Stored in the kit; older projects load every LFO as FREE.
- **REL ALL and LEN ALL (P4).** In PERFORM, FUNC + turn A moves every track's release at once
  (DEC on every machine, REL on the ASR page where there is one, and a step's lock on them),
  -64..+64 (`REL ALL -12`); FUNC + turn B scales every sequenced note's length, 25-400%
  (`LEN ALL 71%`). Rest is 0, a detent, and the programmed sound exactly. Live like the macros:
  not saved, held by a LOCK, shown in the live strip as `RL-`/`RL+` and `LN-`/`LN+`.
- **FEEL (P8).** A timing drift over swing and micro-timing, on the PATTERN page after SWG (D
  turns it): DRIFT 1-6, each track on its own smooth random curve, or WAVE 1-6, the whole groove
  along one four-bar curve, laid back then pushed. Up to 6/96 of a beat, following the tempo,
  seeded so it repeats after a reload. Saved per pattern in a new optional section; older
  projects load with FEEL OFF, and older builds open new ones and play them straight.
- **Macro learn by turning (D11).** In PERFORM push a macro's knob: EDIT opens in LEARN
  (`LEARN B 0/4 TURN KNOBS`). Every knob you turn there, on any page and any track, goes on the
  macro, from where it was (the macro's 0) to where you leave it (its 127). OK, CLR, HOME or
  leaving EDIT ends it (`B: 2 PARAMS`) and the knobs snap back. A push takes a parameter off.
- **The STEP keys are steps in PERFORM and KEYS (D12)**, as on GRID: tap, hold + knob, hold + OK.
  KEYS plays on the pads, so you can play and place steps at once.
- **Hold STEP + an upper pad edits that step (D6):** FILL steps its FILL condition, REPEAT its
  ratchet, MUTE an A:B condition, MUTATE its probability (`STEP 5 PROB 50%`). STEP first edits,
  pad first performs.
- **Hold FILL + tap STEP** writes a FILL trig on the selected track (`FILL TRIG`); again takes it
  off.
- **Hold OK on a PROJECT slot: the pattern menu.** COPY, PASTE, CLEAR and DOUBLE for the pattern
  under the cursor; DOUBLE doubles every track up to 64 steps and copies the steps and locks into
  the new half. A tap on OK still queues the slot. Shortcuts on the slots: FUNC + UP copies,
  FUNC + DOWN twice pastes, FUNC + LEFT twice clears.
- **LOAD DEMO in the FUNC menu** (OK twice): the factory demo, the beat a first start plays.
- **FUNC + pad-row key held repeats** the length change: after 0.4 s, 20 steps a second.
- **The screen frame.** Every screen has the same top line, `A01 3.2 130.0 > 24%` (pattern,
  bar.beat, tempo, transport, load), a one-pixel progress line under it, a live strip on line 2
  listing what is off rest (`K:L FX4 S40 M:AB LP`), and a master meter down the right edge with a
  held peak and a limiter light.
- **Locks on screen.** While a STEP is held, GRID's and EDIT's bars show what that step plays,
  with a hollow tick at the kit value, and a line reads `S5 CUT 64>90`. GRID's cells show what
  each trig carries: a dot for locks, cut corners for a condition or a probability, two teeth
  for a ratchet, an outline for a trigless step. PERFORM's macro bars include pad pressure.
- **Bounce to track.** A RESAMPLE take to FILE is offered to a track once it is on the card
  (`PUT ON TRK 3 GRN`): one OK makes it that track's sample (`T3 GRN: TAKE ON`), LEFT/RIGHT pick
  another track, CLR keeps it a file. It offers a SAMPLE or GRAIN track other than the one you
  bounced first, so layering two tracks into a GRAIN or a sliced SAMPLE is B to `TRK n`, OK,
  PLAY, OK.
- **Hold STEP + THROW (D6's last pad).** STEP first edits: with a STEP held, THROW locks that
  step's delay send at full (`STEP 5 DLY FULL`), and again takes the lock off; FUNC + THROW does
  the same with the reverb send. A throw written into the pattern, saved and undone like any lock.
  THROW pressed first still throws the whole track while held.
- **Step stutter: hold REPEAT, then a STEP (P2).** Every track loops that step at the repeat
  rate, on the beat grid, until you let go (`STUTTER 5 1/16`); FUNC + STEP stutters only the
  selected track. It replays the sequencer, not the audio, so filters, LFOs and tails keep
  moving, and the beat comes back exactly in time. A second STEP moves it. Played, not written.
- **The repeat rate on the arrows (P3).** REPEAT + LEFT/RIGHT, or LEFT/RIGHT while a NOTE RPT
  pad rolls, step the rate through 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T and 1/32 (`REPEAT 1/8T`,
  `ROLL 1/32`). Beat repeat, the rolls and the stutter all follow it at once; line 2 shows it
  (`R1/16`) while your hand is on it.
- **Waveforms and envelopes in EDIT.** SAMPLE's SRC page draws the sample under the knobs, with
  its START..END window bright, the slices dotted and a moving playhead per voice; GRAIN's draws
  the sample with a bracket at POS and a tick for each grain in the air. AMP, ASR and the synths'
  FLTR draw the envelope their knobs shape, with the knob letters on its handles. The picture of
  a sample is made once while it loads (250 bytes a track), so drawing costs nothing.
- **Play modes per track (P1).** The FUNC menu's new TRACK PLAY row, under TRACK SPEED, sets how
  the selected track walks its steps: FWD, REV, PING, or RND1-RND16 (a random step, or a random
  block of 2-16 steps, in each place). The pattern itself never changes, so FWD gives it back at
  once. Swing stays on the grid; micro-timing, conditions, probability, ratchets and locks go with
  their step. The random order is seeded from the pattern, so it repeats after a STOP or a reload.
  Stored per track in each pattern; older projects load with every track FWD, and older builds
  open new projects and play them forward.
- **LOCK on FUNC + MUTATE (D3, D4, D13, D14).** Hold it half a second to store the playing
  pattern's lock (`LOCK A01 STORED`): the pattern whole, every track's sound and fader, the sends,
  the master, the mutes, and the morph and macro positions. Tap it to jump back at once
  (`BACK TO LOCK A01`); tap again to swap back to where you were (`SWAPPED BACK A01`), sound
  included. One lock per pattern, in memory only. Line 2 ends `L`, or `L+` once anything in the
  lock has changed. The sound is one kit for every pattern, so a jump brings that lock's sound
  back everywhere. A MUTATE waiting for OK/CLR is dropped by a jump.
- **FX GLIDE (FUNC menu).** The performance mixer's button jumps ride instead of cutting: tapping
  VALUE, push VALUE + CLR, a slot change (the old effect rides out first), push VALUE +
  LEFT/RIGHT (a crossfade), leaving a playing loop and a LOCK jump. OFF, 1/16, 1 BEAT (default)
  or 1 BAR at the tempo. FUNC + tap VALUE is a hard cut. Turning VALUE never glides.
- **Hold push VALUE + a STEP: the momentary push.** Held 0.3 s, the picked effect rises to full
  over FX GLIDE while the STEP is held and rides out on release. A quick tap still picks; the
  picked slot's own STEP now lets go on the release.

### Fixed

- **The main loop no longer reads the audio side's working copies.** Building WAVE tables,
  KEYS' sustain hold and the LFO page's number and strip read what each track sounds straight
  from the audio side's arrays, which it rewrites every block. The audio side now publishes those
  few bytes each block (`Engine::publish_heard`), and the main loop reads only those. Nothing
  sounds or looks different.
- **RESAMPLE takes reach the card.** Every take failed with `DENIED` before it was written: STRATA
  made the folder `/RECORD` first, and Enaga only allows writes strictly inside it. The file's
  own open makes the folder now. Found by the new bounce scenario in the emulator; the host tests'
  fake card had allowed it.

### Gestures that moved

One home per job (backlog D1, D2, D5). Old projects open unchanged: no file holds a mixer slot.

| Was | Now |
| --- | --- |
| FUNC + VALUE: master DJ filter | Performance mixer slots 1 (LP SWEEP) and 2 (HP SWEEP); the saved setting stays on MIXER's MST page. FUNC + VALUE says `DJ FILTER: SLOT 1/2` |
| FUNC + press VALUE: DJ filter off | Gone with it |
| FUNC + hold push D: tape stop | Slot 8, TAPE STOP. FUNC + push D says `TAPE STOP: SLOT 8` (with a STEP held it still clears the D lock) |
| FUNC + hold MUTATE: delay freeze | Slot 5, ECHO FREEZE. FUNC + MUTATE is LOCK now (above) |
| Slot 7: STUTTER | **MUTE BUS**: every track fades out while the delay and reverb ring on. The stutter is the REPEAT pad's |
| Slot 13: FLANGER | **MORPH**: the A/B scene morph on any screen, beside an effect left up. PERFORM's VALUE still morphs too |
| PERFORM: STEP 1-16 (and FUNC + STEP 1-4) put a parameter on a macro; push a knob in LEARN to add it | **Learn by turning** (above). A push in LEARN only takes a parameter off. FUNC + STEP 5-7 then put the kill EQ's bands on a macro |
| PERFORM: FUNC + STEP 5-7 put the kill EQ's LOW, MID or HI on the macro | **Learn by turning on MIXER's EQ page**: LEARN stays on in MIXER, and turning LOW, MID or HI is learnt like an EDIT knob; a push on a band the macro holds takes it off. FUNC + STEP 5-7 are trigless steps, like the rest |
| Pad-row keys: the step page stopped at 1 and 4 | They go round, as LEFT/RIGHT on GRID and the bank keys in PROJECT already did |
| KEYS: STEP 1-16 play the scale; the pad-row keys change the octave | The STEP keys are steps and the pad-row keys the step page; the pads are the keyboard, the octave is LEFT/RIGHT or the two octave pads |
| NEW PROJECT: the factory demo | An empty project (the factory sounds and mixer, every pan centred, no steps, scenes or macros). The demo is LOAD DEMO |

### Changed

- **The docs' screenshots are frames of the device build running in the emulator** (cl6x),
  redrawn by `tools/emulator_screens.sh`; the user manual, cheatsheet and walkthrough follow the
  redrawn screens (the ENV page, toasts, the live strip, tabs, MAX on SETUP).
- **The last text-list screens are drawn too.** RESAMPLE has a level meter in segments, draws the
  take column by column while it records and shows its length as a bar of its bars. SAMPLES keeps
  six rows and shows the file being heard as a tape under a playhead (its time and rate), or the
  track's own file with its window. SOUNDS draws the preset under the cursor as twenty bars against
  the track's knobs. CHAIN is the song as blocks on a lane with the playhead, KITS draws each slot's
  eight machines as MACHINE's pictures, KEYS shows the held notes, the chord they make and the four
  knobs as gauges, and PATTERN's track swing is eight bars. MENU, SETUP and PROJECTS no longer repeat
  the screen's name under its toast; HELP's title is a lit band.
- **The pattern and machine screens are pictures.** PROJECT's slots show all 128 patterns as a
  map, a block of 4 x 4 per bank: which hold a pattern, which plays and which is queued (it blinks),
  in every bank at once. Under it the shown bank's sixteen are drawn as their steps, eight track
  rows by sixteen columns, the playing one with its playhead. The key, the queue mode, the sample
  pool and how many of the bank's slots are used sit in two small lines at the top, and the page
  tabs (SLOT CHAIN PAT SETUP PROJS) at the bottom. MACHINE (FUNC+EDIT) draws the kit as eight
  pictures, every machine as a picture of its sound (the kick's falling sine, the hats' noise, the
  saws, the wavetable's frames, FM4's operators), and says what the track's machine is for and what
  it costs in voices and CPU.
- **EDIT and MIXER look like the drafts.** EDIT's four knobs are one line in the small font at the
  top (D at the bottom), and the picture takes the screen on every page of every machine: a drum's
  SRC draws the hit its knobs make and lights it as it plays; FLT draws the filter's curve over the
  live spectrum, and on a synth sweeps it after each note; AMP and ENV the envelope with a dot
  riding it; LFO its shape, its output strip and where its destination is; WAVE the waterfall;
  GRAIN the sample, the grains' window and the cloud; FX the lo-fi, the pan and the sends; ARP the
  run; OSC the oscillators, FM's algorithm and ratios. The bottom line says what moves the page's
  knobs (`LFO>CUT 40`), else its hint. MIXER's track pages are an eight-strip desk (meters with a
  held peak, faders, pan, sends; the four the knobs turn lettered), the duck pages dip with their
  source, DLY draws the echoes, REV the tail, ROUTE the routing, MST the draft's master page with a
  momentary LUFS reading, and EQ the kill EQ's curve. The lists keep their rows in the big font with
  the cursor's row lit; the step page draws the track's lane with the step's note on it, and CHAIN
  the song as a strip. docs/SCREENS.md has every screen's rows.
- **The ASR page is ENV and draws the whole envelope**: the amplitude's with DEC in it, and on the
  VA and WAVE machines the filter's under it with FDE. Same knobs (ATK SUS REL FSU).
- **GRAIN's AMP page has DEC on knob A**, as every other machine has: A DEC, B PIT, C SHP, D LVL.
  Only the knobs moved; kits load as they did.
- **REL ALL and LEN ALL are tested on the DSP** (`scenario_moveall`): the emulator turns encoders
  since Enaga 0.3.0. Each click's amount is logged, as the other notices the scenarios read are.
- **Code quality round (no change in sound or controls).** One set of integer clamps in core/,
  the project file names spelled once, CI checks CLAUDE.md's source rules
  (`tools/check_rules.sh`), the scenarios print their log path on a failed check, and the unit
  checklist's file sizes are the real ones (PROJECT.BIN 402 bytes, KIT.BIN 1,329).
- **The performance mixer goes back to rest faster.** A button that sends an effect or the
  loop's mix to 0 (tap VALUE, push VALUE + CLR, a new slot over the old one, letting go of a
  momentary push, leaving a loop, a LOCK jump) now rides out over at most a 1/16 at the tempo
  instead of the whole FX GLIDE; rides in keep FX GLIDE's length. OFF still cuts.
- **An LFO on TUN glides on VA and WAVE.** The pitch moves smoothly instead of in TUN's 0.375
  semitone steps, and a held note follows it (vibrato on a pad). Other machines step as before.
- **Cutoff movement stops at about 100 Hz everywhere (D9).** A scene morph or a macro that takes
  a cutoff down now stops where the LFO already did; only the knob (and a lock) closes the filter
  all the way. LFO depth on a cutoff is about 3 octaves either side at DEP 127, not 5.
- **FUNC + lower pad mutes every pad you press before the bar**, not only the last one; the same
  pad twice takes it back. Stopped, it mutes at once.
- **A cutoff stops at about 100 Hz (D9)** when an LFO or a lock sweeps it down, instead of
  shutting the sound off. A knob already set lower stays where it is.
- **Push VALUE + STEP 16 leaves the looper** like any other effect (D10): a finished loop falls
  silent but is kept, a count-in or a take is cancelled. Push VALUE + CLR, then STEP 16, for a
  new take.
- **44.1 kHz files load through a windowed-sinc resampler** instead of linear interpolation:
  the error at 10 kHz falls from -15 dB to -82 dB. Converting a 30 s stereo file takes about
  0.4 s on the unit (reading it from the card, 1.4 s); 48 and 96 kHz files load as before.
- **One filter on the synth machines (D8).** On VA MONO, VA POLY, WAVE MONO and WAVE POLY the FLTR
  page is now the synth's own filter, CUT RES ENV and the new **FDE** (filter decay: 0 keeps the
  old 0.55 x DEC, 1-127 its own time). The insert filter there is one knob, **TON** on SRC: 64
  flat, lower a low-pass, higher a high-pass. Older projects load their insert filter on these
  tracks as the nearest TON; its drive and resonance are dropped. Other machines are unchanged.
- **Velocity opens the synths' filter (D15).** Around the default velocity 100 (which sounds as
  before): harder is brighter, softer darker, by a per-machine amount. The GROWL preset lost
  its chain drive with the one filter and is quieter.

### Removed

- The performance mixer's flanger (its DSP and 8 KiB ring), with slot 13.

### Known limits

- Tested on one SmplTrek, with the cl6x build. On the 0.2.0 build (2026-11-22) the load stayed under 30% on the default beat, and MAX peaked at 84.3% in long, heavy play with no clicks or dropouts; a build from main boots on Enaga 0.3.0 (2026-11-25), but 0.4.0's load is not measured on the unit yet. Nothing added since 0.3.1 is
  checked on the unit yet. Every other load figure comes from the emulator, which models no caches.
- **Enaga 0.3.0 exactly.** Enaga's loader checks only the ABI number, which 0.2.0 and 0.3.0 share,
  so 0.4.0 on the 0.2.0 firmware loads and then misbehaves.
- WAVE POLY and FM4 share the **12 POLY voices** with VA POLY and that budget is provisional; the
  performance mixer, the looper, LOCK and LIVE RND are not saved; the looper borrows RESAMPLE's
  buffer; streamed samples play forward only; FREEZE is not saved; MIDI clock, SYNC and USB MIDI
  are lightly tested.

## [0.3.1] - 2026-11-23

An early beta: everything merged since 0.3.0, built for the same Enaga 0.2.0 firmware. The GRAIN
overhaul and the sample and machine fixes below have not been through the unit walkthrough
(docs/UNIT_CHECKLIST.md) yet. Release notes: [docs/release/v0.3.1.md](docs/release/v0.3.1.md).
Projects and kits from 0.1.0 to 0.3.0 open in 0.3.1; their GRN tracks play louder (see Changed).

### Added

- **GRAIN's ASR and OSC pages.** ATK, SUS and REL give a cloud a real envelope: with SUS up it
  swells, holds for the step's LEN or while a pad is held, and fades over REL. SCN moves the read
  head through the sample while the note sounds (up to twice real time; a time-stretch at any
  pitch), and PSP sprays each grain's pitch: a detune of up to a semitone, or up to half the
  grains an octave up for a shimmer. All at 0 is the machine as it was.
- **Eight GRN factory sounds:** CLOUD, PAD, FREEZE, SHIMMR, DRIFT, STRTCH, STUTTR, CHOPS. They
  leave POS and PIT alone. Switching a track to GRN loads CLOUD.

### Changed

- **GRAIN is easy to dial in.** About 6 dB louder at the same LVL (a cloud used to sit 11 dB
  under its own sample). PIT plays whole semitones (an octave up was 19 cents off). SIZ reaches
  680 ms (was 390 ms), from 8 ms. Every GRN knob shows its unit: `151MS`, `X4.3` grains in the
  air, `HOLD`, `D15`, `OCT25`. On a GRN track OK in EDIT opens its SOUNDS list and OK in MACHINE
  the sample browser. Older projects open; their GRN tracks play louder, with SIZ a little
  longer at the same knob and PIT on the nearest semitone.
- **Switching a track's machine loads that machine's default sound** (the first entry of its
  SOUNDS list; on SMP, the whole sample from its start). The knobs used to keep their positions,
  so the old machine's values leaked into the new one. FX (lo-fi, pan, sends), LFO and ARP stay;
  MID is untouched.

### Fixed

- A sample picked in the browser on a SMP track plays whole from its first frame. It used to
  keep the track's STA, SLC and DEC, so it started part way in (or on a slice chosen by the
  note) and DEC faded it out early. The pick now sets STA 0, LEN 127, SLC 1 and DEC 127 (hold).

### Known limits

- Tested on one SmplTrek, with the cl6x build. On the 0.2.0 build (2026-11-22) the load stayed under 30% on the default beat, and MAX peaked at 84.3% in long, heavy play with no clicks or dropouts; 0.3.x has not run on the unit yet. The changes above are not yet
  checked on the unit. Every other load figure comes from the emulator, which models no caches.
- The 0.3.0 limits stand: WAVE POLY and FM4 share the **12 POLY voices** with VA POLY and that
  budget is provisional, the performance mixer and the looper are not saved, the looper borrows
  RESAMPLE's buffer, streamed samples play forward only, FREEZE is not saved, MIDI clock, SYNC and
  USB MIDI are lightly tested, and on Enaga 0.2.0 unplug USB when driving gear from MIDI OUT.

## [0.3.0] - 2026-11-23

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

- Tested on one SmplTrek, with the cl6x build. On the 0.2.0 build (2026-11-22) the load stayed under 30% on the default beat, and MAX peaked at 84.3% in long, heavy play with no clicks or dropouts; 0.3.0 itself has not run on the unit yet. The features above are not yet
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
