# Handoff — 2026-11-28

**Read this first, then `CLAUDE.md`, then `docs/SPEC.md` and `docs/PLAN.md`.** The platform review
the spec was corrected against is `docs/archive/REVIEW.md`: history, but its decisions stand.
This file holds what exists nowhere else in the repo: the state in one table, the next steps in
order, and the knowledge that lives in no code comment, commit message or other document. Notes
on single features (LFO, MOD slots, LOCK, choke, arps and the rest) are in
[DESIGN_NOTES.md](DESIGN_NOTES.md). When a fact here becomes stale, fix it here or delete it; do
not append a history. Measurements go to `docs/BUDGET.md`, releases to `CHANGELOG.md`.

**STRATA runs on the unit and has six releases**, v0.1.0 to v0.5.0 (`docs/PLAN.md` has the
table). **v0.5.0 (2026-11-27, Enaga 0.4.0) was planned as the release candidate for 1.0**; Roland lifted
the feature freeze on 2026-11-28. Since the tag,
`main` has gained the CPU guard fix (#211), 16th-note triplets, stereo width and the chorus send
(#213-#216, `CHANGELOG.md` "Unreleased"). On the unit, v0.3.1 and builds from `main` boot and
play; A01 reads 15-28% on the top line and dense generated parts 40-60% (Roland, 2026-11-27).
Most of what came after v0.2.0 has not been walked through on the unit yet: the walkthrough at
the top of `docs/UNIT_CHECKLIST.md` is the next pass.

---

## Where it stands in one table

| | |
| --- | --- |
| `make -C host test` | **3,923,641 checks, 0 failures** on `main` after #217 (2026-11-28): `core/`, `dsp/` and all of `app/`. The same under ASan and UBSan, with `ASAN_OPTIONS=detect_leaks=0` |
| CI | `.github/workflows/ci.yml` on every PR: the suite with GCC, again under ASan and UBSan at -O1, the layout check, the screenshots, and the repo checks (`audit_commands.sh`, `check_rules.sh`, `enaga_check.sh`, `check_scripts.sh`, the packaging and build-cache scripts' tests, the card builder's tests). A docs-only PR skips the test jobs. `tools/ci_local.sh` runs the same lanes in WSL. `enaga-upstream.yml` reports new Enaga releases weekly. No device build or scenario runs in CI |
| `tools/build.sh` | links. Last measured image (cl6x, 2026-11-25): payload 1,224,160 bytes, bss 1,605,248; no BOOT / MCU / SPI sections. `tests/test_memory.cpp` reads these two figures from here, so update both together |
| `tools/scenarios.sh` | **51 scenarios**, run on Roland's PC only. All 50 that existed passed with cl6x on 2026-11-27 with the chorus and stereo width in; worst **34.20%**, `genload` (GENERATE at full density on all eight tracks). The v0.5.0 build: worst 26.34% cl6x, 55.06% GCC-only. Every run's figures, and what each change cost, are in `docs/BUDGET.md`. `tools/profile_guest.py` gives a scenario's guest profile by function |
| unit | v0.3.1 and builds from `main` boot and play. Load on the top line (2026-11-27, main after #208): A01 15-28%, GENERATE on all tracks 40-60%, so the unit reads about 1.2-1.8x the emulator on a light beat and 2.0-2.4x on dense parts (`docs/BUDGET.md`, "GENERATE on every track"). MAX readings from SETUP are still owed |
| memory | init takes **97.32 MiB of a 120 MiB heap**: 22.68 MiB spare against an 8 MiB headroom rule. `tests/test_memory.cpp` itemises it. Never checked on the unit |
| milestones | M0–M7 code-complete; every load and feel exit criterion waits for the unit (`docs/PLAN.md`) |
| VA POLY | 2.16 points per voice with cl6x, 3.82 GCC-only (emulator). **A shared budget of 12 voices across all tracks** (`Engine::kPolyVoiceBudget`), WAVE POLY and FM4 included, unison counting two a note; provisional until the unit's readings |
| Enaga | pinned at release **v0.4.0 (`7487bbf`)** since 2026-11-27; the unit must run Enaga 0.4.0 firmware. `tools/enaga_check.sh` passes |
| branches | work lands on `main` through PRs; merges need Roland's go-ahead in the thread |

## What to do next, in order

0. **There is no feature freeze** (Roland, 2026-11-28: development comes before a stable
   release). Features keep landing on `main`; 1.0 comes when Roland says so. No thread tags or
   releases on its own: a release runs from its own thread (`docs/release/RELEASING.md`).
1. **On the unit: the walkthrough** at the top of `docs/UNIT_CHECKLIST.md`: N1-N7 (Enaga 0.4),
   the smoke test S1-S20, then W1-W60, on a cl6x build from `main` with Enaga 0.4.0 firmware.
2. **Write the readings down where they belong**: unit rows in `docs/BUDGET.md` (MAX from SETUP
   on A01 and on GENERATE, to pin both ratios), ticks in the reference half of
   `docs/UNIT_CHECKLIST.md`, the Unit column of `docs/FEATURES.md`, and anything odd as an issue
   or a thread.
3. **Settle the provisional numbers** from those readings: `kPolyVoiceBudget = 12`, the CPU
   guard's thresholds (`core/cpuguard.hpp`: rise at a 70% average over 85 ms or one 90% block,
   fall after a second's worth of blocks under 55%), and the performance mixer's worst case
   (host estimate: about +2 points).
4. **On Roland's PC, when there is time:** the per-voice costs still owed in `docs/BUDGET.md`
   (E13 the VA rework, E14 WAVE, both compilers; `scenario_wave` and `scenario_fm` themselves
   run in every full pass).
5. **Then, from the reference half of `docs/UNIT_CHECKLIST.md`**, the checks that decide whether a
   milestone stands: a start with no card, pulling the card during a save and during sample
   loading, swapping a sample on a sounding track, the M5 30-minute jam, and M7's hour locked to
   external MIDI clock.

**A new machine** (after 1.0; STRING is postponed) needs its rows in `docs/FEATURES.md`,
`docs/MANUAL.md` (section 3 and the SOUNDS table), `docs/CHEATSHEET.md` (machine table and
abbreviations), `CHANGELOG.md`, the machine list in `docs/OVERVIEW.md` and the README highlights.

Smaller open items, none urgent:

- **Mutation-test the `app/` tests.** `app/` became host-testable late and has not been held to
  the standard below as thoroughly as `core/` and `dsp/`.
- **Two Enaga reports to file upstream**, each with a failing case: GCC 13.3.0's internal compiler
  error on `core/project.cpp` (section 7), and the recurrence-bound `enaga_envelope_render` with
  the profile from `docs/BUDGET.md` ("Optimization review", finding 5). The draft is in the
  project files (`enaga-issue-draft.md`); Roland has not sent it yet.
- **Try Enaga's prebuilt GCC toolchain** (in its SDK zip). It is the same compiler
  `tools/build_toolchain.sh` spends 30 minutes building in a cloud session.
- `delay_ping_pong`, `delay_to_reverb` and the LONG delay's low cut share the ROUTE page with one
  empty slot, because nineteen send fields do not divide into four. Fine, but Roland may want
  them placed differently.
- **The sends' ambient modes** (LONG delay, HALL reverb with shimmer; `dsp/ambient.hpp`) are
  owed U9 on the unit (UNIT_CHECKLIST 13j).

---

## What is NOT on file anywhere else

### 1. Where things can run

| Where | Can | Cannot |
| --- | --- | --- |
| **Roland's PC** (`D:\DEV\STRATA`, builds in WSL) | everything a container can, plus **cl6x** (TI 8.5.0.LTS at `~/ti/ti-cgt-c6000_8.5.0.LTS`), and it writes the SD card | — |
| **Cloud session, `strata-toolchain` environment** | host suite; GCC-only device build after `tools/fetch_toolchain_sources.sh` and `tools/build_toolchain.sh` (~30 min); emulator scenarios after `tools/fetch_emulator.sh`; `tools/render.sh`; ASan | cl6x: TI's installer needs an interactive licence click |
| **Any other cloud session** | host suite (`git submodule update --init --recursive` first), `make -C host render` (the engine to WAV, no emulator), `tools/audit_commands.sh`, `tools/enaga_check.sh` | a device build or the emulator: `ftp.gnu.org` is not reachable |
| **Nowhere but the unit** | hearing it, feel, real load (caches, DDR), the heap | — |

So **every cloud load figure is GCC-only**, and a GCC-only figure over 60% means nothing either way.
Quote load as "emulator, cl6x" or "emulator, GCC-only" unless `docs/BUDGET.md` has a unit row.

Cloud-session limits: the git proxy refuses deleting a remote branch (403), and the
GitHub tools cannot set the repository's description or topics; both need Roland.

The `strata-toolchain` environment's setup script installs `flex` and clones the emulator with a
bare `git clone`; run `tools/fetch_emulator.sh` after it, which checks out
`STRATA_EMULATOR_COMMIT` from `tools/env.sh`. The emulator is a plain clone, not a submodule.
`tools/fetch_toolchain_sources.sh` exists because GCC's `contrib/download_prerequisites` fetches
GMP, MPFR, MPC and ISL over plain HTTP, which the HTTPS-only proxy does not serve.

### 2. The emulator presses keys and turns encoders, and has no pads

Since Enaga v0.3.0, `tools/emulate.py` takes `--key`, `--turn ENC:DETENTS@CYCLE` (encoders A-D
are 0-3, VALUE 4; a detent takes 12 ms) and `--card-mib`. `scenario_plock`, `scenario_visuals`
and `scenario_moveall` turn encoders; the older scenarios were written before and turn none,
which their headers say. A key is held 20 M cycles, so a turn 2 M cycles after a FUNC press
lands inside the hold (FUNC + turn). There is still no pad entry point, so no scenario can drive
a pad (seven of M5's eight performance tools, pad pressure, LOCK, the stutter, D6), and the
emulator cannot read the screen: a scenario sees only the log. Scenarios prove what keys and
turns reach: that every page **draws** on the C674x with the real font, which matters because a
draw off the canvas is a hard fault on the unit and a silent pass on the host. `app/ui_lights.hpp`
logs `scr <MODE> <PAGE>` on each screen change for this, and `notice_logged` puts a notice in the
log; on the unit the first tells "wedged" from "on a page I did not expect". Enaga's log ring
keeps 16 lines, so a scenario that logs more loses its first lines.

### 3. Why the unit may read higher than the emulator

The emulator models no caches and no DDR latency. Compared block for block, the unit read about
1.2-1.8x the emulator on A01 and 2.0-2.4x on dense generated parts (2026-11-27, `docs/BUDGET.md`,
"GENERATE on every track"): the ratio grows with the voices, so every headroom figure in
`docs/BUDGET.md` shrinks by more than one constant factor. Do not plan voice counts from
emulator figures alone, and do not take "hardware is faster" from anywhere until MAX says so.
High-pitch GRAIN and SAMPLE are where the gap should be widest (U3).

### 4. `tests/fakehost.cpp` is why `app/` is testable

Enaga's C++ API is header-only: every call is an inline function over `enaga::host()`, one C
struct of function pointers that the app runtime defines. `host/Makefile` does not link that
runtime, so `tests/fakehost.cpp` defines `enaga::host()` instead, and `app/engine.cpp`,
`app/storage.cpp` and `app/app.hpp` compile for the tests exactly as for the device, with no
`#ifdef` in `app/`. The fake adds what the real host cannot: a programmable clock
(`deliver_tick(24, 37)` puts a tick on frame 37 of the next block; `set_running`, `set_source`),
loads that complete only on `complete_loads()`, failure injection (`set_card(false)`, `set_full`,
`set_read_error`, `set_audio_context`), an in-memory card, and `read_prefix()` (because
`read_file` refuses a buffer smaller than the file, as the ABI does).

`tests/apprig.{hpp,cpp}` sits on top: one shared `Engine` and `Storage` (tens of MiB each, so
file-scope, never locals), `fresh()`, `pump()`, `pump_until_idle()`, `settle()`, `post()`,
`post_delta()`. **Traps it has already fallen into**, fixed and commented in place:

- `fresh()` posts `Cmd::stop` with `a = 1` (cut the tails), and clears the performance layer: a
  macro left at full position by an earlier test overrides the kit and makes a knob look dead.
- `settle()` waits for 400 silent blocks: the delay is silent between its repeats. A test that
  leaves a send open never settles, because `Mixer::add_track` restarts the delay's tail.
- A kick's block peak oscillates with its phase against the block. Compare **energy over a
  span**, never one block's peak.
- `tests/test_ui.cpp`'s `fresh()` puts every UI cursor, page and setting back to a fresh start
  (`UiProbe::view_to_defaults`), the factory scenes and macros, and the engine's performance
  layer. Still walk to a named page (`mixer_page(app, n)`) rather than count key presses, and use
  `knob_result` for encoder A on EDIT's first page (TUN, a semitone a click).
- `Cmd::stop` cuts with `a = 1`; `post_cmd`'s first byte is `a`. Both rigs once posted `c = 1`, a
  plain release, and a pad note rang into the next test.
- Thousands of commands without a pump overflow the command ring, and the drops land on the next
  test.

### 5. The standard this code is held to

- **Mutation testing, not just coverage.** A surviving mutation is a missing test or an
  equivalence argued in a comment in the owning file. docs/MUTATION.md has every file's score,
  the equivalent classes and the accepted gaps; dsp/freeze, lfo and tapestop are not run yet.
- **Golden renders pin every machine's sound** (tests/test_golden.cpp). A deliberate sound
  change regenerates them after listening (`STRATA_GOLDEN=print build/host/strata-tests >
  /tmp/g.txt; python3 tools/golden_update.py /tmp/g.txt`), and the diff of
  tests/golden_data.inc goes in the same commit.
- **AddressSanitizer and UBSan over the whole suite** (`make -C host test SANITIZE=1`, also in
  CI), with `detect_leaks=0`: `BeatRepeat::init` allocates its ring with `new[]` and nothing
  frees it, by design.
- **Bit-exactness gates a DSP refactor**, pinned with `==`, not a tolerance. Every optimization
  (prepare-on-change, cached VA increments, the anti-aliasing loop in
  `core/pcmloops.ti.c`) has a test that runs old and new side by side.
- **A feature with no gesture is dead code.** `tools/audit_commands.sh` lists every `Cmd` with a
  poster but no handler, or a handler but no poster. Run it after any change to
  `app/commands.hpp`. Its KNOWN list holds only `none`, the ring's empty slot.
- **Every screen passes the layout test** in `tests/test_ui.cpp`: Enaga's 5x7 font, 21 characters
  a line, and its charset (no `*`, `,` or `_`). First boot showed overlapping text; this is why.

### 6. Decisions in the code not to re-open without reading them

- **The sample swap protocol.** `clear_sample`, then wait for `Status::sample_epoch[track]` to
  change, then release the range, load, `set_sample`. `SamplePool::release` makes memory reusable
  at once and `core/` cannot see whether a voice still reads it; releasing early sounds like a
  broken sampler, not a broken lifetime.
- **Saving never reads a torn step**: drain the command ring first and defer by a frame if it is
  not empty. A load also needs the transport stopped. `Engine::project_for_load()` is the one
  deliberate hole in "the main context never writes pattern memory": a project is 1.03 MiB.
- **The queue protocol** (`core/queue.hpp`): a queued pattern change lands at the end of the
  playing pass, at once when the transport is stopped (there is no pass), and at once when the
  transport jumps back (PR #22).
- **Sends and master are encoder bytes, commanded by deltas.** A byte cannot hold a NaN and needs
  no float serializer; a delta, not the next byte, because the authoritative byte lives on the
  audio side and sixteen fast clicks would read the same stale byte. `PROJECT.BIN`'s third section
  is optional on read, and absent means the defaults, never "leave the struct alone".
- **Gate length on the `Trig`, release by note for KEYS.** `Trig::gate_frames` carries a step's
  length. A KEYS key-up posts `Cmd::release_note`, which releases by note, so it also lets go of
  the sequencer's same note on that track (decided 2026-11-22).
- **Machines are mono and the insert chain spreads them**; a stereo machine would double its
  track's insert work.
- **`VaFilterCache` is keyed on a float by exact equality**, on purpose: the question is "the same
  bits", and `VaPoly::prepare` clearing it is what makes a stale coefficient impossible.
- **The shared voice budget** (PR #30): past 12 VA POLY voices across all tracks, a new note
  chokes the quietest started voice on any POLY track (a 64-frame fade); a note is dropped only
  when every counted voice is still waiting for its frame. GRAIN and VA MONO are not counted.
- **A SAMPLE or GRAIN track with no file plays the placeholder** (Roland, 2026-11-22): `Cmd::clear_sample`
  gives both readers the static placeholder rather than nothing, so NEW and a reopened factory
  project keep the LOOP part, as a card-less boot does.
- **The factory content is one function**, `factory_project` in `core/factory.cpp`, used by both
  `Engine::init` (no card) and `Storage::start_new` (NEW). Change sounds and patterns there only.
  `project_init` stays blank because the tests' `fresh()` builds on it. It ships macros B ACID,
  C DUB and D DRIVE and leaves A empty, because pad pressure bends macro A on whichever track is
  selected.
- **A macro sweeps from the parameter's live value** (kit, then morph) to its high end, so at 0
  it is exactly the knob and EDIT and the morph keep working (PR #34). Before that, an assigned
  macro froze its parameters at `low`, which is why no macros shipped at first.
- **Navigation follows Enaga's menus**: UP/DOWN move between rows, LEFT/RIGHT sideways, OK picks,
  CLR goes back. New list-like settings go on the arrows, not new chords. `docs/CHEATSHEET.md` has
  the full map; on-screen hints name keys as printed on the panel (FUNC, PROJECT, EDIT).
- **Settled by Roland on 2026-11-22**: note names use sharps everywhere; the scale fold applies to
  both VAs and GRAIN, and to SAMPLE only while unsliced (SLC = 1); one p-lock is removed by
  holding the step and pressing the knob or tapping the parameter; FUNC+STOP also puts a running
  chain back on its first entry; CHAIN repeats are capped at 16; a sample may be 30 s of the
  file at up to 96 kHz (PR #31).
- **`core/` never includes an Enaga header**, which is why `kSendFields` / `kMasterFields` are
  stated in `core/project.hpp` and `static_assert`ed against the enum in `dsp/buses.hpp`.

### 7. Small things noticed and deliberately not fixed

- `dsp/va.cpp`'s flop table predicts ~1,660 cycles a voice; the measurement is ~18,900. The table
  is kept for the order of its stages, which held; `docs/BUDGET.md` has the real costs.
- `core/project.cpp`'s `project_read` carries `#pragma GCC optimize("no-unroll-loops")`: GCC
  13.3.0 for tic6x-elf crashes on it with `-O2 -funroll-loops` (`haifa-sched.cc:1597`). To be
  reported upstream with that file as the failing case.
- Two comments say float division is hardware on this core (`Engine::process`,
  `Granular::fit_length`). It is not; both are at control rate, so harmless.
- `tests/test_engine.cpp` and `tests/scenario_m4.sh` cite older versions of this file for two
  fixed bugs and the queue protocol. The bugs are in commit `1108ee0`; the queue
  protocol is in section 6 above.

- **Test order.** The host suite passes in registration order, with `--only` on any one test,
  as contiguous shards (`--shard K/N`, what `make -C host test JOBS=N` and CI run), and since
  2026-11-26 in shuffled orders: 37 seeds clean with `tools/test_shuffle.sh BIN [SEEDS...]`,
  where they had failed 28-55 checks each. What leaked, now fixed: the scenes and macros (they
  load beside `Project`, so `project_init` left them), every UI cursor and page, the take
  counter (tests named `STRATA_1.WAV`), the stream late counts (they only ever grow), the
  ambient rig's settings, a FREE LFO's phase, the CPU MAX, a stale card error and its notice, a
  held MUTE pad, the track meters' fall, and the UI rig's STOP flag. A failing seed keeps its
  order in `$TMPDIR/strata-shuffle-SEED.txt`; replay it with `strata-tests --names FILE` and cut
  the file in halves to find the test that leaks. Run the tool after adding tests to a rig.
  Known open (2026-11-27): `app_a_motion_recorded_lock_survives_the_block_copy` fails in
  `tools/test_shuffle.sh BIN 10` (`after_lock < unlocked`); halving finds no single culprit, so it
  takes two or more earlier tests together. The
  shards are still contiguous blocks; round robin would now work but nothing needs it.

### 8. Upstream, and moving the Enaga pin

[seawaffle/enaga#1](https://github.com/seawaffle/enaga/pull/1) is open: it keeps libgcc's unwinder
out of an app's link. It is not in v0.4.0 either. **Never modify `third_party/enaga`**; Enaga bugs go
upstream with a failing case.

The pin is **release v0.4.0 (`7487bbf`)** (moved from v0.3.0 on 2026-11-27, Roland's call for the
v0.5 release candidate; v0.3.0 had come from v0.2.0 on 2026-11-25 and added a `battery` entry to the
host table). v0.4.0 keeps the host table and ABI 0, and brings firmware fixes STRATA gains from:
MIDI clock over USB MIDI and no wrong tempo after a long pause, Enaga's own share of an audio block
down from about 7,000 to 2,600 cycles, FAT32's FSInfo written correctly, multi-sector SD reads and
writes, the watchdog fed while a large package loads, and a USB debug port with remote key input
(`tools/usb_debug.py`), a candidate for scripting unit checks. The unit needs 0.4.0 firmware. The rule is to pin release
tags only, and the one that matches the firmware on the unit: Enaga's loader checks nothing but `ENAGA_ABI_VERSION` (still 0),
so headers and firmware can drift apart with nothing failing. On the unit, v0.2.0 logged one
unreproduced crash on CHECK's MIDI page; the fix for a stalled USB host holding up DIN is in
v0.3.0. Do not add a package icon (`ENAGA_PACKAGE_ICON`) while any unit runs firmware older
than 0.2.0: that loader refuses non-zero flags.

#### Moving the Enaga pin

`tools/enaga_check.sh --upstream` reports newer releases and the commits since the pin that touch
`include/`, `runtime/`, `loader/` or `build_app.py`, and exits 3 when there is something to look
at. Without `--upstream` it only checks that the checkout, the docs and the emulator match their
pins. Moving the pin is Roland's decision; once made, one commit, in this order:

1. `git -C third_party/enaga fetch --tags && git -C third_party/enaga checkout vX.Y.Z`, then
   `git add third_party/enaga`.
2. `git -C third_party/enaga diff <old> vX.Y.Z -- include runtime loader tools/build_app.py`, and
   re-check every signature in `docs/ENAGA_API.md` against it. Any `ENAGA_ABI_VERSION` change
   means the unit's firmware must move in step.
3. `make -C host test` (`tests/test_platform.cpp` catches a changed platform constant), then
   `tools/build.sh` with GCC **and** with cl6x, then `tools/scenarios.sh`. Record any load change
   in `docs/BUDGET.md`.
4. Update the pin in `CLAUDE.md`, `docs/ENAGA_API.md`, `THIRD_PARTY_NOTICES.md` (the check fails
   until those three name the new tag and hash), and this section.
5. `tools/enaga_check.sh` passes.

The emulator moves the same way, by `STRATA_EMULATOR_COMMIT` in `tools/env.sh`, then
`tools/fetch_emulator.sh` and `tools/scenarios.sh`.

## Running commands on Roland's Windows host

The repo is on `D:` and every build runs in WSL. `CLAUDE.md` has the working form; the short
version is `MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c '...'`, single
quotes, and a script file in the scratchpad for anything longer than one command. In a Linux
container none of that applies.

**Threads work in worktrees** (`D:\DEV\STRATA-wt\<name>`, each with its own `ENAGA_BUILD`), made
by Windows git, because several threads share `D:\DEV\STRATA` and none may switch its branch. A
new worktree needs `git submodule update --init`. WSL's git cannot read such a worktree, so
`tools/env.sh` sets `STRATA_GIT` / `STRATA_GH` to Windows' `git.exe` / `gh.exe` there (WSL runs
them), and `tools/enaga_check.sh` and `tools/publish_release.sh` use those; gh and the GitHub
credentials exist only on Windows anyway. The emulator clone is untracked and lives in the main
checkout only: `tools/env.sh` finds it from a worktree, and `tools/scenarios.sh` refuses to run
without one rather than report every scenario's SKIP as a pass.

**Worktrees and build folders pile up.** Each thread leaves a worktree, a branch, an
`$HOME/strata-build-<name>` in WSL and often a `build\card-*` folder; nothing removes them. When a
thread's PR has merged, its worktree can go (`git.exe worktree remove`, then `git.exe worktree
prune`) together with its build folder. Clean up only from a script with absolute paths, and only
with Roland's yes.
