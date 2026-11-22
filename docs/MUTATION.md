# Mutation testing

docs/PLAN.md makes mutation testing the house standard: a mutant that survives is either a
missing test or a line that changes no behaviour, and which one it is gets written down. This
file is where it is written down for the line-by-line runs of 2026-11-22. Short arguments also
sit next to the lines themselves, marked `Mutation-equivalent (docs/MUTATION.md)`.

## Running it

```sh
make -C host build                       # build/host must hold an object for every source
python3 tools/mutation_test.py app/engine.cpp -j 4
python3 tools/mutation_test.py core/queue.cpp core/perform.cpp --lines 1-200 --json out.json
```

- Every mutant is one edit to one line: a relational, equality or logical operator flipped,
  `+ -` and `* /` swapped, `+= -=`, `++ --`, `true false`, a small integer moved by one
  (0→1, 1→0, 1→2), a `!` removed, or a whole expression statement deleted. String literals
  and comments are masked first, so nothing lands inside one.
- Each mutant is compiled with the suite's flags, linked against the ordinary build's objects
  in the Makefile's order, and run against the whole suite with a fail-fast runner. 60 s
  without an answer counts as killed (an endless loop).
- Nothing in the tree is touched: each worker mutates a private copy under build/mutation/.
  **Do not rebuild build/host while it runs**; use `ENAGA_BUILD=/tmp/x make -C host build` for
  a side build. And do not edit a file under test: each mutant is made from the file on disk.
- The unmutated suite has to pass inside the harness first, or it stops. It did not, once: a
  test in test_app.cpp hard-coded the project folder, and passed only because of link order.
- About 13 s per mutant per worker on a 4-core container (the suite is most of it):
  app/engine.cpp takes about 50 minutes with -j 4, the seven core files below about 70.

## Results

Score = killed / (killed + survived). Mutants that do not compile are left out.

| File | Before | After | New tests |
| --- | --- | --- | --- |
| app/engine.cpp | 410 / 400 (50.6%) | 551 / 208 (72.6%) | 46 in tests/test_engine_gaps.cpp |
| app/storage.cpp | 473 / 262 (64.4%) | 510 / 225 (69.4%) | 12 in tests/test_storage.cpp, section 7c |
| core/queue.cpp | 163 / 46 (78.0%) | 180 / 29 (86.1%) | 11 in tests/test_queue.cpp |
| core/perform.cpp | 240 / 68 (77.9%) | 255 / 53 (82.8%) | 9 in tests/test_perform.cpp |
| core/scheduler.cpp | 122 / 50 (70.9%) | 143 / 29 (83.1%) | 7 in tests/test_scheduler.cpp |
| core/editops.cpp | 104 / 18 (85.2%) | 112 / 10 (91.8%) | 5 in tests/test_editops.cpp |
| core/mutate.cpp | 161 / 49 (76.7%) | 172 / 38 (81.9%) | 2 in tests/test_mutate.cpp |
| core/timing.cpp | 36 / 15 (70.6%) | 39 / 12 (76.5%) | 2 in tests/test_timing.cpp |
| core/undo.cpp | 44 / 8 (84.6%) | 45 / 7 (86.5%) | 2 in tests/test_undo.cpp |

The figures are killed / survived. "After" is a fresh run with the new tests in (app/engine.cpp
grew by the shared VA POLY voice budget in between, so its totals differ). The after run
was taken before main's macro change to core/perform.cpp merged in.

Not yet run line by line: core/chord, factory, pattern, pcm, project, record, samplepool, scale,
serialize and stepedit (2,260 mutants), and dsp/. They are next, a few files at a time.

## What was a missing test

No run found a bug in the code: every killed survivor was behaviour nothing pinned. The ones
worth knowing, because they are the kind that would have broken silently:

- **The random draws were never pinned across builds.** `probability_roll` and `mutate_draw`
  were tested for spread and for determinism within one build, so deleting a line of the hash
  passed everything. A saved pattern's seed is meant to replay the same probabilities and the
  same mutation after an update (docs/SPEC.md). Both are now pinned to fixed values.
- **The chain's orphaned-entry branch** (a playing entry removed, or a project resumed on a
  pattern that is not an entry) had no test for the bank, for the same-pattern case, or for
  the index being brought back into range.
- **Queue boundaries.** The start tick of a restart outside the grace window and past the first
  pass, an origin equal to the armed tick, and a one-tick pattern were all untested; one mutant
  saturated every start tick to the end of time and passed.
- **A restart kept the old pass boundary** as far as the tests knew, so a chain could have sat
  out extra passes after F1 + F5.
