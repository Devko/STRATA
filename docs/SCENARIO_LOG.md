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
2. Add a row at the top of "Full passes", and replace "The latest full pass" with the new
   per-scenario figures.
3. A failing scenario is named in the row, with the reason if known. A run of a few named
   scenarios is not a full pass: its figures go to `docs/BUDGET.md` with the change they measure.

## Full passes

| Date | Commit | Scenarios | cl6x | GCC-only | Notes |
| --- | --- | --- | --- | --- | --- |
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

Not yet recorded per scenario.
