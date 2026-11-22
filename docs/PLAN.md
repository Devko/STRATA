# STRATA implementation plan

Revised from `docs/SPEC.md` after validating it against Enaga at `1b41111`. Read
`docs/REVIEW.md` for the evidence behind each change. This plan supersedes the spec's
"Milestones" and "Instructions for Claude Code" sections; everything else in the spec stands.

## What changed from the spec's plan

| | Spec | This plan |
| --- | --- | --- |
| Enaga pin | `269c015` | **`1b41111`** — the pin in the spec predates MIDI, SYNC, the clock and pad pressure |
| Master clock | `core/clock`, 96 PPQN, 32.32 fixed point, drift tests | **Deleted.** Enaga owns it; `core/scheduler` sits on `clock::ticks()` |
| MIDI / SYNC | M7: implement in an Enaga fork, upstream as PRs | **Already in Enaga.** M7 shrinks to verifying it on the unit |
| External sync | deferred to M7 | **M1.** It changes transport and tempo behaviour that M1 builds |
| Budget gate | "measure at M1 and M2 exits" | **New M0.5 spike** before any engine work — effects are the dominant cost |
| Budget lever | cut chord voices | **Cut effects or insert stages.** Voices are ~11% of the block in total |
| cl6x | a release risk | **A requirement** for the 60% target (worth ~2.2×) |
| Build host | implied Linux | **WSL Ubuntu**, `build/` inside the WSL filesystem, no cmake |
| Milestones | 8 (M0–M7) | 9 (M0, M0.5, M1–M7), with M7 much smaller |

Net effect: roughly a milestone of platform work removed, one short spike added, and one
deferred concern (external sync) pulled forward into M1.

---

## Where it actually stands (2026-11-22)

Every figure here is from a cloud session: the host suite, the tic6x device build and the
emulator. **Nothing in this project has ever run on the unit**, so no exit criterion involving
load or feel is met, whatever the numbers below say.

| | State | What is actually true |
| --- | --- | --- |
| M0 | code complete | `tools/build.sh` links: APP.ENA, no BOOT, MCU or SPI sections. **Never booted.** Its nine checklist boxes need the hardware |
| M0.5 | folded into M2 | See docs/BUDGET.md; the spike's four configurations were never measured on the unit |
| M1 | code complete | 4-track beat plays in the emulator. Exit wanted "under 15%"; it reads 33.07% GCC-only with a chord on the stab, which is not comparable — different kit, no cl6x, no cache model |
| M2 | code complete | Full kit, mixer, insert chain, sends, master. The sidechain duck was **dead code until this session** (both arms of its depth ternary were 0) |
| M3 | code complete | Sequencer depth, 16 undo levels. `Step::length` became audible only this session, on VA POLY |
| M4 | code complete | Ten files per project, autosave, background sample loading. **Two data-loss bugs found this session** once app/ became host-testable |
| M5 | code complete | Scenes, morph, macros, mutes, beat repeat, throws, mutation. Seven of its eight tools are on the upper PADS and **the emulator cannot press a pad**, so most of it has never executed |
| M6 | **feature-complete, unheard** | Euclidean generator, KEYS, core/chord, VA POLY with chords, GRAIN, resampling, pad pressure — and machine switching, which is what made GRAIN reachable at all. Every feature has host tests; not one has been heard. **Its budget exit is in doubt**: VA POLY costs 3.82 points per voice after an optimisation loop, still 10.1x what SPEC budgets for the whole machine |
| M7 | **the app side is done** | The sync settings are the USER'S, in the Enaga menu: `<enaga/clock.hpp>` and `<enaga/midi.hpp>` expose `source()` and `channel()` and nothing that writes either, so "STRATA only exposes the settings" can only mean showing them. PROJECT mode's SETUP page does, and VALUE now refuses a tempo edit under an external clock as the spec's key table asks. The rest of M7 is unit verification by definition |

Finished after that table was last written, and all of it on `main`:

