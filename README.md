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
![Language](https://img.shields.io/badge/C%2B%2B-20-blue)
![Licence](https://img.shields.io/badge/licence-MIT-green)

[Screens](#screens) · [Features](#features) · [Install](#installing-on-a-smpltrek) ·
[Build](#building) · [Status](#status) · [Documents](#documents)

</div>

> [!WARNING]
> **STRATA is an early beta.** It runs on the SmplTrek and has held up through long sessions on
> one unit, but most features are still to be checked on the hardware
> ([docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md)). Read [Before you install](#before-you-install)
> first.

## What it is

The SmplTrek's stock firmware is sample-only. [Enaga](https://github.com/seawaffle/enaga) is
replacement firmware that turns the unit into a platform for apps. STRATA is one of those apps:
a groovebox that puts synthesized drums, basses and chords next to samples, lets every step
change the sound, and adds mutes, scenes, macros, a performance mixer and a looper so that a set
can keep evolving without stopping.

Design principles, from [docs/SPEC.md](docs/SPEC.md):

- **Performance first.** Every edit works while the transport runs and lands on the beat.
- **Two keys at most.** The common actions take one key, or FUNC plus one key.
- **Timing is sacred.** Steps are scheduled inside the audio callback against Enaga's clock and
  land on an exact sample. Screen or card activity never moves a note.
- **Controlled accidents.** Randomness is seeded, bounded and undoable.
- **Always restorable.** STRATA never writes the SPI flash. Everything lives on the SD card, so
  one official update restores the unit.

## Screens

![STRATA's screens: GRID, the step page, EDIT, MIXER, PERFORM, KEYS, MACHINE, KIT and PROJECT](docs/screens/overview.png)

<sub>The factory project playing. Top: GRID, the step page, a track's sound page. Middle: MIXER,
PERFORM, KEYS. Bottom: MACHINE, the KIT browser, PROJECT. Drawn by the app's own screen code in
Enaga's font (`tools/screenshots.sh`).</sub>

## Features

### Sound

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
  WAVE POLY and FM4, plus **MID**, which plays an outboard synth over MIDI. The drums follow Mutable
  Instruments Plaits' models, reimplemented ([docs/SOUND.md](docs/SOUND.md)).
- **VA MONO and VA POLY:** saw or pulse, a second oscillator detuned or hard-synced, sub, drive,
  a resonant filter that self-oscillates, and an attack-sustain-release page for pads and drones.
- **WAVE MONO and WAVE POLY:** the same voice with 16 built-in wavetables (classic shapes, sync,
  formants, FM, e-piano, vowels, mallets and more), a position the envelope can sweep, and warp.
- **FM4:** four-operator FM in eight algorithms, with feedback and an index envelope, for
  e-pianos, bells, glass, brass, organs and FM basses; chords of up to six notes.
- **SAMPLE and GRAIN:** slices by note, with SLC AUTO cutting a break at its hits; granular
  clouds from the same sample.
- **Factory sounds:** 68 ready-made sounds, loaded into one track at a time while the beat plays.
- **Per track:** drive, multimode filter (LP, BP, HP, 12 or 24 dB), lo-fi, amp envelope,
  sidechain duck, pan, level, an **LFO** (six shapes, free or synced) and an **arpeggiator**.
- **Sends and master:** a tempo-synced ping-pong delay with a LONG mode (up to 12 s, tape wow), a
  reverb with a HALL mode (shimmer), a DJ filter, a three-band kill EQ, a glue compressor and a
  limiter.

### Sequencer

- Up to 64 steps per track, with per-track length, speed and swing (polymeter), and micro-timing
- Parameter locks, trig conditions, probability, ratchets, accents, slides and chords
- Live and motion recording, a Euclidean generator, and KEYS: both pad rows as a
  pressure-sensitive keyboard, with the pattern's scale lit
- 8 banks of 16 patterns with queued changes, and a chain that becomes a song: each entry can
  recall a mute state, and SONG mode stops at the end
- 16 levels of undo

### Performance

- Seven performance pads: **FILL**, **REPEAT**, **MUTE**, **SCENE A**, **SCENE B**, **THROW** and
  **MUTATE**
- **A performance mixer on VALUE:** push VALUE and tap a STEP key to pick one of fifteen effects
  (filter sweeps, risers, echo freeze, reverb wash, stutter, tape stop, bass kill, pump, crush,
  flanger, gate, build), then turn VALUE to bring it in. STEP 16 is a **looper** that records 1 to
  8 bars of the mix and crossfades it against the live beat.
- Scenes with an A/B morph, four macros (three set up in every new project) and stored mute states
- Beat repeat, note repeat, delay and reverb throws, tape stop, delay freeze, kill-EQ switches, and
  seeded mutation you keep or revert
- **Freeze** a track to a loop in memory to save CPU, **resample** the master or an input, or
  **bounce** one track to a file or another track

### Samples, projects and MIDI

- A sample browser on the unit that also reaches the stock firmware's sample pool and project
  folders; WAVs at any rate and depth; long files loaded in the background, and files too big
  for memory streamed from the card
- Projects with autosave, a project browser (open, save as, rename, delete), and a kit library
  shared by all projects
- MIDI out per track; MIDI in for notes, CCs and program changes; follows the unit's clock
  (internal, MIDI clock or SYNC IN)
- **Help on every screen:** hold FUNC for what the knobs and keys do where you are

**Learning it:** [docs/MANUAL.md](docs/MANUAL.md) is the user manual, with the concepts, a
first-beat tutorial on the factory project, every machine, performing, saving and
troubleshooting. [docs/CHEATSHEET.md](docs/CHEATSHEET.md) is the reference to keep next to the
unit: every gesture, screen and abbreviation.

## Installing on a SmplTrek

> [!CAUTION]
> Installing third-party firmware is **at your own risk**. Neither Enaga nor STRATA is made,
> supported or endorsed by Sonicware, and installing them may affect your warranty. Keep an
> official update file at hand so you can restore the unit.

STRATA runs on Enaga, so the unit needs Enaga's firmware first and then the STRATA app on the SD
card. **The easy way is a release zip** from [Releases](https://github.com/Devko/STRATA/releases):
it holds both, laid out for the card (Enaga 0.2.0's `SmplTrek.bin` and
`ENAGA/APPS/STRATA/APP.ENA`), with install steps in its README.txt. Releases are pre-releases
until 1.0. To build `APP.ENA` yourself, see [Building](#building).

### Before you install

From the safety section of [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md). Do not skip these:
a hung image leaves the power key dead.

- [ ] SYSTEM INFO shows **BOOT 1.0.17**. All the recovery evidence is for that bootloader.
- [ ] A second SD card holds an official `SmplTrek.bin`, so you can restore the unit.
- [ ] Holding **HOME** at power-on reaches BOOT MODE.
- [ ] The unit runs on the power adapter **without batteries**, so a hung image can be
      power-cycled.

### 1. Install Enaga v0.2.0

STRATA is built against Enaga **v0.2.0**, and the firmware on the unit must match.

1. Take `SD-CARD/SmplTrek.bin` from the STRATA release zip (byte for byte the file in Enaga's
   own zip), or download the SmplTrek zip from
   [Enaga's v0.2.0 release](https://github.com/seawaffle/enaga/releases/tag/v0.2.0).
2. Copy it to the **root** of the SD card as `SmplTrek.bin` (Enaga's README, "Installing", has
   the details).
3. Hold **HOME** while switching on to reach BOOT MODE, and choose **SYSTEM UPDATE**.

### 2. Copy STRATA to the card

Copy the release zip's `SD-CARD/ENAGA/APPS/STRATA/APP.ENA` (or your own build) to:

```text
/ENAGA/APPS/STRATA/APP.ENA
```

Updating from an earlier STRATA release is only this step: the firmware stays Enaga 0.2.0, and
your projects stay on the card.

Enaga starts the last app that ran, or the only one installed. **FUNC + POWER** opens the Enaga
menu, which lists the apps and switches between them.

### Going back

Install an official SmplTrek system update the same way. Neither Enaga nor STRATA writes the SPI
flash or touches the bootloader, so that restores the unit completely. Projects stay on the card
under `/ENAGA/DATA/`.

## Building

Enaga's tools are **Linux-only**; on Windows, use WSL. No sudo and no cmake are needed.

```sh
git clone --recurse-submodules https://github.com/Devko/STRATA
cd STRATA
git clone --depth 1 https://github.com/seawaffle/smpltrek-re third_party/smpltrek-re

tools/build_toolchain.sh     # once, ~30 min: builds tic6x-elf GCC into ~/strata-toolchain
tools/build.sh               # -> build/APP.ENA
make -C host test            # the host test suite
tools/panel.sh               # build, then play it in the emulator at 127.0.0.1:8766
tools/render.sh              # render the output to a WAV offline
```

- **TI cl6x is optional but load-bearing.** If TI's C6000 compiler is installed, `tools/build.sh`
  uses it for the hot loops and the DSP library; it is about 1.7× faster on STRATA's worst case.
  Without it the build is GCC-only and still works.
- `tools/env.sh` keeps the toolchain, build output and emulator off the `/mnt/d` mount in WSL,
  where a GCC build would crawl.
- The live web panel runs at about 0.4× real time, so its sound breaks up. Use `tools/render.sh`
  to hear it properly.

**Checks.** [CI](.github/workflows/ci.yml) runs on every pull request: the host suite with GCC
and again under AddressSanitizer and UBSan, the layout check over every screen,
`tools/audit_commands.sh` and `tools/enaga_check.sh`. `tools/ci_local.sh` runs the same lanes
locally (`--scenarios` adds the emulator scenarios). The device build, the emulator scenarios
(`tools/scenarios.sh`) and the load figures need the tic6x toolchain, and their real numbers
need cl6x, so they run on a developer's machine and go into [docs/BUDGET.md](docs/BUDGET.md). A
weekly job reports new Enaga releases; moving the pin is a deliberate step
([docs/HANDOFF.md](docs/HANDOFF.md), "Moving the Enaga pin").

### Layout

```text
core/    portable C++20: scheduler, pattern, steps, conditions, locks, mutation, serializer
dsp/     machines and effects; may use <enaga/dsp.h>, which also builds on the host
app/     Enaga glue: the App subclass, input, LEDs, screens, files
host/    host test runner and offline renderer
tests/   host tests and emulator scenarios
tools/   build, toolchain and emulator scripts
```

Enaga's `build_app.py` compiles **one flat directory**, so filenames must be unique across `core/`,
`dsp/` and `app/`, and includes are bare (`#include "pattern.hpp"`). `tools/build.sh` refuses a
collision. Contributors, human or AI, should read [CLAUDE.md](CLAUDE.md) for the full set of rules.

## Status

**[v0.2.0](https://github.com/Devko/STRATA/releases/tag/v0.2.0)** (2026-11-23) is the current
release, an early beta. `main` has moved on since: the performance mixer and looper, the WAVE
and FM4 machines, the VA oscillator page and the stock sample folders are in
[CHANGELOG.md](CHANGELOG.md) under Unreleased.

| | |
| --- | --- |
| On hardware | Runs on Enaga 0.2.0. The default beat loads the CPU under 30%; long, heavy play peaked at 84.3% with no clicks or dropouts |
| Emulator | Peaks of 26-32% on the factory patterns and the ambient sends (cl6x), against a 60% worst-case target |
| Tests | Every feature has host tests, run in CI on every pull request; emulator scenarios cover what keys can reach |
| Still to do | Most of [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) on the hardware, and unit load figures for the newer features |

[docs/FEATURES.md](docs/FEATURES.md) lists every feature with its status and known limits, and
[docs/HANDOFF.md](docs/HANDOFF.md) has the next steps.

## Documents

| Document | What it holds |
| --- | --- |
| [docs/MANUAL.md](docs/MANUAL.md) | The user manual: concepts, a first-beat tutorial, the machines, performing, troubleshooting |
| [docs/CHEATSHEET.md](docs/CHEATSHEET.md) | Every gesture, screen and abbreviation, for playing it |
| [docs/FEATURES.md](docs/FEATURES.md) | Every feature with its status: since when, checked on the unit or not, known limits |
| [CHANGELOG.md](CHANGELOG.md) | What each release added, and its known limits |
| [docs/CONTROLS.md](docs/CONTROLS.md) | The controls concept: why each button does what it does |
| [docs/HANDOFF.md](docs/HANDOFF.md) | **For contributors, start here.** Where it stands and what comes next |
| [docs/SPEC.md](docs/SPEC.md) | The design: sound architecture, sequencer, panel mapping, data model |
| [docs/PLAN.md](docs/PLAN.md) | The build order and milestones |
| [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) | What needs real hardware to check |
| [docs/BUDGET.md](docs/BUDGET.md) | Measured CPU and memory figures |
| [docs/ENAGA_API.md](docs/ENAGA_API.md) | The verified Enaga signatures STRATA uses |
| [docs/SOUND.md](docs/SOUND.md) | Where the drum synthesis comes from |
| [docs/REVIEW.md](docs/REVIEW.md) | The platform review the spec was corrected against |
| [docs/release/RELEASING.md](docs/release/RELEASING.md) | How a release zip is made and published |

## Disclaimer

STRATA is an independent hobby project. It is **not affiliated with, endorsed by or supported by
Sonicware**. Sonicware, SmplTrek, Lofi-12 XT and CyDrums are trademarks of their respective
owners and are used here only to say which hardware STRATA runs on.

STRATA is provided **as is, without warranty of any kind**, and with no guarantee that it works,
keeps your data safe or suits any purpose (see [LICENSE](LICENSE)). You install and use it at
your own risk; the authors are not liable for any damage to your device, your data or anything
else. Please do not contact Sonicware support about STRATA or Enaga.

## Licence

MIT, see [LICENSE](LICENSE). STRATA is built on **Enaga** (MIT) and its drum synthesis follows
**Mutable Instruments Plaits** (MIT, © 2016 Émilie Gillet), reimplemented rather than ported.
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) carries both notices and says exactly what was
taken from each.
