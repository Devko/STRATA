# Handoff — 2026-11-22

**Read this first, then `CLAUDE.md`, then `docs/SPEC.md`, `docs/REVIEW.md` and `docs/PLAN.md`.**

**STRATA has booted on the unit.** On 2026-11-22 Roland ran it on his SmplTrek with Enaga 0.2.0
firmware: it starts and plays the default beat, with the on-screen LOAD reading about 0–26%.
Precise PEAK and six-voice chord figures are still to come from a local build.

Every other figure in this project comes from a cloud container: the host suite, a tic6x device
build and an instruction-counting emulator with **no cache model**. No exit criterion involving
load or feel is met until the unit confirms it. Almost all of the 213 checklist boxes in
`docs/UNIT_CHECKLIST.md` are still open.

What gates everything now: **work `docs/UNIT_CHECKLIST.md` on the unit**.

---

## Where it stands in one table

| | |
| --- | --- |
| `make -C host test` | **999 tests, 539,926 checks, 0 failures** — `core/`, `dsp/` and all of `app/` |
| `tools/build.sh` | **links.** `APP.ENA` 484,160 bytes, bss 1,334,848, no BOOT / MCU / SPI sections |
| `tools/scenarios.sh` | **all 7 pass.** m0 2.87%, m1 39.60%, m3 31.92%, m4 27.15%, m5 34.54%, m6 36.61%, m7 39.60%. No overruns |
| worst measured load | **39.60%** of the block, GCC-only, against a 60% target that assumes cl6x's ~2.2x |
| milestones | M0–M6 feature-complete; **M7's app side is done** and the rest of M7 is unit verification by definition |
| the budget risk | **VA POLY, 3.82 points per voice** after an optimisation loop. Six voices is 22.9% of the block on its own, 10.1x what `docs/SPEC.md` budgets for the whole machine |
| branch | `main`, pushed. The user chose to develop on `main` directly |

## What is NOT on file anywhere, and should be

Everything below is in this file because it exists nowhere else in the repo.

### 1. cl6x cannot be run from a container, and that is now the only blocker that matters

TI's installer needs an interactive licence click, so every load figure in `docs/BUDGET.md` is
the **GCC-only** build. `tools/build.sh` prints `built with GCC only (TI's cl6x not found)` and
carries on. The toolchain is on the user's machine at `~/ti/ti-cgt-c6000_8.5.0.LTS`.

**The cl6x LICENCE question is settled and was a false alarm**, carried as the project's highest
risk for months. The premise was that shipping a binary built with cl6x needs a licence grant.
Reading the build disproved it: no TI code, no TI headers and no TI run-time library reach
`APP.ENA` — cl6x compiles our own and Enaga's source, and GNU `ld` links it with `-nostdlib`
(`nm -u` on the result is empty of TI symbols). Only instruction sequences remain, which is what
a compiler is for. `docs/BUDGET.md` has the file-by-file table. The one third-party library
actually in the binary is **libgcc**, which no document had listed as a risk at all.

What is left is a ten-minute read of the licence text the installer already wrote to that
directory — a confirmation, not a gate. **The AVAILABILITY half of the risk stands**: the 60%
target needs the 2.2x, and 2.2x was measured on the drum loops, which are already branch-free.
It is untested on the branchy melodic code.

### 2. The emulator can press KEYS and nothing else

`emulate.py` has one `--key` option. There is **no pad and no encoder entry point in
`emulator/core/core.h` at all** (checked, not assumed). Consequences that shape every scenario:

- `docs/SPEC.md` puts seven of M5's eight performance tools on the upper **pads** — FILL, REPEAT,
  MUTE mode, both scene selectors, THROW, MUTATE. **None can be driven by any scenario.**
- Every send and master setting is on an **encoder**, so none can be *changed* from a scenario.
  `tests/scenario_m7.sh` therefore proves what keys alone can reach: that all four new MIXER
  pages and PROJECT's SETUP page **draw** on the C674x, in the real build, with the real font.
  A draw that runs off the canvas is a hard fault on the unit and a silent pass on the host.
- Held beat repeat, a moving morph and a moving macro are unmeasured for the same reason.

To make a screen observable from a scenario, `app/app.hpp` logs `scr <MODE> <PAGE>` on a change.
That is also genuinely useful in a unit log: it is the difference between "the app is wedged" and
"the app is on a page I did not expect".

### 3. How a cloud session gets a device build and an emulator

