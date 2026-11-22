<div align="center">

# STRATA

**An 8-track techno performance groovebox for the Sonicware SmplTrek**

Synthesized drums and bass next to samples, a step sequencer with parameter locks and trig
conditions, and a performance layer for long, evolving live sets. Built as an
[Enaga](https://github.com/seawaffle/enaga) app.

[![CI](https://github.com/Devko/STRATA/actions/workflows/ci.yml/badge.svg)](https://github.com/Devko/STRATA/actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-SmplTrek-informational)
![Runs on](https://img.shields.io/badge/runs%20on-Enaga%20v0.2.0-blueviolet)
![Language](https://img.shields.io/badge/C%2B%2B-20-blue)
![Status](https://img.shields.io/badge/status-first%20boot%20on%20hardware-yellow)
![Licence](https://img.shields.io/badge/licence-MIT-green)

[Screens](#screens) · [Features](#features) · [Install](#installing-on-a-smpltrek) ·
[Build](#building) · [Status](#status) · [Documents](#documents)

</div>

> [!WARNING]
> **STRATA is early.** It first booted on a SmplTrek on 2026-11-22 and plays its default beat,
> but most of [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) is still to be worked through on
> the hardware. Read [Before you install](#before-you-install) first. STRATA is a working title.

## What it is

The SmplTrek's stock firmware is sample-only. [Enaga](https://github.com/seawaffle/enaga) is
replacement firmware that turns the unit into a platform for apps, and STRATA is one of those
apps: a groovebox that puts synthesized drums, a bass line and chords next to samples, lets every
step change the sound, and adds mutes, scenes, morphing, beat repeat and mutation so a set can
evolve for an hour without stopping.

Design principles, from [docs/SPEC.md](docs/SPEC.md):

- **Performance first.** Every edit works while the transport runs and lands on the beat.
- **Two keys at most.** The most used actions need one key, or F1 plus one key.
- **Timing is sacred.** Steps are scheduled inside the audio callback against Enaga's clock and
  land on an exact sample. UI or card activity never moves a note.
- **Controlled accidents.** Randomness is seeded, bounded and undoable.
- **Always restorable.** STRATA never writes the SPI flash. All state lives on the SD card, so one
  official update restores the unit.

## Screens

![STRATA's screens: GRID, the step page, EDIT, MIXER, PERFORM, KEYS, MACHINE, KIT and PROJECT](docs/screens/overview.png)

<sub>The factory project playing. Top: GRID, the step page, a track's sound page. Middle: MIXER,
PERFORM, KEYS. Bottom: MACHINE, the KIT browser, PROJECT. Drawn by the app's own screen code on the
host build, in Enaga's font; `tools/screenshots.sh` redraws them.</sub>

## Features

### Sound

Eight tracks, each one machine plus a fixed insert chain. Any track can host any machine; this is
the default techno kit:

| Track | Machine | Character |
| :---: | --- | --- |
| 1 | **KICK** | Sine or triangle body, pitch envelope, click, drive. Tuned to the key |
| 2 | **SNARE** | Tone plus noise; clap mode fires 3–4 noise bursts |
| 3 | **HAT** | Six-square metallic oscillator or sample; chokes track 4 |
| 4 | **HAT** (open) | Same machine, long decay, choked by track 3 |
| 5 | **PERC** | 2-operator FM percussion: rims, toms, blips |
| 6 | **BASS** | Mono VA with accent and slide |
| 7 | **CHORD** | Poly VA, a chord of up to six notes per step |
| 8 | **LOOP** | Sample, slices or a granular loop locked to tempo |

- **Machines:** KICK, SNARE, HAT, PERC, SAMPLE, GRAIN, VA MONO and VA POLY, plus **MID**, which
  plays an outboard synth over MIDI. The drum synthesis follows Mutable Instruments Plaits,
  reimplemented (see [docs/SOUND.md](docs/SOUND.md)). SAMPLE slices by note, and SLC AUTO cuts a
  break at its hits.
- **Per track:** drive, multimode filter (LP, HP, BP, 12 or 24 dB), lo-fi, amp envelope,
  sidechain duck, pan, level, an **LFO** (six shapes, free or synced) and an **arpeggiator**.
- **Sends and master:** a tempo-synced ping-pong delay with a LONG mode (up to 12 s, tape wow) and
  a reverb with a HALL mode (shimmer); a one-knob DJ filter, a three-band kill EQ, glue compressor
  and limiter.

### Sequencer

- Up to 64 steps per track, per-track length, speed and swing (polymeter), micro-timing
- Parameter locks, trig conditions, probability and ratchets
- Live recording and motion recording, a Euclidean generator, and KEYS mode for playing notes in
  a scale
- Banks of patterns with queued changes, and a chain that becomes a song: each entry can recall a
  mute state, and song mode stops at the end
- 16 levels of undo

### Performance

- A fixed row of seven performance pads: **FILL**, **REPEAT**, **MUTE**, **SCENE A**, **SCENE B**,
  **THROW** and **MUTATE**
- Scenes with a morph between them, four macros and mute states
- Beat repeat, note repeat, delay and reverb throws, tape stop, delay freeze, and seeded mutation
  you can keep or revert
- Resampling the master or an audio input to a file or a track

### Samples, projects and MIDI

- A sample browser on the unit; WAVs at any rate and depth, long samples at 48 kHz, and files too
  big for memory streamed from the card
- Projects with autosave, a project browser with save-as, and a kit library shared by all projects
- MIDI out per track, MIDI in for notes, CCs and program changes, and the unit's clock: internal,
  MIDI clock or SYNC IN

New to STRATA? [docs/MANUAL.md](docs/MANUAL.md) is the user manual: the concepts, a first-beat
tutorial on the factory project, every machine, performing, saving and troubleshooting. A
one-page reference for every gesture, screen and abbreviation is in
[docs/CHEATSHEET.md](docs/CHEATSHEET.md).

## Installing on a SmplTrek

> [!CAUTION]
> Installing third-party firmware is **at your own risk**. Neither Enaga nor STRATA is made,
> supported or endorsed by Sonicware, and installing them may affect your warranty. Keep an
> official update file at hand so you can restore the unit.

STRATA runs on top of Enaga, so the unit needs Enaga's firmware first, then the STRATA app on the
SD card. There is no prebuilt STRATA release yet: build `APP.ENA` yourself (see
[Building](#building)).

### Before you install

From the unit safety checklist in [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md). Do not skip
these: a hung image leaves the power key dead.

- [ ] SYSTEM INFO shows **BOOT 1.0.17**. All the recovery evidence is for that bootloader.
- [ ] A second SD card holds an official `SmplTrek.bin`, so you can restore the unit.
- [ ] Holding **HOME (F4)** at power-on reaches BOOT MODE.
- [ ] The unit runs on the power adapter **without batteries**, so a hung image can be
      power-cycled.

### 1. Install Enaga v0.2.0

STRATA is built against Enaga **v0.2.0**, and the firmware on the unit must match.

1. Download the SmplTrek zip from
   [Enaga's v0.2.0 release](https://github.com/seawaffle/enaga/releases/tag/v0.2.0).
2. Copy its update file to the **root** of the SD card as `SmplTrek.bin` (Enaga's README,
   "Installing", has the details).
3. Hold **HOME (F4)** while switching on to reach BOOT MODE, and choose **SYSTEM UPDATE**.

### 2. Copy STRATA to the card

```text
/ENAGA/APPS/STRATA/APP.ENA
```

Enaga starts the last app that ran, or the only one installed. **F1 + POWER** opens the Enaga
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
make -C host test            # the portable code's tests, on the host
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

### Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every pull request and every push
to `main`, in about a minute:

- the whole host suite with GCC, warnings as errors, as parallel shards on every core
  (`make -C host test JOBS=N`), with a 120-second watchdog per test and a 15-minute cap per job;
- the same suite under AddressSanitizer and UBSan at `-O1` (`SANITIZE=1`), sharded the same way;
- the font layout check over every screen (the `ui_layout_*` tests, run by name);
- `tools/audit_commands.sh` and `tools/enaga_check.sh` (local checks only).

There is no compiler cache in CI: installing ccache took longer (22 s) than it saved on a
one-minute run. A pull request that changes only documentation
(`*.md`, `docs/`) skips the two test jobs but still reports them as passed; the repo checks
always run. On `main` the suite also runs once serially, in registration order.

**The same checks locally:** `tools/ci_local.sh` (WSL or Linux) runs every lane above side by side
and prints one line per lane with its time; `--quick` leaves out the sanitizers, and
`--scenarios` adds the emulator scenarios afterwards, which CI cannot run.

**What CI does not run:** the device build, the emulator scenarios and every load figure. They
need the tic6x cross toolchain and, for the real numbers, TI cl6x, which only exists on the
maintainer's PC (its installer needs an interactive licence click). Run `tools/build.sh` and
`tools/scenarios.sh` there after any DSP change, and record the figures in
[docs/BUDGET.md](docs/BUDGET.md) as before.

A weekly job ([`enaga-upstream.yml`](.github/workflows/enaga-upstream.yml)) runs
`tools/enaga_check.sh --upstream` and opens an issue when Enaga has a newer release or a commit
touching the app API. It never moves the pin: see [docs/HANDOFF.md](docs/HANDOFF.md), "Moving
the Enaga pin".

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

**Milestones M0–M6 are feature-complete and M7's app side is done. STRATA boots and plays on a SmplTrek.**

| | |
| --- | --- |
| Host tests | 999 tests, 539,926 checks, all passing |
| Emulator scenarios | All seven pass, no overruns |
| On hardware | First boot 2026-11-22 on Enaga 0.2.0: the default beat plays, on-screen LOAD about 0–26% |
| Worst-case CPU load (emulator) | **23.49%** of the audio block with cl6x, 39.60% GCC-only, against a 60% target |
| Biggest cost | VA POLY at 2.16 points per voice with cl6x (3.82 GCC-only) |

What still needs a person with a unit:

- **Work the unit checklist.** [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) lists everything
  that needs the hardware, safety steps first: card handling, power cycles, sync, and how each
  machine sounds.
- **Measure it.** Precise PEAK figures and the six-voice chord load on the unit are still to be
  recorded in [docs/BUDGET.md](docs/BUDGET.md); the other figures are from the emulator, which
  models no caches.

Load figures are in [docs/BUDGET.md](docs/BUDGET.md), the state of each milestone in
[docs/PLAN.md](docs/PLAN.md), and next steps in [docs/HANDOFF.md](docs/HANDOFF.md).

## Documents

| Document | What it holds |
| --- | --- |
| [docs/HANDOFF.md](docs/HANDOFF.md) | **Start here.** Where it stands, what exists nowhere else, next steps |
| [docs/MANUAL.md](docs/MANUAL.md) | The user manual: concepts, a first-beat tutorial, the machines, performing, troubleshooting |
| [docs/CHEATSHEET.md](docs/CHEATSHEET.md) | One page of gestures, screens and abbreviations, for playing it |
| [docs/SPEC.md](docs/SPEC.md) | What STRATA is: sound architecture, sequencer, panel mapping, data model |
| [docs/PLAN.md](docs/PLAN.md) | The build order. Authoritative for milestones |
| [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) | What needs real hardware to close |
| [docs/BUDGET.md](docs/BUDGET.md) | Measured CPU and memory figures. Estimates do not go here |
| [docs/ENAGA_API.md](docs/ENAGA_API.md) | The verified Enaga signatures STRATA uses |
| [CHANGELOG.md](CHANGELOG.md) | What each release added, and its known limits |
| [docs/release/RELEASING.md](docs/release/RELEASING.md) | How a release zip is made and published |
| [docs/SOUND.md](docs/SOUND.md) | Where the drum synthesis comes from, and how it is used |
| [docs/REVIEW.md](docs/REVIEW.md) | The platform review the spec was corrected against |

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
