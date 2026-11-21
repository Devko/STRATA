# STRATA CPU and memory budget

Measured figures only. Estimates belong in `docs/SPEC.md`; this file records what was actually
observed, and every entry says where it was measured. **Only the unit's numbers count** — the
emulator models no caches or bus latency, so its figures compare versions and nothing else.

One 64-frame block at 372 MHz is **496,000 cycles**. Every share below is a share of that.

## Toolchain status

| | |
| --- | --- |
| tic6x-elf GCC | 13.3.0, built from source by `tools/build_toolchain.sh` into `~/strata-toolchain/tic6x` |
| TI cl6x | **8.5.0.LTS, installed** at `~/ti/ti-cgt-c6000_8.5.0.LTS`; Enaga's `paths.py` finds it automatically |
| Current build | **cl6x** — `tools/build.sh` reports `built with cl6x: the DSP library, chainloops.ti.c, drumloops.ti.c, masterloops.ti.c` |

### cl6x is load-bearing, and it is now measured on our own code

Enaga measured about **2.2×** on its synth example (30% of budget with cl6x, 65% without).
STRATA's own figures now say the same thing, and the decisive pair is the eight-track worst case:

| | cl6x | GCC only |
| --- | --- | --- |
| 8 tracks, cheap settings | **17.00%** | 28.38% |
| 8 tracks, **worst case** (drive, LP24, lo-fi, both sends, all eight tracks) | **23.61%** | **44.26%** |

**M2's exit bar is 40%. With cl6x there is comfortable room; GCC-only fails it.** That settles the
question the first version of this section left open: a GCC-only STRATA does not hit its targets.

### The one thing still open: redistribution

Installing cl6x is done. **Whether TI's terms permit distributing what it builds is not**, and it
is not Claude's to settle. A released `APP.ENA` contains cl6x-compiled code, and the figures above
say we cannot simply fall back to GCC if the answer is no. Worth reading before a public release,
and worth raising with Enaga upstream, which has the same exposure.

Until it is settled, record **every** load figure twice — with cl6x and with
`ENAGA_TI_CGT=none` — so a GCC-only release never becomes a late surprise.

## Reference figures from Enaga (measured on a unit, caches on)

Not STRATA's code; these are the per-unit costs STRATA's budget is built from.

| Workload | Cycles/voice | Share of block | Unit cap |
| --- | --- | --- | --- |
| SYNTH (full VA: 2 osc, sub, noise, filter, 3 ADSR, 2 LFO, 8-slot matrix) | 1,868 | 0.377% | 228 voices (85.9%) |
| SAMPLE (reads DDR) | 2,592 | 0.523% | 163 voices (85.3%) |
| GRAIN (reads DDR) | 2,690 | 0.542% | 160 voices (87.0%) |
| Enaga itself | ~13,000/block | ~3% | — |
| Reverb + delay together | — | **16%** | measured on the emulator, not the unit |

`tests/test_platform.cpp` asserts the arithmetic derived from these, so a submodule bump that
changes a platform constant fails the host tests rather than drifting silently.

## STRATA measurements

### M0 — skeleton

| Configuration | Peak | Blocks | Overruns | Where | Date |
| --- | --- | --- | --- | --- | --- |
| Skeleton, GCC-only | **1.52%** (7,524 cycles) | 653 | 0 | emulator | 2026-11-21 |
| Skeleton, cl6x | not measured — cl6x not installed | | | emulator | |
| Skeleton, GCC-only | **not yet measured** | | | **unit** | |

The skeleton outputs silence, so its load is Enaga's own overhead plus the draw path. It is the
baseline every later figure is read against.

1.52% sits right alongside Enaga's own tone example, which its design log puts at about 1.6% on
the emulator — so the skeleton adds nothing measurable of its own, which is what a baseline
should show. Reproduce with `tests/scenario_m0.sh`, which asserts the figure stays in that range
so an accidental per-block cost shows up as a test failure rather than as drift.

Package size: 7,200 bytes (payload 7,136, bss 24).

### M1 — scheduler and drums

Emulator, 130 BPM, the default four-track techno kit (KICK, SNARE, HAT closed, HAT open), filter
off, lo-fi off, sends closed. 3,072 blocks, **0 overruns** in every run.

