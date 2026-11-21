# STRATA

An 8-track techno performance groovebox for the Sonicware SmplTrek, built as an
[Enaga](https://github.com/seawaffle/enaga) app: hybrid synth and sample engines, a step
sequencer with parameter locks and trig conditions, and a performance layer meant for long,
evolving live sets.

STRATA is a working title. Nothing here is playable yet — see **Status**.

## Documents

| | |
| --- | --- |
| [docs/SPEC.md](docs/SPEC.md) | What STRATA is: sound architecture, sequencer, panel mapping, data model |
| [docs/PLAN.md](docs/PLAN.md) | The build order. Authoritative for milestones |
| [docs/REVIEW.md](docs/REVIEW.md) | The platform review the spec was corrected against |
| [docs/BUDGET.md](docs/BUDGET.md) | Measured CPU and memory figures. Estimates do not go here |
| [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) | What needs real hardware to close |

## Building

Enaga's tools are **Linux-only**. On Windows that means WSL; there is no Windows build path.
No sudo and no cmake are needed.

```sh
git submodule update --init --recursive
git clone --depth 1 https://github.com/seawaffle/smpltrek-re third_party/smpltrek-re

tools/build_toolchain.sh     # once, ~30 min: builds tic6x-elf GCC into ~/strata-toolchain
tools/build.sh               # -> build/APP.ENA
tools/panel.sh               # build, then play it in the emulator at 127.0.0.1:8766
make -C host test            # portable code, on the host
```

Copy `build/APP.ENA` to `/ENAGA/APPS/STRATA/APP.ENA` on the card to run it on a unit.

`tools/env.sh` holds the environment and keeps the toolchain, build output and emulator off
the `/mnt/d` 9p mount, where a GCC build would crawl.

## Layout

```
core/    portable C++20: scheduler, pattern, steps, conditions, locks, mutation, serializer
dsp/     machines and effects; may use <enaga/dsp.h>, which also builds on the host
app/     Enaga glue: the App subclass, input, LEDs, screens, files
host/    host test runner and offline renderer
tests/   host tests and emulator scenarios
tools/   build, toolchain and emulator scripts
```

Enaga's `build_app.py` compiles **one flat directory** with one include path, so `tools/build.sh`
stages `core/`, `dsp/` and `app/` flat. Filenames are therefore a single namespace: they must be
unique across the three, and includes are bare (`#include "pattern.hpp"`). The build refuses a
collision rather than letting one file silently overwrite another.

## Status

**M3 complete.** Eight tracks play — KICK, SNARE, two HATs, PERC, BASS, a mono stab standing in
for CHORD until VA POLY arrives, and the LOOP sampler — through per-track insert chains, a delay
and a reverb send, and a master chain with a sidechain duck. The sequencer has parameter locks,
trig conditions, probability, ratchets, micro-timing, polymeter, live and motion recording, and
16 levels of undo. GRID, EDIT, MIXER and the step page are on the panel.

408 host tests / 221,035 checks, and three scripted emulator scenarios. **Worst case measured so
far: 23.61% of the audio block with cl6x** against M2's bar of 40% (44.26% without cl6x, which
does not pass) — see [docs/BUDGET.md](docs/BUDGET.md).

Next is **M4**: projects, banks, queued pattern changes and background sample loading.

To hear it without a unit, `tools/render.sh` renders the output to a WAV offline; the live web
panel (`tools/panel.sh`) runs at about 0.4× real time, so its sound breaks up.

Two things still need a human:

- **Shipping cl6x's output.** TI's cl6x 8.5.0.LTS is installed and the under-60% target depends
  on it — the measurements say a GCC-only release does not pass at all — but whether TI's licence
  permits redistributing its output inside a released `APP.ENA` has not been read by anyone.
- **Nothing has run on a unit.** Every load and timing figure here is from the emulator, which
  models no caches. Enaga's MIDI, SYNC, USB MIDI, input routing and pad pressure have never run
  on real hardware either, so expect platform bugs as well as app bugs.
  [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) accumulates everything that needs the
  hardware, safety steps first.

## Licence

MIT — see [LICENSE](LICENSE). STRATA is built on **Enaga** (MIT) and its drum synthesis follows
**Mutable Instruments Plaits** (MIT, © 2016 Émilie Gillet), reimplemented rather than ported;
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) carries both notices and says exactly what was
taken from each.
