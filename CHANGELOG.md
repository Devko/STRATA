# Changelog

All notable changes to STRATA. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org/). Before 1.0, any release may change
the project file format or the controls.

## [0.1.0] - unreleased

The first release: STRATA on a SmplTrek running Enaga 0.2.0. Early, and released so that people
with a unit can play it and report back. Release notes: [docs/release/v0.1.0.md](docs/release/v0.1.0.md).

### Requires

- A Sonicware SmplTrek with bootloader BOOT 1.0.17, running **Enaga 0.2.0** exactly. The app is
  built against Enaga's provisional ABI 0, and Enaga's loader checks only that number, so a
  different Enaga release may load STRATA and then misbehave.

### Added

- **Eight tracks**, each one machine plus a fixed insert chain. Machines: KICK, SNARE (with a clap
  mode), HAT, PERC (2-operator FM), SAMPLE (whole, sliced or played as notes), GRAIN, VA MONO
  (accent and slide) and VA POLY (a chord per step). Any track can host any machine.
- **Inserts per track:** drive, multimode filter (LP, HP, BP, 12 or 24 dB), lo-fi, amp envelope,
  sidechain duck, pan and level.
- **Sends and master:** tempo-synced ping-pong delay, reverb, a one-knob DJ filter, glue
  compressor and limiter.
- **Sequencer:** up to 64 steps per track, per-track length and speed, swing and micro-timing,
  parameter locks, trig conditions, probability, ratchets, live and motion recording, a Euclidean
  generator, KEYS mode for playing notes in a scale, and 16 levels of undo.
- **One LFO per track** on EDIT's fifth page: six shapes, free (0.06-32 Hz) or synced (8 bars to
  1/32, dotted and triplet), routable to any of the 16 parameters, and lockable, scene-able and
  macro-able like them. Off by default; older kits and projects load with it off.
- **Per-track swing** on PROJECT's PATTERN page: each track follows the pattern's swing or has its
  own, 50-75%.
- **Projects:** banks A-H, queued pattern changes, a pattern chain with repeats, autosave, and
  samples loaded in the background while the beat plays. Samples up to 30 s at up to 96 kHz.
- **Performance:** seven performance pads (FILL, REPEAT, MUTE, SCENE A, SCENE B, THROW, MUTATE),
  scenes with a morph between them, four macros, mute states, beat repeat, delay and reverb throws,
  seeded mutation you can keep or revert, pad pressure, and resampling the output to a file or a track.
- **Sync:** follows the unit's clock (internal, MIDI clock or SYNC IN), chosen in the Enaga menu.
  Under an external clock STRATA refuses tempo edits and follows the transport.
- **Factory project**, on first start and for every NEW project: a techno kit at 130 BPM in
  C minor, patterns A01 GROOVE, A02 GROOVE+, A03 BREAK and A04 PEAK, a chain that walks them
  (shipped off), scenes DUB, DARK and TENSION, three macros (B ACID, C DUB, D DRIVE; A is left
  free for pad pressure), and eight mute states.
- **CPU readout:** the top line shows the load of the audio block.

### Known limits

- Tested on one SmplTrek: TODO(release) the result of the unit walkthrough, and the MAX load
  read on A01 and A04. Every other load figure comes from the emulator, which models no caches.
- VA POLY is the expensive machine: all tracks together share **12 VA POLY voices**. Past that,
  the quietest sounding voice is cut with a short fade. The number is provisional.
- MIDI clock, SYNC in and out, and USB MIDI are Enaga features that STRATA may be the first app to
  use on a unit. Expect rough edges there.
- The Lofi-12 XT and CyDrums are the same board, but STRATA has not run on either.
