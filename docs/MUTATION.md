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

The second pass (also 2026-11-22) covered the ten remaining core files and dsp/; its table is
below. The harness mutates `*.cpp` only, so the `*.ti.c` hot loops (core/pcmloops.ti.c, the
grain, chain, master and repeat loops) are covered through their callers and the golden renders,
not line by line.

| File | Before | After | New tests |
| --- | --- | --- | --- |
| core/chord.cpp | 84 / 14 (85.7%) | 88 / 10 (89.8%) | tests/test_chord.cpp |
| core/factory.cpp | 53 / 221 (19.3%) | 53 / 221 (19.3%) | none: preset content |
| core/pattern.cpp | 206 / 33 (86.2%) | 217 / 22 (90.8%) | tests/test_pattern.cpp |
| core/pcm.cpp | 194 / 31 (86.2%) | 200 / 25 (88.9%) | tests/test_pcm.cpp |
| core/project.cpp | 308 / 54 (85.1%) | 314 / 48 (86.7%) | tests/test_project.cpp |
| core/record.cpp | 64 / 28 (69.6%) | 69 / 23 (75.0%) | tests/test_record.cpp |
| core/samplepool.cpp | 122 / 28 (81.3%) | 127 / 23 (84.7%) | tests/test_samplepool.cpp |
| core/scale.cpp | 42 / 12 (77.8%) | 46 / 8 (85.2%) | tests/test_scale.cpp |
| core/serialize.cpp | 256 / 43 (85.6%) | 264 / 35 (88.3%) | tests/test_serialize.cpp |
| core/stepedit.cpp | 184 / 43 (81.1%) | 194 / 33 (85.5%) | tests/test_stepedit.cpp |
| dsp/buses.cpp | 137 / 117 (53.9%) | 183 / 71 (72.0%) | tests/test_chain.cpp, goldens |
| dsp/capture.cpp | 59 / 23 (72.0%) | 60 / 22 (73.2%) | tests/test_capture.cpp |
| dsp/chain.cpp | 96 / 40 (70.6%) | 112 / 24 (82.4%) | tests/test_chain.cpp, goldens |
| dsp/grain.cpp | 189 / 113 (62.6%) | 205 / 97 (67.9%) | tests/test_grain.cpp, goldens |
| dsp/hat.cpp | 112 / 71 (61.2%) | 134 / 49 (73.2%) | goldens |
| dsp/kick.cpp | 108 / 74 (59.3%) | 128 / 54 (70.3%) | goldens |
| dsp/perc.cpp | 135 / 74 (64.6%) | 154 / 55 (73.7%) | goldens |
| dsp/repeat.cpp | 27 / 38 (41.5%) | 32 / 33 (49.2%) | tests/test_repeat.cpp, goldens |
| dsp/sample.cpp | 274 / 169 (61.9%) | 295 / 148 (66.6%) | goldens |
| dsp/snare.cpp | 144 / 84 (63.2%) | 167 / 61 (73.2%) | goldens |
| dsp/va.cpp | 165 / 67 (71.1%) | 168 / 64 (72.4%) | goldens |
| dsp/vapoly.cpp | 93 / 72 (56.4%) | 108 / 57 (65.5%) | goldens |