| | |
| --- | --- |
| **the sends and the master** | Both commands had no handler AND no caller, so every delay and reverb setting and the spec's master DJ filter sat at a compile-time default for the life of the app, in no file. All fourteen are now encoder bytes in PROJECT.BIN's third (optional) section, reachable from four new MIXER pages and F1 + VALUE |
| **the tempo, quantize and `chain_enabled`** | All three round-tripped through the serializer while nothing in `app/` wrote them. A project saved at 142 BPM reopened at 130 |
| **the melodic fold** | `machine_is_melodic` settled the POLY/MONO asymmetry M6 left open: both VAs fold a step's note into the key, the drums and samplers do not |
| **VA POLY, 4.30 → 3.82 points a voice** | A fused output tail in `dsp/valoops.ti.c` and filter coefficients shared between the voices of a chord. Both bit-exact; every scenario with a VA voice came down ~1.6 points |

The two things that gate everything and cannot be done here: **boot `build/APP.ENA` on a unit**,
and **answer the cl6x licence question**. docs/BUDGET.md records cl6x as worth ~2.2x and the 60%
target depends on it, so a GCC-only figure over 60% means nothing either way.

docs/UNIT_CHECKLIST.md holds the gestures to try, and **none of its boxes is ticked.**

---

## M0 — Toolchain and skeleton

Everything here is setup; none of it is STRATA's design. Do it once, in WSL.

1. **Build host.** `apt install build-essential python3 git` in WSL Ubuntu 24.04. `cmake` is
   absent and stays absent — the host runner is a plain Makefile. Keep the repo on `/mnt/d`
   for editing but point `ENAGA_BUILD` at a path inside the WSL filesystem, because building
   across 9p is slow.
2. **Dependencies.**
   - `third_party/enaga` as a submodule pinned at `1b41111`.
   - `seawaffle/smpltrek-re` cloned separately; `ENAGA_EMULATOR=<re>/emulator` (the directory
     holding `cpu.py`). It is not a submodule — Enaga's tools take it by environment variable.
3. **Cross toolchain.** `tools/build_tic6x_binutils.sh` then `tools/build_tic6x_gcc.sh`
   (10–30 minutes, into `third_party/enaga/tools/tic6x`).
4. **TI cl6x.** Install `ti-cgt-c6000` (8.5.0.LTS is what Enaga is tested with) so
   `*.ti.c` and the DSP library get the 2.2×. **Read TI's redistribution terms in this step**
   and record the answer in `docs/BUDGET.md` — the 60% target depends on shipping cl6x-built
   code.
5. **Repo layout** as the spec describes: `core/ dsp/ app/ host/ tests/ tools/ docs/`,
   plus `docs/SPEC.md` (move the concept doc there), `docs/REVIEW.md`, `docs/PLAN.md`,
   `docs/BUDGET.md`, `docs/UNIT_CHECKLIST.md`, and `CLAUDE.md` (below).
6. **`tools/build.sh`.** Stages `core/`, `dsp/`, `app/` flat into `build/stage/` and runs
   `build_app.py`. It **must fail loudly on a duplicate basename** across the three folders —
   `build_app.py` globs one flat directory and names objects from the stem, so a collision
   would otherwise silently drop a file. Also stage `*.ti.c` and all headers.
7. **`tools/panel.sh`.** Builds, then opens Enaga's web panel on `127.0.0.1:8766`.
8. **Empty app.** Draws `STRATA`, the device name from `enaga::system::device()`, and the
   audio load. Nothing else.
9. **Host runner.** `make -C host test` builds `core/` with host g++ and runs the tests.
   `core/` includes no Enaga headers; `dsp/` may include `<enaga/dsp.h>`, which builds on the
   host too.

**Exit:** the empty app runs in the emulator panel *and* on the unit; `make -C host test`
passes with one command; `tools/build.sh` rejects a deliberately duplicated filename; and
`docs/BUDGET.md` records whether cl6x is present and what its licence permits.

---

## M0.5 — Budget spike

**New, and the most valuable two days in this plan.** The spec plans to measure at the M1 and
M2 exits and to "cut voices before features". Both are wrong: voices are about 11% of the
block in total, while delay and reverb together measured 16% on their own. The dominant cost
is the fixed per-block chain, and it is cheapest to learn its real size *before* any engine
is written against it.

