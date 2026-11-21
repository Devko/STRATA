# Handoff: where M4 stands, and what a cloud session can and cannot do

Written mid-M4 when the development machine had to go off, and **updated by the cloud session
that picked it up afterwards**. `main` is green and untouched at `d0d2c23` (M3 plus the licence).
`m4-wip` now holds the M4 work.

## The build is no longer broken

The snapshot at `d455543` was deliberately unbuildable: `app/` called functions that did not
exist. That is fixed. As of `5611588`:

| | state |
|---|---|
| `core/project` | 51 tests, 8 of 8 mutants caught |
| `core/queue` | 42 tests, 7 of 7 new mutants caught |
| `core/samplepool` | 33 tests, 24 of 24 mutants caught |
| `core/pcm` | 37 tests, green with the full suite |
| `app/` | compiles clean, every TU, `-Wall -Wextra -Werror` |
| `make -C host test` | **571 tests, 425,261 checks, 0 failures** |

`main` was 408 tests / 221,035 checks; the four M4 modules add 163 tests.

## Read this before trusting the above

**Nothing in M4 has been verified on the unit, or in the emulator, or measured.** The cloud
session has no toolchain. What "`app/` compiles" means precisely:

- all 30 translation units (`core/`, `dsp/`, `app/`) compile with the device build's own flags;
- every symbol they reference resolves within STRATA except `enaga::host()`, which Enaga's
  runtime supplies — so no call dangles;
- `tools/build.sh`'s flat-namespace check passes.

It does **not** mean the app links, boots, or sounds like anything. That is the first job on the
machine, and it is why (a), (b) and (c) below come before any merge.

## What a cloud session CAN do

- `make -C host test` — the host suite. It needs the `third_party/enaga` submodule for headers
  (`git submodule update --init --recursive`) and nothing else. Every `core/` module is fully
  testable here, and that is most of M4.
- Compile-check `core/`, `dsp/` and `app/`, and check that every symbol resolves.
- Write and review code. Everything in `docs/`.

## What a cloud session CANNOT do, and why

| | why |
|---|---|
| Device build (`tools/build.sh`) | needs the tic6x GCC cross toolchain, built from source by `tools/build_toolchain.sh` (~30 minutes), and it lives outside the repo at `~/strata-toolchain`. In the cloud, `ftp.gnu.org` is also refused by the network policy, so it cannot even be built there |
| Anything with cl6x | TI's installer requires accepting a licence interactively. `docs/BUDGET.md` records that cl6x is load-bearing for the CPU target |
| Emulator scenarios (`tests/scenario_m*.sh`) | needs the toolchain **and** `third_party/smpltrek-re`, a **plain clone, not a submodule** — `git clone https://github.com/seawaffle/smpltrek-re third_party/smpltrek-re`. The clone itself works from the cloud; the toolchain does not |
| `tools/render.sh` (listening) | same as the emulator |
| Any load figure for `docs/BUDGET.md` | same, and the numbers would be meaningless anyway without the same toolchain |

## Next steps, in order

Steps 1 to 3 of the original handoff are done. What is left:

1. **`tools/build.sh` must link for the device.** Everything compiles, so what remains is the
   link and whatever the tic6x toolchain says that a host compiler does not.
2. **`tests/scenario_m3.sh` must still pass** — it proves undo end to end, and `app/engine.cpp`
   changed underneath it.
3. **Then the parts that need files**, none of it started: project save and load through
   `enaga::files`, autosave, `on_card(bool)`, background sample loading, and F3 PROJECT mode.
   `docs/PLAN.md` M4 and the "Saving" part of `docs/SPEC.md` are the spec. The two protocols
   below are not to be re-derived.
4. **The M4 emulator scenario**, then the load measured with and without cl6x, then
   `docs/BUDGET.md` and the new M4 section of `docs/UNIT_CHECKLIST.md`.
5. `tools/render.sh` for a listen — nothing has been heard since M3.

Do not merge `m4-wip` to `main` until 1 and 2 are green.

## What the refactor to the project model ended up as

The live pattern lives *inside* the project: `Engine::live()` is `project_.live()`. An edit lands
in the pattern that gets saved, a pattern change is an index change rather than an 8 KiB copy in
the audio path, and a save reads exactly the memory the audio side plays from.

Finished since the snapshot:

- The seven new commands in `Engine::apply`. `queue_pattern` switches immediately when the mode
  is `immediate` **or the transport is stopped** — with the clock still, no tick arrives to make
  a queued change due, so anything else leaves a pending switch that never happens. It arms from
  `tick_`, the tick the sequencer is actually on, which is why `core/queue.hpp` puts arming on
  the audio side at all.
- `set_sample` validates the offset, frame count and alignment against the region before forming
  a pointer. An odd offset is refused: a misaligned 16-bit read on the C674x does not fault, it
  silently returns the wrong address.
- `samplers_[]` beside `machines_[]`, filled in `init()`. `Cmd::set_sample` has to reach a
  `Sampler` and the build has no RTTI, so the tracks hosting one are recorded rather than a
  `Machine*` being reinterpreted.
- **The sample region moved to the main context**, as `app/engine.hpp` always said it should.
  `app/app.cpp` allocates the 64 MiB *ceiling* in `App::init` — not the default budget, because a
  project may raise its budget long after `init()` is over — and owns the `SamplePool`. No pool
  state is shared across the ring: the handover is a command carrying an offset, a frame count
  and a channel count.
- `Status::live_bank` / `live_slot` are published with the rest of the status every block, not in
  `switch_pattern`, because a project load will move the live pattern without going through it.

## Two decisions worth not re-litigating

- **The sample swap protocol.** `SamplePool::release` makes memory reusable immediately and
  `core/` deliberately cannot check whether a voice is still reading it. So the order is
  `clear_sample` → **wait for `Status::sample_epoch[track]` to change** → release the range →
  load → `set_sample`. Releasing before the epoch confirms means overwriting memory a voice is
  reading, which sounds like a broken sampler rather than a broken lifetime. `docs/SPEC.md`
  specifies this as a generation counter. The audio side's half is implemented; the loader's half
  is not written yet.
- **Saving must not read a torn step.** A save runs in the main context while the audio side may
  be applying an edit to the live pattern. The rule is: drain the command ring first, and defer
  the save by a frame if it is not empty. The UI is single-threaded, so once the ring is empty
  and the UI is the one saving, nothing is writing pattern memory at all. A project *load*
  additionally requires the transport stopped — with the clock stopped `clock::ticks()` yields
  nothing, so the scheduler never reads the pattern. `Engine::project_for_load()` is the hole in
  "the main context never writes pattern memory", and it is deliberate: a load replaces 1.03 MiB,
  which cannot travel through a 16-byte command.

## One small thing noticed in passing

`tests/test_record.cpp` and `tests/test_timing.cpp` each define `degenerate_input_is_handled` and
`a_long_run_stays_consistent`. They compile (the registrations are file-scope statics) but the
suite prints each name twice, so a failure does not say which file it was in. Pre-existing, from
M1/M2, and not worth mixing into an M4 commit.

## Upstream

[seawaffle/enaga#1](https://github.com/seawaffle/enaga/pull/1) is open: a fix keeping libgcc's
unwinder out of an app's link (64-bit division and modulo could not link at all) plus two
documentation notes. Verified against their own 81-test suite. Nothing in STRATA depends on it
being merged; our pin stays at `1b41111` either way.
