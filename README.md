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

**M0 complete**: toolchain, repo skeleton, staged build, host test harness, and an app that
reports the device, the audio load and Enaga's clock state.

Next is **M0.5**, a budget spike that measures the fixed per-block cost of 8 insert chains and
both send effects before any engine is written — the largest unmeasured term in the design.

Two things need a human:

- **TI's cl6x is not installed.** It is worth about 2.2× on DSP code and the under-60% target
  depends on it, but installing it means accepting TI's licence, and shipping its output raises
  a redistribution question. See [docs/BUDGET.md](docs/BUDGET.md).
- **Nothing has run on a unit.** Enaga's MIDI, SYNC, USB MIDI, input routing and pad pressure
  have never run on real hardware either, so expect platform bugs as well as app bugs.