Two commands, and it took real work to discover:

```sh
tools/fetch_toolchain_sources.sh
```

```sh
tools/build_toolchain.sh
```

`binutils` takes a few minutes, `gcc` ten to thirty. `tools/fetch_toolchain_sources.sh` exists
because GCC's own `contrib/download_prerequisites` fetches GMP, MPFR, MPC and ISL over **plain
HTTP**, which an HTTPS-only proxy does not serve at all; it fetches the same four over HTTPS,
lays them out where `download_prerequisites` would, and retries.

The environment needs `ftp.gnu.org` and `gcc.gnu.org` allowed. A personal cloud environment
named `strata-toolchain` has them, plus a setup script that installs `flex` (only in the apt
sources after `apt-get update`) and clones the emulator:

```sh
tools/fetch_emulator.sh     # clones it and checks out STRATA_EMULATOR_COMMIT (tools/env.sh)
```

That is a **plain clone, not a submodule**, so `tools/env.sh` pins its commit instead; a bare
`git clone` would take whatever upstream is that day. (The `strata-toolchain` environment's setup
script still does a bare clone: run `tools/fetch_emulator.sh` after it.) The toolchain build is deliberately NOT in the setup
script: it would make every session start ten minutes slower. So a fresh session must run those
two commands before any scenario, and they are ~30 minutes of the session's time.

### 4. `tests/fakehost.cpp` is why `app/` is testable, and how it works

Enaga's C++ API is header-only: every call in `<enaga/app.hpp>`, `<enaga/clock.hpp>`,
`<enaga/files.hpp>` and the rest is an inline function over `enaga::host()`, which returns one
plain C struct of **38 function pointers** that is "defined by the app runtime". `host/Makefile`
does not link Enaga's `runtime.cpp`, so `tests/fakehost.cpp` defines it instead:

```cpp
namespace enaga {
const enaga_host_t &host() { return strata::test::FakeHost::the().abi(); }
}
```

`app/engine.cpp`, `app/storage.cpp` and `app/app.hpp` therefore compile **for the tests exactly
as for the device, with no `#ifdef` anywhere in `app/`**. The fake gives the tests things the
real host cannot:

- a **programmable clock**: `deliver_tick(24, 37)` puts a tick on frame 37 of the next block;
  `set_running(bool)`, `set_source(ENAGA_CLOCK_MIDI)`, `control_calls(ENAGA_CLOCK_TEMPO)`.
- **held asynchrony**: a `files::load` does not complete until `complete_loads()` is called, so a
  test can assert what the app does *while* a load is in flight.
- **failure injection**: `set_card(false)`, `set_full`, `set_read_error`, `set_audio_context`.
- an in-memory card: a 4 MiB arena and a 48-entry table.
- `read_prefix()`, which exists because `read_file` mirrors the ABI and **refuses a buffer
  smaller than the file**.

`tests/apprig.{hpp,cpp}` is the rig on top: one shared `Engine`/`Storage` (85 MiB each, so they
are file-scope and never locals), `fresh()`, `pump()`, `pump_until_idle()`, `settle()`,
`last_block()`, `post()`, `post_delta()`, `set_send_byte()`, `set_master_byte()`.

**Traps the rig has already fallen into, all of them fixed and commented in place:**

- `fresh()` must post `Cmd::stop` with **`a = 1`** (cut the tails). It had `c = 1`, so the
  "stop twice" never happened and a 60 ms release tail polluted every onset measurement.
- `fresh()` must clear the **performance layer**. `apply_performance` builds what the machines
  read as kit → morph → macros, so a macro left assigned at full position by an earlier test
  overrides the kit — and a test that then sets a kit parameter and listens finds its knob dead.
  It cost real time to find and looked exactly like a per-block-copy bug returning.
- `settle()` waits for **400 consecutive silent blocks**, not one. The master delay is silent
  *between* its repeats, so one silent block proves nothing.
- `Mixer::add_track` **restarts the delay's tail every block** a track with a non-zero send gain
  is rendered. A test that leaves a send open can never settle. Drop the send gain first.
- A kick's **block peak oscillates with its phase** against the block length — about 15 blocks
  per cycle at 50 Hz. Any "louder/quieter" claim read from one block is noise. Measure **energy
  over a whole span**. Three onset-counting attempts died on this, and an agent's "later tick =
  quieter block" ordering claim was withdrawn for the same reason (measured 0.113, 0.184, 0.180,
  0.127 — not monotone).
