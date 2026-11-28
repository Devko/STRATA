# STRATA: rules for Claude Code

## Before any change

- Read docs/HANDOFF.md (the current state and next steps), then docs/SPEC.md and docs/PLAN.md.
  Notes on single features are in docs/DESIGN_NOTES.md. Ask before changing a decision these
  record, or one in docs/archive/REVIEW.md (the platform review the spec was corrected against;
  code comments cite it by section).
- No feature freeze: features land on main as usual (Roland lifted the 1.0 freeze on
  2026-11-28; development comes before a stable release for now).
- **Merge only when Roland writes "merge it"** (or an equally plain yes) in the thread that owns
  the PR. A release runs from its own thread, only when Roland says so
  (docs/release/RELEASING.md). Never re-release an existing tag: bump the patch version.
- Keep HANDOFF.md current, not a history: measurements go to docs/BUDGET.md, feature notes to
  docs/DESIGN_NOTES.md, user-facing changes to CHANGELOG.md "Unreleased".

## Platform

- Target: an Enaga app on the Sonicware SmplTrek (TI C6748, 372 MHz; never call it a Zoom).
  Enaga is pinned at release v0.4.0 (7487bbf), to match the firmware on the unit. Move it only by
  docs/HANDOFF.md "Moving the Enaga pin", and keep tools/enaga_check.sh passing.
  **docs/ENAGA_API.md has the verified signatures STRATA uses — read it first.**
  For anything not in it, read the header in third_party/enaga/include/enaga/. Never invent
  an Enaga API name: the plausible guesses are mostly wrong.
- Do not write a master clock. Enaga owns it: <enaga/clock.hpp>, 96 PPQN, and
  clock::ticks() in audio() gives each tick's frame in the block. STRATA schedules steps on
  those ticks and owns swing, speed, length and micro-timing only.
- STRATA does not own the tempo. CLOCK SRC is the user's setting; start/stop/set_tempo
  return false under external sync. Always handle the refusal and follow on_transport.
- Device-only traps the host suite cannot see: init() must finish well inside Enaga's 1 s
  main-loop watchdog (build anything heavy on demand, never at boot); the runtime has no
  nothrow new (plain new is fatal on OOM, malloc returns null); tic6x GCC 13.3 can ICE at
  -O2 -funroll-loops (STRATA_NO_UNROLL). Before calling a card good, do a device build and the
  scenarios on Roland's PC.
- Never modify third_party/enaga. Enaga bugs are reported upstream with a failing case.
- Never write the SPI flash. Never produce update files with BOOT or MCU sections. Release zips
  do not bundle the Enaga binary (the card builder in web/ does that).

## Code

- Settings structs are built by a static `make(...)` at control rate, not by setters.
  Oscillators take `dt` AND `inv_dt` because hot loops must not divide. Prefer the block
  functions (`Svf::process`, `Envelope::render`) to the per-sample ones.
- audio(): no allocation, blocking, files, drawing or LEDs. Buffers are interleaved stereo
  (L R L R). Cross-context state only through single-writer atomics or SPSC rings.
- Allocate all voices, buffers and pools in init(). Save state in stop() — destructors
  never run. No std::vector, std::string or iostreams. No libm: use <enaga/dsp.hpp> math.
  init() keeps at least 8 MiB of the 120 MiB heap free (tests/test_memory.cpp itemises it).
- C++20, -fno-exceptions -fno-rtti, -Wall -Wextra -Werror. Stack frames at most 16 KiB,
  no VLAs or alloca.
- Never cast file bytes to structs: serialize byte by byte. No float division in hot loops.
- File format: from 1.0 every build reads every 1.0 file. New data goes in a new optional
  section (or on the end of an existing one); older files must open and sound as before.
- Hot inner loops go in *.ti.c files: plain C, branch-free (use ?:), building with both GCC
  and TI cl6x. cl6x is worth about 1.7x on STRATA's scenarios and the 60% target depends on
  it. cl6x runs only on Roland's PC, so cloud load figures are GCC-only. A cl6x miscompile
  stays out, even if it sounds fine.
- build_app.py compiles one flat folder with one include path. Every filename must be
  unique across core/, dsp/ and app/, and includes are bare ("pattern.hpp").
  tools/build.sh and tools/check_rules.sh enforce it; do not work around it.
- core/ never includes Enaga headers. Every core change comes with a host test
  (`make -C host test`). tools/check_rules.sh holds the rules a grep can check.

