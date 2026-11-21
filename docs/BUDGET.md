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
names the three files. (The "Toolchain status" table above predates that install and still reads
"not installed" — it needs correcting by whoever owns it. The *second* open question there,
whether TI's licence permits redistributing cl6x output in a released `APP.ENA`, is untouched by
this work and still needs the owner's reading.)

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

### Later milestones

| Milestone | Target | Measured (cl6x) | Measured (GCC) | Date |
| --- | --- | --- | --- | --- |
| M1, 4-track beat at 130 BPM | under 15% | | | |
| M2, 8 tracks worst case | under 40% | | | |
| M6, everything worst case | under 60% | | | |

## Memory

Budgeted in `docs/SPEC.md` at about 76 MiB of the ~124 MiB app heap. Nothing measured yet; the
skeleton allocates nothing.

| Use | Budget | Measured |
| --- | --- | --- |
| Samples, int16 (32 MiB default, 64 MiB ceiling) | 32-64 MiB | |
| Beat repeat ring (2 bars at 60 BPM, stereo float) | 2.93 MiB | |
| Delay and reverb buffers | 2 MiB | |
| Patterns, kit, scenes, 16 undo levels | 2 MiB | |
| Recording ring and stream buffers | 1 MiB | |
| App image, voice pools, UI | 4 MiB | |
