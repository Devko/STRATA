# Handoff: M4 and M5, now actually built and run

Written mid-M4 when the development machine had to go off, and rewritten twice since by the
cloud sessions that picked it up. **The headline has changed: there is now a device build and
three emulator scenarios pass.**

## State

| | |
|---|---|
| `make -C host test` | **684 tests, 454,581 checks, 0 failures** |
| `tools/build.sh` | **links.** APP.ENA, 395,800 bytes; 396 KiB text, 1.21 MiB bss; no BOOT, MCU or SPI sections |
| `tests/scenario_m3.sh` | **passes**, 22.66% peak, 0 overruns — the undo trail is exactly right after everything M4 and M5 did to `app/engine.cpp` |
| `tests/scenario_m4.sh` | **passes** (new): autosave, save, and a queued change switching immediately with the clock stopped |
| `tests/scenario_m5.sh` | **passes** (new): beat repeat's always-on cost measured at +0.82 points |
| `tools/render.sh` | 12.2 s rendered; peak +0.00 dBFS (the limiter), rms -17.6 dBFS |
| **cl6x** | still impossible: TI's installer needs an interactive licence click. Every figure above is GCC-only and the ~2.2x cl6x is worth has NOT been applied |

So M4 and M5 are no longer unverified code. What they are is **verified in an emulator with no
cache model, on a GCC-only build**. `docs/BUDGET.md` states both caveats next to every number,
and the unit still decides.

## How a cloud session gets a device build

It took real work to discover; it is now two commands.

```sh
tools/fetch_toolchain_sources.sh   # then
tools/build_toolchain.sh           # binutils a few minutes, gcc 10-30
```

`tools/fetch_toolchain_sources.sh` exists because GCC's own `contrib/download_prerequisites`
fetches GMP, MPFR, MPC and ISL over **plain HTTP**, which an HTTPS-only proxy does not serve at
all — so the GCC build fails at that step. It fetches the same four over HTTPS, lays them out
where `download_prerequisites` would, and retries, which the Enaga scripts' bare `curl -sSL` does
not.

The cloud environment also needs `ftp.gnu.org` and `gcc.gnu.org` allowed. A personal environment
named `strata-toolchain` has them, plus a setup script that installs `flex` (only in the apt
sources after an `apt-get update`) and clones `third_party/smpltrek-re` for the emulator. The
toolchain build is deliberately NOT in that script: it would make every session start ten minutes
slower.

## What is still not verified

The emulator can press **keys and nothing else** — `emulate.py` has one `--key` option and no pad
or encoder support. docs/SPEC.md puts seven of M5's eight tools on the upper PADS, so FILL,
REPEAT, MUTE mode, the two scene selectors, THROW and MUTATE **cannot be driven by any emulator
scenario**. Held beat repeat, a moving morph and a moving macro are unmeasured for the same
reason, and `docs/UNIT_CHECKLIST.md`'s M5 section is written for the unit because of it.

Nothing has run on hardware, nothing has been measured with cl6x, and the sample loader has never
seen a real WAV — the emulator's card holds the app and nothing else.

What a compile still means, for the parts no scenario reaches:

- all 30 translation units (`core/`, `dsp/`, `app/`) compile with the device build's own flags;
- every symbol they reference resolves within STRATA except `enaga::host()`, which Enaga's
  runtime supplies — so no call dangles;
- `tools/build.sh`'s flat-namespace check passes.

It does **not** mean a path no scenario walks works. The three scenarios between them cover
booting, the transport, editing, undo, the queue, a save, an autosave and entering PERFORM — not
the sample loader, not a card being pulled, not the performance row.

**The merge to `main` happened anyway, on the user's instruction**, with the original rule ("do
not merge until the device build and `scenario_m3.sh` are green") knowingly set aside. `main`
before the merge was `d0d2c23`, if it needs to go back.

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

All of M4 and most of M5 is written. None of it past `core/` and `dsp/`'s host tests has run.

1. **`tools/build.sh` must link for the device.** Everything compiles and every symbol resolves,
   so what remains is the link and whatever the tic6x toolchain says that a host compiler does
   not.
2. **`tests/scenario_m3.sh` must still pass** — it proves undo end to end, and `app/engine.cpp`
   has changed under it twice since.
3. **Measure `dsp/repeat`.** It is the one DSP change M5 made, it is a per-block tax on the
   master output, and whether it fits is the entire question about it. `docs/BUDGET.md` has the
   rows waiting and `docs/UNIT_CHECKLIST.md`'s M5 section leads with them. If it does not fit,
   `docs/SPEC.md` ranks cutting effects above cutting voices.