- `mixer_bank_` and the UI's other mode state are **not reset by `fresh()`** — that is correct,
  it is where the user left the mode. So a test that presses the selection key a fixed number of
  times lands wherever the previous test got to. `tests/test_ui.cpp`'s `mixer_page(app, n)` walks
  to a named page instead.
- Posting thousands of commands without pumping **overflows the 16-byte command ring**, and the
  drops land on whatever the next test posted. The tempo test now pumps as it sweeps.

### 5. The standard this code is held to

- **Mutation testing**, not just coverage. A surviving mutation is either a missing test or an
  equivalence that must be **argued in the owning file**. Example from this session: the
  `machines_[track]->prepare()` in `Engine::fire` turned out to be redundant — `audio()` resolves
  every tick *before* the render loop, and that loop prepares from the same `effective_`.
  Removing it changed nothing in 996 tests. It was **kept**, with the comment rewritten to say
  what it actually defends (that no machine's `trigger` reads a prepared setting). The comment
  was the thing that needed fixing, not the line.
- **AddressSanitizer over the whole suite after any DSP change.** It found a real harness defect:
  `CHECK_EQ` compared `const char *` with `==`, so five name tests were comparing addresses and
  passed only because `-O2` pools identical string literals. At `-O1` under ASan all five failed
  at once. Fixed with a non-template `strcmp` overload in `tests/check.hpp`.
  **Run it with `detect_leaks=0`**: `BeatRepeat::init` allocates its 3 MiB ring with `new[]` and
  nothing frees it, by design — on the unit `init()` runs once and destructors never run — so
  LeakSanitizer reports ~16 allocations from the repeat tests. Those are expected.
- **Bit-exactness is the gate for a refactor of DSP.** Both of this session's VA POLY
  optimisations are bit-exact and each has a test that pins equality with `==` rather than a
  tolerance. Float multiplication is not associative: a fusion that formed `(gain * clipped) *
  env` instead of `gain * (clipped * env)` would be a different signal and every exact
  expectation would drift one ulp at a time.
- **A feature with no gesture is dead code, and M5 shipped four of them.** `core/scale` went in
  with no way to pick a scale; `mute_store`, `mute_recall`, `set_auto_fill` and `macro_assign`
  all went in with no key. The sends and the master were the same mistake at a larger scale: two
  commands with **no handler and no caller at all**. The check is mechanical and worth running
  after every milestone:

```sh
sed -n "/^enum class Cmd/,/^};/p" app/commands.hpp | grep -oE '^    [a-z_]+,' | tr -d ' ,' | while read -r n; do printf '%-18s %s %s\n' "$n" "$(grep -c "Cmd::$n\b" app/app.hpp)" "$(grep -c "Cmd::$n\b" app/storage.cpp)"; done | awk '$2 == 0 && $3 == 0'
```

Both counts, because the card side posts six commands the UI never does — `clear_sample`,
`set_sample`, `project_loaded`, `kit_store`, `clear_lock`, `resample_release`. As of this
handoff the audit's `kit_recall` exception is gone: the kit browser (F1 + F8 twice) posts it from
`app/storage.cpp`, with `a` = 1 to copy the kit the browser read into the engine's inbox
(`Engine::kit_for_recall`). `tools/audit_commands.sh` dropped it from its KNOWN list.
`Cmd::none` is the ring's
empty slot and is never posted by anything.

### 6. What this session changed, and the reasoning that is not in the commits

Four commits on `main`: `3793cb4`, `ce98129`, `a124d73`, `5bceff5`. The commit messages are long
and carry the reasoning; what follows is only what they do not.

**The sends and master were the largest functional hole in the app.** `Cmd::set_sends` and
`Cmd::set_master` were in the enum with nothing handling them and nothing posting them. Design
decisions worth not re-litigating:

- The fourteen settings are **encoder bytes**, not floats, in `ProjectSettings::sends` /
  `::master`. A byte is what the encoder produces, a byte cannot hold a NaN that would poison
  every later block, and the serializer has no float method — storing one would mean bit-casting
  through a buffer, which `CLAUDE.md` forbids.
- The commands carry a **delta in the field's own units**, not the next byte. The first cut
  carried the byte, and a sweep of sixteen clicks read the same stale byte sixteen times and
  moved the knob by one, because the authoritative byte lives on the audio side. This is the same
  rule `app/commands.hpp` already stated for every other encoder gesture; `Cmd::set_quantize`
  was changed for the same reason before it ever shipped.
- The delay's byte is a **division**, not a time, because `docs/SPEC.md` makes send A
  tempo-synced. `Cmd::set_tempo` re-derives the seconds, which is why the engine now handles a
  command whose comment used to say "handled in app/".
- `PROJECT.BIN` grew a **third section, optional on read** — 251 → 277 bytes. `find_sections`
  gained a tolerance for a missing tail section, which is what lets the format grow without a
  version bump. **Absent means the DEFAULTS explicitly**, not "leave the struct alone": the
  project is a buffer the app reuses, so leaving it would carry the previous project's delay into
  one whose file says nothing about it. That bug was written and then caught by its own test.

**Three UI deviations from the spec's key table**, recorded in `docs/REVIEW.md` §2a. Every key in
that table is taken, plain and with F1, so each took a key that does nothing in the mode it is
used in: the sends went to four new MIXER pages on the selection keys; the record quantize amount
moved off F1 + VALUE (which the spec gives to the master DJ filter) to a SETUP page; that page
sits on the octave keys in PROJECT mode, where they were setting GRID's step page, which is not
on screen there.

**M7 turned out to be much smaller than PLAN.md thought.** CLOCK SRC, CLOCK OUT, SYNC OUT and
MIDI CH are **not settable from an app**: `<enaga/clock.hpp>` and `<enaga/midi.hpp>` expose
`source()` and `channel()` and no writer, because they are the user's settings in the Enaga menu.
So "STRATA only exposes the settings" can only mean *showing* them, which the SETUP page does.
The one real behaviour the spec asks for and `app/` did not have is that **VALUE refuses a tempo
edit** when the source is not INTERNAL; before, the knob moved the app's own number and Enaga
threw the call away, so the two drifted apart with every click.

**The VA POLY optimisation loop.** Measured, bit-exact, 4.30 → 3.82 points per voice;
`docs/BUDGET.md` has the tables and the three avenues examined and rejected. The one thing not in
the commit: the **average** saving per voice (0.33) is smaller than the **marginal** one (0.48),
and both are honest — the bass is a `VaMono` with one voice so it shares nothing, and the first
chord voice of each slice is the one that computes what the others read. Quote the marginal
figure when asking whether a part fits.

### 7. Decisions recorded in the code that should not be re-opened without reading them

- **The sample swap protocol.** `clear_sample` → wait for `Status::sample_epoch[track]` to change
  → release the range → load → `set_sample`. `SamplePool::release` makes memory reusable
  immediately and `core/` deliberately cannot check whether a voice is still reading it.
  Releasing before the epoch confirms means overwriting memory a voice is reading, which sounds
  like a broken sampler rather than a broken lifetime.
- **Saving must not read a torn step.** Drain the command ring first and defer the save by a
  frame if it is not empty. A *load* additionally requires the transport stopped — with the clock
  stopped `clock::ticks()` yields nothing, so the scheduler never reads the pattern.
  `Engine::project_for_load()` is the one deliberate hole in "the main context never writes
  pattern memory": a load replaces 1.03 MiB, which cannot travel through a 16-byte command.
- **A duration on the `Trig`, not a `release(note)` virtual.** `Trig::gate_frames` is additive and
  `Engine::gate_frames_for` fills it from `Step::length`. A held KEYS key still cannot be
  released — it has no known duration — so `Cmd::release_note` stays unimplemented.
- **Machines are mono and the chain spreads them.** `render_add_stereo` was a comment promising a
  function declared nowhere. The real cost of a stereo machine is that `InsertChain` is mono, so
  a stereo track doubles that track's insert work.
- **`VaFilterCache` is keyed on a float by exact equality**, which is right here for the same
  reason it is usually wrong: the question is "are these the same bits", not "are these about
  equal". `VaPoly::prepare` clearing it is the one thing that must happen for a stale coefficient
  to be impossible.
- **`core/` never includes an Enaga header**, which is why `kSendFields` / `kMasterFields` are
  stated in `core/project.hpp` and `static_assert`ed against `SendField::count` in `dsp/buses.hpp`
  rather than taken from the enum.

- **The factory content is one function** (2026-11-22). `factory_project` in core/factory.cpp
  builds the kit, mixer, bank A's four patterns, the chain (off), mute states, scenes and macros.
  `Engine::init` (no card) and `Storage::start_new` (NEW) both call it, so they cannot drift
  apart again; NEW used to give a kit with LVL 0 everywhere. `project_init` stays blank, which is
  what the tests' `fresh()` builds on. `make -C host render` renders it to WAV without an
  emulator (host/render.cpp).

### 8. Open questions that need a person, not a session

1. ~~The note spelling disagrees with itself~~ **Settled 2026-11-22: sharps everywhere.**
   `core/scale.cpp`'s `note_name` now spells `C#`, `D#` ... like the step page's `C#4`; a test
   holds the two overloads to the same spelling.
2. ~~Should SAMPLE or GRAIN fold?~~ **Settled 2026-11-22.** GRAIN folds like the VAs; SAMPLE
   folds only while unsliced (SLC = 1); a sliced sample never folds, because there the note
   picks the slice. `machine_folds_note` (core/project.hpp) replaced `machine_is_melodic`.
   Still to be heard on the unit (docs/UNIT_CHECKLIST.md).
3. ~~`CLAUDE.md` line 62 is wrong~~ **Settled 2026-11-22.** "All 18 voices together are about
   11% of the block" held for the drums, not for a sustaining voice (six VA POLY voices are
   22.9% on their own). With the user's approval CLAUDE.md now states the measured figure; the
   rule it supports, cut effects or insert stages before voices, is unchanged.
