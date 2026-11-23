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
| Current build | **cl6x** — `tools/build.sh` reports `built with cl6x: the DSP library, chainloops.ti.c, drumloops.ti.c, grainloops.ti.c, masterloops.ti.c, repeatloops.ti.c, valoops.ti.c` |

### cl6x is load-bearing, and it is now measured on our own code

Enaga measured about **2.2×** on its synth example (30% of budget with cl6x, 65% without).
STRATA's own figures now say the same thing, and the decisive pair is the eight-track worst case:

| | cl6x | GCC only |
| --- | --- | --- |
| 8 tracks, cheap settings | **17.00%** | 28.38% |
| 8 tracks, **worst case** (drive, LP24, lo-fi, both sends, all eight tracks) | **23.61%** | **44.26%** |

**M2's exit bar is 40%. With cl6x there is comfortable room; GCC-only fails it.** That settles the
question the first version of this section left open: a GCC-only STRATA does not hit its targets.

### Redistribution: settled (2026-11-22)

This heading used to say whether TI's terms permit distributing what cl6x builds was still open.
It was examined and the premise was wrong: no TI code, headers or run-time library reach
`APP.ENA`. The reasoning and the one TI file still worth reading are in
**"The cl6x licence question, examined"** further down. What stands is availability: cl6x runs
only on the user's machine, so cloud figures are GCC-only. Keep recording load figures both ways
whenever cl6x is available.

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

### First boot on the unit (2026-11-22)

