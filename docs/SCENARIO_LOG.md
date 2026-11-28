# Scenario log

Every full run of `tools/scenarios.sh` on Roland's PC, newest first: the commit, the compiler,
how many passed and the worst peak. `docs/HANDOFF.md` keeps one line pointing here. What a change
costs, and why, stays in `docs/BUDGET.md`; this file only says what each full pass read.

All figures are the emulator's peak load per 64-frame block. The emulator models no caches or DDR
latency, so they compare builds and nothing else; the unit reads higher (`docs/BUDGET.md`, "GENERATE
on every track"). The target is a worst case under 60% with cl6x, the build that ships. GCC-only
figures are a regression guard.

## How to add a run

1. In your own worktree, `tools/scenarios.sh -j 4` with cl6x, then again with `ENAGA_TI_CGT=none`
   and its own `ENAGA_BUILD` (CLAUDE.md, "Running commands").
2. Add a row at the top of "Full passes" (a full run prints it at the end, ready to paste: fill
   in the other compiler's column and the notes), and replace "The latest full pass" with the new
   per-scenario figures.
3. A failing scenario is named in the row, with the reason if known. A run of a few named
   scenarios is not a full pass: its figures go to `docs/BUDGET.md` with the change they measure.

## Full passes

| Date | Commit | Scenarios | cl6x | GCC-only | Notes |
| --- | --- | --- | --- | --- | --- |
| 2026-11-28 | `db96b69` (after #228, Track Launch) | cl6x 54 of 55, GCC-only 55 of 55 | worst **34.99%** (`genload`) | worst **64.19%** (`genload`) | `projects` failed with cl6x only: the rename's copy found the 4 MiB card full; fixed by an 8 MiB card (`3800908`), which passes with both compilers at the same peaks |
| 2026-11-27 | `main` with chorus and width | 50 of 50 | worst **34.20%** (`genload`) | not run | chorus (send C) and stereo width in |
| 2026-11-27 | `46cc054` (v0.5.0) | 47 of 47 | worst **26.34%** (`ratchetspan`) | worst **55.06%** (`ambient`) | the release candidate build; `docs/BUDGET.md`, "The v0.5.0 release candidate build" |
| 2026-11-27 | Enaga v0.4.0 pin | 47 of 47 | worst 30.05% (`wave`) | not run | |
| 2026-11-27 | VA voice's fused back half | 47 of 47 | every VA scenario 1.4-2.0 points lower | not run | |
| 2026-11-27 | the second review's fixes | 46 of 46 | | not run | |
| 2026-11-27 | the LONG delay in passes | 45 of 45 | worst 30.42% (`wave`) | not run | |
| 2026-11-26 | choke groups | 42 of 42 | worst 32.26% (`ambient`) | not run | |
| 2026-11-25 | live visuals part 1 | 36 of 36 | worst 30.93% (`ambient`) | not run | |
| 2026-11-25 | LFO TRIG, TUN glide | 33 of 33 | worst 31.05% (`ambient`) | not run | |
| 2026-11-25 | play modes per track | 30 of 30 | | not run | |
| 2026-11-25 | one filter on the synths | 28 of 28 | | not run | |

Before 2026-11-25 the scenarios were run a few at a time; those figures are in `docs/BUDGET.md`.

## The latest full pass

2026-11-28, `main` at `db96b69` (#228, Track Launch merged), Enaga v0.4.0, `-j 4`. Image with
cl6x: payload 1,691,888 bytes, bss 1,670,656 (`APP.ENA` 1,691,952; GCC-only 1,660,336). Peaks
repeat exactly from run to run on the same build: `factory` read 23.62% / 45.36% in two runs each.

Most scenarios play the factory beat (A01) and read the same 23.62% / 45.36%: the figure that
differs is the cost of what the scenario adds. GCC-only, `genload` (64.19%) and `energy` (61.67%)
are over 60%; their scenarios hold that build to a 70% tripwire, a regression guard only.
`freeze` prints windowed `ld` lines instead of a peak.

| Scenario | cl6x | GCC-only |
| --- | --- | --- |
| `ambient` | 25.47% | 55.38% |
| `arp` | 23.62% | 45.36% |
| `asr` | 23.62% | 45.36% |
| `audition` | 6.34% | 6.48% |
| `auditionplay` | 25.50% | 45.36% |
| `bounce` | 25.99% | 45.82% |
| `choke` | 23.62% | 45.36% |
| `chorus` | 24.07% | 47.84% |
| `clear` | 23.62% | 45.36% |
| `click` | 23.27% | 45.85% |
| `controls` | 23.62% | 45.36% |
| `duck` | 23.62% | 45.36% |
| `energy` | 34.62% | 61.67% |
| `factory` | 23.62% | 45.36% |
| `feel` | 23.62% | 45.66% |
| `fm` | 24.68% | 49.74% |
| `freeze` | no figure | no figure |
| `gen` | 26.85% | 48.45% |
| `genload` | **34.99%** (the worst) | **64.19%** (the worst) |
| `glide` | 23.62% | 45.36% |
| `grain` | 24.37% | 52.04% |
| `jamrec` | 25.90% | 46.56% |
| `killeq` | 25.55% | 49.00% |
| `kits` | 23.62% | 45.36% |
| `launch` | 24.68% | 45.92% |
| `lfo` | 23.62% | 45.36% |
| `liverand` | 25.24% | 44.26% |
| `locks` | 23.62% | 45.58% |
| `m0` | 4.40% | 4.87% |
| `m1` | 23.62% | 45.36% |
| `m3` | 22.70% | 40.58% |
| `m4` | 14.52% | 27.12% |
| `m5` | 23.62% | 45.36% |
| `m6` | 23.62% | 45.36% |
| `m7` | 23.62% | 45.36% |
| `midi` | 23.06% | 41.57% |
| `modslots` | 23.62% | 45.36% |
| `moveall` | 23.62% | 45.36% |
| `osc` | 23.62% | 45.36% |
| `pad` | 23.77% | 45.36% |
| `pictures` | 24.21% | 51.46% |
| `playmode` | 23.62% | 45.36% |
| `plock` | 23.62% | 45.62% |
| `projects` | 25.41% (FAILed CARD FULL; passes on an 8 MiB card, same peak) | 45.36% |
| `ratchetspan` | 27.79% | 48.96% |
| `recmode` | 23.62% | 45.36% |
| `samples` | 23.62% | 45.36% |
| `slots` | 23.62% | 45.36% |
| `sounds` | 24.28% | 44.58% |
| `stepkeys` | 23.62% | 45.36% |
| `triplets` | 25.40% | 46.19% |
| `tune` | 23.62% | 45.36% |
| `visuals` | 25.41% | 45.82% |
| `wave` | 26.18% | 50.03% |
| `width` | 26.68% | 51.13% |