4. **`delay_ping_pong` and `delay_to_reverb` share a MIXER page with two empty slots**, because
   ten send fields do not divide into four. Fine, but a person may want them placed differently.

### 9. Small things noticed and deliberately not fixed

- `tests/test_record.cpp` and `tests/test_timing.cpp` each define `degenerate_input_is_handled`
  and `a_long_run_stays_consistent`. They compile (registrations are file-scope statics) but the
  suite prints each name twice, so a failure does not say which file it was in. Pre-existing from
  M1/M2.
- `dsp/va.cpp`'s flop table derives ~1,660 cycles per voice. The measurement is ~18,900 — wrong
  by a factor of eleven, because 2.5 flops per cycle was taken from Enaga's example and is far
  too generous for this branchier, un-pipelined code. The table is kept because the **order** of
  its stages held up; it now says to read `docs/BUDGET.md` for what anything costs.
- `core/project.cpp`'s `project_read` carries a `#pragma GCC optimize("no-unroll-loops")` around
  one function. GCC 13.3.0 for tic6x-elf **crashes** compiling it with `-O2 -funroll-loops`
  (internal compiler error in the instruction scheduler, `haifa-sched.cc:1597`). To be reported
  upstream with that file as the failing case.

### 10. Upstream

[seawaffle/enaga#1](https://github.com/seawaffle/enaga/pull/1) is open: a fix keeping libgcc's
unwinder out of an app's link (64-bit division and modulo could not link at all) plus two
documentation notes, verified against their own 81-test suite. It is **not** in v0.2.0. Nothing in
STRATA depends on it being merged. **Never modify `third_party/enaga`** — Enaga bugs go upstream
with a failing case.

**The pin is release `v0.2.0` (`3da7f89`)**, moved from `1b41111` on 2026-11-22. The rule is to
pin **release tags only**, and the one that matches the Enaga firmware on the unit: Enaga's loader
checks nothing but `ENAGA_ABI_VERSION` (still 0), so headers and firmware can drift apart with
nothing failing. v0.2.0 is the release Enaga's own unit session 5 ran on a SmplTrek. What it
changed since `1b41111`:

- **Among the headers STRATA uses**, only a comment in `midi.hpp` and an optional package flag in
  `abi.h` (`ENAGA_PACKAGE_ICON`, for a 16×16 menu icon from an `icon.txt`). STRATA has no icon,
  so its package writes 0 there as before. **Do not add one** while any unit runs firmware older
  than 0.2.0: that loader refuses any non-zero flags.
- New and unused by STRATA: `<enaga/ui.hpp>` (List, ParamPage, Dialog, TextEntry),
  `<enaga/bytes.hpp>`, and an SDK zip with a **prebuilt GCC 13.3.0 toolchain** — the same compiler
  `tools/build_toolchain.sh` builds, so it could replace the 30-minute build. Not tried yet.
- `build_app.py`: when the app folder is outside Enaga's tree, intermediates go to
  `<app>/build`; `tools/build.sh` passes `-o`, so only its staging folder sees that.
- Firmware: unit session 5 on 0.2.0 logged one unreproduced crash on CHECK's MIDI page. The
  fix for a stalled USB host holding up DIN (`192f03f`) came **after** the tag.

At the bump, `make -C host test` passed unchanged (539,924 checks, 0 failures). The device
build and the scenarios were **not** run at v0.2.0 from a container (no tic6x toolchain). The first
device build after the bump is step 3 of the procedure below; do it before the unit session.

#### Moving the Enaga pin

`tools/enaga_check.sh --upstream` reports newer releases and the commits since the pin that touch
`include/`, `runtime/`, `loader/` or `build_app.py`, and exits 3 when there is something to look
at. Without `--upstream` it only checks that the checkout, the docs and the emulator match their
pins. Moving the pin is the user's decision; once made, one commit, in this order:

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

---

## What to do next, in order

Everything below the first item can be done from a container. The first item cannot, and it is
worth more than all the rest together.

1. **Work `docs/UNIT_CHECKLIST.md` on the unit.** It boots and plays the default beat
   (2026-11-22); almost every box is still open. Start with the sections that decide
   whether a milestone stands: does it start with no card; does a power cycle restore the
   project; does swapping a sample on a sounding track stay clean; does pulling the card during a
   save leave the project intact. Then the new sections at the end — the sends and the master,
   and VA POLY after the optimisation loop.
2. **Run the load with cl6x**, on the machine that has it, for a 4- and a 6-voice chord, and put
   both figures in `docs/BUDGET.md` next to the GCC-only table. `CLAUDE.md` asks for both columns
   after a DSP change and only one of them can be produced in a container. **This is the number
   that decides whether M6's budget exit stands.**
3. **Mutation-test the new `app/` tests** (task #19, never done). `app/` became host-testable only
   recently and its tests have not been held to the standard the rest of the project is. The two
   data-loss bugs found when `app/` first became testable are the argument.
4. **Read `app/` again.** It was written with no way to run it. Ten bugs were found by re-reading
   or by mutation testing that no compiler could have caught, and the odds that those were the
   last ten are poor. The places where a mistake is **silent rather than loud**: the epoch wait in
   `Storage::step_sample_epoch`, the slice arithmetic in `step_sample_convert` and
   `step_sample_resample` (every pointer there is an offset into the 64 MiB region), and the
   deferred-save condition in `Storage::service`.
5. **Check the memory.** 64 MiB sample region + 27.5 MiB loader staging + 2.93 MiB repeat ring is
   94.5 MiB of the ~124 MiB heap before anything else, and the staging was never in the spec's
   table. Cheap to check and the figure most likely to be a problem.
6. **The two open questions above that need an ear** — the note spelling, and whether a sample
   track folds.

## What a container CAN and CANNOT do

| Can | |
| --- | --- |
| `make -C host test` | the whole suite, including `app/`, against `tests/fakehost.cpp`. Needs the `third_party/enaga` submodule for headers (`git submodule update --init --recursive`) and nothing else |
| `tools/build.sh` | a GCC-only device build, **after** `tools/build_toolchain.sh` (~30 min) |
| `tools/scenarios.sh` | all seven emulator scenarios, after the toolchain **and** `tools/fetch_emulator.sh` |
| `tools/enaga_check.sh` | the Enaga and emulator pins; `--upstream` also reports what upstream has added since |
| `tools/render.sh` | an offline render, same requirements |
| ASan / UBSan over the suite | with `detect_leaks=0` |

| Cannot | why |
| --- | --- |
| Anything with cl6x | the installer needs an interactive licence click. Every load figure is GCC-only |
| Drive a pad or an encoder in the emulator | `emulate.py` has `--key` and nothing else |
| Hear anything | no audio out, and `tools/render.sh` only writes a file |
| Know whether it feels right | the thing the whole checklist exists for |

## Running commands, if the session is on the user's Windows host

The repo is on `D:` and every build runs in WSL, so commands go through `wsl.exe` and Git Bash
mangles them unless path conversion is off. `CLAUDE.md` has the full form; the short version:

```sh
MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c 'cd /mnt/d/DEV/STRATA && make -C host test'
```

Use `bash -c` with single quotes — `bash -lc` with `VAR=...` assignments loses the variables —
and for anything longer than one command **write a script to the scratchpad and run it**, because
inline heredocs and `$VAR` get mangled and `$?`/`PIPESTATUS` read back wrong through the
`wsl.exe` layer. In a Linux container none of that applies and the commands above are literal.
