# STRATA implementation plan

Revised from `docs/SPEC.md` after validating it against Enaga (`docs/REVIEW.md` has the evidence
behind each change). This plan supersedes the spec's "Milestones" and "Instructions for Claude
Code" sections; everything else in the spec stands. `docs/HANDOFF.md` holds the next steps in
detail, `docs/BUDGET.md` every measured figure, `docs/UNIT_CHECKLIST.md` what is left to check
on the unit.

## What changed from the spec's plan

| | Spec | This plan |
| --- | --- | --- |
| Enaga pin | `269c015` | **release `v0.2.0` (`3da7f89`)**, the firmware on the unit; only release tags are pinned (`docs/HANDOFF.md`, "Moving the Enaga pin") |
| Master clock | `core/clock`, 96 PPQN, 32.32 fixed point, drift tests | **Deleted.** Enaga owns it; `core/scheduler` sits on `clock::ticks()` |
| MIDI / SYNC | M7: implement in an Enaga fork, upstream as PRs | **Already in Enaga.** M7 shrinks to verifying it on the unit |
| External sync | deferred to M7 | **M1.** It changes transport and tempo behaviour that M1 builds |
| Budget gate | "measure at M1 and M2 exits" | **New M0.5 spike** before any engine work: effects are the dominant fixed cost |
| Budget lever | cut chord voices | **Cut effects or insert stages first.** True for the drums, which go idle; a sustaining VA POLY voice is not cheap (2.16 points with cl6x, 3.82 GCC-only), so VA POLY has a **shared budget of 12 voices** across all tracks |
| cl6x | a release risk | **A requirement** for the 60% target. Worth 1.6–1.8x on STRATA's scenarios (2.2x on Enaga's synth). Its licence question is settled: no TI code reaches `APP.ENA` |
| Build host | implied Linux | **WSL Ubuntu**, `build/` inside the WSL filesystem, no cmake |
| Host tests | `core/` and `dsp/` | **`app/` too**, through `tests/fakehost.cpp` (see Testing) |
| Milestones | 8 (M0–M7) | 9 (M0, M0.5, M1–M7), with M7 much smaller, then the **v0.1 release** |

---

## Where it stands (2026-11-22, evening)

Every figure is from the host suite, a device build or the emulator, except one: **STRATA booted
on the unit on 2026-11-22** (Enaga 0.2.0) and plays the default beat with the on-screen LOAD at
about 0–26%. No exit criterion involving load or feel is met until the unit confirms it.