| Configuration | LOAD on screen | Where | Date |
| --- | --- | --- | --- |
| Default beat, Enaga 0.2.0 firmware | **about 0–26%** | **unit** | 2026-11-22 |
| Default beat, MAX build (#17), cl6x | top line never reached 30% | **unit** | 2026-11-22 |
| **Stress: long free play, "big effects, a lot of stuff"** | **MAX 84.3%** | **unit** | 2026-11-22 |

Read by eye, not logged. The first two rows are the top-line LOAD. The stress row is the held MAX
on the bottom line (#17) after a long session of free play on the controls-remap build (main
e1ff619, cl6x, VA fusion off). What was on at the peak is not known, so it cannot be split by
feature. It is well past the 60% target and past "Everything at once"'s ~63% unit estimate below,
but under 100%: a block at 100% would overrun. Roland heard no clicks or noise at any point in
that session. The clean A01 and A04 MAX figures (60 s each, MAX restarted by STOP then PLAY) and
the six-voice STRATA6 figure are still to be recorded here.

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

### The cl6x licence question, examined — and the premise was wrong

docs/SPEC.md's risk table says "TI cl6x licence for distributing built code" blocks "the headline
claim, not just a release date", docs/REVIEW.md rates it the project's **highest** risk, and this
document has carried it as "still needs a human's reading" since M2. Several sessions, including
one that repeated it a dozen times in an evening, treated it as established. **Nobody had checked
it, and the technical premise it rests on is false.**

What `third_party/enaga/tools/build_app.py` actually does with cl6x, read line by line:

| In the shipped `APP.ENA` | Where it comes from | Licence |
| --- | --- | --- |
| `dsp/*.ti.c` objects | STRATA's own source, compiled by cl6x | ours |
| `libenaga_dsp.a` | Enaga's `runtime/dsp/*.c`, compiled by cl6x | MIT |
| everything else | STRATA and Enaga, compiled by GCC | ours / MIT |
| `libgcc` | GNU | GPL + Runtime Library Exception |
| **TI headers** | **none** — all ten `.ti.c`/`dsp/*.c` files include only our own headers and `<enaga/dsp*.h>` | — |
| **TI run-time support library** | **none** — the link is GNU `ld` with `-nostdlib`, and `build_app.py` then asserts `nm -u` is empty | — |

So the only TI-originated content in the binary is **instruction sequences TI's code generator
chose from our source**. No TI code, no TI headers, no `rts6740_elf.lib`. That is what a compiler
is for, and compiler licences restrict redistributing the TOOLS, not their output.

**The inversion worth noticing:** the one third-party library actually linked into `APP.ENA` is
`libgcc`, and no document here lists it as a risk. Its GPL-with-Runtime-Library-Exception is what
permits shipping a binary that links it -- the exception exists for exactly this -- but it is a
real third-party licence in the artefact, where TI's contributes none.

**This is not a legal opinion and it is still unverified.** What is left is a ten-minute read of
a file that is already on the development machine: TI's installer ships its licence inside
`~/ti/ti-cgt-c6000_8.5.0.LTS` (a `LICENSE`, `*manifest*` or `license` directory). Read the grant
clause and look for (1) an explicit right to reproduce and distribute code the tools produce,
(2) whether that right is conditioned on running on a TI device -- the C6748 satisfies such a
condition -- and (3) any notice requirement attached to OUTPUT rather than to the tools. A cloud
session cannot do this: `ti.com` and `software-dl.ti.com` are outside its allowed network.

Until that read happens the honest status is **"unverified, and the reason it was thought to be a
blocker does not survive looking at the build"** -- not "blocked".

**Read 2026-11-22, on Roland's PC, for the v0.1.0 release.** The install has no `LICENSE` file or
directory. Its licence text is `C6000_Code_Generation_Tools_8.5.x_manifest.html` (manifest
SRAS00011890, 2024-08-11), and the RTS files are itemised in its `.spdx`. What it says:

- **The compiler** (`bin/`, TI and EDG) is under the "TI Text File" licence. Its conditions cover
  redistributing **that software**: binary redistribution only unmodified, no reverse engineering,
  use licensed only with TI devices, and the notice reproduced when the software itself is
  redistributed. It says nothing about code the compiler produces. STRATA redistributes none of
  the compiler.
- **The run-time library** (`lib/`, `include/`) carries the only clause about object code: object
  code compiled from TI's RTS **source** is licensed for use only with TI devices. STRATA links no
  RTS and includes no TI header (the table above). Even if it did, the C6748 is a TI device.
- **Boost** (inside the compiler) excludes "machine-executable object code generated by a source
  language processor" from its notice requirement. The LLVM/NCSA, HP and BSD texts apply to
  components STRATA does not ship.

(1) There is no explicit grant over the output, and no clause claims it. (2) The one TI-device
condition attaches to RTS-derived object code, which STRATA does not contain; the unit is a TI
device anyway. (3) No notice requirement attaches to output. No condition was found that stops
shipping cl6x-built `APP.ENA`. This is a reading, not a legal opinion; any clickwrap accepted at
install time is not in the installed tree and was not re-read.

### All seven scenarios with cl6x, and VA POLY per voice (2026-11-22)

The column every section below was missing. Run on the development machine (WSL, cl6x
8.5.0.LTS), `main` at `bf8e5a4`, emulator `smpltrek-re` at `4b0d711`, then again on the Enaga
v0.2.0 pin with the emulator at `11ac9a0`: **every figure below came out identical on both, to
the cycle, and the cl6x `APP.ENA` is byte-identical** (476,008 bytes, SHA-256 `353f8956…`). Host
suite 539,926 checks, 0 failures. `tools/build.sh` sends `chainloops`, `drumloops`, `grainloops`,
`masterloops`, `repeatloops` and `valoops` (`*.ti.c`) and the DSP library through cl6x.

| Scenario | cl6x | GCC only | ratio |
| --- | --- | --- | --- |
| `scenario_m0.sh` — init, nothing playing | **2.54%** | 2.87% | 1.13x |
| `scenario_m1.sh` — the beat | **23.49%** | 39.60% | 1.69x |
| `scenario_m3.sh` — the default beat | **19.17%** | 31.92% | 1.67x |
| `scenario_m4.sh` — + stop, queue, save | **16.94%** | 27.16% | 1.60x |
| `scenario_m5.sh` — + PERFORM, scenes, macros | **19.37%** | 34.54% | 1.78x |
| `scenario_m6.sh` — + KEYS, three keys recorded | **21.28%** | 36.61% | 1.72x |
| `scenario_m7.sh` — + every MIXER page and SETUP | **23.49%** | 39.60% | 1.69x |

0 overruns in every run. **Worst case 23.49% with cl6x, against the 60% target.**

**VA POLY per voice**, by the method of the sections below (swap the stab's chord type in
`build_default_beat` (now `groove` in core/factory.cpp), re-run `scenario_m3.sh`; the bass adds one more `VaVoice`):

| Chord on the stab | VaVoices | cl6x | GCC only |
| --- | --- | --- | --- |
| none (a single note) | 2 | 12.81% | 20.63% |
| `5TH` | 3 | 14.87% | 24.30% |
| `MIN7` — the default kit | 5 | **19.17%** | 31.92% |
| `MIN11` — all six voices | 7 | **23.48%** | 39.55% |

**2.16 points per voice with cl6x** (MIN7 → MIN11; 2.06 from none → 5TH), against **3.82**
GCC-only, so cl6x is worth **1.77x on the voice** — less than the 2.2x Enaga measured on its own
synth, more than enough. Six VA POLY voices are therefore ~13.0% of the block on the C674x
model, which is still ~5.7x the 11,208 cycles `docs/SPEC.md` budgets for the whole machine:
explanation 1 of "VA POLY costs 4.25 points per voice" below is now ruled out as the whole
story (cl6x is not worth an order of magnitude here), and explanation 2 — SPEC budgeted a cheaper
voice than the one that was built — is what is left. The whole machine still fits: the worst
scenario is under 24%, so **M6's budget exit stands on the emulator**, and only the unit, with
its caches, can confirm it.

### M4, M5 and M6 — measured in the emulator, GCC-only (2026-11-21)

A cloud session built the tic6x toolchain from source and ran the emulator, so these are real
numbers rather than estimates. **What is still missing is cl6x**: TI's installer needs an
interactive licence click, so every figure here is the GCC-only build and the ~2.2x this document
records for cl6x has NOT been applied. A GCC-only number near the target is therefore not a
failure, and one comfortably under it is not yet a pass on the unit.

Emulator caveats that apply to every row: no cache model, and **peak depends on run length** --
the same build reads 33.07% over 1,257 blocks and 28.0% over 9,362 -- and this time the LONGER
run reads lower, because the 1,257-block run's window happens to catch the chord. Compare
versions, not targets.

| Scenario | Blocks | Peak (GCC-only) | Overruns |
| --- | --- | --- | --- |
| `scenario_m0.sh` — init, nothing playing | 653 | **2.44%** | 0 |
| `scenario_m1.sh` — the beat, M1's own tripwire | 1,257 | **41.26%** | 0 |
| `scenario_m3.sh` — the default beat, M6 build | 1,257 | **33.56%** | 0 |
| `scenario_m4.sh` — + stop, queue A02, save | 1,661 | **28.86%** | 0 |
| `scenario_m5.sh` — + PERFORM, scene capture, macro assign | 1,258 | **35.86%** | 0 |
| `scenario_m6.sh` — + KEYS, three keys recorded | 1,661 | **38.25%** | 0 |
| `tools/render.sh --seconds 8` — 8.2 s continuous | 6,338 | **41.1%** | 0 |

**m1 and m3 play the same beat and read 41.26% and 33.56%**, which is the run-length effect this
section opens with and not a contradiction: m1's window happens to catch the block where the
chord, the open hat and the sampler overlap. Compare a scenario against itself across builds,
never one against another.

**The sidechain duck costs +0.32 points**, and it was free before only because it did nothing.
`Engine::init` set every track's duck depth with `t == duck_source_ ? 0.0f : 0.0f` -- both arms
zero -- so docs/SPEC.md's M2 duck was dead code while `trigger_duck` still fired on every kick
and `begin_block` still rendered the curve. The cost of turning it on is seven per-sample
multiplies, which is what this figure is, and the rendered mix loses 0.23 dB of RMS: a duck doing
its job. The DEPTH (0.35) is a judgement and not a measurement -- SPEC gives the duck's attack,
hold and release and no depth -- so it wants settling by ear.

**The baseline moved, twice, and both moves are explained below.** The previous build of the same
scenarios read 22.72 / 22.72 / 23.51 / 30.64. `Step::length` becoming audible took 2.4 points
OFF, and registering VA POLY on the CHORD track with a four-note chord put 12.7 ON.

#### GRAIN: written, verified, unmeasured, and unreachable

`dsp/grain` is in the tree and host-tested, and it has **no track**. docs/SPEC.md's default kit
fills all eight with other machines, so GRAIN cannot sound until a track's machine is switchable,
which is not written. `machines_` therefore never points at it, `--gc-sections` drops its code,
and APP.ENA is the same size with it in the tree as without.

That means a plain build proves only that it COMPILES for the C674x. To prove it LINKS -- the
hazard that matters on this toolchain, where a 64-bit divide pulls in libgcc's ARM unwinder and
the whole app stops linking -- every entry point was forced into the link by taking a `Machine *`
to it, which keeps the vtable and so keeps every virtual:

| | APP.ENA | bss |
| --- | --- | --- |
| GRAIN in the tree, unreferenced | 422,600 | 1,265,512 |
| GRAIN forced into the link | **439,568** | 1,267,600 |

So GRAIN is 16,968 bytes of code and 2,088 bytes of state, it has no undefined symbols
(`build_app.py` refuses them), and it allocates nothing at all -- not even in `init()`, because it
reads PCM the track already holds.

#### GRAIN measured: +1.6 points at mid density, +9.2 at maximum overlap

Measured the same way VA POLY was -- GRAIN put in the Sampler's slot on track 8 of the default
kit, fed the placeholder sample, `scenario_m3.sh` re-run. Same build, same beat, GCC-only:

| Track 8 | `scenario_m3.sh` peak | Over a silent track |
| --- | --- | --- |
| silent (`machines_[7] = nullptr`) | 32.33% | — |
| the Sampler, as shipped | 33.39% | +1.06 |
| GRAIN, DEN 0 (lowest overlap) | 33.91% | **+1.58** |
| GRAIN, DEN 64 (the default kit's knob) | 33.91% | **+1.58** |
| GRAIN, DEN 127 (maximum overlap, the full pool) | 41.56% | **+9.23** |

So GRAIN is about 1.5x the Sampler at the density a kit ships with, and **+9.23 points -- about
45,800 cycles -- with all eight grains in the air**. docs/SPEC.md budgets 21,520 cycles (4.3%),
so the worst case is 2.1x over, GCC-only. That is a far better position than VA POLY's 10.1x, and
`dsp/grain.hpp`'s derived estimate (~9,000-11,000 cycles for eight grains) was 4.2x optimistic --
wrong in the same direction as VA POLY's, by less.

DEN 0 and DEN 64 reading the same figure is not a mistake: at both, few enough grains overlap
that the peak block holds the same number of them.

**This figure is a FLOOR, not a worst case, and the reason is the one thing the estimate could
not bound.** The placeholder sample is 400 ms of mono int16 -- 38 KiB, which sits in cache
entirely -- so eight grains reading it are eight streams over one resident buffer. The open
question was always the cache behaviour of eight concurrent reads into a MULTI-SECOND sample with
heavy SPR, where every grain starts at a fresh random offset and a sprayed cloud may be almost
all L1D misses. The emulator has no cache model, so the emulator cannot answer that at any
density, with any sample. Only the unit can.

**The rest of the estimate should still not be trusted.** The derivation in
`dsp/grain.hpp` counts ~19 flops and 5 integer ops per sample per grain and scales off Enaga's
two published figures to land at ~1,000-1,300 cycles per grain, so ~9,000-11,000 for eight
(1.8-2.2% of the block) against SPEC's 21,520. The measurement above puts the real figure at
~5,700 cycles per grain on a cache-resident sample, so the per-grain derivation is out by about
four. For VA POLY the same kind of derivation was out by ten. **Two machines, two derived
estimates, both optimistic** -- which is the argument for measuring the next one before
designing around its cost rather than after.

The part of GRAIN's estimate that could not be bounded at all is the cache behaviour of eight
concurrent read streams. Under heavy SPR each grain starts at a fresh random offset, so a sprayed
cloud may be almost entirely L1D misses -- and the emulator has no cache model, so the emulator
cannot answer it either. Only the unit can.

#### VA POLY costs 4.25 points per voice, and that is the figure to worry about

> Superseded in part: it is **3.82** since the optimisation loop two subsections below, which
> keeps the method and the reasoning here and replaces only the number. Everything about where
> the cycles are, and about SPEC's figure, still holds.

The default kit's CHORD track plays a MIN7 -- four notes -- so this is measurable by changing one
chord type in `build_default_beat` and re-running `scenario_m3.sh`. Four runs, same build, same
beat, GCC-only:

| Chord on the stab | Voices sounding | `scenario_m3.sh` peak | Over one voice |
| --- | --- | --- | --- |
| none (a single note) | 1 | 20.32% | — |
| `5TH` | 2 | 24.48% | +4.16 |
| `MIN7` — **what the default kit now plays** | 4 | **33.07%** | +12.75 |
| `MIN11` | 6 | 41.67% | +21.35 |

Linear at **4.25 points per voice**, which is about **21,100 cycles per voice per block**. Six
voices is therefore ~127,000 cycles, or **25.6% of the block on its own**.

docs/SPEC.md budgets **11,208 cycles for VA POLY entire**. The GCC-only measurement is **11.3x
that**, and even at the 2.2x this document records for cl6x it would be ~5x. One of three things
is true and the unit decides which:

1. **cl6x is worth much more than 2.2x on this code.** Plausible: `dsp/va.cpp` is C++ rather than
   a `.ti.c` loop, it is the branchiest hot code in the project, and 2.2x was measured on the
   drum loops, which are already branch-free. This is the optimistic case and it is untested.
2. **SPEC's figure was for a cheaper voice than the one that was built.** Each VA POLY voice is a
   whole `VaVoice`: saw, synced pulse, sub triangle, an SVF and two envelopes, the same voice VA
   MONO uses. Six of those is six VA MONOs. SPEC's 11,208 is close to **two** of them.
3. **The emulator is wrong about this code in particular.** It has no cache model, and a VA voice
   is almost pure arithmetic on registers -- the case an instruction-counting emulator should
   model BEST, not worst. So this is the least likely of the three.

CLAUDE.md's rule of thumb -- "all 18 voices together are about 11% of the block" -- is now known
to be wrong for the melodic machines, and by a wide margin. It holds for the drums, which are
short one-shots that go idle within a step; it does not hold for a sustaining voice. **A six-voice
chord is not a corner case** -- MIN11 is in the table precisely so all six can be used.

**Where the cycles are: 18% control-rate, 82% per-sample.** Measured, not argued, by sweeping
`VaVoice::kControl` on the MIN7 beat -- 64 frames a block, so 16/32/64 is 4/2/1 control steps:

| `kControl` | control steps per block | `scenario_m3.sh` peak | saved |
| --- | --- | --- | --- |
| 16 (shipped) | 4 | 33.07% | — |
| 32 | 2 | 31.12% | 1.95 |
| 64 | 1 | 30.17% | 2.90 |

Halving the step count saves 1.95 and halving it again saves 0.95, which is a **fixed ~0.98
points per control step** across the five VaVoices sounding (the bass plus four chord voices), so
~0.78 of each voice's 4.25 points. The rest is the per-sample loop.

That settles which lever is worth pulling, and it does NOT depend on cl6x or on the unit:

- **Raising `kControl` is not it.** Going to 64 is a 4x coarser envelope and filter update -- an
  audible cost -- for 2.9 points of 33. Reverted; the shipped value stays 16.
- **The per-sample loop is where the money is**, so the real option is the **buffered maths**
  `dsp/vapoly.cpp`'s header names: one oscillator pass over a whole slice for all six voices
  instead of six passes over one. That is a rewrite of the voice's inner loop, which is why it
  should wait for a figure from the unit rather than from an emulator with no cache model.
- **Dropping six voices to four** is the blunt option and is worth 8.5 points by this table.

One caveat on the split: it is GCC-only. cl6x pipelines a branch-free per-sample loop far better
than it does the branchy control-rate code, so on the unit the per-sample share should be SMALLER
than 82% -- which would make the control-rate term relatively more important, not less. That is
reasoning, not measurement, and it is one more thing the unit decides.

#### The optimisation loop: 4.30 -> 3.82 points per voice, bit-exact

Two changes, each measured on its own, same build and same beat as the table above, GCC-only.
CLAUDE.md asks for figures with AND without cl6x after a DSP change; the without-cl6x column is
the only one this environment can produce, because cl6x is not installed here (`tools/build.sh`
says so: "built with GCC only"). Both changes move work that cl6x compiles BETTER than GCC does
-- a branch-free fused loop, and a control step with two fewer transcendentals -- so the cl6x
column should show at least these savings and probably more. That is reasoning, and it is owed a
run on the machine that has the toolchain.
The method is that table's: change one chord type in `build_default_beat` and re-run
`scenario_m3.sh`, which gives a 5-voice reading and a 7-voice one (the chord plus the bass, both
on a `VaVoice`), and therefore a per-voice figure rather than a total to argue about.

| Build | MIN7, 5 VaVoices | MIN11, 7 VaVoices | per voice |
| --- | --- | --- | --- |
| before | 33.56% | 42.16% | **4.30** |
| + the fused output tail | 32.62% | 40.75% | 4.07 |
| + shared filter coefficients | **31.92%** | **39.55%** | **3.82** |

**11.3% off a voice**, and both changes are BIT-EXACT: every expectation in tests/test_va.cpp
and tests/test_vapoly.cpp held unchanged, and each change has a test that pins the equality
rather than a tolerance.

Across the whole suite, which is the figure to quote because it is what a scenario actually
reads -- every one of the six that has a VA voice in it came down by the same ~1.6 points:

| Scenario | before | after |
| --- | --- | --- |
| `scenario_m0.sh` — init, nothing playing | 2.87% | 2.87% |
| `scenario_m1.sh` — the beat | 41.26% | **39.60%** |
| `scenario_m3.sh` — the default beat | 33.56% | **31.92%** |
| `scenario_m4.sh` — + stop, queue, save | 28.81% | **27.15%** |
| `scenario_m5.sh` — + PERFORM, scenes, macros | 35.92% | **34.54%** |
| `scenario_m6.sh` — + KEYS, three keys recorded | 38.25% | **36.61%** |
| `scenario_m7.sh` — + every MIXER page and SETUP | 41.26% | **39.60%** |

m0 is unchanged to the cycle, which is the control: it plays nothing, so it has no VA voice to
make cheaper, and an unchanged number there says the difference is the voices and not the build.

The AVERAGE saving per voice is smaller than the marginal 0.48, and both numbers are honest. A
scenario's five VaVoices save 1.64 between them, 0.33 each: the bass is a `VaMono` with one
voice, so it passes no cache and gets only the fused tail, and the FIRST chord voice of each
slice is the one that computes the coefficients the others read. The marginal voice -- the sixth
of a chord, the one that decides whether a part fits -- saves the full 0.48.

**1. The fused output tail: 0.19 points per voice.** `dsp/valoops.ti.c`'s `strata_va_tail` does
in one pass what was `enaga_soft_clip_buffer`, then `enaga_multiply`, then a `mix_ramp` written
out in va.cpp -- three passes over the slice with the 16-float slice buffer written and read
back twice between them. dsp/va.cpp's own header had named this extraction since M2 and it is
now the only *.ti.c file the melodic machines have; everything else in the slice is an Enaga
block function and so already goes through cl6x inside Enaga's library.

The arithmetic is unchanged down to the associations -- `gain * (clipped * env[i])`, because
`enaga_multiply` formed that product before the ramp multiplied by the gain and float
multiplication is not associative. `va_the_fused_tail_is_bit_exact_against_the_three_passes_it_replaced`
runs the three Enaga functions and the fused kernel over the same data and compares with `==`.

**2. Shared filter coefficients: 0.24 points per sharing voice.** The voice's filter has no key
tracking, which dsp/va.cpp argues for musically -- "a bass line wants the same squelch at every
note of a pattern". So `g` and `k` depend on the settings, the filter envelope's value and the
accent, and NOT on the note: the voices of a chord, gated on the same frame with the same
shapes, run bit-identical filter envelopes and all six were computing the same `semitones` and
the same `tan_pi`, the two most expensive things in the control step. `VaFilterCache` (eight
entries, a linear scan, keyed on the envelope value by exact float equality) lets the first
voice of a slice compute them and the rest read them back.

0.70 points at four chord voices and 1.20 at six, so 0.23 and 0.24 per voice that shares -- a
consistent **~31% of the 0.78-point control step**, which is about what is left after the
per-voice `semitones(pitch - 69)` and the oscillators' two reciprocals, the parts that cannot be
shared because they depend on the note.

Three things were examined and NOT done, with the reasons, so they are not re-examined from
scratch:

- **Skipping oscillator passes at zero level** cannot help: the VA voice's mix is FIXED in
  dsp/va.cpp (`kLevelOsc1` 0.60, `kLevelOsc2` 0.14, `kLevelSub` 0.40, `kLevelNoise` 0.016), not
  user-settable, so no pass is ever at zero and a guard would be dead weight in the hot path.
- **Writing the noise instead of clearing and then adding it** would save one 1-store-per-sample
  pass of the eight, and would change the order of the oscillator sum -- float addition is not
  associative, so the voice would no longer be bit-exact. A ulp of drift for 1% of a pass is the
  wrong trade.
- **The "buffered maths" the previous section proposed -- one oscillator pass for all six voices
  -- does not work**, and it is worth writing down why rather than leaving it as a standing idea.
  Each voice has its own filter state, its own two envelopes and its own noise stream, so the
  only thing that could be shared is a sum taken BEFORE the filter -- which is a different
  instrument, not an optimisation. The voices are necessarily rendered one at a time.

What is left in the per-sample loop is Enaga's: the PolyBLEP saw, the synced PolyBLEP pulse, the
sub triangle, the noise, one or two SVF stages and the amplitude envelope. None of it is
STRATA's to rewrite (CLAUDE.md: never modify third_party/enaga), and all of it already compiles
through cl6x in Enaga's own library. **So the remaining lever really is cl6x's 2.2x and the
unit, which is where this section's predecessor left it.** Six voices now cost 22.9% of the
block GCC-only rather than 25.8%; against docs/SPEC.md's 11,208 cycles for the whole machine
that is 10.1x rather than 11.3x, and the three explanations in the previous section stand.

**`Trig::gate_frames` costs +0.06 points** and nothing more. The previous build of the same four
scenarios read 22.66 / 22.66 / 23.48 / 30.58, so converting `Step::length` to frames in
`Engine::fire` -- three multiplies and a table read per trig, at control rate -- is a rounding
error, and VA POLY's per-voice countdown costs nothing at all here because VA POLY is not yet
registered on a track. The countdown's real cost arrives with that registration: it adds one
compare per voice per control slice, and a slice cut on the gate's last frame.

The gate is also the first thing in STRATA to make a slice shorter than `VaVoice::kControl` on
purpose. That matters for the budget rather than the sound: the per-slice control work is paid
per slice, so a very short gate pays it more often. It is bounded -- one extra slice per note,
because the countdown stops at the gate -- but `dsp/vapoly.cpp` carries a `!s.pending` guard
whose only job is to keep a WAITING trig's short gate from re-slicing the note it is replacing,
which would not be bounded. Measured: re-slicing a started voice is bit-transparent, so that
guard is a cost guard and nothing else.

**Beat repeat's always-on cost: under 0.1 points now, and it was +0.82 when it was measured.**
Both numbers are right: the tax is a constant per block -- one 64-frame stereo copy and a
128-parameter compare -- and the peak block is now the chord's rather than an idle one, so the
same constant is a smaller share of a larger peak. `scenario_m5.sh` carries the m3 baseline and
fails if the gap ever exceeds five points, which is the check that matters; the original +0.82 is
the honest figure for the tax itself. That is the one DSP figure M5 owed. It covers `dsp/repeat`'s ring copy of every block plus the per-block performance pass,
and it is the shape expected of one 64-frame stereo copy and a 128-parameter compare.
`tests/scenario_m5.sh` carries the baseline and fails loudly if the gap ever exceeds five points.

**M6's 37.61% is 4.5 points above the same build's default beat**, and the whole of it is KEYS:
the scenario plays three keys into the pattern, so more tracks sound than `scenario_m3.sh` has.
GRAIN is still not registered, and it is the costliest voice in docs/SPEC.md's table -- so the
figure above is not the worst case, and the per-voice number in the table is the reason to get
a real measurement before adding it to a track.

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

### Send tails held to -60 dB (2026-11-22, not measured)

`Mixer::resolve_sends` used to stop the delay after at most 8 s and the reverb after 12 s unfed.
They now run until the tail is 60 dB down (up to 272 s for feedback 0.95 on a 2 s delay), because
the cut was audible and the frozen buffer replayed later. **The worst case is unchanged**: every
figure above already has both sends running. What changes is how long the sends' fixed cost lasts
after the last hit with high feedback, which matters for battery and for any headroom a quiet
passage was relying on. Not re-measured: this cloud session has no emulator and no cl6x. Run the
load scenario with feedback at 0.95 to confirm.

**Measured 2026-11-22 (WSL, cl6x).** The default kit has every SNA closed, so the stock scenarios
never feed the delay and read the same at any feedback. With SNA opened to full on every track
(a test patch in `build_default_beat`, not committed): `scenario_m3.sh` 19.83% at feedback 0.40
and 19.83% at 0.95; a 32 s `tools/render.sh` peaks at 120,852 cycles (24.4%) at 0.40 and 121,186
(24.4%) at 0.95, 0 overruns in 24,282 blocks, RMS -14.8 and -7.2 dBFS. The longer tail costs
nothing at the peak, as argued above.

### Factory content (2026-11-22, not measured)

core/factory.hpp replaced the hand-built default beat. Two things it does cost load, and neither
has been measured, because this cloud session has no emulator and no cl6x:

- **The sends are open on the default kit.** Chord, rim, blips, clap and the open hat feed the
  delay and the reverb, where the old default kept every send closed (the "cheap settings" row
  above, 17.00% cl6x). The "worst case" row already has both sends running on every track, so
  the sends add nothing that row did not already pay for; they move the DEFAULT beat from the
  cheap row towards it.
- **A03 BREAK lets chord voices overlap.** Its two chords decay over about 0.7 s and start
  0.92 s apart with four notes each, so VA POLY can hold all six voices -- the `MIN11` row above
  (23.48% cl6x, 39.55% GCC-only, measured with the sends closed). Six is the machine's ceiling,
  so no factory pattern can go past it.

Estimate for the heaviest factory pattern (A03 or A04): the six-voice row plus the sends, about
**27-29% cl6x and 44-48% GCC-only**, inside 60% either way. `tests/scenario_factory.sh` plays A01
and then A04 and records the peak; run it, and `scenario_m3.sh` for the new default beat, with
and without cl6x, and put the figures here.

### KEYS note release (2026-11-22, not measured)

`Machine::release(note)` was added to VA, VA POLY and SAMPLE for KEYS mode's key-up. It runs once
per key release, in the command handler, and touches no render loop: the only per-voice addition
is a flag test when a voice STARTS. No figure above changes. Not re-measured, because this cloud
session has no emulator and no cl6x; the next load scenario run on Roland's PC covers it.

### The arpeggiator (2026-11-22, not measured)

The arp (core/arp.hpp, the scheduling in app/engine.cpp) adds no per-sample work: a hit is an
ordinary `trigger`, as a step's is. What it adds is control rate: one `ArpSettings::make` (four
multiplies and shifts) per step fired, and per clock tick a check of two runs on each of eight
tracks, plus a re-read of the four bytes for a held KEYS run. With every arp off that is sixteen
flag tests a tick, about 0.5 ticks a block at 120 BPM. Off is bit-exact: zeroed bytes are OFF and
every golden render is unchanged.

A running arp costs what its notes cost. A 1/32 arp on a one-step bass note is two triggers where
there was one, so the voice load is the machine's, at the higher note rate: on VA POLY each hit
takes a voice from the shared budget of 12 (`Engine::kPolyVoiceBudget`), which is the ceiling
whatever the arp does. Not re-measured, because this cloud session has no emulator and no cl6x;
`tests/scenario_lfo.sh` now also walks to the ARP page, and U7/U8-style figures for a running
arp come from 13h on the unit.

### The master kill EQ (2026-11-22, host only)

A three-band DJ isolator on the master, after the DJ filter (`Mixer::kill_eq`, loop in
`dsp/masterloops.ti.c`, so it goes through cl6x). Two Linkwitz-Riley crossovers per channel
(250 Hz and 2.5 kHz, two Butterworth SVF stages a side) plus an all-pass that keeps LOW in phase
with the rest: **7 SVF steps per channel per sample**, 14 a sample for the stereo bus, with the
three gains and the fade ramped across the block. **It costs nothing while bypassed**: with all
three bands flat and no kill switch on, `kill_eq` returns after three compares, and the output is
the input to the bit (`tests/test_killeq.cpp`). It starts on the first knob move or switch and
stops again about 30 blocks after everything is back to flat (the gains glide, then a four-block
fade). So it is not a fixed tax: it is paid while a performer is using it.

Measured in a cloud session (x86 2.3 GHz, GCC -O2), so these are proxies, not unit figures:

| Measure | Host time per 64-frame block | Share |
| --- | --- | --- |
| The kill EQ's loop, stereo, running | 1,683 ns | |
| The DJ filter (two SVF cascades), for comparison | 475 ns | the EQ is 3.5x the DJ filter |
| A01 GROOVE, whole engine, EQ bypassed (`host/render.cpp`'s loop) | ~23,900 ns | |
| A01 GROOVE with LOW killed, whole engine | ~25,250 ns | +5.5% (+1,300 ns) |
| A04 PEAK with LOW killed, whole engine | ~28,400 ns against ~25,000 | +6-13%, noisy |

Scaled by the default beat's emulator figures (A01 is `scenario_m3.sh`'s beat: 19.17% cl6x,
31.92% GCC-only), the loop's 7% of the host block is an **estimate of about 1.3 points with
cl6x and 2.2 GCC-only**, against the ranked-ideas list's guess of 0.5-1 point. The worst case
then moves from 23.49% to about **24.8% cl6x** (39.60% to about 41.8% GCC-only) with the EQ
running, inside 60% either way. The loop is a chain of dependent SVF steps, which is exactly the
shape cl6x's software pipelining overlaps across samples and GCC does not, so the cl6x figure
could come in well under the proxy; only the emulator says.

If it is too dear on the unit, in order: drop the LOW all-pass (6 SVFs, the sum ripples a
fraction of a dB around 250 Hz), then 12 dB crossovers (3 SVFs, but the MID kill falls to about
-15 dB, which is no longer a kill).

To measure on Roland's PC: **E6** below (`tests/scenario_killeq.sh`, which kills all three bands
on the EQ page and reads the peak, against `scenario_m7.sh` with the EQ bypassed), both compilers.
On the unit: **U6**.