## The check loop: per PR, and the full pass

Scenarios are where the time goes (a full pass is 66 emulator runs, a quarter of an hour of
every core; docs/BUDGET.md, "Build and check times"), so a PR runs only what its change needs:

1. `make -C host build -j8 && make -C host test` (shards on every core by default; `JOBS=1` runs
   one process), and `make -C host warn -j8` before pushing C++.
2. `tools/scenarios.sh --changed`, with cl6x: the scenarios that name a changed file, the load
   set after a DSP change, and a smoke set (boot, the factory beat, the worst load, the live
   screens). `tools/scenario_pick.py --why FILE...` shows why each is picked. Add your new or
   changed scenario's file to the commit and it is picked too.
3. After a DSP change, `ENAGA_TI_CGT=none ENAGA_BUILD=$HOME/strata-build-<name>-gcc
   tools/scenarios.sh --changed` as well, for the GCC-only column in docs/BUDGET.md.

The full pass, `tools/scenarios.sh` with both compilers, is not per PR: it runs on `main` after a
batch of merges, before a card goes to the unit and before a release, from the thread doing that,
and is logged in docs/SCENARIO_LOG.md. A change to the harness itself (tools/env.sh, build.sh,
enaga_cached.py, emu_drive.py, scenarios.sh, Enaga) makes `--changed` pick every scenario.

Scenarios queue for machine-wide emulator slots (`STRATA_EMU_SLOTS`, default seven of every eight cores),
so run them at any `-j` from any worktree: parallel threads take turns instead of slowing every
run down. The device build caches each object by its inputs, shared by every worktree
(`~/strata-build/cache/obj`), and compiles the rest in parallel; `STRATA_ENAGA_CACHE=off` builds
from scratch.

## Every feature or fix

- Host test, then an emulator scenario (scenarios call `python3 -B tools/enaga_cached.py
  emulate`, never emulate.py; walk a menu with tools/menu_rows.py, never a press count), then a
  line in docs/UNIT_CHECKLIST.md, a row in docs/FEATURES.md and an entry in CHANGELOG.md
  "Unreleased".
- UI: every screen passes the layout test in tests/test_ui.cpp (Enaga's 5x7 font, 21 characters
  a line, no '*', ',' or '_'). Every input must visibly do something (screen or LED): a key with
  no job on a screen goes in `quiet_reason` with a reason. Gestures follow docs/CONTROLS.md
  (change it there first) and update the FUNC help in core/help.cpp. Redraw docs/screens with
  tools/screenshots.sh after a screen changes.
- A deliberate sound change regenerates the golden renders: `STRATA_GOLDEN=print
  build/host/strata-tests > /tmp/g.txt; python3 tools/golden_update.py /tmp/g.txt`, and commits
  tests/golden_data.inc with the change.
- After any DSP change, run the load scenario and update docs/BUDGET.md with figures both
  with and without cl6x. Worst case must stay under 60%. Cut effects or insert stages before
  cutting voices. Drum voices are cheap (one-shots that go idle); a sustaining voice is not:
  VA POLY costs 2.16 points per voice with cl6x and 3.82 GCC-only (emulator, docs/BUDGET.md),
  which is why VA POLY, WAVE POLY and FM4 share a budget of 12 voices across all tracks
  (Engine::kPolyVoiceBudget; a unison note counts two).
- Docs read as a manual, not a changelog: no "now", "as before" or "used to" in MANUAL or
  CHEATSHEET, no hype, no process chatter in README.
- Small commits, one concern each, each with its test.
- Before pushing C++ changes, run `make -C host warn -j8` as well as the tests: CI builds at
  -O1 too, where GCC reports uninitialized variables that the -O2 test build does not.

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
- Several threads share `D:\DEV\STRATA`: never switch its branch. Work in your own worktree,
  made from WSL with `tools/wt.sh new <name> <your-branch>` (in `D:\DEV\STRATA-wt\<name>`, with
  Enaga checked out; `tools/env.sh` there sets `ENAGA_BUILD=$HOME/strata-build-<name>`). When its PR has merged, `tools/wt.sh rm <name>` removes it and its build
  folder; it refuses uncommitted, unpushed or stray ignored files. Never remove another thread's
  worktree. Run other cleanup commands only from scripts with absolute paths. From WSL,
  `tools/env.sh` finds the main checkout's emulator and Windows' `git.exe`/`gh.exe` for it
  (docs/HANDOFF.md, "Running commands on Roland's Windows host").
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