Build a throwaway app — not STRATA, just a load harness:

- 8 insert chains (drive → `Svf` → lo-fi → amp env → duck → pan → level) over 64-frame
  interleaved stereo, fed from noise.
- `StereoDelay` on send A, `Reverb` on send B, delay feeding reverb.
- Master: DJ filter, glue compressor, soft limiter.
- No sequencer, no voices, no UI beyond a load readout.

Measure on the unit (the emulator models no caches, so its numbers only compare versions):

| Configuration | Why |
| --- | --- |
| Full chain, cl6x | the real fixed tax |
| Full chain, `ENAGA_TI_CGT=none` | what a GCC-only release would cost |
| Sends off | the effects' share alone |
| 4 insert chains instead of 8 | whether insert stages are the lever |

**Exit:** `docs/BUDGET.md` holds measured figures for the fixed chain, and a go/no-go on 8
full insert chains plus both sends. If the fixed tax exceeds ~30%, change the architecture
now — candidates, cheapest first: mono send busses, a shared filter for the drum tracks,
dropping lo-fi from the fixed chain to an optional stage, or a cheaper reverb.

---

## M1 — Scheduler and drums

The first playable instrument, and the milestone that absorbs external sync.

- **`core/pattern`**: `Step`, `Lock`, `TrackPattern`, `Pattern` exactly as the spec sketches
  them (the 12-byte layout checks out and keeps `uint16_t lock_first` aligned). Versioned
  serializer, magic `STRA`, section table, CRC-32 over the payload, byte-by-byte — never cast
  file bytes to a struct.
- **`core/scheduler`**: given a tick number, which steps fire on which tracks. Owns swing
  (50–75% on even 16ths), per-track length and speed, and micro-timing. *Not* the clock.
  - Design note: swing and micro-timing both shift a step off its nominal tick, and negative
    micro-timing means firing *before* it. Since the shift is bounded (±11 ticks < half a
    16th), invert the mapping — for each arriving tick, scan the few steps whose nominal tick
    lies in `[t - 11 - swing, t + 11]`. That is a bounded, branch-free-able scan of at most
    about 3 steps per track per tick.
- **`dsp/`**: KICK, SNARE, HAT, each with a host renderer that writes 4 bars to WAV.
- **`app/`**: GRID mode, 4 tracks, play, stop, tempo, swing; the command ring and status
  publication from the spec's architecture section.
- **External sync, pulled forward from M7**: read `clock::source()`; handle `clock::start()`
  and `set_tempo()` returning `false`; follow `on_transport`; show `EXT` instead of a tempo
  when the source is not internal.

**Exit:** a 4-track beat at 130 BPM on the unit under 15% load; onset jitter under 0.1 ms
measured from a line-out recording; and the transport follows MIDI clock from an external
source (emulator is enough here — the unit check is M7).

---

## M2 — Full kit and mixer

- PERC (2-op FM), SAMPLE, VA MONO machines.
- The insert chain, both sends, the master bus, the sidechain duck — all sized by M0.5's
  measurements rather than by estimate.
- EDIT and MIXER modes; kits.

**Exit:** the techno template plays all 8 tracks under 40% worst-case load, **measured on the
unit**, with `docs/BUDGET.md` updated.

---

## M3 — Sequencer depth

Parameter locks, trig conditions, probability, ratchets, micro-timing, polymeter, live and
motion recording, 16 levels of undo.

Add from the review: **MIDI note input stamped with the audio sample counter**, the same
latency compensation the spec plans for pad hits — `on_midi` is main context and carries no
timestamp, so notes and pads share one mechanism.

**Exit:** every step field in the spec's Sequencer section works and survives a save and
reload; mutation is deterministic from the pattern seed in host tests.

---

## M4 — Projects and storage

Projects, banks, queued pattern changes, chains, background sample loading, autosave.

- Default sample budget **32 MiB** (about 4.2 s to load at the measured 7,600 KiB/s), raisable
  per project rather than the spec's flat 64 MiB.