| | State | What is actually true |
| --- | --- | --- |
| M0 | code complete, **boots on the unit** | `tools/build.sh` links APP.ENA with no BOOT, MCU or SPI sections, with GCC and with cl6x |
| M0.5 | folded into M2 | Its four configurations were never measured on the unit; the emulator figures are in `docs/BUDGET.md` |
| M1 | code complete | Beat in the emulator: 23.49% cl6x, 39.60% GCC-only (all eight tracks and a chord, so not the 4-track exit's configuration). Unit unmeasured |
| M2 | code complete | Eight-track worst case 23.61% cl6x, 44.26% GCC-only (emulator), against a 40% exit |
| M3 | code complete | Locks, conditions, ratchets, micro-timing, polymeter, recording, 16 undo levels; undo also in PERFORM and PROJECT |
| M4 | code complete | Projects, banks, chains, autosave, background loading. **A kit browser** (16 kits shared by all projects), and the **factory project** (a tuned kit, A01–A04, scenes, mute states) on first boot and NEW. Samples up to 30 s of the file at up to 96 kHz |
| M5 | code complete | Scenes, morph, macros (three shipped in the factory project; a macro sweeps from the live value, so 0 is the knob), mutes, beat repeat, throws, MUTATE (OK keeps, CLR reverts). Most of it is on pads, which the emulator cannot press, so most of it has never run outside the host tests |
| M6 | **feature-complete, unheard** | Euclid, KEYS (a held key sustains), chords, VA POLY, GRAIN, resampling, pad pressure, machine switching. Budget exit stands **on the emulator** (worst scenario under 24% with cl6x); a shared 12-voice VA POLY budget keeps a POLY-heavy kit inside it |
| M7 | **app side done** | The sync settings are the user's, in Enaga's menu; PROJECT's SETUP page shows them and VALUE refuses a tempo edit under an external clock. The rest is verification on the unit |
| v0.1 | **in preparation** | Release notes, CHANGELOG and packaging in the thread "Prepare the v0.1 release"; a user manual in "Write the STRATA user manual". CI runs the host suite, ASan/UBSan and the repo checks on every PR. Waits for the unit's numbers (below) |

Also on `main` since the first boot, all host-tested and none heard yet: the arrow-key navigation
model and a layout test over every screen; the CHAIN and PATTERN pages in PROJECT; chains that
keep their place while edited; FUNC+STOP resetting a running chain; removing one p-lock; KEYS
release by note; a low-pass before downsampling a loaded sample; and three bit-exact
optimizations (a track is prepared only when its inputs move, cached VA increments, and the load-time
anti-aliasing loop in `core/pcmloops.ti.c`).

## What comes next, in order

`docs/HANDOFF.md` has each step in detail.

1. **PR #17, the held CPU MAX readout**, merged tonight on Roland's PC after `main` is merged into
   it and the suite and every scenario pass with cl6x and GCC-only.
2. **The emulator runs E1–E5** and `scenario_factory.sh`, both compilers, into `docs/BUDGET.md`.
3. **Tonight's walkthrough on the unit** (top of `docs/UNIT_CHECKLIST.md`), with STRATA and
   STRATA6 on the card.
4. **The first unit rows in `docs/BUDGET.md`** (U1–U5) and the ticks in UNIT_CHECKLIST.
5. **The final VA POLY voice budget**, set from U1 and U2 (MAX on the default beat and with six
   voices and more POLY tracks). 12 is provisional.
6. **v0.1**, once 1–5 are done: the milestone exits below that tonight settles are recorded,
   and the release thread's notes say plainly which are still emulator-only.
7. **The rest of the unit checklist**: card pulls during save and load, a start with no card, the
   M5 jam, and M7's hour on external clock.

---

## M0 — Toolchain and skeleton

Everything here is setup; none of it is STRATA's design. Do it once, in WSL.

1. **Build host.** `apt install build-essential python3 git` in WSL Ubuntu 24.04. `cmake` is
   absent and stays absent — the host runner is a plain Makefile. Keep the repo on `/mnt/d`
   for editing but point `ENAGA_BUILD` at a path inside the WSL filesystem, because building
   across 9p is slow.
2. **Dependencies.**
   - `third_party/enaga` as a submodule pinned at a release tag (now `v0.2.0`, `3da7f89`).
   - `seawaffle/smpltrek-re` cloned separately; `ENAGA_EMULATOR=<re>/emulator` (the directory
     holding `cpu.py`). It is not a submodule — Enaga's tools take it by environment variable.
3. **Cross toolchain.** `tools/fetch_toolchain_sources.sh` then `tools/build_toolchain.sh`
   (10–30 minutes, into `~/strata-toolchain/tic6x`).
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
M2 exits and to "cut voices before features". Both are wrong for the drums, whose voices go idle and
were about 11% of the block in total, while delay and reverb together measured 16% on their own
(a sustaining VA POLY voice turned out to be the exception, see M6). The dominant cost
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

**Where it stands:** the feature list is complete and host-tested. The load exit stands on the
emulator: the worst scenario is 23.49% with cl6x. VA POLY costs 2.16 points a voice with cl6x
(3.82 GCC-only), far above what SPEC budgeted for the whole machine, and since machine switching
any track can host it, so `Engine::kPolyVoiceBudget` caps VA POLY at 12 voices across all
tracks. The unit's MAX readings set the final cap.

---

## M7 — Sync on hardware

Was a milestone of platform work; now verification. Enaga's MIDI DIN, SYNC and USB MIDI are
implemented but **have never run on a unit** — STRATA may be the first thing to exercise them.

- ~~Sync settings UI in STRATA (source, clock out, SYNC out, MIDI channel).~~ **Done, and
  smaller than it looked.** None of the four is settable from an app: `<enaga/clock.hpp>` and
  `<enaga/midi.hpp>` expose `source()` and `channel()` and no writer, because they are the
  user's settings in the Enaga menu. PROJECT mode's SETUP page (PROJECT, then the screen key RIGHT three times; the octave keys until docs/CONTROLS.md) shows
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

## v0.1 — first release

Not in the spec. The first build handed to anyone but Roland: `main` once the unit has been
walked through and its numbers recorded.

- Release notes and a CHANGELOG, a packaging script that produces the card folder and a
  README.txt, and a draft GitHub release (thread "Prepare the v0.1 release").
- A user manual (thread "Write the STRATA user manual"); `docs/CHEATSHEET.md` stays the one-page
  reference.
- CI on every PR (`.github/workflows/ci.yml`, merged in PR #33): the host suite, ASan/UBSan,
  the layout check, the command audit and the Enaga pin check.

**Exit:** tonight's walkthrough done, U1–U5 in `docs/BUDGET.md`, the VA POLY budget set from
them, and every milestone exit the release notes claim backed by a unit row or labelled
emulator-only.

---

## Testing

The spec's three tiers stand: host tests, then scripted emulator runs, then the unit checklist,
with only the unit's numbers counting for load and timing.

**Host tests cover `app/` as well as `core/` and `dsp/`.** The tiers put `app/` outside host
testing because it needs Enaga, and Enaga needs hardware. That was false: Enaga's C++ API is
header-only over one struct of function pointers that the app runtime defines, so
`tests/fakehost.cpp` defines it and `app/` compiles for the tests exactly as for the device, with
no `#ifdef`. `app/app.cpp` alone stays outside, because `ENAGA_APP(Strata)` defines the package's
entry points.

- The "no drift over a simulated hour" test belongs to Enaga. STRATA's scheduler tests drive a
  synthetic tick stream, which keeps them fast and deterministic.
- Every load figure is recorded twice, with and without cl6x.
- **Mutation testing is the house standard** for `core/`, `dsp/` and `app/`: a surviving mutation
  is a missing test or an equivalence written down in the owning file.
- **AddressSanitizer and UBSan over the whole suite** (`make -C host test SANITIZE=1`), in CI on
  every PR.
- **Bit-exactness** gates a DSP refactor, pinned with `==`.
- **Every screen passes the layout test** in `tests/test_ui.cpp` (Enaga's font, 21 characters a
  line, its charset).
- `tools/audit_commands.sh` after any change to `app/commands.hpp`: no command without both a
  poster and a handler.

`CLAUDE.md` in the repository root is the rule set for Claude Code. It replaces the copy this plan
used to carry, and the spec's first prompts, which were for M0 and are history.