### Tape stop and delay freeze (2026-11-22, host only)

Both are held moves and **cost nothing until pressed**: the tape stop records only while it runs
(the read never needs a frame from before the press), and the freeze records forward from the
press rather than keeping a history. Neither touches the default path, so every render that
never presses them is unchanged to the bit (the goldens pass untouched). Loops in
`dsp/stoploops.ti.c`, so both go through cl6x.

| Measure (x86 2.3 GHz, GCC -O2, per 64-frame block) | Host time |
| --- | --- |
| Tape stop, running (write, interpolated read, level, crossfade; stereo) | ~220 ns |
| Delay freeze, looping (stereo read and crossfade) | ~115 ns |
| Delay freeze, recording (stereo copy) | ~27 ns |
| The master peak detector, for scale | ~57 ns |

Against A01's ~23.9 us host block that is about 0.9% for the tape stop and 0.5% for the freeze
(plus a clear and two mixes for routing the delay through its wet planes while frozen), so an
**estimate of about 0.2 points cl6x / 0.3 GCC-only for the tape stop and 0.1 / 0.2 for the
freeze**, and only while held. The freeze also keeps the delay running while held, which the
worst case already counts (both sends running). No emulator scenario: both are held key
combinations, and the emulator's `--key` only taps. Unit check: **U8** (UNIT_CHECKLIST 13g).