- **Lock-pool refusal at exactly one over** a full pool, counting exactly the destination steps.
- **PERFORM.BIN** damage at the exact boundaries, and macro and scene indices one past the end.
- **Storage**: refused samples leaving the loader idle, TOO LONG and NO ROOM, slices of a long
  file arriving whole and in order, and a later NEW or reload winning.
- One test-harness defect: `test_app` depended on link order (fixed in its own commit).

## What survives on purpose

Each class below was checked by reading the code, and a sample of each by running the mutant.

**Clamp boundaries.** `x < lo ? lo : x` against `x <= lo ? lo : x`, and the same at the top:
at the boundary both return the same value. Every `clamp_*` helper in core/ and app/ produces
these.

**Saturation boundaries.** `previous > kMaxTick - multiple` against `>=`: at equality the
saturated value and the sum are both kMaxTick (core/queue.cpp, three places).

**Loop bounds one past the end.** `i < kTracks` against `i <= kTracks` reads or writes one past
an array. That is undefined behaviour the suite cannot see by value; AddressSanitizer can, and
docs/PLAN.md already requires an ASan run after DSP changes.

**Defaults that are always overwritten.** Member initialisers of `ChainCursor`, `wav::Format`
and the like, whose every field is assigned before it is read.

**Unreachable refusals.** The A_B condition's divisor check and the unknown-kind fallback in
`condition_passes` (condition_kind() never yields them), `queue_arm` refusals the engine has
already ruled out, BAD PATH (kSamplePathBytes is 112, under the 255 the check is for),
MISALIGNED, `out_done_ > 0xFFFFFF`, and `ratchet_frame`'s `offset <= 0` (offset is at least 0.5).

**Work done twice.** A reset or assignment that a later line repeats:

- `Cmd::restart`'s `origin_ = 0`: resolve_tick sends an origin past the new tick back to 0.
- `next_bar_tick_ += kTicksPerBar`: the resync below it finds the same bar by a division.
- set_machine's `prepare()`, which the render loop repeats from the same `effective_`
  (docs/HANDOFF.md, "The standard this code is held to").
- `Storage::request_save`'s `autosave_pending_ = false`: service() clears both flags.
- `Scheduler::reset`'s `first_pass_loop = 0`: overwritten on the first firing, before any read.
- `Scheduler::configure`'s `pattern_seed_`: tick() sets it from the pattern it is handed before
  any draw (that tick() does so is now tested).
- `chain_advance`'s `played = 0` in the orphaned branch: chain_player_removed and chain_start
  have already zeroed it.
- `find_sections` zeroing its outputs: every caller passes zeroed arrays.
- `UndoStack::clear`'s `has_last_ = false` and `base_ = 0`: `last_` is reset to a NONE gesture,
  which never matches (tests/test_undo.cpp), and the ring is indexed from `base_` wherever it is.
- `UndoStack::redo` storing the live pattern, and its `has_last_ = false`: the next undo stores
  it again before moving, and the second is argued in core/undo.cpp itself.
- The CRC placeholder in `write_container`: finish_container patches it.

**Outcome-equivalent refusals.** A different check that refuses the same file for the same
reason: in `find_sections`, `sections == 0` against `== 1` (no wanted section is found either
way: malformed), a section at exactly the payload's end (too short for its contents: malformed),
and `table > payload` against `>=`.

**Safety margins.** `steps_firing_at` widens its candidate window by one step at each end;
the window already covers the largest swing and micro shift, so narrowing it changes nothing
the tests can reach, and widening it more only walks more candidates. Kept as a margin.

**Rounding that the fallbacks absorb.** `macro_range_through`'s rounding of a negative
intermediate: its results are pinned through the contract instead (the range passes through
the knob's value to within one step, and is never flat), over a grid of every case.

**Default content.** The default kit and pattern values (app/engine.cpp, roughly its first 450
lines) are content, not logic: tests/test_boot.cpp checks what the default beat must do, and a
value moved by one there is a different preset, not a bug.

## Accepted gaps

Real behaviour without a test, left for now because the test costs more than the risk:

- `Storage::init` and the settings file (runs once per process; a second Storage costs 90 MiB).
- `Storage::open_last`'s sequence numbers, the backlog shift, path truncation helpers, and the
  drain in `save_for_shutdown`.
- In the engine: a trig's frame and the accent and slide flags reaching the machine, the lock
  pool compaction path through the engine, and the duck.

## Found along the way

- `Engine::resample_bars_` is written and cleared but never read: the take's length is the
  frame count handed to capture_.arm(). Noted in app/engine.cpp.