"Before" for dsp/ already had the first 51 golden renders in; a run without any (stopped at 600
of 2,689 mutants) had 153 of the first 271 buses.cpp mutants surviving. The after runs were taken
before main's dsp/sample.cpp and dsp/vapoly.cpp changes merged in; vapoly's after figure is from
before the last golden renders. Most of what survives in dsp/ is in the classes below: fresh-object
resets (a third of the machines' survivors), steal tie-breaks, and the sampler's fast-path window.

"After" for the second pass re-ran only the survivors: the code under test did not change, so a
mutant the old suite killed is still killed.

## Golden renders

The DSP files could not be pinned by property tests alone. A hit that sounds, decays and moves
with its knobs passes whether a coefficient is right or one off, so most dsp/ survivors were
sound changes nobody would catch until a listening test. tests/test_golden.cpp renders every
machine, the insert chain over its seven filter modes, the mixer and beat repeat with fixed
patches and trig patterns, and compares each render with tests/golden_data.inc: 24 samples at
fixed positions and the mean square, within 1e-7 absolute plus 1e-4 relative. The renders are
bit-stable at -O2 and under ASan at -O1.

They do not say the sound is right (docs/SOUND.md and the unit checklist do that); they say it
has not changed by accident. **A deliberate sound change regenerates them**, after listening:

```sh
make -C host build && STRATA_GOLDEN=print build/host/strata-tests > /tmp/g.txt
python3 tools/golden_update.py /tmp/g.txt      # rewrites tests/golden_data.inc
```

and the diff of golden_data.inc goes in the same commit as the DSP change. 89 renders: two patches
of three spaced hits per machine, a dense pattern (retriggers that steal, a chord bigger than any
pool, key ups, STOP twice) over four more patches, and a steal's splice summarised sample by
sample.

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

The second pass found no bug either. What it found unpinned:

- **Every mixer setter.** sends_from_bytes on the default bytes lands on the defaults, so a
  setter that dropped its value, or reported failure, still passed. Each field is now set to a
  value that is not its default, and to both sides of its range.
- **The tail gate's length**, now pinned to the block for both sends, and the output level's
  ramp in both directions (a ramp up from 0.5 to 1 could have been skipped).
- **One-frame blocks** through the mixer, the insert chain, capture and beat repeat.
- **State that a reset must drop**: the duck's envelope and a trigger aimed past the block, the
  chain's filter and lo-fi hold on clear_state(), the half frame a pitched-down repeat leaves.
- **A muted hard-panned track** that the chain called silent while one gain was still ramping.
- **Grain**: a sample swapped in under unchanged knobs keeps its old read step unless
  set_sample() recomputes it (the engine prepares only when knobs move); a spawn into a full
  pool steals the grain furthest through its window and takes the full-pool crowd gain.
- **Core**: chord packing and inversion at the top of the range, scale folds onto 0 and 127,
  step-edit labels and walks at both ends, the resampler's steps and antialias coefficients
  against a formula, damaged lock runs, a full lock pool, serialize's malformed boundaries and a
  bank cut mid-pattern.
- **The sound of every machine**, by golden renders (above).

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

### Second pass: core/ and dsp/

**Fresh-object resets.** Every machine's init(), and Mixer, Capture and BeatRepeat's, assigns
state that the member initialisers already hold and zero-fills buffers that are written before
they are read. Each object is init()ed once per process, and never reused. (A reset that runs
again later, such as Duck::reset or InsertChain::clear_state, is tested.)

**Steal tie-breaks.** Which of two equally loud drum voices is stolen, and the rotating cursors
in VaPoly, Sampler and Granular, change the choice only between voices or grains at exactly the
same level or window phase.

**A fade that cannot span two slices.** A stolen drum or sample voice fades over 24 frames from
the top of the block, so with audio()'s 64-frame blocks the gain carried between slices of the
fade is never read. The splice itself is pinned sample by sample by golden renders.

**Sub-block slicing.** `n = frames - done < kBlock ? ...` in every render_add: audio()'s block is
kBlock, so the loop runs once.

**Paths that compute the same samples.** Sampler's safe window (safe_lo, safe_hi and the run_end
estimate) and Granular's fit_length choose between an unclamped fast loop and the clamped one.
Narrowing them costs CPU and changes nothing; widening them reads past the buffer, which is the
loop-bounds class above (ASan, and the arithmetic bound tests in test_grain.cpp).

**Skip tests that only save CPU.** InsertChain::silent() when the answer would only add zeros,
a duck at depth 0, a send at gain 0, the limiter's skip test (unity below the threshold), the
master ramp skipped at unity, a cached reciprocal recomputed, and the send tails running a block
longer or shorter past -60 dB.

**Identical or equal at the boundary.** `fltr + 0` against `fltr - 0`; pan and DJ at their exact
centre; `cos_turns(0.25)` is exactly 0, so the hard-pan snap is the law's own value; a zero
attack makes `gate_on(true)` and `gate_on(false)` the same.

**Unreachable.** mode_from's clamp (a parameter byte is at most 127), delay_division_index's
clamp, a null from `new`, the channel count of a refused sample, start_voice() on a machine
with no sample (trigger() refuses first).

**Preset content.** core/factory.cpp is the factory kits and patterns: 221 of its 274 mutants
are a value moved by one, which is a different preset. tests/test_factory.cpp checks what each
kit must do, as test_boot.cpp does for the default beat.

**Core.** Checks that run only inside a static_assert (stepedit's every_label_fits); a Reader's
position after it has failed (every caller stops); `(p - 128) * 256` against `(p + 128) * 256`
for 8-bit PCM, equal after the int16 wrap; floor_to_int's negative path, which
stamp_normalise's carry repairs; record's candidate -1, which cannot win; samplepool merges that
coalesce() repeats; div_u64 by a denominator of 1, which no caller passes.

## Accepted gaps

Real behaviour without a test, left for now because the test costs more than the risk:

- `Storage::init` and the settings file (runs once per process; a second Storage costs 90 MiB).
- `Storage::open_last`'s sequence numbers, the backlog shift, path truncation helpers, and the
  drain in `save_for_shutdown`.
- In the engine: a trig's frame and the accent and slide flags reaching the machine, the lock
  pool compaction path through the engine, and the duck.

Second pass:

- Grain: set_sample() clamps SPR to at least 1 frame rather than 0 until the next prepare(), a
  grain onset at 1/65536 of a frame, and the window's last entry (1.5e-4) zeroed: all below
  what a golden render's tolerance can see.
- The send tails' exact formula is pinned at one setting each; feedback at exactly 0.001 is not.
- dsp/freeze, dsp/lfo, dsp/tapestop and Sampler's new code arrived from main after these runs
  and have not been run line by line yet.

## Found along the way

- `Engine::resample_bars_` is written and cleared but never read: the take's length is the
  frame count handed to capture_.arm(). Noted in app/engine.cpp.
