# STRATA CPU and memory budget

Measured figures only. Estimates belong in `docs/SPEC.md`; this file records what was actually
observed, and every entry says where it was measured. **Only the unit's numbers count** — the
emulator models no caches or bus latency, so its figures compare versions and nothing else.

One 64-frame block at 372 MHz is **496,000 cycles**. Every share below is a share of that.

## Toolchain status

| | |
| --- | --- |
| tic6x-elf GCC | 13.3.0, built from source by `tools/build_toolchain.sh` into `~/strata-toolchain/tic6x` |
| TI cl6x | **not installed** — `ENAGA_TI_CGT` falls back to `none`, so `*.ti.c` and Enaga's DSP library build with GCC |
| Current build | **GCC-only** |

### The cl6x question — open, and it needs a decision

cl6x is worth about **2.2×** on DSP-heavy code: Enaga's own synth example measures 30% of the
audio budget built with cl6x and 65% with GCC alone. STRATA's 8-tracks-under-60% target is not
reachable on a GCC-only build, so this is not an optimisation detail — it decides whether the
headline target is achievable.

Two things are unresolved and neither is Claude's to settle:

1. **Installing it.** TI's C6000 code generation tools are a download that requires accepting
   TI's licence agreement. That acceptance is the owner's, not an agent's, so cl6x has not been
   installed. Once `~/ti/ti-cgt-c6000_*` exists, Enaga's `paths.py` picks it up automatically
   and `tools/env.sh` stops forcing `none`.
2. **Redistributing what it builds.** A released `APP.ENA` would contain cl6x-compiled code.
   Whether TI's terms permit distributing that output needs reading before the architecture
   leans any further on the 2.2×. Enaga upstream has the same exposure, so it is worth raising
   there too.

Until both are settled, record **every** load figure twice — with cl6x and with
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

### M0.5 — budget spike (the fixed per-block tax)

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
