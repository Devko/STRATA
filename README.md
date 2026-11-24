<div align="center">

# STRATA

**An 8-track techno performance groovebox for the Sonicware SmplTrek**

Synthesized drums, basses and chords next to samples, a step sequencer with parameter locks and
trig conditions, and a performance layer built for long live sets. An
[Enaga](https://github.com/seawaffle/enaga) app.

[![CI](https://github.com/Devko/STRATA/actions/workflows/ci.yml/badge.svg)](https://github.com/Devko/STRATA/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Devko/STRATA?include_prereleases&label=release)](https://github.com/Devko/STRATA/releases)
![Platform](https://img.shields.io/badge/platform-SmplTrek-informational)
![Runs on](https://img.shields.io/badge/runs%20on-Enaga%20v0.2.0-blueviolet)
![Licence](https://img.shields.io/badge/licence-MIT-green)

[Try it](#try-it) · [Highlights](#highlights) · [Documentation](#documentation) ·
[Status](#status)

</div>

> [!WARNING]
> **STRATA is an early beta.** It runs on the SmplTrek, but most features have not been checked
> on the hardware yet. Install it at your own risk, and read the
> [safety checks](docs/INSTALL.md#before-you-install) first.

## What it is

The SmplTrek's stock firmware is sample-only. [Enaga](https://github.com/seawaffle/enaga) is
replacement firmware that turns the unit into a platform for apps. STRATA is one of those apps:
a groovebox that puts synthesized drums, basses and chords next to samples, lets every step
change the sound, and adds mutes, scenes, macros, a performance mixer and a looper so that a set
can keep evolving without stopping.

![STRATA's screens: GRID, the step page, EDIT, MIXER, PERFORM, KEYS, MACHINE, KIT and PROJECT](docs/screens/overview.png)

<sub>The factory project playing, drawn by the app's own screen code in Enaga's font.</sub>

## Try it

1. Download the latest zip from [Releases](https://github.com/Devko/STRATA/releases). It holds
   Enaga 0.2.0's firmware and the STRATA app, laid out for the SD card.
2. **Install Enaga v0.2.0 once:** copy `SD-CARD/SmplTrek.bin` to the root of the SD card, hold
   **HOME** while switching on, and choose **SYSTEM UPDATE**.
3. **Copy the app** `SD-CARD/ENAGA/APPS/STRATA/APP.ENA` to `/ENAGA/APPS/STRATA/APP.ENA` on the
   card, and switch on. The factory project plays on PLAY.

Updating STRATA later is step 3 only; your projects stay on the card. An official SmplTrek update
restores the unit completely, because neither Enaga nor STRATA writes the SPI flash.
[docs/INSTALL.md](docs/INSTALL.md) has the safety checks, switching apps and going back.

## Highlights

- **Eleven machines on eight tracks, any machine on any track:** KICK, SNARE, HAT, PERC, SAMPLE,
  GRAIN, VA MONO, VA POLY, WAVE MONO, WAVE POLY and FM4, plus MID for an outboard synth.
  76 factory sounds load into one track at a time while the beat plays.
- **A sequencer built for techno:** up to 64 steps per track with their own length, speed and
  swing, parameter locks, trig conditions, ratchets, accents, slides, chords, micro-timing, a
  Euclidean generator, live and motion recording, and 16 levels of undo.
- **KEYS:** both lower pad rows become a pressure-sensitive keyboard with the pattern's scale lit.
- **Performance:** fill, beat and note repeat, mutes, scenes with an A/B morph, macros, throws,
  seeded mutation you keep or revert, and a performance mixer on VALUE with fifteen effects and a
  looper.
- **Samples and projects:** a sample browser that also reaches the stock SmplTrek folders,
  slicing (SLC AUTO cuts a break at its hits), streaming for long files, resampling, freeze and
  bounce, and projects with autosave.
- **Timing is sacred:** steps land on an exact sample of Enaga's clock, and MIDI clock or SYNC IN
  can lead.
- **Help on every screen:** hold FUNC to see what the knobs and keys do where you are.

The full tour, with the factory kit and every effect, is in
[docs/OVERVIEW.md](docs/OVERVIEW.md).

## Documentation

**Playing it**

- [User manual](docs/MANUAL.md): concepts, a first-beat tutorial, every machine, performing,
  saving and troubleshooting
- [Cheatsheet](docs/CHEATSHEET.md): every gesture, screen and abbreviation, to keep next to the
  unit
- [Installing](docs/INSTALL.md) and [what changed in each release](CHANGELOG.md)
- [Features and their status](docs/FEATURES.md): what exists and what has been checked on the
  unit

**Working on it**

- [Building and testing](docs/BUILDING.md)
- [Handoff](docs/HANDOFF.md): where it stands and what comes next; contributors start here
- [All documents](docs/README.md)

## Status

**[v0.3.1](https://github.com/Devko/STRATA/releases/tag/v0.3.1)** (2026-11-23) is the current
release, an early beta for Enaga 0.2.0. It boots and plays on the unit. On an earlier build the
default beat loaded the CPU under 30%, and long, heavy play peaked at 84.3% with no dropouts.
Most of the [unit checklist](docs/UNIT_CHECKLIST.md) is still to be run on the hardware.

## Disclaimer

STRATA is an independent hobby project. It is **not affiliated with, endorsed by or supported by
Sonicware**. Sonicware, SmplTrek, Lofi-12 XT and CyDrums are trademarks of their respective
owners and are used here only to say which hardware STRATA runs on.

STRATA is provided **as is, without warranty of any kind**, and with no guarantee that it works,
keeps your data safe or suits any purpose (see [LICENSE](LICENSE)). You install and use it at
your own risk; the authors are not liable for any damage to your device, your data or anything
else. Please do not contact Sonicware support about STRATA or Enaga.

## Licence

MIT, see [LICENSE](LICENSE). STRATA is built on **[Enaga](https://github.com/seawaffle/enaga)**
(MIT) and its drum synthesis follows **Mutable Instruments Plaits** (MIT, © 2016 Émilie Gillet),
reimplemented rather than ported. [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) carries both
notices and says exactly what was taken from each.