| Configuration | Peak | Cycles |
| --- | --- | --- |
| Idle, transport stopped | **2.14%** | 10,629 |
| Playing, cl6x | **20.14%** | 99,878 |
| Playing, GCC only | **22.32%** | 110,727 |

**The important finding: cl6x is only buying 1.11× here, not the 2.2× Enaga measured.** That is
not a disappointment in cl6x — it is because almost none of STRATA's DSP is going through it.
`build_app.py` sends only `*.ti.c` files and Enaga's own DSP library to cl6x, and every
per-sample loop in the machines and the insert chain is currently plain C++ in a `.cpp`. **The
2.2× is available and unclaimed.** Moving the hot loops into `*.ti.c` is the single biggest
lever left, and `CLAUDE.md` already requires it; it has simply not been done yet.

**Against M1's exit criterion of under 15%, this does not pass — but the criterion is about the
unit, and these are emulator figures.** Three reasons the unit number should be lower:

- the emulator models no caches or bus latency, and Enaga measured pure computation running
  about **25% faster** on real hardware
- this is the **peak** block, which is the one where several drums trigger together; the average
  is well below it
- none of the `*.ti.c` work above has been done

So the honest position is that M1's load criterion is **not closed** and cannot be closed without
hardware. It is a line in `docs/UNIT_CHECKLIST.md`, not a tick here.

Package: 133,888 bytes (payload 133,824, bss 26,096).

### M2 — hot loops in `*.ti.c`

M1's finding, answered. `build_app.py` sends only `*.ti.c` files and Enaga's own DSP library to
cl6x, STRATA had **no** `*.ti.c` file at all, and that is why cl6x was buying 1.11× there instead
of the 2.2× Enaga measures on its own synth example. Five sets of per-sample loops have now moved
into three plain-C files, which cl6x compiles while every other translation unit still goes
through GCC:

| File | What moved into it |
| --- | --- |
| `dsp/drumloops.ti.c` | KICK's body shaper and its click; SNARE's tuned pair; HAT's square bank and its VCA; the declick ramp all three share |
| `dsp/chainloops.ti.c` | the insert chain's drive, lo-fi and duck — the three loops chain.cpp's own cost comment named |
| `dsp/masterloops.ti.c` | the master's peak detector, gain ramp, soft limiter and the interleave |

**The move is a compilation-unit change and nothing else**: the same expressions in the same
order, through the same inline `<enaga/dsp/math.h>` functions. Checked two ways. The host suite
passes unaltered — 175,313 checks for the subset this work touches, 218,456 with the machines
being written in parallel, 0 failures — and a renderer built twice from one harness, once against
`dsp/` as of the M1 commit and once against it now, produces **byte-identical** output over
32,768 frames of a four-track beat with every extracted stage switched on (drive, a filter mode,
lo-fi, the duck, both sends, voice stealing and the limiter).

Same scenario as M1 in every other respect: `tests/scenario_m1.sh`, emulator, 130 BPM, the
default four-track kit, 3,072 blocks, **0 overruns in every run**. cl6x is **8.5.0.LTS**, now
installed at `~/ti/ti-cgt-c6000_8.5.0.LTS`; `tools/build.sh` reports `building with cl6x` and
names the three files. (This paragraph used to say the "Toolchain status" table above still read
"not installed"; it was corrected at the end of M2 and now records the install. The *second*
open question there, whether TI's licence permits redistributing cl6x output in a released
`APP.ENA`, is untouched by this work and still needs a human's reading. Note for anyone checking
the version: the flag is `cl6x --compiler_revision`, which prints `8.5.0`. `--version` is not a
cl6x flag — it exits non-zero and prints nothing, which reads exactly like a missing compiler.)

Read the workload before the figures: every number below is the **four-track** kit, with
`app/engine.cpp` rendering `machines_[0..3]` and M1's default pattern, which is what M1 measured.
Each before/after pair was measured minutes apart with nothing between them but the extraction
itself. The parallel M2 work has since wired PERC and three more machines into the default kit,
so `tests/scenario_m1.sh` on a later tree measures a **different**, heavier workload and will not
reproduce these figures — it needs its own baseline, taken once that kit settles.

