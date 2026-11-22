# STRATA

An 8-track techno performance groovebox for the Sonicware SmplTrek, built as an
[Enaga](https://github.com/seawaffle/enaga) app: hybrid synth and sample engines, a step
sequencer with parameter locks and trig conditions, and a performance layer meant for long,
evolving live sets.

STRATA is a working title. Nothing here is playable yet — see **Status**.

## Documents

| | |
| --- | --- |
| [docs/HANDOFF.md](docs/HANDOFF.md) | **Start here.** Where it stands, what exists nowhere else, next steps |
| [docs/SPEC.md](docs/SPEC.md) | What STRATA is: sound architecture, sequencer, panel mapping, data model |
| [docs/PLAN.md](docs/PLAN.md) | The build order. Authoritative for milestones |
| [docs/REVIEW.md](docs/REVIEW.md) | The platform review the spec was corrected against |
| [docs/BUDGET.md](docs/BUDGET.md) | Measured CPU and memory figures. Estimates do not go here |
| [docs/ENAGA_API.md](docs/ENAGA_API.md) | The verified Enaga signatures STRATA uses |
| [docs/SOUND.md](docs/SOUND.md) | Where the drum synthesis comes from, and how it is used |
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

**M0–M6 feature-complete, and M7's app side is done; nothing has run on a unit.** Eight tracks —
KICK, SNARE, two HATs, PERC, BASS, VA POLY chords and the LOOP sampler, with GRAIN and machine
switching — through per-track insert chains, tempo-synced delay and reverb sends, and a master
chain with the DJ filter and sidechain duck. The sequencer has parameter locks, trig conditions,
probability, ratchets, micro-timing, polymeter, live and motion recording, a Euclidean generator,
KEYS and 16 levels of undo. Projects, banks, queued pattern changes, autosave and background
sample loading are in (M4); scenes, morph, macros, mutes, beat repeat, throws and mutation (M5).

999 host tests / 539,926 checks and seven emulator scenarios, all passing. **Worst case measured:
39.60% of the block, GCC-only**, against a 60% target that assumes cl6x's ~2.2x. The budget risk
is VA POLY at 3.82 points per voice. Details in [docs/BUDGET.md](docs/BUDGET.md); the state of
each milestone in [docs/PLAN.md](docs/PLAN.md); next steps in [docs/HANDOFF.md](docs/HANDOFF.md).

To hear it without a unit, `tools/render.sh` renders the output to a WAV offline; the live web
panel (`tools/panel.sh`) runs at about 0.4× real time, so its sound breaks up.

Still needs a human:

- **Nothing has run on a unit.** Every load and timing figure is from the emulator, which models
  no caches. [docs/UNIT_CHECKLIST.md](docs/UNIT_CHECKLIST.md) holds everything that needs the
  hardware, safety steps first, and none of it is ticked.
- **cl6x figures.** TI's cl6x runs only on the developer's machine, so the cloud figures are
  GCC-only. The licence question once filed here is settled (docs/BUDGET.md): no TI code reaches
  `APP.ENA`.
- **Hearing it.** Nothing has been listened to on the hardware yet, including the scale fold on
  GRAIN and unsliced SAMPLE tracks (decided 2026-11-22, with notes spelled in sharps everywhere).

## Licence

MIT — see [LICENSE](LICENSE). STRATA is built on **Enaga** (MIT) and its drum synthesis follows
**Mutable Instruments Plaits** (MIT, © 2016 Émilie Gillet), reimplemented rather than ported;
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) carries both notices and says exactly what was
taken from each.