4. **Check the memory.** 64 MiB sample region + 18 MiB loader staging + 2.93 MiB repeat ring is
   85 MiB of the ~124 MiB heap before anything else, and the staging was never in the spec's
   table. This is the figure most likely to be a problem and it is cheap to check.
5. **Work `docs/UNIT_CHECKLIST.md`'s M4 and M5 sections.** Every box is open. The ones that
   decide whether the milestones stand: does it start with no card, does a power cycle restore
   the project, does swapping a sample on a sounding track stay clean, does pulling the card
   during a save leave the project intact.
6. The M4 and M5 emulator scenarios, then `tools/render.sh` for a listen — nothing has been
   heard since M3.

### Read the app code before trusting it

It was written with no way to run it. Six bugs were found by re-reading or by mutation-testing
that a compiler could not have caught, and the odds that those were the last six are poor:

- `pool_.reset()` after a project load forgets every reservation while the samplers still hold
  pointers into those ranges. Its own contract says the caller must have silenced every track
  first, and nothing had. Each track now goes through clear → epoch → release in turn.
- the first convert slice ran while the job was still `sample_read`, so the next pass found
  `read_done_` false and the conversion stalled after one slice.
- a new project inherited the old one's name, so saving it would have overwritten a project the
  user never asked to touch.
- `app/engine.cpp` stored 60 in `Pattern::root`, a field `core/serialize.cpp` reduces to `% 12`
  on load, and then used it as a MIDI note — so the default pattern's notes would have come back
  at the bottom of the MIDI range after a save and a load. The core side was right and tested;
  the engine was the side that disagreed.
- `dsp/repeat`'s `hold()` guard looked redundant to every test, because nothing called it twice
  in a row. `hold(pad_down)` once a block is the obvious wiring, and without the guard the loop
  re-arms every block and stutters at the block rate.
- the scene morph's skip of an untouched parameter looked equivalent to writing the kit value
  back, because every test handed it a fresh copy of the kit. It is not equivalent once a macro
  has written into that buffer, which is how `app/` now calls it.
- the bar counter resynced only on a FORWARD jump, so a stop and start — which relocates the
  clock — left auto-fill firing in the wrong place for as many bars as the jump was long.
- holding FILL with the transport stopped did nothing, `resolve_tick` being the only thing that
  resolved it.

And a whole class of mistake worth naming, because M5 made it three times: **a feature with no
gesture.** `core/scale` went in with no way to pick a scale; `mute_store`, `mute_recall`,
`set_auto_fill` and `macro_assign` all went in with no key. Nothing catches this — it compiles,
it is tested, and it is dead. The check that found them was mechanical: grep `app/app.cpp` for
each `Cmd::` the engine handles and count the posts. Worth doing again after any milestone.

The places to look hardest, because they are where a mistake is silent rather than loud: the
epoch wait in `Storage::step_sample_epoch`, the slice arithmetic in `step_sample_convert` and
`step_sample_resample` (every pointer there is an offset into the 64 MiB region), and the
deferred-save condition in `Storage::service`.

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

## What M4 deliberately does not have

- **No file browser.** A project's name comes from `SETTINGS.TXT` and a track's sample path from
  its kit, which is all that "reopens the last project" and "loaded whole into RAM at project
  open" need. Choosing a different project or sample from the card is a browser, and a browser
  was not in M4's list.
- **No streaming.** `docs/SPEC.md` puts streams at recording and loops over 30 s. The loader
  refuses a file over 30 s that also needs resampling (`TOO LONG`) rather than pretending.
- **Project::bank_dirty is unused by `app/`.** Why is under "Why the dirty flags are here" in
  `app/storage.hpp`: an edit happens on the audio side and a save in the main context, so that
  bool would be cross-context state of exactly the kind CLAUDE.md forbids. The app tracks
  dirtiness in one context instead, and `core/project.cpp` keeps the field for its own use.

## Two decisions worth not re-litigating

- **The sample swap protocol.** `SamplePool::release` makes memory reusable immediately and
  `core/` deliberately cannot check whether a voice is still reading it. So the order is
  `clear_sample` → **wait for `Status::sample_epoch[track]` to change** → release the range →
  load → `set_sample`. Releasing before the epoch confirms means overwriting memory a voice is
  reading, which sounds like a broken sampler rather than a broken lifetime. `docs/SPEC.md`
  specifies this as a generation counter. **Both halves are now implemented** — the audio side in
  `Engine::apply`, the loader in `Storage::step_sample_epoch` — and neither has run.
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