| Configuration | Peak | Cycles |
| --- | --- | --- |
| M1, playing, cl6x (no `*.ti.c`) | 20.14% | 99,878 |
| M1, playing, GCC only | 22.32% | 110,727 |
| **M2, playing, cl6x** | **12.02%** | **59,601** |
| **M2, playing, GCC only** | **21.64%** | 107,315 |

**cl6x now buys 1.80×, where M1 measured 1.11×**, and the four-track beat went from 20.14% to
12.02% of the block — a 1.68× speed-up of the whole audio callback, Enaga's own ~3% of fixed
overhead included.

#### What each extraction bought

Measured one at a time, in this order; each row is the whole app with that extraction added to
the ones above it.

| Moved into `*.ti.c` | cl6x peak | Cycles | Saved | GCC-only peak |
| --- | --- | --- | --- | --- |
| — (M1 baseline, reproduced) | 20.14% | 99,878 | — | 22.32% |
| KICK body shaper + click | 17.00% | 84,297 | 15,581 (3.14 points) | 22.08% |
| SNARE tuned pair | 15.30% | 75,907 | 8,390 (1.70 points) | 22.08% |
| HAT square bank + VCA | 13.55% | 67,196 | 8,711 (1.75 points) | 21.95% |
| chain drive + lo-fi + duck | 13.07% | 64,813 | 2,383 (0.48 points) | 21.95% |
| master peak + ramp + limiter + interleave | 12.02% | 59,601 | 5,212 (1.05 points) | 21.64% |

The order of the first three is exactly what the machines' own cost comments predicted. KICK's
shaper is the single most expensive loop in the app — a reciprocal **and** a sine every sample,
because dirtiness blends between them — and it alone is 3.14 points. SNARE's pair is two
reciprocals and no sine. HAT's bank is six integer accumulators, which is why six oscillators
cost less than one of KICK's samples.

