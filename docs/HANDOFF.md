# Handoff: where M4 stands, and what a cloud session can and cannot do

Written mid-M4 when the development machine had to go off. The `m4-wip` branch holds everything
that existed at that moment, including work that was still being written. **`main` is green and
untouched** — the last good state is `d0d2c23`, which is M3 plus the licence.

## Read this first: the build on `m4-wip` is BROKEN, deliberately

`m4-wip` is a snapshot, not a working tree. Three separate reasons:

1. `app/engine.cpp`, `app/engine.hpp` and `app/commands.hpp` are **half-converted** to the
   project model (see "The refactor in flight" below). They call functions that do not exist yet.
2. `tests/test_project.cpp` **does not exist** — the agent writing `core/project.cpp` was still
   on its tests when the snapshot was taken, so that module is unverified.
3. `core/pcm.cpp` and `core/queue.cpp` had just been written or revised and have not been run
   together with everything else.

Do not try to make `m4-wip` build by deleting things. Finish it, in the order below.

## What a cloud session CAN do

- `make -C host test` — the host suite. It needs the `third_party/enaga` submodule for headers
  (`git submodule update --init --recursive`) and nothing else. This is where the value is: every
  `core/` module is fully testable here, and that is most of M4.
- Write and review `core/`, `dsp/` and `app/` code.
- Everything in `docs/`.

## What a cloud session CANNOT do, and why

| | why |
|---|---|
| Device build (`tools/build.sh`) | needs the tic6x GCC cross toolchain, built from source by `tools/build_toolchain.sh` (~30 minutes), and it lives outside the repo at `~/strata-toolchain` |
| Anything with cl6x | TI's installer requires accepting a licence interactively. `docs/BUDGET.md` records that cl6x is load-bearing for the CPU target |
| Emulator scenarios (`tests/scenario_m*.sh`) | needs the toolchain **and** `third_party/smpltrek-re`, which is a **plain clone, not a submodule** — `git clone https://github.com/seawaffle/smpltrek-re third_party/smpltrek-re` |
| `tools/render.sh` (listening) | same as the emulator |
| Any load figure for `docs/BUDGET.md` | same, and the numbers would be meaningless anyway without the same toolchain |

So: a cloud session can take `core/` to green and write the `app/` code, but **nothing can be
called verified until the development machine measures it again.** Say so rather than implying
otherwise.

## Next steps, in order

1. **Finish `core/project.cpp`'s tests** (`tests/test_project.cpp`, which is missing). The
   invariants that matter are in `core/project.hpp`'s comments and in the brief that produced the
   module: round-trip each of the three file types field by field, a bank is 16 patterns back to
   back using `pattern_read`'s `consumed`, one damaged pattern inside a bank costs one pattern
   (the other 15 still load and `refused` says 1), and loading is value-tolerant but
   structure-strict.
2. **Get `make -C host test` green** with all four new modules. Before this snapshot the suite
   was 408 tests / 221,035 checks / 0 failures on `main`; the four M4 modules add roughly 120
   tests. `tests/check.hpp`'s `Registry::kMax` is 1024 and an overflow now counts as a failure.
3. **Finish the refactor in flight** (below).
4. **Then** the parts that need files: project save/load, autosave, `on_card(bool)`, background
   sample loading, and F3 PROJECT mode. None of it is written yet.

## The refactor in flight

The live pattern now lives *inside* the project rather than being a copy of it:
`Engine::live()` is `project_.live()`. The reason is in `app/engine.hpp`'s comment — an edit
lands in the pattern that gets saved, a pattern change is an index change rather than an 8 KiB
copy in the audio path, and a save reads exactly the memory the audio side plays from.

Done:
- `app/engine.cpp`: all 43 uses of the old `pattern_` member renamed to `live()`.
- `app/engine.hpp`: `Project project_`, `Queue queue_`, `ChainPlayer chain_`, the `live()`
  helpers, the `project()` / `queue()` / `chain()` accessors, `switch_pattern`, `cross_boundary`,
  `project_for_load`, `next_boundary_`.
- `app/engine.cpp`: `kit_from_params` / `params_from_kit`, `switch_pattern`, `cross_boundary`,
  the queue and chain handling inside `resolve_tick`, and the `kKitParams == kParams`
  static_assert.
- `app/commands.hpp`: `queue_pattern`, `cancel_queue`, `set_chain`, `kit_store`, `kit_recall`,
  `clear_sample`, `set_sample`; `Status::sample_epoch[]`, `live_bank`, `live_slot`; `pad2_`
  renamed to a usable `u`.

Not done, and the build fails until they are:
- The `switch` cases in `Engine::apply` for the seven new commands. The intended bodies are
  written out in this session's transcript; the non-obvious ones are `queue_pattern` (which
  switches immediately when the transport is stopped or the mode is `immediate`, because there is
  no "end of the current pass" to wait for) and `clear_sample` (which must `kill_all()` the
  track's voices and then bump the epoch).
- `Engine::tick_`, `Engine::boundary_known_` and the `sample_region_` setter are referenced but
  not all declared.
- The sample region's allocation moved out of `Engine::init` and has not landed in `app/app.cpp`
  yet. It is 64 MiB — the **ceiling**, not the default budget — from the heap in `init()`,
  because a project may raise its budget long after `init()` is over.

## Two decisions worth not re-litigating

- **The sample swap protocol.** `SamplePool::release` makes memory reusable immediately and
  `core/` deliberately cannot check whether a voice is still reading it. So the order is
  `clear_sample` → **wait for `Status::sample_epoch[track]` to change** → release the range →
  load → `set_sample`. Releasing before the epoch confirms means overwriting memory a voice is
  reading, which sounds like a broken sampler rather than a broken lifetime. `docs/SPEC.md`
  specifies this as a generation counter.
- **Saving must not read a torn step.** A save runs in the main context while the audio side may
  be applying an edit to the live pattern. The rule is: drain the command ring first, and defer
  the save by a frame if it is not empty. The UI is single-threaded, so once the ring is empty
  and the UI is the one saving, nothing is writing pattern memory at all. A project *load*
  additionally requires the transport stopped — with the clock stopped `clock::ticks()` yields
  nothing, so the scheduler never reads the pattern.

## Upstream

[seawaffle/enaga#1](https://github.com/seawaffle/enaga/pull/1) is open: a fix keeping libgcc's
unwinder out of an app's link (64-bit division and modulo could not link at all) plus two
documentation notes. Verified against their own 81-test suite. Nothing in STRATA depends on it
being merged; our pin stays at `1b41111` either way.
