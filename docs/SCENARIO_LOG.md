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
| 2026-11-28 | `dacb1f1` (PR #261, the harness fixes) | cl6x 66 of 66, GCC-only 65 of 66 | worst **35.40%** (`genload`) | worst **64.41%** (`genload`) | cl6x on Roland's PC at `-j 8` in 309 s; GCC-only in a 4-core cloud session with Enaga's prebuilt GCC 13.3, where `m0` idled at 5.08% over its 5% line (now 6% for GCC-only) |
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

2026-11-28, PR #261 at `dacb1f1` (`main` at `8c3ae12` plus the harness fixes), Enaga v0.4.0. cl6x on
Roland's PC at `-j 8` (309 s); GCC-only in a cloud session with Enaga's prebuilt GCC 13.3 toolchain
at `-j 4` (474 s). Peaks repeat exactly from run to run on the same build.

Most scenarios play the factory beat (A01) and read the same 23.84% / 45.54%: the figure that
differs is the cost of what the scenario adds. GCC-only, `genload` (64.41%) and `energy`
(62.05%) are over 60%; their scenarios hold that build to a 70% tripwire, a regression guard
only.

| Scenario | cl6x | GCC-only |
| --- | --- | --- |
| `ambient` | 25.84% | 55.55% |
| `arp` | 23.84% | 45.54% |
| `arrlaunch` | 29.95% | 46.13% |
| `arrrec` | 23.99% | 46.83% |
| `asr` | 23.84% | 45.54% |
| `audition` | 6.30% | 6.58% |
| `auditionplay` | 26.17% | 45.54% |
| `bounce` | 25.84% | 46.01% |
| `choke` | 23.84% | 45.54% |
| `chorus` | 24.20% | 48.01% |
| `clear` | 23.84% | 45.54% |
| `click` | 23.24% | 46.30% |
| `controls` | 23.84% | 45.54% |
| `duck` | 23.84% | 45.54% |
| `energy` | 34.57% | 62.05% |
| `factory` | 23.84% | 45.54% |
| `feel` | 23.84% | 45.84% |
| `fm` | 25.37% | 49.92% |
| `freeze` | 25.70% | 45.76% |
| `gen` | 26.70% | 48.72% |
| `genload` | **35.40%** (the worst) | **64.41%** (the worst) |
| `glide` | 23.84% | 45.54% |
| `grain` | 24.93% | 52.22% |
| `jamrec` | 25.77% | 46.81% |
| `killeq` | 25.93% | 49.17% |
| `kits` | 23.84% | 45.54% |
| `launch` | 24.52% | 46.11% |
| `lfo` | 23.84% | 45.54% |
| `linein` | 24.45% | 45.72% |
| `liverand` | 24.97% | 44.44% |
| `locks` | 23.84% | 45.78% |
| `m0` | 4.61% | 5.08% |
| `m1` | 23.84% | 45.54% |
| `m3` | 22.88% | 40.76% |
| `m4` | 14.70% | 27.31% |
| `m5` | 23.84% | 45.54% |
| `m6` | 23.84% | 45.54% |
| `m7` | 23.84% | 45.54% |
| `midi` | 23.28% | 41.79% |
| `modslots` | 23.84% | 45.54% |
| `moveall` | 23.84% | 45.54% |
| `osc` | 23.84% | 45.54% |
| `pad` | 23.97% | 45.54% |
| `pictures` | 24.43% | 51.65% |
| `playmode` | 23.84% | 45.54% |
| `plock` | 23.84% | 45.82% |
| `projects` | 25.27% | 45.54% |
| `queuehold` | 23.84% | 45.54% |
| `ratchetspan` | 27.65% | 49.25% |
| `recmode` | 23.84% | 45.54% |
| `samples` | 23.84% | 45.54% |
| `sections` | 25.84% | 45.90% |
| `shortcuts` | 25.77% | 46.81% |
| `slots` | 26.25% | 45.85% |
| `sounds` | 25.17% | 44.76% |
| `stepkeys` | 23.84% | 45.54% |
| `tempo` | 23.96% | 45.54% |
| `transpose` | 23.92% | 45.54% |
| `triplets` | 25.92% | 46.40% |
| `tune` | 23.84% | 45.54% |
| `usersounds` | 23.84% | 45.54% |
| `varispeed` | 32.80% | 57.55% |
| `visuals` | 25.27% | 46.01% |
| `wave` | 26.79% | 50.30% |
| `wavescan` | 25.70% | 45.56% |
| `width` | 26.87% | 51.31% |