Memory: the freeze's loop is 1.46 MiB (4 s of stereo float: two 2 s delay times) and the tape
stop's ring 0.46 MiB (1.25 s), both taken in init. `tests/test_memory.cpp` itemises them: init
is about 104.1 MiB of the 120 MiB heap with the stream rings from main, 15.9 MiB spare.

### The performance mixer and the looper (2026-11-23, host only)

Push VALUE + STEP picks one of fifteen effects and VALUE turns its amount; STEP 16 is a master
looper (`core/perffx.hpp`, `dsp/masterfx.hpp`, `dsp/looper.hpp`). **One effect plays at a time**,
which is what bounds the cost: the worst case is the dearest single slot plus the looper, not
the sum of sixteen. At amount 0 nothing runs and the mixer gets the project's own settings, so
every render that never turns VALUE is unchanged to the bit (the goldens pass untouched). New
loops in `dsp/masterfxloops.ti.c` and `dsp/looperloops.ti.c`, so both go through cl6x.

Slots 1-9 are overrides on stages that already exist and are already costed above: the DJ
filter (LP/HP SWEEP, HP RISER, BUILD), the sends (every track's send raised; the worst case
already runs both sends on all eight tracks), the delay freeze, beat repeat, the tape stop and
the kill EQ (BASS KILL starts it: about 1.3 points cl6x / 2.2 GCC-only, the estimate in "The
master kill EQ"). Slots 10-14 and the looper are new code:

| Measure (x86 2.1 GHz, GCC -O2, per 64-frame block) | Host time | Estimate cl6x / GCC-only |
| --- | --- | --- |
| Idle (no effect up): one check | ~5 ns | 0 |
| PUMP (gain from the beat phase, stereo) | ~170 ns | 0.14 / 0.23 points |
| GATE (same loop) | ~170 ns | 0.14 / 0.23 |
| CRUSH (sample-and-hold, quantize) | ~90 ns | 0.07 / 0.12 |
| NOISE RISER (xorshift, one-pole HP, add) | ~210 ns | 0.17 / 0.28 |
| FLANGER (two interpolated taps, feedback, 8 KiB ring) | ~340 ns | 0.27 / 0.45 |
| Looper recording (float to int16, stereo) | ~135 ns | 0.11 / 0.18 |
| Looper playing (int16 read, crossfade, stereo) | ~850 ns | 0.7 / 1.1 |

Scaled as the kill EQ's and the tape stop's were, against A01's ~23.9 us host block (19.17% cl6x,
31.92% GCC-only on the emulator). The looper's play figure is high for what it does because the
benchmark's 400 KiB loop streams through the host's caches; on the unit it is a sequential DDR
read of 256 bytes a block, which is the cheap case for this DDR (sequential, as a frozen track's). **Worst case with
the mixer: BASS KILL (~1.3) or BUILD (DJ filter + noise, ~0.6) plus a playing loop (~0.7), about
2 points cl6x / 3.4 GCC-only**, so about 25.5% against the 60% target from the 23.49% cl6x worst
case. The CPU guard needs no change: it sheds POLY voices and grains, and this cost is bounded.

Memory: none new on the heap. The looper records into RESAMPLE's 5.76 MiB capture buffer (the
engine keeps the two apart), and the flanger's 8 KiB ring is a member of the engine.

No emulator scenario: every gesture is push VALUE held with another key, and the emulator's
`--key` only taps (as for the tape stop). To measure on the unit: **U11** in
`docs/UNIT_CHECKLIST.md` 13o (MAX with BUILD and a playing loop on A01, both compilers if the
GCC card is built).

### The sends' ambient modes: LONG delay and HALL reverb (2026-11-22, host only)

A second mode on each send (Roland's choice over a third send; `dsp/ambient.hpp`): send A's LONG
delay and send B's HALL reverb, loops in `dsp/ambientloops.ti.c`, so both go through cl6x. The
classic modes stay the default and their path is untouched: every golden render passes unchanged.
Each mode runs only while it has a tail, as the classic ones do, and a mode switched away from
rings out unfed, so for a few seconds after a switch **both** of a send's modes run.

| Measure (x86 2.3 GHz, GCC -O2, per 64-frame block, steady state) | Host time |
| --- | --- |
| Classic delay (Enaga StereoDelay, ping-pong) | ~305 ns |
| Classic reverb (Enaga Reverb) | ~1,600 ns |
| LONG delay (12 s line, wow, four all-passes a side, low cut, tone) | ~1,400 ns |
| HALL reverb (eight swept lines, four all-passes, shimmer) | ~2,500 ns |

Classic sends together ~1.9 us, ambient ~3.9 us: about twice the send cost. Against A01's ~23.9
us host block, and with the tape stop's proxy (0.9% of the host block ~ 0.2 points cl6x, 0.3
GCC-only), that is an **estimate of +1.9 points cl6x / +2.8 GCC-only** with both ambient modes
on, and **+3.6 / +5.3** for the few seconds after switching while the classic tails still ring.
On the worst case above (23.61% cl6x, 44.26% GCC-only, both sends running) that is about 27% and
50% at the very worst, inside the 60% target. The cl6x share is the least certain number here:
the per-sample feedback loops carry a dependency from frame to frame, which software pipelining
cannot hide as well as it does in the voices' loops.

Emulator: **E11**, `tests/scenario_ambient.sh` (push A on DLY+ and REV+ switches both modes on the
default beat, whose chord and clap feed both sends), against `scenario_m3.sh`, with cl6x and
`ENAGA_TI_CGT=none`. Unit: **U9** (UNIT_CHECKLIST 13j). First measured on Roland's PC on
2026-11-23 (main 5120347 plus the re-spaced key presses; before that fix the walk never reached
DLY+ and REV+ and the scenario's figure was the plain beat).

| Scenario | cl6x | GCC-only |
| --- | --- | --- |
| `scenario_m3.sh` (classic) | 25.42% | not measured |
| `scenario_ambient.sh` (LONG + HALL, both tails at first) | **31.79%** | **58.94%** |

**+6.4 points with cl6x, against the +1.9 estimate above**, and GCC-only is 1.06 points under the
60% line: the heaviest scenario now, by a margin. The estimate took the host's ratio of the
modes' cost to the voices'; the emulator says the feedback loops are much worse on the C674x
than on the host. Still inside the target with cl6x, which is what ships; U9 on the unit decides
whether the cuts below are needed.

If the unit's MAX says it does not fit, cut in this order (CLAUDE.md: effects before voices): the
HALL's shimmer (a two-tap read and a ring write a frame), its modulation (a phasor and the
fractional reads), then the LONG delay's diffusion to two all-passes a side.

### Per-track LFO (2026-11-22, host only)

One LFO per track, parameters 16-19 (dsp/lfo.hpp). It runs once per **block**, not per sample,
and moves its destination's byte. The cost is the LFO step itself (a few dozen instructions)
plus one `prepare` of the modulated track in each block where that byte actually changed. There
is no per-sample work, so the `*.ti.c` loops are untouched and nothing here needs cl6x. The
extra code is control-rate C++, which the cl6x build also compiles with GCC, so the absolute
cost should be about the same with both compilers.

Host figures: callgrind, x86, GCC -O2, the factory project, 6,000 blocks, init (~15.1 M Ir)
subtracted. The bench is a throwaway driver of `Engine`, not a test.

| Case | Change in Ir | Share of the block's work |
| --- | --- | --- |
| Every LFO off, against `main` (PEAK, slot 3) | +2.6 M on 735.6 M | +0.35%, about 430 instructions a block for 8 compares and the copy |
| 8 LFOs on a machine parameter (SRC 2), slot 3 | +15.5 M | +2.1% |
| 8 LFOs on DRV, slot 3 | +24.7 M | +3.4% |
| 8 LFOs, synced 1/16, on DRV, slot 3 | +24.7 M | +3.4% |
| 8 LFOs on PAN, slot 3 | +16.4 M | +2.3% |
| 8 LFOs on DRV, slots 0, 1, 2 | | +3.2%, +3.3%, +3.0% |

- **Off is bit-exact.** With every LFO off the render matches `main` bit for bit (energy
  20167.792 on both), and `tests/test_lfo.cpp` holds that with two engines side by side. Zeroed
  bytes are OFF, so no golden render changes.
- **On costs about 515 instructions per LFO per block at worst**: the chain's DRV re-prepare is
  the dearest destination. In load points that is roughly 0.15-0.3 points each, and **1.1 to 1.4
  points for all eight** at the GCC-only emulator's scale. With cl6x the worst case is about 23.5%
  plus about 2.5 points (the cost is control-rate code, so it does not shrink with cl6x and
  grows a little as a share of the block). That is well under 60%.
- A slow, deep LFO moves its byte less often than once a block, so the figures above (a fast free
  rate and a 1/16) are the expensive end.
- E7 and U7 below measure it for real.

### Load time: the downsample low-pass (2026-11-22, host only)

A file above 48 kHz is now low-passed before the load-time resample (`Antialias` in core/pcm:
8th-order Butterworth at 20 kHz, four float biquads per channel). Before, a 96 kHz file's 34 kHz
came back at 14 kHz at full level; now it is more than 30 dB down (tests/test_pcm.cpp). **This is
not an audio-block cost**: it runs in the main loop's convert slices, once per file, and only for
files above 48 kHz. 44.1 kHz files, the common case, are not filtered at all.

| Measure | Figure | Where |
| --- | --- | --- |
| Filter, 30 s stereo 96 kHz (11 MiB of int16) | 52-64 ms, 170-210 MiB/s | host, x86 2.3 GHz, GCC -O2 |
| Linear resample of the same | 11 ms | same |
| Filter on the unit, estimated | 0.6 s (cl6x) to about 2 s (GCC) for that file | 4 serial biquads, about 40 cycles per sample at best |
| Card read of the same file, for comparison | 1.5 s at 7,600 KiB/s | measured earlier |

So the worst case roughly doubles the load time of a long 96 kHz file, and one 48,000-frame convert
slice grows by an estimated 10-26 ms, which is main-loop time (screen and keys), not audio. Not
measured on the unit: docs/UNIT_CHECKLIST.md has the line. If a slice turns out to stall the
screen, the fix is a smaller slice for downsampled files, not dropping the filter.

### Optimization review (2026-11-22, host only)

A review of the audio path for cycles, after the first unit boot. Everything here was measured in a
cloud session with **no emulator and no cl6x**, so the figures are host (x86, GCC -O2) proxies:
callgrind instruction counts and wall time over the default beat, 130 BPM, with denormals flushed
as the C674x FPU flushes them. They say what moved and in which direction, not what the unit will
read. The emulator and cl6x runs that turn them into figures are listed at the end.

#### Profile of the default beat (host, callgrind, 6,000 blocks)

