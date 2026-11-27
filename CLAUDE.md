# STRATA: rules for Claude Code

- Read docs/HANDOFF.md (the current state and next steps), then docs/SPEC.md and
  docs/PLAN.md before any change, and follow the milestone order in PLAN.md. Ask before changing
  a decision they record, or one in docs/archive/REVIEW.md (the platform review the spec was
  corrected against; code comments cite it by section).
- Target: an Enaga app on the Sonicware SmplTrek (TI C6748, 372 MHz). Enaga is pinned at
  release v0.3.0 (4878b7c), to match the firmware on the unit. Move it only by docs/HANDOFF.md
  "Moving the Enaga pin", and keep tools/enaga_check.sh passing. **docs/ENAGA_API.md has
  the verified signatures STRATA uses — read it first.**
  For anything not in it, read the header in third_party/enaga/include/enaga/. Never invent
  an Enaga API name: the plausible guesses are mostly wrong.
- Settings structs are built by a static `make(...)` at control rate, not by setters.
  Oscillators take `dt` AND `inv_dt` because hot loops must not divide. Prefer the block
  functions (`Svf::process`, `Envelope::render`) to the per-sample ones.

## Running commands (Windows host, WSL build)

The repo is on `D:` but every build runs in WSL. Commands therefore go through `wsl.exe`, and
Git Bash mangles them unless path conversion is off. This form works:

```sh
MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- \
    bash -c 'cd /mnt/d/DEV/STRATA && make -C host test'
```

- Use `bash -c` with single quotes. `bash -lc` with `VAR=...` assignments loses the variables.
- For anything longer than one command, **write a script to the scratchpad and run
  `bash /mnt/c/.../script.sh`**. Inline heredocs and `$VAR` get mangled, and `$?`/`PIPESTATUS`
  read back wrong through the wsl.exe layer, so verify exit codes inside a script file.
- `ls`, `cat`, `grep` and the editing tools work directly on `D:\DEV\STRATA` — only builds and
  tests need WSL.
- There is no cmake and no sudo. Do not add a dependency needing either.
- To build a subset by hand (when the full suite is red from someone else's work in progress),
  **compile Enaga's `runtime/dsp/*.c` with `gcc -std=c11`, not `g++`** — they use `restrict`,
  which is not C++. Two commands, then link:

```sh
gcc -std=c11 -O2 -Ithird_party/enaga/include -c third_party/enaga/runtime/dsp/*.c
g++ -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -O2 \
    -Icore -Idsp -Itests -Ithird_party/enaga/include \
    tests/runner.cpp tests/test_X.cpp dsp/X.cpp *.o -o /tmp/t && /tmp/t
```
- Do not write a master clock. Enaga owns it: <enaga/clock.hpp>, 96 PPQN, and
  clock::ticks() in audio() gives each tick's frame in the block. STRATA schedules steps on
  those ticks and owns swing, speed, length and micro-timing only.
- STRATA does not own the tempo. CLOCK SRC is the user's setting; start/stop/set_tempo
  return false under external sync. Always handle the refusal and follow on_transport.
- audio(): no allocation, blocking, files, drawing or LEDs. Buffers are interleaved stereo
  (L R L R). Cross-context state only through single-writer atomics or SPSC rings.
- Allocate all voices, buffers and pools in init(). Save state in stop() — destructors
  never run. No std::vector, std::string or iostreams. No libm: use <enaga/dsp.hpp> math.
- C++20, -fno-exceptions -fno-rtti, -Wall -Wextra -Werror. Stack frames at most 16 KiB,
  no VLAs or alloca.
- Never cast file bytes to structs: serialize byte by byte. No float division in hot loops.
- Hot inner loops go in *.ti.c files: plain C, branch-free (use ?:), building with both GCC
  and TI cl6x. cl6x is worth about 1.7x on STRATA's scenarios and the 60% target depends on
  it. cl6x runs only on Roland's PC, so cloud load figures are GCC-only.
- build_app.py compiles one flat folder with one include path. Every filename must be
  unique across core/, dsp/ and app/, and includes are bare ("pattern.hpp").
  tools/build.sh enforces the uniqueness; do not work around it.
- core/ never includes Enaga headers. Every core change comes with a host test
  (`make -C host test`).
- Every feature: host test, then an emulator scenario, then a line in docs/UNIT_CHECKLIST.md.
- After any DSP change, run the load scenario and update docs/BUDGET.md with figures both
  with and without cl6x. Worst case must stay under 60%. Cut effects or insert stages before
  cutting voices. Drum voices are cheap (one-shots that go idle); a sustaining voice is not:
  VA POLY costs 2.16 points per voice with cl6x and 3.82 GCC-only (emulator, docs/BUDGET.md),
  which is why VA POLY shares a budget of 12 voices across all tracks (Engine::kPolyVoiceBudget).
- Never modify third_party/enaga. Enaga bugs are reported upstream with a failing case.
- Never write the SPI flash. Never produce update files with BOOT or MCU sections.
- Before pushing C++ changes, run `make -C host warn -j8` as well as the tests: CI builds at
  -O1 too, where GCC reports uninitialized variables that the -O2 test build does not.
- Small commits, one concern each, each with its test.
