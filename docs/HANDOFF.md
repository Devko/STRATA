# Handoff — 2026-11-24

**Read this first, then `CLAUDE.md`, then `docs/SPEC.md`, `docs/REVIEW.md` and `docs/PLAN.md`.**
This file holds what exists nowhere else in the repo: the state in one table, the next steps in
order, and the knowledge that lives in no code comment, commit message or other document. When a
fact here becomes stale, fix it here or delete it; do not append a history.

**STRATA runs on the unit and has four releases.** v0.1.0 (2026-11-22), v0.2.0, v0.3.0 and v0.3.1 (2026-11-23,
early betas) are on GitHub; v0.3.1 boots and plays on the unit (2026-11-24), but nothing from 0.3.x has been measured or walked through there yet. On the unit the default beat loads under 30%, and a long, heavy
session peaked at 84.3% MAX with no dropouts. Almost everything added since the first evening has
been heard by nobody: the walkthrough at the top of `docs/UNIT_CHECKLIST.md` is the next pass.

---

## Where it stands in one table

| | |
| --- | --- |
| `make -C host test` | **1,754 tests, 3,485,153 checks, 0 failures** on `main` after PR #126: `core/`, `dsp/` and all of `app/` |
| CI | `.github/workflows/ci.yml` on every PR: the suite with GCC, again under ASan and UBSan at -O1 (`make -C host test SANITIZE=1`), the layout check, `tools/audit_commands.sh` and `tools/enaga_check.sh`. A docs-only PR skips the test jobs. `enaga-upstream.yml` runs `enaga_check.sh --upstream` weekly and opens an issue when upstream has moved. No device build or scenario runs in CI |
| `tools/build.sh` | links. Last measured image (cl6x, 2026-11-25, EDIT's pictures): payload 1,117,368 bytes, bss 1,565,352 (with LOCK and play modes); no BOOT / MCU / SPI sections. `tests/test_memory.cpp` reads these two figures from here, so update both together |
| `tools/scenarios.sh` | **33 scenarios**, run on Roland's PC only. All 33 pass with cl6x with EDIT's waveforms and envelopes merged onto play modes, LOCK and FX GLIDE (`scenario_pictures` new: every picture page walked while the beat plays; worst 30.55%, ambient). All 32 passed with cl6x after play modes merged onto LOCK and FX GLIDE (`scenario_playmode` new: FUNC menu TRACK PLAY walked to RND4 while the beat plays, then a save; worst 30.54%, ambient). All 31 passed with cl6x with LOCK and FX GLIDE (`scenario_glide` new; worst 30.34%, ambient). All 30 pass with cl6x after backlog D1/D2/D5 merged onto locks on screen (`scenario_slots` new: mixer slots 7 and 13 and a retired chord). All 29 passed with cl6x after locks on screen (`scenario_plock` new: hold STEP + turn on GRID and EDIT; worst 30.18%, ambient). All 27 pass with cl6x on `ce5fc6b` (the v0.3.1 build, 2026-11-23), `scenario_wave` once its table check followed #133; all 27 again with cl6x on Enaga v0.3.0 (2026-11-25), loads unchanged (worst 30.16%, ambient); all 28 with cl6x after D12 (`scenario_stepkeys` new, `scenario_m6` now checks KEYS' STEP keys toggle steps, since the emulator cannot press pads) |
| unit | v0.3.1 on Enaga 0.2.0 boots and plays (2026-11-24). Load figures are from the v0.2.0-era build: under 30% on the default beat, 84.3% MAX in heavy play. Nothing after #111 has been measured or walked through on the unit |
| memory | init takes **97.04 MiB of a 120 MiB heap** (LOCK's 1.06 MiB included): 22.96 MiB spare against an 8 MiB headroom rule. `tests/test_memory.cpp` itemises it. Never checked on the unit |
| milestones | M0–M7 code-complete; every load and feel exit criterion waits for the unit (`docs/PLAN.md`) |
| VA POLY | 2.16 points per voice with cl6x, 3.82 GCC-only (emulator). **A shared budget of 12 voices across all tracks** (`Engine::kPolyVoiceBudget`), WAVE POLY included, provisional until the unit's readings |
| Enaga | pinned at release **v0.3.0 (`4878b7c`)** since 2026-11-25; the unit must run Enaga 0.3.0 firmware. `tools/enaga_check.sh` passes |
| branches | work lands on `main` through PRs; merges need Roland's go-ahead in the thread |

## What to do next, in order

1. **On Roland's PC: the load runs still owed.** `tools/scenarios.sh` once on `main` (it has
   `scenario_wave.sh` and `scenario_fm.sh`, never run), then E13 (the VA rework), E14 (WAVE's cost
   per voice and its boot time) and E15 (FM4's cost per voice) from `docs/BUDGET.md`, with cl6x and with `ENAGA_TI_CGT=none`. BUDGET has only
   host estimates for both so far.
2. **On the unit: today's walkthrough** at the top of `docs/UNIT_CHECKLIST.md`, items 1-19, on a
   build from `main` at PR #126 or later with cl6x. It covers everything since v0.1.0, including
   the performance mixer and looper (16), the stock sample folders (17), the VA OSC page (18),
   WAVE (19) and FM4 (20).
3. **Write the readings down where they belong**: unit rows in `docs/BUDGET.md`, ticks in the
   reference half of `docs/UNIT_CHECKLIST.md`, the Unit column of `docs/FEATURES.md`, and anything
   odd as an issue or a thread.
4. **Settle the provisional numbers** from those readings: `kPolyVoiceBudget = 12`, the CPU
   guard's thresholds (`core/cpuguard.hpp`: rise at a 70% average or one 90% block, fall after a
   second under 55%), and the performance mixer's worst case (host estimate: about +2 points).
5. **A new machine** needs its rows in `docs/FEATURES.md`, `docs/MANUAL.md` (section 3 and the
   SOUNDS table), `docs/CHEATSHEET.md` (machine table and abbreviations), `CHANGELOG.md` and the
   machine list in `docs/OVERVIEW.md` and the README highlights. STRING is postponed.
6. **Then, from the reference half of `docs/UNIT_CHECKLIST.md`**, the checks that decide whether a
   milestone stands: a start with no card, pulling the card during a save and during sample
   loading, swapping a sample on a sounding track, the M5 30-minute jam, and M7's hour locked to
   external MIDI clock.

**Controls.** docs/CONTROLS.md is the reference the key handlers follow; change a gesture there
first, then in the code, `core/help.cpp`, the cheatsheet, the manual and the unit checklist.

**Phase 2 of the backlog** (Claude Doc "STRATA new approach: review, overlay and backlog";
Roland accepted all 15 decisions as recommended, 2026-11-25). Done: macro learn by turning
(D11), STEP keys as steps (D12), hold STEP + pad edits the step (D6), the retired chords and
slots 7 MUTE BUS / 13 MORPH (D1, D2, D5), LOCK on FUNC + MUTATE (D3, D4, D13, D14;
`core/lockstore.hpp`, docs/CONTROLS.md "LOCK") with FX GLIDE and the momentary push. **Next, in
order:** bounce-to-track ("put on track n" after a RESAMPLE bounce, one OK); then hold STEP +
THROW (the delay-send lock D6 left out).

LOCK, for whoever touches it next: the audio side keeps the 128 locks and the swap-back and does
the copies (`Engine::capture_live` / `apply_live`); the UI only times the hold (500 ms), posts
`lock_store` / `lock_tap`, and acts on `Status::lock_event` (notice, a pending MUTATE dropped,
morph and macro shadows re-read, the mixer ridden to rest). The concept's "no lock stored: jump
to the pattern as last saved" is NOT built: it needs a copy of every pattern as loaded; a tap
with no lock says `NO LOCK: HOLD STORES`. Undo is untouched by a jump. The emulator cannot press
pads, so LOCK is host-tested only (tests/test_lockstore.cpp, tests/test_ui.cpp); UNIT_CHECKLIST
16c walks it. FX GLIDE lives in the UI (`perf_ride_to`, `core/perffx.hpp` `perf_ride_at`); the
UI tests run with it OFF (`fresh()`), and its own tests turn it on. FUNC + MUTATE, FUNC + push D and
FUNC + VALUE only show a hint now (`notice_logged` in app/app.hpp), so LOCK can take FUNC +
MUTATE outright. The master DJ byte stayed on MIXER's MST page on purpose: an old project saved
with it set must be able to put it back.

Smaller open items, none urgent:

- **KICK's click path** runs for the whole note after its transient has decayed: about a quarter
  of the host profile. Skipping it is not bit-exact (the shared noise advances differently), so it
  needs a listening test first. `docs/BUDGET.md`, "Optimization review", finding 4.
- **Mutation-test the `app/` tests.** `app/` became host-testable late and has not been held to
  the standard below as thoroughly as `core/` and `dsp/`.
- **Two Enaga reports to file upstream**, each with a failing case: GCC 13.3.0's internal compiler
  error on `core/project.cpp` (below), and the recurrence-bound `enaga_envelope_render` with the
  profile from `docs/BUDGET.md` (finding 5). Roland parked upstream work on 2026-11-23 ("focus on
  STRATA first").
- **Try Enaga v0.2.0's prebuilt GCC toolchain** (in its SDK zip). It is the same compiler
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

Cloud-session limits found today: the git proxy refuses deleting a remote branch (403), and the
GitHub tools cannot set the repository's description or topics; both need Roland.

The `strata-toolchain` environment's setup script installs `flex` and clones the emulator with a
bare `git clone`; run `tools/fetch_emulator.sh` after it, which checks out
`STRATA_EMULATOR_COMMIT` from `tools/env.sh`. The emulator is a plain clone, not a submodule.
`tools/fetch_toolchain_sources.sh` exists because GCC's `contrib/download_prerequisites` fetches
GMP, MPFR, MPC and ISL over plain HTTP, which the HTTPS-only proxy does not serve.

### 2. The emulator can press keys and nothing else

`emulate.py` has `--key` and nothing more, and `emulator/core/core.h` has no pad or encoder entry
point at all. So no scenario can drive a pad (seven of M5's eight performance tools, pad pressure)
or turn an encoder (every send, master and EDIT setting). Scenarios prove what keys reach: that
every page **draws** on the C674x with the real font, which matters because a draw off the canvas
is a hard fault on the unit and a silent pass on the host. `app/app.hpp` logs `scr <MODE> <PAGE>`
on each screen change for this; on the unit it tells "wedged" from "on a page I did not expect".

### 3. Why the unit may read higher than the emulator

The emulator models no caches and no DDR latency. Its cl6x default beat is 19.17%
(`scenario_m3.sh`); the unit's LOAD showed up to about 26% at first boot. If MAX confirms that
ratio, every headroom figure in `docs/BUDGET.md` shrinks by it. Do not plan voice counts from
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
- UI mode state (`mixer_bank_` and the like) survives `fresh()` on purpose; walk to a named page
  (`mixer_page(app, n)` in `tests/test_ui.cpp`) instead of counting key presses.
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
  merged today (prepare-on-change, cached VA increments, the anti-aliasing loop in
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
  and as contiguous shards (`--shard K/N`, what `make -C host test JOBS=N` and CI run). It does
  NOT pass in a random order: a shuffled run fails dozens of `ui_`, `app_`, `storage_` and
  `ambient_` tests and once crashed, because `tests/apprig.cpp`'s `fresh()` still lets some engine
  and app state through from one test to the next. Sharding found and fixed three such leaks
  (the bar count and an armed take, the previous kit's sample requests, a stale project error);
  the rest are why the shards are contiguous blocks and not round robin. Replay any order with
  `strata-tests --names FILE` (one test name per line) to find the test that leaks.

### 8. Upstream, and moving the Enaga pin

[seawaffle/enaga#1](https://github.com/seawaffle/enaga/pull/1) is open: it keeps libgcc's unwinder
out of an app's link. It is not in v0.3.0 either. **Never modify `third_party/enaga`**; Enaga bugs go
upstream with a failing case.

The pin is **release v0.3.0 (`4878b7c`)** (moved from v0.2.0 on 2026-11-25; v0.3.0 adds a
`battery` entry to the host table, so the unit needs 0.3.0 firmware). The rule is to pin release
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