| Share of x86 instructions | Where |
| --- | --- |
| 27% | KICK: the body shaper is 8%, but the **click path** (noise, two one-poles, an SVF, three mixes) runs for the whole note, long after its transient envelope has decayed |
| 16% | `enaga_envelope_render` across every machine: branch-free but a loop-carried one-pole, so it cannot pipeline below its recurrence on either compiler |
| 15% | VA POLY's four-note chord; VA MONO 6% |
| 18% | SNARE; HAT 11% |
| 9% | `enaga_svf_process` |
| ~3% | every `prepare()` together -- per block, per sounding track, plus once more per trig |

Two of these are scalar control-rate code that stays GCC-compiled in the cl6x build (only `*.ti.c`
and Enaga's library go to cl6x), so on the unit they are a larger share than x86 suggests: the
prepares, and the VA voice's per-slice pitch maths.

#### Three changes, all bit-exact

| Change | How it is held to bit-exact | Host effect |
| --- | --- | --- |
| **Prepare only on change** (`app/engine.cpp`): a track's machine and insert chain are prepared only when its 16 parameter bytes, its mix or its machine moved, and every applied command clears the memo (a sample, a machine switch, a kit recall or a project load are all commands) | two engines side by side, one preparing every block and one memoised, through locks, knob moves, machine switches, a sample load, a mute, a kit recall and a project load: every output sample compared (`engine_skipping_an_unchanged_prepare_is_bit_exact`). Removing the per-command reset fails it with 2,914 samples different | **-3.3%** of the default beat's instructions; output hash identical over 30,000 blocks |
| **VA oscillator increments cached** (`dsp/va.cpp`): `semitones` and two reciprocals per slice per voice only when the pitch moved (VA POLY never glides), and the ramp's `reciprocal(n)` only when n changed | a copy of the voice made to forget the cache before every slice, compared through a held note and a glide (`va_cached_oscillator_increments_are_bit_identical_to_computed_ones`) | -0.5% of the default beat; more per voice in a six-voice chord |
| **Load-time anti-aliasing loop in `core/pcmloops.ti.c`**, with stereo filtered in one pass so the two channels' cascades overlap | against the old loop, mono and stereo, uneven slices, two rates, state included (`the_antialias_loop_is_bit_exact_against_the_loop_it_replaced`) | 30 s of 96 kHz stereo: 69-70 ms -> 59-61 ms. The real gain is cl6x pipelining the cascade, which x86 cannot show |

**Expected on the unit, unmeasured:** the prepare memo removes the most scalar work of the three.
Enaga measured scalar GCC transcendentals at roughly 70 cycles each on this core ("pitch 3.2%: five
exp2 and two divisions" per voice-step, docs/design.md); the default beat's prepares are on the
order of a hundred of those a block, plus the second prepare on every trig, so **0.5 to 2 points
off the peak** is the plausible range in both builds. Nothing changes in a block where a lock,
scene or knob really did move: those prepare as before.

#### Findings that are not changes

1. **The worst case in this file is the default kit's, not the app's.** Since machine switching
   (M6), any track can host VA POLY, and nothing caps voices across tracks. At 2.16 points a
   voice (cl6x, emulator), eight tracks of six-voice chords would be ~104% of the block before the
   chains and sends, and **four** would reach about 60% with the other four tracks playing. GRAIN at full density is similar
   (+9.2 points a track, GCC-only). **Answered 2026-11-22 (Roland chose a shared budget):**
   `Engine::kPolyVoiceBudget` caps VA POLY voices across all tracks at 12. A new note past the
   cap fades out the quietest started voice on any POLY track (`VaVoice::choke`, 64 frames), and
   is dropped if every counted voice is still waiting for its frame. Twelve voices are ~26 points
   with cl6x on emulator figures; the unit's MAX sets the final value (UNIT_CHECKLIST, "The
   shared VA POLY voice budget"). GRAIN and VA MONO are not in the budget.
2. **The unit may be slower than the emulator, not faster.** The emulator's cl6x default beat is
   19.17% (`scenario_m3.sh`); the unit's on-screen LOAD read up to about 26% at first boot. If
   the MAX readout confirms that ratio (~1.35x), the cache and DDR costs the emulator cannot see
   are real, and every headroom figure above shrinks by it. Do not plan voice counts from the
   "25% faster on hardware" figure until MAX says so.
3. **More voices: yes, with care.** Against 60%, the cl6x worst case of 23.5% leaves ~36 points:
   about 16 more VA voices on emulator figures, or ~9 if the unit is 1.35x. Raising VA POLY from
   six voices to eight costs ~4.3 points (cl6x) and fits either way -- but only once (1) is
   answered, because it raises the ceiling of every POLY track at once.
4. **KICK's click path** (27% of the host profile, the largest machine) keeps running for the
   whole note after `transient_env_` has decayed to nothing audible. Skipping it once the
   transient envelope is idle would be a large saving, but it is **not bit-exact**: the shared
   noise generator would advance differently, so later hits get different (equally random) noise.
   It wants a listening test, so it is not in this change.
5. **`enaga_envelope_render` is recurrence-bound.** Decay and release are geometric
   (`l *= 1 - d` with sustain 0), which a closed form or a two-step unroll could pipeline. That is
   Enaga's code (CLAUDE.md: never modify it) and would not be bit-exact; worth raising upstream
   with this profile.
6. **Cache, by working set** (the unit has 32 KiB L1D and 256 KiB L2, all cache, DDR behind):
   - The reverb's eight lines and four diffusers are ~100 KiB, which fits L2 but not L1D, and
     competes there with ~396 KiB of code (the hot subset is much smaller) and the sample reads.
   - The delay (768 KiB) and the beat-repeat ring (2.93 MiB) are sequential streams: a few new
     lines a block each, so cheap whatever their size.
   - **GRAIN's feared "all misses" is the wrong picture at normal pitch:** each grain reads a
     contiguous run per block (64 frames x 2-4 bytes), so 1-2 new lines per grain, ~16 for eight
     grains. The bad case is **high pitch**: at the 32x step ceiling a stereo grain reads 8 KiB a
     block, every tap a new line, ~500 misses a block for eight grains. SAMPLE at high pitch is
     the same in miniature. Only the unit can price a miss.
   - The undo snapshot (8.2 KiB, once per edit gesture) evicts a quarter of L1D in the block it
     happens in; a one-off, not a trend.
7. **Send tails to -60 dB** (above): the worst case is unchanged, because every figure above
   already runs both sends. What changes is how long they keep running unfed -- up to 272 s at
   feedback 0.95 on a 2 s delay -- which only matters for headroom a quiet passage relied on.
8. Two comments say a float division is hardware on this core (`Engine::process`,
   `Granular::fit_length`). It is not: GCC inlines a reciprocal estimate and two refinements
   (Enaga's docs/design.md), cl6x calls `__c6xabi_divf`. Harmless where they are, at control rate.

#### To measure on Roland's PC (emulator, cl6x and GCC)

| # | What | How | Record |
| --- | --- | --- | --- |
| E1 | All seven scenarios, before and after this change | `tools/scenarios.sh` on `main` before the merge (eb46d43) and after, once with cl6x and once with `ENAGA_TI_CGT=none` | four columns in a new table here; expect a small drop with cl6x, larger GCC-only |
| E2 | VA POLY per voice, after | MIN7 vs MIN11 in `build_default_beat`, `scenario_m3.sh`, both compilers | does 2.16 (cl6x) / 3.82 (GCC) move? |
| E3 | The app's real worst case | temporarily set four tracks to `va_poly` with MIN11 chords in `build_default_beat`; then six; then eight | where it crosses 60% and where it overruns: sizes the voice budget in finding 1 |
| E4 | The prepare memo's share | `scenario_m5.sh` (scenes and macros move parameters, so it prepares more) against `scenario_m3.sh` | the gap between them, before and after |
| E6 | The master kill EQ running | `tests/scenario_killeq.sh` against `tests/scenario_m7.sh` (same beat, EQ bypassed), with cl6x and with `ENAGA_TI_CGT=none` | the difference is the EQ's cost; the proxy above says ~1.3 cl6x / ~2.2 GCC-only |
| E7 | Eight LFOs running | `tests/scenario_lfo.sh` as it is (every LFO off: should match `scenario_factory.sh`), then again after a temporary edit to `factory_kit` in core/factory.cpp setting each track's `params[17] = 40` (RAT, about 3 Hz free), `params[18] = 127` (DEP) and `params[19] = 38` (DST = DRV), with cl6x and with `ENAGA_TI_CGT=none`; revert the edit | the difference is eight LFOs' cost; the host figures say about 1.1-1.4 points GCC, and about the same with cl6x |
| E12 | The ASR page | `tests/scenario_asr.sh`, then the held-chord `factory_kit` edit in "The ASR page" above, `scenario_factory.sh`, both compilers; revert | **done 2026-11-23:** off = A01 (26.15% / 49.32%); held +3.60 cl6x / +4.71 GCC (see "The ASR page") |
| E13 | The OSC page and the VA rework | `tests/scenario_osc.sh` against `scenario_factory.sh` before it, then the worst-case `factory_kit` edit in "The OSC page" below, both compilers; revert | the factory beat should not rise; worst case at most ~0.1 point a voice over the old voice |
| E14 | WAVE MONO and WAVE POLY | `tests/scenario_wave.sh` (tracks 6 and 7 switched to WMN and WPL on keys) against `scenario_factory.sh` on the same commit, both compilers; and the boot: the cycles to `strata ready` against the same run before the WAVE commit | see "WAVE MONO and WAVE POLY" below: about 1.0-1.4x a VA voice expected; the table build is a one-off at boot |
| E5 | Load time of a 15 s 96 kHz stereo WAV (30 s of the file now loads too: `kSrcFrames` is 30 s of 96 kHz frames since 2026-11-22) | through the emulator's card, cl6x vs GCC | wall time to playable; screen should not stall |

#### To measure on the unit (with the MAX readout from PR #17)

| # | What | Record |
| --- | --- | --- |
| U1 | MAX on the default beat, this build | against the emulator's cl6x `scenario_m3.sh`: the unit/emulator ratio (finding 2) |
| U2 | MAX with the chord at MIN11 (six voices), then with tracks 5 and 6 also switched to VA POLY playing chords | per-voice cost on the unit; where it overruns |
| U3 | GRAIN on a multi-second sample, DEN 127, SPR 127, at PIT 64 and at PIT 127 with a high note | the DDR cost of high-pitch grains (finding 6) |
| U4 | Delay FBK at max, TIM 1/1, one hit, then silence | LOAD stays up while the tail rings and drops when it ends |
| U6 | MAX on A01 GROOVE with the kill EQ running (FUNC + push A, then let it play 60 s with LOW killed), against U1 | the EQ's cost on the unit; should match E6's cl6x difference times the unit/emulator ratio |
| U7 | MAX on A01 GROOVE with LFOs on four tracks (EDIT, selection RIGHT four times to LFO; C = DEP 127, D = DST DRV or CUT, B = RAT a fast free rate), against U1 | the cost of four LFOs on the unit; about half of E7's difference times the unit/emulator ratio |
| U5 | Load a 15 s 96 kHz stereo WAV (`unit-test/STRATA_1.WAV` in the project files; `SWEEP_30S_96K.WAV`, 30 s, now loads as well) | seconds to playable; whether the screen or keys stall during it |

### Sampling from the input (2026-11-22): unmeasured, expected negligible

`Engine::process` now runs `strata_master_peak` over the input block every block (128 floats,
for the RESAMPLE meter), and a take from the input converts `in` instead of `out` -- the same
conversion a master take already does. No voice, filter or effect is added. Not yet measured:
add `scenario_m3.sh` both ways after this change to the next emulator session (the emulator
cannot select an input route, so the conversion path is only reachable on the unit: section 8c
of the walkthrough asks for MAX while recording from the input).

### Streaming (2026-11-22): unmeasured, estimated under 1 point per streamed track

Costs nothing on a project that streams nothing: `top_up_streams` and `watch_streams` test one
flag per track. A streamed track adds, per block, (a) `pcm_to_s16` over the frames that arrived
(64 at the root pitch, up to 512 while refilling after a restart) and a copy of the carry, and
(b) the stream voice in `dsp/sample.cpp`, which fetches its four taps through a per-sample
branch (head, fifo or silence) instead of `generate()`'s branch-free interior. It is one voice,
against the four a whole-loaded SAMPLE track may run, and it is plain C++ rather than a
`*.ti.c` loop: an estimate of 30-40 cycles a sample puts it at **0.4-0.5 points a streamed track**
with the conversion, GCC or cl6x alike. No scenario streams yet (the emulator has no file large
enough on its card); measure with section 15d of the walkthrough: LOAD with three streamed
tracks against none. If it is worse than 1 point a track, the fetch is the lever: a fast path
for a run of samples wholly inside the head or wholly inside the fifo.

### SLC AUTO (2026-11-22): no audio cost, one pass at load

The hits are found in the main context, as each slice of a load is written into the pool
(`core/onsets.hpp`): one abs and one add per sample, and a few compares per 256-frame hop. That
is less than the int16 conversion beside it, so a load should not be measurably slower. In the
audio context nothing per sample changes: the cuts are worked out in `Sampler::recompute_window`
at control rate (at most 15 compares), and a trigger reads two of them instead of multiplying.
With AUTO off (the default) the Sampler renders bit for bit as before.

### MIDI out (after v0.1, 2026-11-22)

**Not measured yet: no emulator or device toolchain in the session that built it.** Owed:
`tests/scenario_midi.sh` with cl6x and with `ENAGA_TI_CGT=none`, on the PC (UNIT_CHECKLIST, "On the
PC"), and section 8d's LOAD on the unit.

What it adds to `audio()`, all control rate and none of it in a `*.ti.c` loop:

- `MidiOut::begin_block` and `end_block`: two passes over 64 slots of 8 bytes, every block.
- per trig on a track with a channel: one `enaga::midi::send` of 3 bytes per note (a chord is up
  to six), and a scan of the 64 slots for the same pitch.
- per block, for each MID track with a channel: 8 compares of its CC slots, a send when one moved.
- **MID itself renders nothing** and reports `active() == false`, so the mixer skips the track:
  no machine render, no insert chain, no prepare. A part moved from VA POLY to MID frees its
  voices (2.16 points each with cl6x) and its chain.

Estimate: well under 0.1 point with eight MIDI tracks, against the 2-4 points one sustaining
track costs. The timing is the real limit, not the CPU: a note leaves in the block its step falls
in (up to 1.33 ms early, because sends carry no timestamp), and DIN takes 0.96 ms a 3-byte
message, so a six-note chord's last note leaves about 6 ms after its first.

Memory: `MidiOut` 512 bytes and the CC memo 136 bytes in `Engine`; MIDI IN's held-note map 2 KiB
in the app; MID adds 8 bytes (a vtable pointer) per track to the machine pool. All bss.

### The cl6x vectorisation pass (2026-11-22, host only, cl6x figures owed tonight)

**Is there vector code for this chip?** The core is a **C674x** (`build_app.py` builds with
`-mv6740` and GCC with `-march=c674x`). It has **no packed-float instructions**: the two-way
single-precision SIMD people mean by "vector float" on TI DSPs (`DMPYSP`, `DADDSP`, `CMPYSP`) is
the C66x, a later core. What the C674x has is (a) an eight-unit VLIW pipeline that cl6x fills by
**software pipelining** -- overlapping successive iterations of a branch-free loop -- which is
where the 1.7-1.8x in the tables above comes from, and (b) the C64x+ **packed 16-bit integer**
instructions (`DOTP2`, `ADD2`, `MPY2`, ...), which suit int16 sample data and nothing else
STRATA computes. So vectorising here means giving the pipeliner loops it can overlap, and using
packed 16-bit where a loop reads int16.

The numbers that decide what is worth doing are the C674x latencies: a float add, subtract or
multiply takes **4 cycles** to come back, `INTSP` 4, a load 5. A loop whose next iteration needs
this iteration's float result -- every filter, envelope, one-pole and gain ramp -- cannot start
an iteration more often than the length of that chain, however idle the eight units are. Those
are the loops this pass targets.

#### What changed, and what each change is expected to buy

Every change is **bit-exact on the host** (memcmp against the code or Enaga function it replaced,
and the 89 golden renders unchanged). The first four rows are on in the build that merges; the
two flags are off until tonight's measurements.

| Change | Where | Mechanism | Expected on cl6x (reasoned, not measured) |
| --- | --- | --- | --- |
| **In-place filter kernels** (`dsp/filterloops.ti.c`) | insert chain filter, DJ filter, reverb high-pass, KICK, SNARE, HAT | Enaga's `enaga_svf_process` / `_cascade` / `enaga_one_pole_process` / `enaga_biquad_process` take an `in` and an `out` that may alias, so cl6x must issue the next sample's load after this sample's store: a load (5) plus the path from input to output joins the recurrence. STRATA always filters in place, so these run Enaga's own inline steps over **one restrict buffer** | one-pole ~18 -> ~12 cycles/sample; 24 dB cascades ~30 -> ~20; single 12 dB SVF **no change** (its own state recurrence, ~20, is already as long as the alias chain) |
| **DJ filter in stereo** (same file) | master | both channels' cascades in one loop: two independent recurrences overlap | ~2 x 30 -> ~22 cycles/frame while the DJ filter is on |
| **VA back half fused** (`strata_va_back`, `dsp/valoops.ti.c`) | every VA MONO and VA POLY voice | filter, amp envelope and output tail were three passes, each recurrence-bound (cascade ~30, envelope ~15, gain ramp ~5 cycles/sample) plus three loop prologues per 16-frame slice; one loop overlaps them, so a sample costs about the slowest chain | LP24: ~880 -> ~350 cycles a slice, **~2,100 cycles (~0.4 points) per voice per block**, so VA POLY from 2.16 to about **1.7 points a voice**; ~5 points across the 12-voice budget. Less in the 12 dB modes |
| **SAMPLE's interior read** (`dsp/sampleloops.ti.c`) | every SAMPLE voice not streaming | the Hermite read loop was C++, so GCC compiled it in every build: M2's situation exactly. Same expressions, now cl6x | M2's 1.5-2x on the loop: roughly ~25 -> ~6-8 cycles/sample mono, ~0.2-0.3 points per sounding voice |
| `STRATA_TI_PACKED` (**off**) | SAMPLE and GRAIN stereo taps | a stereo int16 frame as one unaligned 32-bit load and `DOTP2` by (1, 1): L + R in one instruction instead of two loads and an add. These loops are load-bound (two .D units, eight taps a stereo Hermite sample) | stereo SAMPLE read ~8 -> ~5 cycles/sample; GRAIN stereo similar. Exact in integers, but only cl6x can run it, hence off |
| `STRATA_TI_HINTS` (**off**) | every new kernel | `MUST_ITERATE(1)` on loops whose function has returned for n <= 0: cl6x can drop the zero-trip guard. No larger minimum or multiple is true in general, because slices are cut at trigs and gates | a few cycles per call; probably under 0.3 points in total. Measured to decide, not assumed |

**Not done, and why:**

- **`_rcpsp` for `enaga_reciprocal`** in the drum shapers (an 8-bit hardware estimate and two
  Newton steps instead of a bit trick and three). Saves two multiplies of about twenty in KICK's
  shaper, changes the sound by an ulp the host cannot reproduce, and so could not be checked by
  the golden renders. Not worth it.
- **Rewriting Enaga's oscillators, envelope or SVF** (CLAUDE.md forbids touching `third_party/`).
  The kernels above call Enaga's own inline steps (`enaga_svf_step`, `enaga_one_pole_step`,
  `enaga_biquad_step`), so a pin bump that changes a filter changes STRATA with it; the envelope
  body is the one copy, and `va_the_fused_back_half_is_bit_exact_against_filter_envelope_and_tail`
  fails if Enaga's envelope ever moves away from it. Two upstream suggestions follow from this
  pass and are worth an Enaga issue once tonight's report backs them: in-place variants of the
  filter block functions with one restrict buffer, and the envelope's decay as a closed form.
- **Pairing VA POLY voices** in one loop (two voices' filters side by side, as the DJ filter's
  channels are). The next lever after the fusion, and a bigger rewrite of `VaVoice::run`; wait
  for `tools/ti_loops.py` to say whether the fused loop is still recurrence-bound.
- **TI's DSPLIB.** Its float kernels are FIRs, FFTs, matrix and vector maths, none of which STRATA
  runs per sample; its filters are biquads in a different form. It would add a TI library to the
  package, which the licence section above was careful to keep out, for nothing STRATA needs.

#### Tonight, on Roland's PC (WSL, cl6x)

| # | What | How | Record |
| --- | --- | --- | --- |
| E8 | Per-loop pipelining report | `python3 -B tools/ti_loops.py --enaga` and again with `-D STRATA_TI_HINTS=1 -D STRATA_TI_PACKED=1` | paste both tables here. Any `NOT PIPELINED` line is the next fix; `bound = recurrence` on a hot loop says pairing would pay |
| E9 | A/B of the whole pass | `bash tools/ti_ab.sh --gcc` (about an hour: six variants x six scenarios x two compilers, plus renders) | the two load tables it prints, and its audio check. **Expected: every variant `identical`.** A difference means cl6x reassociates differently in a fused loop than in separate ones (`--fp_reassoc`), and is a finding to bring back, not a reason to merge a flag |
| E10 | VA POLY per voice after the pass | E2's method (MIN7 vs MIN11 in `groove()`), on `head` | was 2.16 cl6x / 3.82 GCC; expected ~1.7 cl6x |

#### Tonight's results (2026-11-22, Roland's PC, cl6x 8.5.0.LTS, emulator 11ac9a0)

`tools/ti_ab.sh --gcc` on 426a4b5 (main + #65 + #17). `base` did not build (a tic6x GCC ICE in
`core/factory.cpp` on the older tree, fixed by PR #71), so there is no before/after column yet.

| Scenario, cl6x | nofuse | fused (head then) | hints | packed |
| --- | --- | --- | --- | --- |
| m3 | 25.24% | 23.48% (-1.76) | 23.45% | 23.48% |
| m1 / m7 / factory / samples | 25.96% | 24.27% (-1.69) | 24.25% | 24.27% |
| ambient | 31.62% | 29.78% (-1.84) | 29.75% | 29.78% |

GCC only: 49.16 -> 48.47 (m1), 44.12 -> 43.25 (m3). **Audio:** with GCC every variant is
identical. With cl6x, `hints` and `packed` are identical to the fused head, but the fused head
and `nofuse` differ on 365,891 of 401,018 samples, worst 1.44 of full scale.
`tools/render_diff.py` then compared each cl6x render with its GCC render: **the fused head built
by cl6x is bit-identical to GCC's; the unfused path built by cl6x is the one that departs** --
from GCC and from the fused head in the same way. Its first difference is tiny (6.2e-5 of full
scale at 0.366 s), and from 0.25 s on the difference is the size of the signal while each
window's peak and the RMS stay identical: the two renders play the same notes with state or
phase drifted apart, not a voice gone silent. So the fusion stays on, and the open question is
the old three-pass path under cl6x -- which is what the unit has run until #61, and is still how
Enaga's `enaga_svf_cascade`, `enaga_svf_process` and `enaga_envelope_render` are built for every
caller that uses them. Being chased in the vectorisation thread (cl6x `--fp_reassoc` on Enaga's
library first: a rounding difference that flips a level threshold, such as `VaVoice::sounding()`
deciding whether the next note resets the oscillator phases, would look exactly like this).

**Corrected the same evening, and the fusion is OFF again (PR #73).** A clean rerun
(`reassoc_check.sh`: one worktree, every build from scratch, 4 s renders compared with the GCC
build of the unfused path) found the opposite of the paragraph above: **cl6x and GCC render the
unfused path identically** (with and without `--fp_reassoc=off`), and **the fused loop built by
cl6x is the one that departs** -- first by 6.2e-5 of full scale at 0.365 s, then by the size of
the signal with peaks and RMS unchanged. The earlier cl6x-vs-GCC comparison was on the ti_ab
renders and cannot be reproduced; this rerun is the one to trust. The suspect is cl6x
reassociating the fused loop's float arithmetic (it reorders freely by default), giving a
rounding difference that tips a voice threshold (`VaVoice::sounding()` decides whether the next
note resets the oscillator phases). The deciding run is the fused build with `--fp_reassoc=off`.
Until then the unit runs the unfused path, which every build renders the same.

**Decided (same evening): it is not rounding, and the fusion stays off.** The fused loop built
by cl6x with `--fp_reassoc=off` departs from GCC in exactly the same way (366,019 of 401,018
samples, first at 0.3646 s); the GCC device build of the fused loop is identical to the unfused
path. The C674x has no fused multiply-add, so contraction cannot explain it either. What is left
is cl6x compiling `strata_va_back` to something other than its C -- a compiler bug, or undefined
behaviour in the kernel that GCC happens to tolerate -- and neither is acceptable in the audio
path. To bring the ~1.7 points back: build `valoops.ti.c` with cl6x at `-O2` and `-O0` to see
where the result changes, read the pipelined loop in the `-k` assembly, and only then report a
cl6x bug or fix the kernel. Not before v0.1.

`tools/ti_loops.py --enaga` confirmed the in-place reasoning: Enaga's `enaga_svf_cascade` has a
54-cycle version (in and out may alias) beside its 21-cycle one, and an in-place call gets the
54; `strata_svf_cascade_inplace` is 21. `enaga_one_pole_process` 18 vs `strata_one_pole_inplace`
13, `enaga_biquad_process` 27 vs 9. `hints` buys 0.02-0.03 points and `packed` nothing, so both
stay off. Not pipelined: `strata_antialias_stereo` and `strata_kill_eq`'s second loop (no
schedule found), `strata_long_delay_process` and `strata_hall_process:153` (control code in the
loop), and Enaga's `enaga_oscillator_sync_add:177` (control code).

**Turning a flag on** is one line in `dsp/tihints.h`, and needs all three: E9's audio check says
`identical` for that variant, its load column is lower than `head` in the scenarios it touches
(`samples` for PACKED; all of them for HINTS), and `make -C host test` still passes. Then record
the figures here and move the flag's row in the table above to "on".

### Everything at once (2026-11-22, an estimate, not a measurement)

Every afternoon feature was priced alone. Added together, on the worst case above (`scenario_m1`
/ `m7`: the default beat with drive, LP24, lo-fi and both sends on all eight tracks, 23.49% cl6x
and 39.60% GCC-only, emulator), with each one at its own section's figure:

| On top of the worst case | cl6x (points) | GCC only (points) | Source |
| --- | --- | --- | --- |
| The worst case itself | 23.5 | 39.6 | the table above |
| VA POLY at the shared cap: 12 voices, against the default beat's 4 | +17.3 | +30.6 | 8 x 2.16 / 3.82 |
| The master kill EQ running | +1.3 | +2.2 | "The master kill EQ" |
| Eight LFOs, on the dearest destination (DRV) | +2.5 | +1.4 | "Per-track LFO" |
| Tape stop and delay freeze, both held | +0.3 | +0.5 | "Tape stop and delay freeze" |
| Four streamed tracks | +2.0 | +2.0 | "Streaming" |
| MIDI out on all eight tracks, arp and note repeat | ~0.1 | ~0.1 | control rate; their voices are the cap's |
| **Total, emulator** | **~47** | **~76** | |
| **If the unit is 1.35x the emulator** (BUDGET finding 2) | **~63** | | |

So on the emulator with cl6x everything at once fits under 60% with ~13 points to spare, and the
GCC-only figure is over it, as it has been since M2 (meaningless either way: CLAUDE.md). **On the
unit it may not fit**: if first boot's ~1.35x holds, the all-on figure is about 63%. The lever, in
CLAUDE.md's order, is effects and insert stages first, then `kPolyVoiceBudget` (each voice off
the cap is 2.16 points, or ~2.9 at 1.35x). The arp and note repeat add no cost of their own past
the cap: every POLY hit they make is counted against the 12.

Two things the cap does not bound, and the table does not count:

- **GRAIN at full density is outside the voice budget**: +9.2 points a track GCC-only ("GRAIN
  measured"), roughly +5.4 with cl6x at the usual 1.7x. Two dense GRAIN tracks in place of two
  drum tracks would take the unit figure past 70%.
- **VA MONO is outside it too**, but it is one voice a track, about 2.2 points each at most.

To measure: U-row "everything on" on the unit with the MAX readout -- four POLY tracks holding
MIN11 chords with the arp at 1/32, the kill EQ with a band killed, eight LFOs on DRV, tape stop
held, freeze held, two streamed stems -- and E3 on the PC first.

### The CPU guard (2026-11-23, host only)

The 84.3% MAX above has no ceiling behind it: more chords or a denser cloud would take a block
past 100%. `core/cpuguard.hpp` is the ceiling. Each audio block reads `enaga::audio_load()` (the
previous block's cost) and keeps an ~11 ms average; at 70% average, or one block at 90%, it rises
a level (at most one per 100 ms, or per 11 ms on a 90% block), and after each full second under
55% it falls one. A level takes two voices off the shared POLY budget (12, 10, 8, 6, 4; ~2.2
points each with cl6x) and shrinks every GRAIN pool (8, 6, 4, 3, 2 grains; a full cloud is +9.2
points GCC-only). Level 4 therefore frees about 17 points of POLY and most of each dense GRAIN
track. Shed POLY voices take the existing 64-frame choke, quietest first, one a block; GRAIN skips
spawns instead of stealing, so nothing is cut mid-window.

**It is a safety net, not the budget.** CLAUDE.md's order (effects and insert stages before
voices) still governs what the worst case is designed to; the guard only acts when play goes past
it, and it sheds voices rather than effects because dropping a reverb or a delay mid-tail is
heard at once, while the quietest POLY voice fading over 1.3 ms mostly is not.

**Cost:** a one-pole average and three compares a block, plus a scan of eight tracks' POLY
voices only while the budget is lowered. Under 0.1 points; not measured. At level 0 the engine
renders bit-for-bit as before (the golden renders and the bit-exact tests pass untouched).

**Owed:** the thresholds (700 / 550 / 900 per mille) are provisional. Set them from the unit
(UNIT_CHECKLIST, "The CPU guard"): A01 must never raise it, and the 84% session must stay
under 100% with it. The scenarios never reach the guard's thresholds; nothing on the emulator
exercises it.

### The ASR page: held notes on the VAs (2026-11-23, host only)

VA MONO and VA POLY read four more bytes (ATK SUS REL FSU, params 24-27) in `VaSettings::make`,
once a block, into the same four `EnvelopeShape::make` calls as before: the control-rate cost is
unchanged, and **the per-sample code is untouched** -- the same oscillators, filter, envelope
render and tail, with an envelope whose sustain level is no longer 0. VA MONO gained VA POLY's
gate countdown (one compare and subtract per control slice while a gate counts). All four bytes
at 0 is bit-identical to before: `golden_dense_retriggers` and every VA golden render pass
untouched, so A01 and the factory content cost exactly what they did.

What changes is **how long a voice lives**. With SUS up a chord holds for its LEN (up to 64 steps)
or for as long as pads are held, and REL up to 8 s extends the tail, so far more notes overlap
than DEC alone ever allowed. The ceiling is still the shared budget of 12 POLY voices
(`Engine::kPolyVoiceBudget`, 2.16 points each with cl6x, 3.82 GCC-only: ~26 / ~46 points at the
cap), and above it the CPU guard (PR #83) lowers the cap. Expect held pads to sit at the cap far
more often than stabs did, so U10 (six held pads over A01) is the reading that matters.

**E12, measured on Roland's PC (2026-11-23, main 5120347, emulator):** `tests/scenario_asr.sh`
walks EDIT to the ASR page on a drum and on VA POLY with every byte 0; the held-chord build sets
track 7's `params[25] = 100` (SUS) and `params[26] = 90` (REL, about 1.9 s) in `factory_kit`
(core/factory.cpp) for one `scenario_factory.sh` run, then reverts.

| Build | cl6x | GCC only |
| --- | --- | --- |
| `scenario_factory.sh` (A01 as shipped) | 26.15% | 49.32% |
| `scenario_asr.sh` (ASR page, all 0) | 26.15% | 49.32% |
| `scenario_factory.sh`, A01's stabs held (SUS 100, REL 90) | 29.75% | 54.03% |
| Held chords cost | **+3.60** | **+4.71** |

"Off" matches A01 exactly with both compilers, as the bit-exact host renders promised. Holding
A01's stabs costs 3.6 points with cl6x: under two extra voices' worth at 2.16 a voice, because
A01's stabs are spaced and few overlap. Six held pads over A01 (U10 on the unit) can still reach
the 12-voice cap; that reading is the one that matters.

### A05 PAD: held chords as factory content (2026-11-23, host only)

A05 (core/factory.cpp, `pad`) holds a VA POLY chord through every bar with REL 2 s, so the chord
track never goes idle: four or five voices hold, and six for the moment after each change. The
voicings share notes, so a change never needs a seventh voice and nothing is stolen
(tests/test_factory.cpp checks the union of each pair). The rest of the pattern is lighter than
A01: four kicks, four closed hats, an occasional open hat and rim, a three-note bass a bar, no
clap or loop.

Estimate, from E12 and the per-voice figure: six held voices are 6 x 2.16 = 13 points cl6x (23
GCC-only) for the chord track, against A01's stabs at a few voices for a fraction of the bar. A01
measures 26.15% / 49.32%, and holding its stabs added 3.60 / 4.71, so A05 should land near **30%
cl6x and 52-55% GCC-only**: under 60% either way, the GCC figure closest to it. Host render
(`make -C host render`): the mix peaks at -1.2 dBFS, RMS -16.7 (A01: -17.0), the chord stem 2 dB
under the kick. `tests/scenario_pad.sh` measures it; it has not run (the cloud cannot build the
toolchain).

| Build | cl6x | GCC only |
| --- | --- | --- |
| `scenario_pad.sh` (A05 PAD) | | |

### Freeze and bounce (2026-11-23, host only)

FREEZE (FUNC menu) records one track's machine output, before its chain, for 1/2/4/8 bars and
then plays that loop from RAM (dsp/frozenloops.ti.c: one int16-to-float multiply-add a frame,
branch-free, with a gain ramp at freeze and unfreeze). The machine is killed and skipped while
the track is frozen; the chain, sends and duck stay live. Estimate: a six-voice VA POLY track is
6 x 2.16 = 13 points cl6x (23 GCC-only); its frozen loop is one sequential 2-byte read a frame,
about 0.5 points either way, so freezing a held pad saves about 12 points cl6x (22 GCC-only).
While the take records the track costs what it did plus one copy of the block.

Bounce (RESAMPLE source TRK 1-8) taps one track after its chain, before the duck: one block copy
only while a bounce records, nothing otherwise.

Memory: frozen loops use their own pool slots (8-15) out of the 64 MiB sample budget, at 2 bytes a
frame mono: 8 bars at 120 BPM is 16 s, 1.5 MiB a track.

`tests/scenario_freeze.sh` freezes track 7 of the default beat (`--pad`: of A05 PAD, the held
chord) and prints the app's `ld <avg> <max>` lines (one a bar, app/app.hpp) after the freeze;
`--baseline` freezes and then unfreezes, and prints the bars with track 7 live again. The app
logs four bars, from the second whole bar after the change, and only after a freeze or unfreeze:
four bars are one pass of A05 (a chord a bar, four or five notes each), so both runs measure the
same chords; and the skipped bar lets an unfrozen chord track come back with its release tails.
#110 logged half-second windows after PLAY and pattern changes too, which pushed early lines out
of Enaga's 16-line log ring and broke six other scenarios' log checks; #114 fixed that but its
baseline windows began right after the unfreeze, before the next chord. The first version printed a "last block" figure that the emulator's
report does not have, so it always read 0.00%.

Measured on Roland's PC at f5d66dd, cl6x, A01 (factory beat), track 7 frozen, with a temporary
windowed log of the same kind (375 polls a window):

| Window | Live | Frozen |
| --- | --- | --- |
| max, three bars compared | 27.0 / 25.3 / 24.4% | 19.0 / 18.4 / 17.2% |
| average | 13.96% | 13.38% |

Freezing A01's chord track saves about **7 points at the peaks and 0.6-0.7 on average**: A01's
stabs sound for a fraction of each bar, so the saving lands where they do. Recording the take
costs about 0.3-0.4 points while it runs.

The held pad is the case freezing is for. `scenario_freeze.sh` on Roland's PC (cl6x, PR #115 head
da99883, 8.2 G cycles a run, all four runs OK, no overruns), four bars from the second whole bar
after the freeze or unfreeze, one pass of A05's four chords, avg/max % a bar:

| Run | avg | max | bars |
| --- | --- | --- | --- |
| A05 live (`--pad --baseline`) | 21.23% | 26.9% | 21.4/26.9 21.5/26.9 20.4/25.4 21.6/26.9 |
| A05, track 7 frozen (`--pad`) | 10.10% | 15.7% | 9.9/15.7 10.1/15.7 10.3/15.7 10.1/15.7 |
| A01 live (`--baseline`) | 14.22% | 26.4% | 14.1/25.5 14.3/25.6 14.3/26.4 14.2/26.2 |
| A01, track 7 frozen | 13.45% | 19.1% | 13.4/18.4 13.4/18.4 13.5/19.1 13.5/19.1 |

Freezing A05's held chord **halves the load: 11.1 points off the average and 11.2 off the max**,
close to the estimate of 12. Freezing A01's stabs saves 0.8 on average and 7.3 at the peaks. The
bars spread by at most 1.1 points, so these compare like with like. Earlier readings with
half-second windows (#110: A05 live 20.27%, #114: 17.29%) covered different bars in the live and
frozen runs and are superseded. The peak since boot is 26-27% in every run, because it includes
the bars before the freeze. GCC-only figures were not taken (cl6x ships).

### The OSC page and the VA sound rework (2026-11-23, host only)

VA MONO and VA POLY got an eighth EDIT page, OSC (WAV OS2 SUB DRV, params 28-31), a new RES
curve that reaches self-oscillation, and a pre-filter drive. What it does to the per-voice code:

- **OSC 2 is no longer always on.** The old voice ran a +19-semitone pulse hard-synced to OSC 1 on
  every note, the costliest stage in the voice (va.cpp's table). Now it runs only when OS2 is in
  its sync half (64-127); OS2 1-63 runs a second plain saw instead (cheaper than sync), and OS2 0
  runs nothing. OSC 1 is a saw or a PolyBLEP pulse, about the same cost.
- **The sub is skipped at SUB 0.** It was always on.
- **The resonance limiter is free per sample.** It reads the resonant stage's two state values at
  the end of each 16-frame slice and raises k for the next one. (A band clip inside the SVF
  recurrence was tried first and cost 2.3x on the host: dropped.)
- **DRV > 0 swaps Enaga's block filter for `strata_va_drive_filter`** (dsp/valoops.ti.c): the same
  SVF steps with one `enaga_soft_clip` per sample before them. DRV 0 keeps Enaga's own loops.

Host bench (x86, GCC -O2, 64-frame blocks, ns per output sample, old voice vs new):

| Patch | Old | New | Change |
| --- | --- | --- | --- |
| VA MONO, factory ROLL (saw, SUB, a little DRV) | 25.2 | 20.1 | -20% |
| VA MONO, worst case (pulse, OS2 sync, SUB, DRV up, LP24) | 25.5 | 25.7 | +1% |
| VA POLY 6 voices, factory STAB (saw + sub) | 116.2 | 77.1 | -34% |
| VA POLY 6 voices, worst case (as above) | 126.9 | 132.7 | +5% |

So the factory beat should get cheaper (its bass and chord dropped the synced pulse), and a voice
with every OSC option turned on costs at most about 5% more than the old fixed voice: on the
2.16-point cl6x figure, about 0.1 point a voice, 1.3 points at the 12-voice cap. The 12-voice
budget and the CPU guard stay as they are. None of this is measured with cl6x or on the
emulator yet:

**E13, to measure on Roland's PC (emulator):** `tests/scenario_osc.sh` (walks EDIT to the OSC page
on a drum and on VA POLY, A01 as shipped) against `scenario_factory.sh` from the commit before the
OSC page, both compilers. Then the worst case: temporarily set tracks 6 and 7 in `factory_kit`
(core/factory.cpp) to `params[28] = 64` (WAV, pulse), `params[29] = 100` (OS2 sync),
`params[30] = 102` (SUB) and `params[31] = 64` (DRV), one `scenario_factory.sh` run each compiler,
and revert.

| Build | cl6x | GCC only |
| --- | --- | --- |
| `scenario_factory.sh`, before the OSC page | 26.15% | 49.32% |
| `scenario_osc.sh` (A01 as shipped, new voice) | | |
| `scenario_factory.sh`, tracks 6 and 7 at the worst case | | |

### WAVE MONO and WAVE POLY (2026-11-23, host only)

The wavetable voice is the VA voice with its oscillators swapped for one or two readers of two
neighbouring int16 frames (dsp/waveloops.ti.c: two linear interpolations and a crossfade a sample,
branch-free, ready for cl6x). The filter, the envelopes, the sub and the tail are the VA's.

Host, x86 GCC -O2, a six-note held chord on one POLY machine, nanoseconds per voice per 64-frame
block, best of two runs (noisy; the ratios are what to read):

| Voice | ns / voice / block | vs VA saw |
| --- | --- | --- |
| VA, saw, OS2 off | 857 | 1.00 |
| VA, saw, OS2 detune | 901 | 1.05 |
| VA, saw, OS2 sync | 1,169 | 1.36 |
| WAVE, one reader | 1,121 | 1.31 |
| WAVE, DET (two readers) | 1,291 | 1.51 |
| WAVE, DET and WRP | 1,591 | 1.86 |

So a WAVE voice costs about what a synced VA voice does, and the worst knob setting (DET and WRP
together) about 1.4x that. Scaled from VA POLY's 2.16 points a voice with cl6x (3.82 GCC-only),
expect roughly **2.2-3.0 points a voice with cl6x and 4-5 GCC-only**, unmeasured. WAVE POLY shares
VA POLY's budget of 12 voices and the CPU guard sheds its voices the same way, so the worst case is
twelve voices at the upper figure, about 9 points above twelve VA voices. If the emulator says
more, the lever is WRP's loop (`strata_wave_add_warp`, a float index and a compare per sample).

**Boot:** the 16 tables are computed in `Engine::init` (a float FFT, 256 frames, 10 mip levels):
25.8 ms on the host. The C6748 has no x86 FPU throughput; expect 0.5-1.5 s added to the boot,
to be measured as E14.

**Memory:** 2,626,560 bytes of tables plus 0.2 MiB of build scratch: init now asks for 111.77 MiB
of the 120 MiB heap, 8.23 MiB spare against the 8 MiB rule (tests/test_memory.cpp itemises it).
Since the review after #122 the scratch is borrowed from the capture buffer, idle at init, and
zeroed again after the build: 111.59 MiB, **8.41 MiB spare**. With the loader's file buffer cut
to one 2 MiB chunk in the same PR: **97.05 MiB, 22.95 MiB spare**.
FM and STRING, which follow, allocate under 0.1 MiB.

**E14, to measure on Roland's PC (emulator):**

| Build | cl6x | GCC only |
| --- | --- | --- |
| `scenario_factory.sh`, same commit | | |
| `scenario_wave.sh` (WMN bass, WPL chord) | | |
| cycles to `strata ready`, before / after | | |

### Later milestones

| Milestone | Target | Measured (cl6x) | Measured (GCC) | Date |
| --- | --- | --- | --- | --- |
| M1, 4-track beat at 130 BPM | under 15% | | 33.07% emulator, all 8 tracks, a 4-note chord on one | 2026-11-21 |
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
heap is the real one. M4 and M5 together take 64 MiB for the sample region, 27.5 MiB of loader staging (18 MiB before
2026-11-22, when the 30 s limit became 30 s of a file at up to 96 kHz) and 2.93 MiB
for the beat-repeat ring, which is 94.5 MiB before anything else, against a spec table that did not
count the staging at all.

| Use | Budget | Measured |
| --- | --- | --- |
| Samples, int16 (32 MiB default, 64 MiB ceiling) | 32-64 MiB | **64 MiB** reserved at init, from the heap. The CEILING, not the default: a project may raise its budget long after `init()` is over, and CLAUDE.md allows no allocation after it |
| Sample loader staging (M4) | not budgeted | **13 MiB since 2026-11-23** (raw_ became one 2 MiB chunk: every bigger file was already read in chunks; init 97.05 MiB, 22.95 MiB spare). Before: **27.5 MiB**, from the heap: 16.5 MiB for one whole file (`files::load` wants a buffer for all of it; 30 s of 96 kHz stereo 24-bit plus 64 KiB of chunks) and 11 MiB for a resample's int16 source (30 s of 96 kHz stereo). Sized from `docs/SPEC.md`'s own 30 s in-RAM limit counted in the FILE's length, and a FIXED cost rather than a per-sample one. Was 18 MiB when the limit was counted in 48 kHz frames. `tests/test_memory.cpp`: init is 101.2 MiB of a 120 MiB heap, 18.8 MiB spare against an 8 MiB headroom rule. **Worth checking against the heap on the unit** -- 64 + 27.5 is 91.5 MiB of the ~124 MiB |
| Beat repeat ring (2 bars at 60 BPM, stereo float) | 2.93 MiB | **2.93 MiB** (3,072,000 bytes) reserved at init. Below 60 BPM the ring holds less than two bars, which costs nothing: the longest loop offered is a quarter note, 3 s even at 20 BPM |
| Delay and reverb buffers | 2 MiB | **1.10 MiB** in Enaga's classic StereoDelay and Reverb, plus **4.99 MiB** for the ambient modes (2026-11-22): the LONG delay's 12 s of stereo float and its all-pass rings (4.44 MiB) and the HALL's lines (0.55 MiB). `tests/test_memory.cpp`: init is **109.1 MiB** of the heap, **10.9 MiB spare** against the 8 MiB rule |
| Patterns, kit, scenes, 16 undo levels | 2 MiB | **~148 KiB** so far: 17 whole-pattern undo snapshots (~140 KiB) plus the live pattern and the clipboard. In bss, not the heap |
| Recording ring and stream buffers | 1 MiB | **1.13 MiB**: four 256 KiB `files::Stream` rings from the heap (streaming, 2026-11-22), plus eight 16 KiB frame fifos in the engine (bss). The take is not here: it is `Capture`'s own 5.49 MiB. `tests/test_memory.cpp`: init is now **102.2 MiB** of the heap, 17.8 MiB spare |
| App image, voice pools, UI | 4 MiB | **396 KiB** of text, 52 bytes of data and **1.21 MiB of bss** in the linked ELF (`tic6x-elf-size`). The bss is mostly the live Project at 1.03 MiB plus the undo stack; the 85 MiB above is heap, taken in init() and not in the image |
