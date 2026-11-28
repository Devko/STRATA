# Building and testing STRATA

Enaga's tools are **Linux-only**; on Windows, use WSL. No sudo and no cmake are needed.

```sh
git clone --recurse-submodules https://github.com/Devko/STRATA
cd STRATA
tools/fetch_emulator.sh      # the emulator, at the commit tools/env.sh pins

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
- `tools/emulator_screens.sh` redraws `docs/screens/` from the device build running in the
  emulator: it walks every screen with keys and knobs and keeps the frames the docs show (MUTATE
  and SCENE need pads, which the emulator lacks, so those two come from the host draw).
  `tools/screenshots.sh` is the quicker host-only route: the same screen code on the host build.
- `tools/readme_clip.py` records the README's clip (`docs/media/strata.gif`) the same way: the
  TECHNO showcase project playing, a scripted walk on a timeline, one frame every 50 ms, then a
  GIF in the screenshots' look. Its docstring has the two commands.

## Checks

[CI](../.github/workflows/ci.yml) runs on every pull request: the host suite with GCC and again
under AddressSanitizer and UBSan, the layout check over every screen, the screenshots, and the
repo checks: `tools/audit_commands.sh` (every command posted and handled), `tools/check_rules.sh`
(CLAUDE.md's source rules), `tools/enaga_check.sh` (the Enaga pin), `tools/check_scripts.sh`
(every script parses), the tests of the release packaging and the Enaga build cache, and the card
builder's tests in `web/`. A docs-only change skips the test jobs. `tools/ci_local.sh` runs the same lanes locally (`--scenarios` adds
the emulator scenarios).

The device build, the emulator scenarios (`tools/scenarios.sh`) and the load figures need the
tic6x toolchain, and their real numbers need cl6x, so they run on a developer's machine and go
into [BUDGET.md](BUDGET.md). A weekly job reports new Enaga releases; moving the pin is a
deliberate step ([HANDOFF.md](HANDOFF.md), "Moving the Enaga pin").

## Layout

```text
core/    portable C++20: scheduler, pattern, steps, conditions, locks, mutation, serializer
dsp/     machines and effects; may use <enaga/dsp.h>, which also builds on the host
app/     Enaga glue: the App subclass, input, LEDs, screens, files
host/    host test runner and offline renderer
tests/   host tests and emulator scenarios
tools/   build, toolchain and emulator scripts
```

Enaga's `build_app.py` compiles **one flat directory**, so filenames must be unique across
`core/`, `dsp/` and `app/`, and includes are bare (`#include "pattern.hpp"`). `tools/build.sh`
refuses a collision.

## Contributing

Contributors, human or AI, read [HANDOFF.md](HANDOFF.md) first, then [CLAUDE.md](../CLAUDE.md)
for the full set of rules: real-time limits in `audio()`, memory, file formats, and the order a
feature goes through (host test, emulator scenario, a line in
[UNIT_CHECKLIST.md](UNIT_CHECKLIST.md)). [release/RELEASING.md](release/RELEASING.md) says how a
release zip is made.