The chain row is small **only because this kit asks so little of the chain**: in the default kit
the filter is off, lo-fi is off and the duck depth is zero, so exactly one of the three loops ran
at all, on one track of four (the kick's drive). With eight tracks driving, crushing and ducking
it is eight passes a block, which is M0.5's measurement to make. The master row is the opposite
case — those four loops run on every block whatever is playing, which is why they were worth
moving even at a few flops a sample.

The GCC-only column barely moves, and it should not: with `ENAGA_TI_CGT=none` the same compiler
builds the same arithmetic either side of the move and only the translation unit changes (the
3,412 cycles it does drop are the loops no longer being inlined into the C++ around them).
**Essentially the whole gain is cl6x's**, which is the point of the file split and the reason
`CLAUDE.md` asks for it.

#### What was deliberately left alone

- **`Svf::process`, `Svf::cascade`, `Envelope::render`, `OnePole::process`, `enaga_mix`,
  `enaga_mix_product`, `enaga_scale`, `enaga_multiply`, `enaga_ramp_stereo_add`, the delay and
  the reverb.** Already C block functions in Enaga's library, which `build_app.py` builds with
  cl6x. chain.cpp named the filter as its largest per-track stage; it needed nothing at all. This
  is most of the per-sample work left in the insert chain and all of it in the sends.
- **`Duck::render`.** A sequential state machine (attack, a hold counter, release) run **once per
  block for the whole mixer**, not once per track, whose per-sample work is one add. Its stage
  transitions are a loop-carried dependency cl6x cannot pipeline either, so the move would buy
  almost nothing and would put the envelope's shape at risk for it.
- **The send bus's mono-to-stereo copy** in `finish_block`: one block copy, only while the delay
  is live, with no arithmetic to pipeline.
- **Everything in `prepare()` and the machines' slice loops.** Control rate, or a handful of
  iterations per block.
- **A cheaper shaper.** kick.cpp and snare.cpp both suggest a polynomial fit for 1/(1+|t|) over
  [1, 2] in place of `enaga_reciprocal`. That would change the sound, so it belongs in its own
  change with a listening test — not hidden inside a compilation-unit move.

#### Is there more to get this way?

For this kit, the returns have flattened. **No hand-written per-sample loop is left in the M1
audio path** apart from `Duck::render` and that one block copy; what remains is Enaga's ~3% of
fixed overhead, its cl6x-built block functions (now the bulk of the per-sample cost), the
per-voice control-rate arithmetic and the voice logic. So the next load lever for four drum
tracks is not another `*.ti.c` file but fewer or cheaper stages. The next *useful* `*.ti.c` work
is in code that does not exist yet: the chain's own loops once eight tracks actually drive and
duck (M0.5), and whatever per-sample loops PERC, SAMPLE and VA MONO bring with them — each of
those should be born in a `*.ti.c`, not moved into one later.

Against the milestone: M1's criterion is under 15% **on the unit**. The emulator now says 12.02%
with cl6x, and it models no caches while Enaga measures pure computation running about 25% faster
on real hardware — so this is now the right side of the bar on the emulator, which is still not
the same as passing it. The unit decides (`docs/UNIT_CHECKLIST.md`). And a GCC-only build still
does not get there: 21.64% for four tracks leaves nothing for the other four, which is exactly
the exposure the "Toolchain status" section asks to keep visible.

Package, with only M1's own sources staged — PERC and SAMPLE are in the tree being written in
parallel, so the repo build's figure is not a clean comparison: 144,768 → **143,776** bytes with
cl6x (payload 144,704 → 143,712, bss unchanged at 26,120), and 155,840 → 155,808 GCC-only. The
image got *smaller*: each loop was inlined at every call site before and is one called function
now. Neither figure matches M1's 133,888 because `app/` grew in parallel with this work. Worth
recording for the comparison above: while the figures in this section were being taken, PERC and
SAMPLE existed in `dsp/` but contributed **0 bytes** to the image — `--gc-sections` dropped them
because nothing referenced them yet, and the repo build and this isolated build came out the same
size to the byte — so their arrival between runs cannot have moved any figure here.

### M2 — eight tracks (the exit criterion)

All eight track slots filled and sounding, 130 BPM, 3,072 blocks, **0 overruns in every run**.
Emulator figures, so they compare versions rather than settle the target — but the *ratio*
between the two columns is the point here, and that is a property of the compilers.

| Configuration | cl6x | GCC only |
| --- | --- | --- |
| Cheap settings (filters off, lo-fi off, sends closed) | **17.00%** | 28.38% |
| **Worst case**: drive, LP24, lo-fi and both sends on all eight tracks | **23.61%** | **44.26%** |

**M2's exit is "all 8 tracks under 40% worst-case load", and with cl6x that passes with room to
spare. GCC-only does not pass at all.**

Two things to read carefully before treating this as settled:

- **Track 7 is a stand-in.** CHORD wants VA POLY, which is M6, so that slot currently holds a
  second mono VA. VA POLY has six voices against this one, so the real eight-track worst case
  will be **higher** than 23.61% — by roughly five more computed voices, which the measured
  1,868-cycle figure puts at about 1.9 points, plus whatever its chord logic costs.
- Track 8 plays a **synthesised placeholder loop** (eight slices built in `Engine::init`), not a
  WAV off the card. Real sample playback reads DDR, which Enaga measured at about 40% more than
  computing, so that track will also cost more once M4 can load files.

So: comfortably inside the bar today, with two known increases still to come and 16 points of
headroom to absorb them.

### M3 — sequencer depth (no per-block cost, and that is the finding)

M3 adds undo, live and motion recording, the step page, copy and paste and polymeter. None of it
is DSP, so the expected per-block cost is **zero**, and measuring it is how you find out whether
some of it accidentally landed in the audio callback anyway.

Same scenario, same kit, same tempo as M2's first row: `tests/scenario_m1.sh`, 3,072 blocks.

| | cl6x | GCC only |
| --- | --- | --- |
| M2, default kit | 17.00% (84,320 cycles) | 28.38% |
| **M3, default kit** | **17.01%** (84,381 cycles) | **28.92%** (143,446 cycles) |

0 overruns in every run. With cl6x the difference is **61 cycles**, which is noise: the
sequencer's new work is all control-rate, happening when a command arrives rather than every
block, so a milestone with no DSP in it moves the per-block cost by nothing.

The GCC-only column moved further — 0.54 points, about 2,700 cycles — and that is worth not
glossing over, because nothing new runs per block in either build. The likely reason, **stated as
a hypothesis and not measured**: with `ENAGA_TI_CGT=none` the hot path is the C++ in `dsp/` and
`app/engine.cpp`, and `Engine::apply`'s switch grew by a dozen cases in this milestone, which can
change GCC's inlining and scheduling decisions for the whole translation unit. With cl6x the hot
inner loops live in `*.ti.c` and are compiled apart from all of that, which is exactly why they
were moved there and why that column barely moves. If this matters later, the way to settle it is
the same trick M2 used for the extraction: build one renderer against both trees and compare.

Either way it changes no conclusion. The cheap kit was never near the bar in either build, and
GCC-only's *worst case* already failed at M2 (44.26% against 40%).

**A peak figure is only meaningful next to its run length**, and this milestone made that
concrete in two directions. `tools/render.py`'s 20-second render covers 15,209 blocks and finds
**17.67%** (87,620 cycles) on the same kit — a longer run catches a worse block, so 17.67% is
the honest "worst seen so far" and the two 17.0% figures above are a like-for-like comparison at
3,072 blocks rather than the ceiling.

In the other direction, `tests/scenario_m3.sh` reports **13.68%** for the same kit, which is *not*
a different result —
it runs 700 M cycles against the other's 1,600 M, so its peak window never catches the block
where the open hat, the stab and the sampler overlap. A peak figure is only comparable against
the same run length, which is why the load tracking stays on the longer scenario.

#### What M3 costs instead: memory, and a spike per edit

| | M2 | M3 |
| --- | --- | --- |
| `APP.ENA` | 202,168 bytes | **246,720 bytes** |
| bss (zeroed at load) | ~69 KiB | **208,952 bytes** |

The bss growth is almost entirely `core/undo.hpp`'s 17 whole-pattern snapshots — 16 levels plus
the slot the live state is parked in on the way past — at about 8.2 KiB each. That is a
deliberate trade: `docs/SPEC.md` budgets 2 MiB for "patterns, kit, scenes, 16 undo levels" and
snapshots use a fourteenth of it, in exchange for an undo that cannot be wrong about what it
restores. A delta scheme would have had to describe every kind of edit correctly, lock-pool
relocation included.

Per-edit costs, all of them in audio context and all of them once per edit rather than per block:

| Operation | Cost | When |
| --- | --- | --- |
| `undo_mark` | one ~8.2 KiB struct copy, about 8,000 cycles — 1.6% of one block | once per edit *gesture*, not per encoder click (`Gesture` coalescing) |
| `undo` / `redo` | two of the same | a key press |
| `range_paste` | 2 KiB stack frame, up to 1,024 `Lock` copies | a paste |
| `lock_delta` on a full pool | one pool compaction, then a retry | only when the pool is full |

The gesture coalescing is what keeps the first row honest: a 40-click encoder sweep is **one**
snapshot, not forty, which is also what keeps the history 16 *edits* deep rather than 16 clicks.

#### Audio measured off the render, not just the load counter

`tools/render.py` (new, and what `docs/SPEC.md`'s testing table has always asked for) renders the
output offline to a WAV, draining the emulator's capture ring as it fills rather than reading it
once at the end — Enaga's own `emulate.py` keeps only the last 32,767 frames, 0.68 s.

On the default kit: **peak 0.00 dBFS, RMS −17.8 dBFS**, with 4 samples of 65,534 sitting at
exactly −2^31. Those four are the master's soft limiter **holding its ceiling**, not the output
clipping: the limiter is written so it "cannot be driven past full scale"
(`dsp/masterloops.ti.c`), and a float of exactly −1.0 converts to exactly −2^31. An 18 dB crest
factor on a drum mix that is touching the limiter a handful of times a second is what a techno
master is supposed to look like. Worth re-measuring if the kit's levels ever change, because the
same figure would mean something quite different if the count went from 4 to 400.

### M0.5 — budget spike (the fixed per-block tax)

**Folded into the M2 worst case above, deliberately.** The spike was planned as a throwaway
harness to measure the fixed chain before any engine existed. By the time the schedule reached
it, the real insert chain and buses were already written and tested, so building a second
imitation of them would have measured a copy instead of the thing. The worst-case row above —
drive, a 24 dB filter, lo-fi, the duck and both sends on all eight tracks — is the measurement
the spike was for, taken on the real code.

What it says: the fixed tax is the difference between the two M2 rows, **6.6 points** with cl6x
(17.00% to 23.61%). That is far below the ~30% that would have forced an architecture change,
and well under the 16% that delay and reverb alone measured before the chain learned to stop
running them when nothing feeds them. **No architecture change is needed.**

The original four-configuration table is kept below for the record; the first two rows are
answered by the M2 table above.

The point of the spike: voices are about 11% of the block in total, while delay and reverb
alone measured 16%. The dominant cost is the fixed chain, and these four rows decide whether
the 8-track architecture holds. See `docs/PLAN.md` M0.5.

| Configuration | Load | Where | Date |
| --- | --- | --- | --- |
| 8 insert chains + both sends + master, cl6x | | **unit** | |
| 8 insert chains + both sends + master, GCC-only | | **unit** | |
| Sends off (insert chains + master only) | | **unit** | |
| 4 insert chains instead of 8 | | **unit** | |

**Go/no-go:** if the fixed tax exceeds ~30%, change the architecture before writing engines.
Candidates, cheapest first: mono send busses, a shared filter across the drum tracks, lo-fi out
of the fixed chain into an optional stage, or a cheaper reverb.

### M4 and M5 — measured in the emulator, GCC-only (2026-11-21)

A cloud session built the tic6x toolchain from source and ran the emulator, so these are real
numbers rather than estimates. **What is still missing is cl6x**: TI's installer needs an
interactive licence click, so every figure here is the GCC-only build and the ~2.2x this document
records for cl6x has NOT been applied. A GCC-only number near the target is therefore not a
failure, and one comfortably under it is not yet a pass on the unit.

Emulator caveats that apply to every row: no cache model, and **peak depends on run length** --
the same build reads 22.66% over 1,257 blocks and 29.2% over 9,362. Compare versions, not
targets.

| Scenario | Blocks | Peak (GCC-only) | Overruns |
| --- | --- | --- | --- |
| `scenario_m3.sh` — the default beat, M5 build | 1,257 | **22.66%** | 0 |
| `scenario_m4.sh` — + stop, queue A02, save | 1,661 | **22.66%** | 0 |
| `scenario_m5.sh` — + PERFORM, scene capture, macro assign | 1,258 | **23.48%** | 0 |
| `tools/render.sh --seconds 12` — 12.2 s continuous | 9,362 | **29.2%** | 0 |

**Beat repeat's always-on cost: +0.82 points** (23.48 against 22.66 on the same build and the
same beat). That is the one DSP figure M5 owed. It covers `dsp/repeat`'s ring copy of every
block plus the per-block performance pass, and it is the shape expected of one 64-frame stereo
copy and a 128-parameter compare. `tests/scenario_m5.sh` carries the 22.66% baseline and fails
loudly if the gap ever exceeds five points.

Still unmeasured, because the emulator can press KEYS and nothing else (`emulate.py` has one
`--key` option and no pad or encoder support) and seven of M5's eight tools are on the upper
PADS:

| Change | Expected shape | Measured |
| --- | --- | --- |
| `dsp/repeat` HELD | one interpolated stereo read a frame, replacing the output | needs pads: the unit |
| Scene morph moving | 128 interpolations per move, control rate, shift not divide | needs an encoder |
| A macro moving | at most 16 values per move, control rate | needs an encoder |
| A throw | one parameter forced per block while held | needs pads |
| Mutation | one-off per press, main context | needs pads |

### What the first device build found

Three failures that no host compiler and no host test could have produced, all fixed:

1. **`core/pcm.cpp` included `<cstring>`.** The app is built `-ffreestanding -nostdlib` against a
   libstdc++ configured `--disable-hosted-libstdcxx`, which does not ship it. Now
   `__builtin_memcpy`, which needs no header.
2. **A GCC internal compiler error** compiling `core/project.cpp`'s `project_read`: "in priority,
   at haifa-sched.cc:1597". It needs `-O2` *and* `-funroll-loops` together -- `-O2` alone, and
   `-O2` with either scheduler pass off, all compile the file. `-funroll-loops` is one of
   build_app.py's flags and third_party/enaga is not ours to change, so the narrowest fix is a
   `push_options` / `no-unroll-loops` pragma around that one function. **To be reported upstream
   with that file as the failing case.**
3. **`dsp/repeatloops.h` used `unsigned int` where it meant `uint32_t`.** On tic6x `uint32_t` is
   `long unsigned int` -- a different type, same width -- so every call site was a compile error
   on the device and silently fine on x86. `dsp/chainloops.h` and `dsp/drumloops.h` take
   `<stdint.h>` for exactly this reason; now so does this one.

And one that stopped the LINK rather than a compile: a 64-bit divide in `Storage::progress`
pulled libgcc's `__c6xabi_divull` in, and on this toolchain that object also drags in the
ARM-style unwinder, whose `__exidx_start` / `__exidx_end` the app's linker script does not define.
The whole app failed to link for the sake of a percentage on the screen. Now 32-bit arithmetic,
which both products fit in.

**That last one is the same failure `seawaffle/enaga#1` fixes from the other side**, and it means
`docs/HANDOFF.md` was wrong to say "nothing in STRATA depends on it being merged". Nothing does
*now*, because the divide is gone -- but the next 64-bit division anywhere in `core/` or `app/`
will not link until that PR lands. `core/pcm.cpp` already hand-writes `div_u64` rather than
divide 64-bit, which is the pattern to follow.

### Later milestones

| Milestone | Target | Measured (cl6x) | Measured (GCC) | Date |
| --- | --- | --- | --- | --- |
| M1, 4-track beat at 130 BPM | under 15% | | 22.66% emulator, 4 of 8 tracks sounding | 2026-11-21 |
| M2, 8 tracks worst case | under 40% | | | |
| M6, everything worst case | under 60% | | | |

The M1 row is above its 15% target in the emulator, which does not settle it: there is no cache
model here and the unit's figures in the table above this one were measured with caches on. It is
the first thing a unit session should re-measure.

## Memory

Budgeted in `docs/SPEC.md` at about 76 MiB of the ~124 MiB app heap. The heap figures below are
what the code asks for in `init()`; the image figures are read off the linked ELF.

**The 85 MiB does fit.** The emulator boots the app to `APP RUNNING` rather than `APP INIT
FAILED`, and `App::init` returns false if any of those allocations fails -- so all three
scenarios passing is also the memory check. It is still worth re-checking on the unit, where the
heap is the real one. M4 and M5 together take 64 MiB for the sample region, 18 MiB of loader staging and 2.93 MiB
for the beat-repeat ring, which is 85 MiB before anything else, against a spec table that did not
count the staging at all.

| Use | Budget | Measured |
| --- | --- | --- |
| Samples, int16 (32 MiB default, 64 MiB ceiling) | 32-64 MiB | **64 MiB** reserved at init, from the heap. The CEILING, not the default: a project may raise its budget long after `init()` is over, and CLAUDE.md allows no allocation after it |
| Sample loader staging (M4) | not budgeted | **18 MiB**, from the heap: 11.5 MiB for one whole file (`files::load` wants a buffer for all of it) and 5.8 MiB for a resample's int16 source. Both sized from `docs/SPEC.md`'s own 30 s in-RAM limit, and a FIXED cost rather than a per-sample one. **This is the figure most worth checking against the heap on the unit** -- it was not in the spec's table and 64 + 18 is 82 MiB of the ~124 MiB |
| Beat repeat ring (2 bars at 60 BPM, stereo float) | 2.93 MiB | **2.93 MiB** (3,072,000 bytes) reserved at init. Below 60 BPM the ring holds less than two bars, which costs nothing: the longest loop offered is a quarter note, 3 s even at 20 BPM |
| Delay and reverb buffers | 2 MiB | |
| Patterns, kit, scenes, 16 undo levels | 2 MiB | **~148 KiB** so far: 17 whole-pattern undo snapshots (~140 KiB) plus the live pattern and the clipboard. In bss, not the heap |
| Recording ring and stream buffers | 1 MiB | |
| App image, voice pools, UI | 4 MiB | **396 KiB** of text, 52 bytes of data and **1.21 MiB of bss** in the linked ELF (`tic6x-elf-size`). The bss is mostly the live Project at 1.03 MiB plus the undo stack; the 85 MiB above is heap, taken in init() and not in the image |