- Wire **`on_card(bool)`**: on removal, open files are already dead and must be closed. At
  most 8 open files and 8 streams per app.
- Save state in **`stop()`** — destructors never run on this platform.

**Exit:** a power cycle restores the last state; pulling the card during a save, during
sample loading and during resampling never corrupts the project and never needs a restart.

---

## M5 — Performance layer

Mutes and mute states, fill, scenes and morph, beat repeat, throws, macros, mutation.
Scenes morph **base kit values only**, not locks (see the review).

**Exit:** a 30-minute jam using only the performance row, F7 PERFORM and pattern changes,
with no edit screens and no underruns.

---

## M6 — Full concept

VA POLY with chords, GRAIN, KEYS mode, resampling, pad pressure via `on_pad_pressure`
(Enaga's own cube curve — app-side filtering is no longer needed), Euclidean generator.

**Machine switching was added to this milestone**, because GRAIN was otherwise unreachable: the
default kit fills all eight tracks and nothing could free a slot. The plan put it off over "a
track switching to VA POLY would want six voices it does not currently own", which turned out to
be a memory question with a measured answer — 72.6 KiB for every track to own every machine —
so every track owns one of each and switching is a pointer swap. KIT.BIN already had the field.

**Exit:** every machine and tool in the spec, worst-case load under 60% on the unit.

**Where it stands:** the feature list is complete and host-tested, and the load figure is the
open question. VA POLY measures 4.25 points a voice (docs/BUDGET.md), so a six-voice chord is
25.6% of the block on its own against SPEC's 11,208 cycles for the whole machine. Whether that
passes depends entirely on cl6x, which nobody has been able to run.

---

## M7 — Sync on hardware

Was a milestone of platform work; now verification. Enaga's MIDI DIN, SYNC and USB MIDI are
implemented but **have never run on a unit** — STRATA may be the first thing to exercise them.

- ~~Sync settings UI in STRATA (source, clock out, SYNC out, MIDI channel).~~ **Done, and
  smaller than it looked.** None of the four is settable from an app: `<enaga/clock.hpp>` and
  `<enaga/midi.hpp>` expose `source()` and `channel()` and no writer, because they are the
  user's settings in the Enaga menu. PROJECT mode's SETUP page (F3, then OCTAVE RIGHT) shows
  CLOCK SRC, the tempo and whose it is, MIDI CH and the record quantize amount, and says where
  the first three are changed. VALUE refuses a tempo edit when the source is not INTERNAL,
  which is the one behaviour the spec's key table asks for and `app/` did not have.
- Unit verification: one hour locked to the Force's MIDI clock with no drift; STRATA as clock
  master driving external gear; SYNC in and out against a drum machine.
- Expect to find bugs in Enaga, not in STRATA. Report them upstream with the failing case;
  note that SYNC polarity settings are not built and the USB IDs are TinyUSB test values.

**Exit:** an hour locked to external MIDI clock with no drift, and STRATA driving external
gear as master, both on the unit.

---

## Testing

The spec's three tiers stand, with one of them **widened this session**: host tests now cover
`app/` as well as `core/` and `dsp/`, then scripted emulator runs, then a unit checklist, with
only the unit's numbers counting for load and timing.

**`app/` in the host tests is a change to this plan, made because the plan's own reasoning no
longer held.** The tiers put `app/` outside host testing because it needs Enaga, and Enaga needs
hardware. That turned out to be false: Enaga's C++ API is header-only over one struct of function
pointers that the app runtime defines, and the host Makefile does not link that runtime -- so
`tests/fakehost.cpp` defines it instead and `app/` compiles for the tests exactly as it does for
the device, with no `#ifdef` in it. The cost of leaving it untested was five bugs in two sessions,
two of which lost a user's project. `app/app.cpp` is still outside, because `ENAGA_APP(Strata)`
defines the package's entry points.

Three adjustments beyond that:

- The "no drift over a simulated hour" test moves to Enaga. STRATA's scheduler tests cover
  swing, speed, length, micro-timing, conditions and seeded mutation against a *synthetic*
  tick stream, which also makes them fast and deterministic.
- Every load figure is recorded twice, with and without cl6x, so the GCC-only cost stays
  visible instead of being discovered at release.
- **Mutation testing is the house standard** for every module in `core/` and `dsp/`, and now for
  `app/`: a surviving mutation is either a missing test or a line that changes no behaviour, and
  which one it is gets written down. Two independent passes over the same module have twice found
  behaviours the first pass missed, so a module written by one party is mutation-tested by
  another where that is possible.
- **AddressSanitizer over the whole suite** after a DSP change. It is a dozen lines of script and
  it has found what reading did not: an out-of-range read that a value test cannot see, and a
  `CHECK_EQ` on `const char *` that compared addresses and so passed five tests for no reason.

---

## `CLAUDE.md` to install at M0

The spec's version with the corrections folded in:

```markdown
# STRATA: rules for Claude Code

- Read docs/SPEC.md, docs/REVIEW.md and docs/PLAN.md before any change, and follow the
  milestone order in PLAN.md. Ask before changing a decision they record.
- Target: an Enaga app on the Sonicware SmplTrek (TI C6748, 372 MHz). Enaga is pinned at
  1b41111. Read third_party/enaga/docs/api.md for every API you use.
- Build in WSL, not Windows. Keep build/ inside the WSL filesystem. There is no cmake.
- Do not write a master clock. Enaga owns it: <enaga/clock.hpp>, 96 PPQN, and
  clock::ticks() in audio() gives each tick's frame in the block. STRATA schedules steps
  on those ticks and owns swing, speed, length and micro-timing only.
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
  and TI cl6x. cl6x is worth about 2.2x and the 60% target depends on it.
- build_app.py compiles one flat folder with one include path. Every filename must be
  unique across core/, dsp/ and app/, and includes are bare ("pattern.hpp").
- core/ never includes Enaga headers. Every core change comes with a host test.
- Every feature: host test, then an emulator scenario, then a line in docs/UNIT_CHECKLIST.md.
- After any DSP change, run the load scenario and update docs/BUDGET.md with figures both
  with and without cl6x. Worst case must stay under 60%. Cut effects or insert stages before
  cutting voices: all 18 voices together are about 11% of the block.
- Never modify third_party/enaga. Enaga bugs are reported upstream with a failing case.
- Never write the SPI flash. Never produce update files with BOOT or MCU sections.
- Small commits, one concern each, each with its test.
```

---

## First prompts, corrected

1. "Read docs/SPEC.md, docs/REVIEW.md and docs/PLAN.md. Do M0: repo layout, Enaga as a
   submodule pinned at **1b41111**, smpltrek-re cloned with `ENAGA_EMULATOR` set,
   `tools/build.sh` (flat staging with a duplicate-filename check) and `tools/panel.sh`.
   Build the tic6x toolchain in WSL. Make an empty STRATA app that draws its name, the device
   and the audio load, and confirm it runs in the emulator panel. Record cl6x's presence and
   licence terms in docs/BUDGET.md. Stop and report."
2. "Do M0.5, the budget spike: a throwaway load harness with 8 insert chains, both sends and
   the master chain, no voices. Measure on the unit with cl6x, without cl6x, with sends off,
   and with 4 chains instead of 8. Write the figures to docs/BUDGET.md and recommend go or
   no-go on 8 full insert chains. Stop and report."
3. "Implement `core/pattern`: the Step, Lock, TrackPattern and Pattern structures from the
   spec plus the versioned serializer with CRC-32. Host tests: round-trips, and corrupt or
   unknown-version files refused without a crash."
4. "Implement `core/scheduler` on top of Enaga's `clock::ticks()`: swing, per-track length and
   speed, and micro-timing, using the bounded reverse scan in PLAN.md M1. Host tests drive it
   from a synthetic tick stream — no real clock, no drift tests."
5. "Implement KICK, SNARE and HAT in `dsp/` with a host renderer that writes 4 bars to WAV.
   Then build M1's GRID mode in `app/`, including external-sync handling. Measure the load and
   report against M1's exit criteria."
