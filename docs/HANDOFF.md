# Handoff — 2026-11-27

**Read this first, then `CLAUDE.md`, then `docs/SPEC.md` and `docs/PLAN.md`.** The platform review
the spec was corrected against is `docs/archive/REVIEW.md`: history, but its decisions stand.
This file holds what exists nowhere else in the repo: the state in one table, the next steps in
order, and the knowledge that lives in no code comment, commit message or other document. When a
fact here becomes stale, fix it here or delete it; do not append a history.

**STRATA runs on the unit and has five releases.** v0.1.0 (2026-11-22), v0.2.0, v0.3.0 and
v0.3.1 (2026-11-23, early betas for Enaga 0.2.0) and v0.4.0 (2026-11-26, the first for Enaga
0.3.0, and the first zip without Enaga's firmware) are on GitHub; v0.3.1 boots and plays on the
unit (2026-11-24), and a build from `main` after #139 boots on the unit's 0.3.0 firmware
(2026-11-25). Nothing from 0.3.x on has been measured or walked through there yet. On the unit the default beat loads under 30%, and a long, heavy session peaked at
84.3% MAX with no dropouts. Almost everything added since the first evening has been heard by
nobody: the walkthrough at the top of `docs/UNIT_CHECKLIST.md` is the next pass. **v0.5.0 is
planned as the release candidate for 1.0** (Roland, 2026-11-27): `docs/PLAN.md`, "Release
candidate", has what must land first, the feature freeze after it, and the checks.

---

## Where it stands in one table

| | |
| --- | --- |
| `make -C host test` | **3,880,300 checks, 0 failures** on `main` at PR #199 (2026-11-27): `core/`, `dsp/` and all of `app/`. The same under ASan and UBSan, with `ASAN_OPTIONS=detect_leaks=0` |
| CI | `.github/workflows/ci.yml` on every PR: the suite with GCC, again under ASan and UBSan at -O1 (`make -C host test SANITIZE=1`), the layout check, the screenshots, `tools/audit_commands.sh`, `tools/check_rules.sh`, `tools/enaga_check.sh`, `tools/check_scripts.sh` (every script in tools/ and tests/ parses) and the card builder's tests (`web/`). Both builds use ccache. A docs-only PR skips the test jobs. `tools/ci_local.sh` runs the same lanes in WSL. `enaga-upstream.yml` runs `enaga_check.sh --upstream` weekly and opens an issue when upstream has moved. No device build or scenario runs in CI |
| `tools/build.sh` | links. Last measured image (cl6x, 2026-11-25, sample audition merged onto #168): payload 1,224,160 bytes, bss 1,605,248 (with the live screens, bounce to track, step stutter, EDIT's pictures, LOCK and play modes); no BOOT / MCU / SPI sections. `tests/test_memory.cpp` reads these two figures from here, so update both together |
| `tools/scenarios.sh` | **47 scenarios**, run on Roland's PC only. **All 47 pass with cl6x with KICK's spent click skipped** (2026-11-27, Enaga v0.4.0; the default beat 24.06% -> 23.16%, worst 26.34% `ratchetspan`; output within -129 dB, docs/BUDGET.md). **All 47 pass with cl6x with the hat's fused back half and WAVE's mip table** (2026-11-27, PR #206, on Enaga v0.4.0; worst 27.45% `ratchetspan`, `wave` 27.94% -> 26.62%; docs/BUDGET.md). **All 47 pass with cl6x on Enaga v0.4.0** (2026-11-27, main `ebc8428`; worst 30.05%, `wave`; every figure 0.5-1.1 points under v0.3.0, docs/BUDGET.md; merged onto #203, all 47 pass again with cl6x, worst 28.10% `ratchetspan`). **All 47 pass with cl6x with the VA voice's fused back half on** (2026-11-27; the default beat 26.60% -> 25.12%, worst 29.20% `scenario_gen`; docs/BUDGET.md). **All 47 pass with cl6x with the HALL in passes** (2026-11-27; worst 30.79%, `scenario_wave`; `ambient` 30.67% -> 28.60%; an emulator render A/B of a change that moves load is not an oracle any more, docs/BUDGET.md "The HALL in passes"). **All 45 pass with cl6x with the LONG delay in passes** (2026-11-27, PR #196, before GEN and EVOLVE came in; worst 30.42%, `scenario_wave`; `ambient` 32.33% -> 30.19%, docs/BUDGET.md). `tools/profile_guest.py` gives any scenario's guest profile by function. **All 47 pass with cl6x with GEN and EVOLVE merged onto #184-#193** (2026-11-27; `scenario_gen` new, Python-driven because the track menu needs a 500 ms hold of OK and `--key` holds 54 ms; worst 32.28%, ambient). `scenario_gen` evolves all eight tracks every pass: 30.20% (29.83% before the merge), against 29.65% for the generated part alone; turning all eight on the boundary itself had peaked at 65.90%, which is why `Engine::evolve_lead` turns one track a tick. **All 45 pass with cl6x with arp depth, ARPS and the new sounds (#184) merged onto #183** (2026-11-27; `scenario_arp` new; worst 31.97%, ambient; #185 and #187 merged after that run). **All 44 pass with cl6x with ratchets over LEN** (2026-11-26; `scenario_ratchetspan` new; worst 31.91%, ambient), before #183's `scenario_recmode` came in; after the merge `ratchetspan` and `recmode` were run again and pass. `scenario_recmode` (2026-11-26, #183, chords recorded note for note) walks the FUNC menu's REC MODE row with REC on: 26.57% cl6x, run with `choke`, `factory` and `m6`, which still pass; the emulator cannot play a chord into a step, so the recording itself is host-tested (`polyrec_*`). **All 43 pass with cl6x with CLICK and COUNT-IN** (2026-11-26; `scenario_click` new; worst 32.29%, ambient). All 42 pass with cl6x with choke groups (2026-11-26; `scenario_choke` new, pushes C and D on MIXER's TRK 1-4; worst 32.26%, ambient). Live visuals part 5 (WAVE's waterfall, GRAIN's sparks; `scenario_wave` ends on WAVE POLY's SRC page) passed all 36 of its day with cl6x before this merge; merged onto #169, all 41 pass with cl6x (worst 32.20%, ambient). Part 6 (MIXER's spectrum, vectorscope, fader meters; `scenario_visuals` runs twice, the second on MST) passed all 41 with cl6x (31.82%), and so did the screens rebuilt to the mockups on top of it (figures in docs/BUDGET.md). `tools/emulator_frames.py` records the real screen frame by frame. **All 41 pass with cl6x with sample audition (#169) merged onto MOD slots (#168)** (2026-11-25; `scenario_audition` new, the first to check the captured output: a 44.1 kHz tone streamed from the card is heard whole, in tune, at -6 dB; worst 31.82%, ambient, as on main). `scenario_modslots` (MOD slots) learns slot 1 by push C, `--turn` and OK; all 40 pass with cl6x on `main` after #167 (worst 31.82%, ambient). All 39 pass with cl6x with random lock ranges and parameter slide (#167, `scenario_locks` new) merged onto LIVE RND (#166); worst 31.69%, ambient. `scenario_liverand` (LIVE RND, #166) drives FUNC + turn D and FUNC + push D: a `--turn` 2 M cycles after a FUNC `--key` lands inside the key's 20 M-cycle press (26.99% cl6x, 46.07% GCC-only, run alone; the full set was not rerun with it). All 37 before it pass with cl6x with the review fixes after #163 (`scenario_moveall` new; worst 31.59%, ambient, within the half point a peak moves between runs). All 36 pass with cl6x with live visuals part 1 (`scenario_visuals` new: draw() 0.57 ms a frame on GRID; worst 30.93%, ambient), and with part 2's audio taps (worst **31.16%**, ambient; GCC-only 56.48%), and with part 3's GRID and PERFORM views (`scenario_visuals` logs draw() on PERFORM: 1.34 ms), and with part 4's LFO picture (`scenario_visuals` walks on to EDIT's LFO page and turns DEP and DST with `--turn`). All 35 pass with cl6x with LFO TRIG, TUN glide and the cutoff floor merged onto #152 (`scenario_lfo` now pushes TRIG; worst 30.93%, ambient). **All 35 pass with cl6x with REL ALL, LEN ALL and FEEL merged onto `main` after #154** (2026-11-25; worst 30.75%, ambient; 34 passed on `main` after #153 at 30.55%). Added on 2026-11-25, each with the PR that brought it: `scenario_stepkeys` (D12), `scenario_plock`, `scenario_slots`, `scenario_glide`, `scenario_playmode`, `scenario_pictures`, `scenario_bounce` (the only run of a take through Enaga's real file layer), `scenario_feel` (FEEL walked on the PATTERN page). `scenario_moveall` turns REL ALL and LEN ALL with `--turn` (the review PR after #163). The pad gestures (stutter, LOCK, D6) have none: the emulator cannot press pads. The worst case moved from 30.16% to 30.75% over the day. GCC-only figures for that day's work are not taken |
| unit | v0.3.1 on Enaga 0.2.0 boots and plays (2026-11-24). Load figures are from the v0.2.0-era build: under 30% on the default beat, 84.3% MAX in heavy play. Nothing after #111 has been measured or walked through on the unit |
| memory | init takes **97.32 MiB of a 120 MiB heap** (LOCK's 1.06 MiB and the audition's 0.28 MiB included): 22.68 MiB spare against an 8 MiB headroom rule. `tests/test_memory.cpp` itemises it. Never checked on the unit |
| milestones | M0–M7 code-complete; every load and feel exit criterion waits for the unit (`docs/PLAN.md`) |
| VA POLY | 2.16 points per voice with cl6x, 3.82 GCC-only (emulator). **A shared budget of 12 voices across all tracks** (`Engine::kPolyVoiceBudget`), WAVE POLY included, provisional until the unit's readings |
| Enaga | pinned at release **v0.4.0 (`7487bbf`)** since 2026-11-27 (v0.3.0 from 2026-11-25); the unit must run Enaga 0.4.0 firmware. `tools/enaga_check.sh` passes |
| branches | work lands on `main` through PRs; merges need Roland's go-ahead in the thread |

## What to do next, in order

0. **The release candidate**, `docs/PLAN.md` "Release candidate": file format option C and the
   HALL question first (EVOLVE saved, #196 and #197 are in), then the feature freeze. No
   thread tags or releases on its own: the release runs from its own thread when Roland says so.
1. **On Roland's PC: the load runs still owed.** `tools/scenarios.sh` once on `main` (it has
   `scenario_wave.sh` and `scenario_fm.sh`, never run), then E13 (the VA rework), E14 (WAVE's cost
   per voice and its boot time) and E15 (FM4's cost per voice) from `docs/BUDGET.md`, with cl6x and with `ENAGA_TI_CGT=none`. BUDGET has only
   host estimates for both so far.
2. **On the unit: the walkthrough** at the top of `docs/UNIT_CHECKLIST.md`, S1-S20 and then W1-W54, on a
   build from `main` with cl6x and Enaga 0.4.0 firmware. It covers everything since v0.1.0, from
   the CPU guard to LOCK, step stutter, play modes, the MOD slots, sample audition and the
   redrawn screens (#171-#176).
3. **Write the readings down where they belong**: unit rows in `docs/BUDGET.md`, ticks in the
   reference half of `docs/UNIT_CHECKLIST.md`, the Unit column of `docs/FEATURES.md`, and anything
   odd as an issue or a thread.
4. **Settle the provisional numbers** from those readings: `kPolyVoiceBudget = 12`, the CPU
   guard's thresholds (`core/cpuguard.hpp`: rise at a 70% average or one 90% block, fall after a
   second under 55%), and the performance mixer's worst case (host estimate: about +2 points).
5. **A new machine** needs its rows in `docs/FEATURES.md`, `docs/MANUAL.md` (section 3 and the
   SOUNDS table), `docs/CHEATSHEET.md` (machine table and abbreviations), `CHANGELOG.md` and the
   machine list in `docs/OVERVIEW.md` and the README highlights. STRING is postponed.
6. **Then, from the reference half of `docs/UNIT_CHECKLIST.md`**, the checks that decide whether a
   milestone stands: a start with no card, pulling the card during a save and during sample
   loading, swapping a sample on a sounding track, the M5 30-minute jam, and M7's hour locked to
   external MIDI clock.

**The UI class is split over files** (2026-11-25): `app/app.hpp` holds the helpers, `init`, the
private helpers and all the state, and includes sixteen `app/ui_*.hpp` fragments inside the class
body (input, gestures, lights, live, screen, edit, editview, listview, pickview, modslots, mixer,
mixview, browsers, perfmix, perform, project). What each screen draws, row by row, is docs/SCREENS.md. They are not
headers of their own. `tools/audit_commands.sh` reads all of them.

**Controls.** docs/CONTROLS.md is the reference the key handlers follow; change a gesture there
first, then in the code, `core/help.cpp`, the cheatsheet, the manual and the unit checklist.

**Phase 2 of the backlog** (Claude Doc "STRATA new approach: review, overlay and backlog";
Roland accepted all 15 decisions as recommended, 2026-11-25). Done: macro learn by turning
(D11), STEP keys as steps (D12), hold STEP + pad edits the step (D6), the retired chords and
slots 7 MUTE BUS / 13 MORPH (D1, D2, D5), LOCK on FUNC + MUTATE (D3, D4, D13, D14;
`core/lockstore.hpp`, docs/CONTROLS.md "LOCK") with FX GLIDE and the momentary push, bounce to
track (a take to FILE offered to a SAMPLE or GRAIN track, one OK; docs/CONTROLS.md "Bounce to
track") and hold STEP + THROW (the delay-send lock). **Phase 2 is complete.**

**RESAMPLE had never written a take on Enaga** until 2026-11-25: Storage made `/RECORD` first, and
Enaga writes only strictly inside `/RECORD`, so every take failed `DENIED`. Fixed, and
`tests/fakehost.cpp` now refuses writes outside the data folder, `/SAMPLES` and `/RECORD` as the
firmware does (`write_check`; `seed_folder` lays out a PC-made card). No take has been heard on the
unit yet: UNIT_CHECKLIST's resample items are the first real test.

**Phase 5 item 5** (CyDrums P4 and P8): REL ALL and LEN ALL on FUNC + turn A / B in PERFORM
(`core/moveall.hpp`; live, rest 0 with a detent, in a LOCK, `RL`/`LN` in the live strip) and FEEL
on the PATTERN page (`core/timing.hpp` feel_offset, pattern file section 5, D turns it).
`scenario_moveall` turns REL/LEN ALL in the emulator (`move_all_notice` logs each click);
`scenario_feel` walks FEEL. UNIT_CHECKLIST W29 walks both.

**LIVE RND** (CyDrums P1's live half, `core/liverand.hpp`, docs/CONTROLS.md "LIVE RND"): FUNC +
turn D in PERFORM sets a unit (OFF, 1-16) for the armed tracks, FUNC + push D arms or disarms the
selected one (all eight at start). It lives in `PerformState` (`rnd_unit`, `rnd_armed`) and the
scheduler asks `live_play_mode` for each track's mode every tick, so it reuses TRACK PLAY's RND
draw and never writes the pattern; the recorder asks the same function. In a LOCK (`LiveState`
grew two bytes), not saved. Default armed set is all eight: Roland may prefer the kick out by
default. `scenario_liverand` walks it; UNIT_CHECKLIST W36.

**Phase 5, sound-design depth** (same doc, CyDrums S2/S3). Done: LFO TRIG (FREE, RETRIG,
ONE-SHOT), TUN gliding on the synths, D9's leftover (the ~100 Hz floor for the morph and the
macros, LFO depth on a cutoff about +-3 octaves), and the two knob-learned MOD slots per track
(below).

MOD slots, for whoever touches them next (core/modslot.hpp has the rules):
- **Not parameters**, for TRIG's reason: three bytes a slot in `KitTrack::mod`, KIT.BIN's
  optional section 10, set whole by `Cmd::set_mod_slot` (sanitized as a file's slot is). No lock,
  scene, morph or macro moves them.
- **Applied in `run_mod_slots`, after the LFO**, onto `modulated_`; a slot keeps `modulating_`
  on, so `lfo_on()` now reads `lfo_running_` (the LFO's own DEP and DST), not `modulating_`. A
  slot on LFO runs the LFO though its DEP is 0. ENV is a second `TrackLfo` per track
  (`mod_env_`) on the LFO's shape and rate. VEL comes through `note_started(track, velocity)`,
  PRS through `Cmd::set_pressure` (posted only for a track with a slot on PRS), RND from its own
  seeded `Random` per track.
- **Destinations** are the sound parameters, ASR and OSC (`mod_dest_allowed`): not the LFO's
  (its own source) and not the arp's (read when a note is scheduled, from `effective_`).
- **The learn** is `app/ui_modslots.hpp`, MACRO LEARN's gesture for one knob: push C / D on the
  LFO page. The last knob turned that moved is the slot's; every touched knob goes back.
- `scenario_modslots` learns a slot by the gesture. A
  `--key` landing while a `--turn` is still clicking was not seen by the app in one run; leave a
  turn of 20 detents about 300 M cycles before the next key.

**CLICK and COUNT-IN** (CyDrums S6, docs/CONTROLS.md "CLICK and COUNT-IN"): dsp/click.hpp is a
recursive-sine click (loop in dsp/clickloops.ti.c) mixed after RESAMPLE's tap, beside the
audition. `Engine::metronome_tick` fires it from the clock's own ticks. A count-in is
`Cmd::count_in`, posted BEFORE `enaga::clock::start()` (and taken back if the start is refused):
the clock starts at once and `count_in_end_` ticks are subtracted from every tick the sequencer,
the rolls, the stutter and the arps see, from `Status::tick` and from the recorder's stamps
(`Status::count_in_ticks`). A STOP (seen as running going false) or a restart ends it. Host tests
in tests/test_click.cpp and test_ui; `scenario_click`; UNIT_CHECKLIST W47.

**Ratchets over LEN** (Roland's RATE + span flag, 2026-11-26; core/pattern.hpp kRatchetSpan): bit 7
of `Step::ratchet` makes the low nibble a rate (kRatchetRateTicks) and the ramp bits 4-6 run
across the roll. The scheduler emits the first hit with `StepEvent::span_ticks/span_hits`;
`Engine::span_start` (from `fire`) keeps the run and `span_tick` hits it on the clock's ticks,
each hit through `fire` with ratchet_index 1. Every place that drops deferred ratchet hits also
calls `span_clear`. The step editor's RTCH walks 1-8 and then the eight rates. Recording a note
repeat as a roll (P3) is meant to write exactly this byte. tests/test_ratchetspan.cpp,
`scenario_ratchetspan`, UNIT_CHECKLIST W49.

**Choke groups** (CyDrums S4, docs/CONTROLS.md "Choke groups"): `KitTrack::choke`, KIT.BIN's
optional section 11. `Engine::note_started` now takes the note's frame and calls `choke_group`,
which marks the other tracks of the group; after each such track renders, `run_choke` fades its
whole output from that frame over 64 frames (`strata_choke_fade` in dsp/chainloops.ti.c, the fade
carrying into the next block) and then `kill_all`. It is a fade on the TRACK, not a voice, so
every machine chokes alike, and a block renders whole: a note on the cut track at or after the
choke's frame in the same block cancels the choke, and two tracks of a group on one frame both
sound. The HAT machine's own track 3 to 4 pair (`rewire_machines`) is untouched. Host tests in
tests/test_choke.cpp; UNIT_CHECKLIST W46.

**Sample audition** (backlog Phase 5, "sample audition in the browser"): push A in SAMPLES
streams the file under the cursor to the output (dsp/audition.hpp; app/storage.hpp "sample
audition" has the protocol). It is a fifth Enaga stream with its own 256 KiB ring, beside the job
machine, so it never waits for a load and takes none of the four track rings. Played at the file's
own rate with Catmull-Rom, after every effect and the RESAMPLE tap, at a fixed -6 dB. A late card
holds its place rather than skipping. `scenario_audition` is the one scenario that checks the
captured output (the emulator keeps only the run's last 0.68 s, hence its short run). UNIT_CHECKLIST
W36 walks it; the level against a real mix is the open question.

Arp depth and ARPS, for whoever touches them next (core/arp.hpp has the design):
- **VAR, RHYTHM, ACCENT and RATCHET are not parameters**, for TRIG's reason: four table indices a
  track in `KitTrack::arp`, KIT.BIN's optional section 12 (11 is the choke groups', PR #181), set
  by `Cmd::set_arp_ext` (push A-D on the ARP page) or whole by `Cmd::arp_preset` (ARPS, OK on the
  ARP page; core/arppresets.hpp). The tables' indices are on disk: new entries go on the end.
- **A rest does not advance the note** (`ArpRun::slot` counts the grid, `hit` the notes).
- **Ratchet repeats are frame countdowns** (`Engine::arp_ratchets`, after the deferred step hits
  and before the block's ticks, running or stopped), clamped so no two hits of a track land in
  one block: the one-pending-trig contract `dispatch` relies on.
- A kit-library recall brings the arp settings, and since #188 LFO TRIG, the MOD slots, SLC
  AUTO and the choke group too: every optional KIT.BIN section. A new kit section must be added
  to the recall as well (`Cmd::kit_recall`).

LFO, for whoever touches it next (dsp/lfo.hpp has the detail):
- **TRIG is not a parameter.** It is `KitTrack::lfo_trig` in KIT.BIN's optional section 9, set
  by `Cmd::set_lfo_trig` (push B on the LFO page). All 32 bytes a track has are spoken for. Since
  format decision C (2026-11-26) the masks over them are 64 bits (`Engine::locked_`, the step's
  lock and range masks, the scene morph's), so a 33rd parameter is a new page's optional sections
  in KIT.BIN and PERFORM.BIN, not a rewrite. Watch the lock entry's param field: ids 32-36 are
  already POLY's extra chord notes (#183), so a new page needs ids past them or a new lock kind.
- **Every note goes through `Engine::note_started`** (the trig light and TRIG's restart). A new
  way to start a note must call it, not `status_.bump_triggered`.
- **WAV and DST are numbers** (format decision C, 2026-11-26): WAV 0-5, DST 0 OFF or 1-16, with
  `kit_param_max` (core/project.hpp) the highest a knob, a lock or a random range may set. A new
  shape is a new enum value and a bigger maximum, nothing else. Every file that stores the bytes
  marks its layout (KIT.BIN 13, PERFORM.BIN 8, a pattern 6) and an unmarked one is converted once
  on read with `kit_lfo_from_band`. An older build reads the numbers as low bands (SIN onto OFF,
  mostly): the release notes must say so.
- **TUN on VA and WAVE is a float** (`Machine::set_pitch_mod`, added where the voice caches its
  oscillator increments); every other machine and every other destination still moves the byte.
  FM4 and GRAIN could take it the same way.
- **The cutoff floor** lives in `apply_performance` for the morph and the macros and in
  `run_lfos` for the LFO, both through `lfo_cutoff_param` / `lfo_cutoff_floor`. A step lock and
  the kit are the knob's and are not floored.

LOCK, for whoever touches it next: the audio side keeps the 128 locks and the swap-back and does
the copies (`Engine::capture_live` / `apply_live`); the UI only times the hold (500 ms), posts
`lock_store` / `lock_tap`, and acts on `Status::lock_event` (notice, a pending MUTATE dropped,
morph and macro shadows re-read, the mixer ridden to rest). The concept's "no lock stored: jump
to the pattern as last saved" is NOT built: it needs a copy of every pattern as loaded; a tap
with no lock says `NO LOCK: HOLD STORES`. Undo is untouched by a jump. The emulator cannot press
pads, so LOCK is host-tested only (tests/test_lockstore.cpp, tests/test_ui.cpp); UNIT_CHECKLIST
20c walks it. FX GLIDE lives in the UI (`perf_ride_to`, `core/perffx.hpp` `perf_ride_at`); the
UI tests run with it OFF (`fresh()`), and its own tests turn it on. FUNC + MUTATE, FUNC + push D and
FUNC + VALUE only show a hint now (`notice_logged` in app/app.hpp), so LOCK can take FUNC +
MUTATE outright. The master DJ byte stayed on MIXER's MST page on purpose: an old project saved
with it set must be able to put it back.

Step stutter and the rate (Phase 5, CyDrums P2 P3; docs/CONTROLS.md "REPEAT"), for whoever
touches them next: one rate (`RepeatLength`, UI-owned, posted by `Cmd::set_repeat`) serves beat
repeat, the rolls and the stutter; LEFT/RIGHT walk it by ticks (`repeat_length_step`), VALUE in
the enum's order. The stutter is `Cmd::step_stutter`: the engine silences the masked tracks'
scheduled events and fires `stutter_event` on the roll's grid, while `Scheduler::tick` keeps
running, so the release lands in time. It is all tracks by default, FUNC + STEP the selected one
(the backlog proposed the opposite; Roland may want it flipped). The first STEP turns the audio
beat repeat off for the rest of that REPEAT hold. Not built: a two-STEP loop (two steps held
alternate). A NOTE RPT roll IS recorded now (P3): `record_note(..., roll)` puts a span ratchet byte in `Cmd::record_hit`'s `e` and the pad's release gives LEN through the RecHold path, on any machine. Host-tested only: the emulator cannot press pads.

Random lock ranges and parameter slide (Phase 5, CyDrums; docs/CONTROLS.md "Random ranges and
slides"), for whoever touches them next: both live in the lock's param byte (bit 6 range, bit 7
slide; `core/pattern.hpp`), and every lock function matches by `lock_key`, so a slide never makes a
second lock. A range is its own entry (one of the step's 8 slots), drawn by `lock_spread` from the
seed, track, step, pass and parameter. The scheduler looks ahead for a slid step only
(`StepEvent::glide_*`, in play order); `Engine::start_slides` / `run_slides` glide at control rate
before the LFOs. Hold STEP + FUNC + push no longer clears a lock: it toggles the slide.
`scenario_locks` walks both gestures; UNIT_CHECKLIST W37.

Smaller open items, none urgent:

- **KICK's click path** runs for the whole note after its transient has decayed: about a quarter
  of the host profile. Skipping it is not bit-exact (the shared noise advances differently), so it
  needs a listening test first. `docs/BUDGET.md`, "Optimization review", finding 4.
- **Mutation-test the `app/` tests.** `app/` became host-testable late and has not been held to
  the standard below as thoroughly as `core/` and `dsp/`.
- **Two Enaga reports to file upstream**, each with a failing case: GCC 13.3.0's internal compiler
  error on `core/project.cpp` (below), and the recurrence-bound `enaga_envelope_render` with the
  profile from `docs/BUDGET.md` (finding 5). Roland parked upstream work on 2026-11-23 ("focus on
  STRATA first").
- **Try Enaga v0.2.0's prebuilt GCC toolchain** (in its SDK zip). It is the same compiler
  `tools/build_toolchain.sh` spends 30 minutes building in a cloud session.
- `delay_ping_pong`, `delay_to_reverb` and the LONG delay's low cut share the ROUTE page with one
  empty slot, because nineteen send fields do not divide into four. Fine, but Roland may want
  them placed differently.
- **The sends' ambient modes** (LONG delay, HALL reverb with shimmer; `dsp/ambient.hpp`) are
  owed U9 on the unit (UNIT_CHECKLIST 13j).

---

## What is NOT on file anywhere else

### 1. Where things can run

| Where | Can | Cannot |
| --- | --- | --- |
| **Roland's PC** (`D:\DEV\STRATA`, builds in WSL) | everything a container can, plus **cl6x** (TI 8.5.0.LTS at `~/ti/ti-cgt-c6000_8.5.0.LTS`), and it writes the SD card | — |
| **Cloud session, `strata-toolchain` environment** | host suite; GCC-only device build after `tools/fetch_toolchain_sources.sh` and `tools/build_toolchain.sh` (~30 min); emulator scenarios after `tools/fetch_emulator.sh`; `tools/render.sh`; ASan | cl6x: TI's installer needs an interactive licence click |
| **Any other cloud session** | host suite (`git submodule update --init --recursive` first), `make -C host render` (the engine to WAV, no emulator), `tools/audit_commands.sh`, `tools/enaga_check.sh` | a device build or the emulator: `ftp.gnu.org` is not reachable |
| **Nowhere but the unit** | hearing it, feel, real load (caches, DDR), the heap | — |

So **every cloud load figure is GCC-only**, and a GCC-only figure over 60% means nothing either way.
Quote load as "emulator, cl6x" or "emulator, GCC-only" unless `docs/BUDGET.md` has a unit row.

Cloud-session limits found today: the git proxy refuses deleting a remote branch (403), and the
GitHub tools cannot set the repository's description or topics; both need Roland.

The `strata-toolchain` environment's setup script installs `flex` and clones the emulator with a
bare `git clone`; run `tools/fetch_emulator.sh` after it, which checks out
`STRATA_EMULATOR_COMMIT` from `tools/env.sh`. The emulator is a plain clone, not a submodule.
`tools/fetch_toolchain_sources.sh` exists because GCC's `contrib/download_prerequisites` fetches
GMP, MPFR, MPC and ISL over plain HTTP, which the HTTPS-only proxy does not serve.

### 2. The emulator presses keys and turns encoders, and has no pads

Since Enaga v0.3.0, `tools/emulate.py` takes `--key`, `--turn ENC:DETENTS@CYCLE` (encoders A-D
are 0-3, VALUE 4; a detent takes 12 ms) and `--card-mib`. `scenario_plock`, `scenario_visuals`
and `scenario_moveall` turn encoders; the older scenarios were written before and turn none,
which their headers say. A key is held 20 M cycles, so a turn 2 M cycles after a FUNC press
lands inside the hold (FUNC + turn). There is still no pad entry point, so no scenario can drive
a pad (seven of M5's eight performance tools, pad pressure, LOCK, the stutter, D6), and the
emulator cannot read the screen: a scenario sees only the log. Scenarios prove what keys and
turns reach: that every page **draws** on the C674x with the real font, which matters because a
draw off the canvas is a hard fault on the unit and a silent pass on the host. `app/ui_lights.hpp`
logs `scr <MODE> <PAGE>` on each screen change for this, and `notice_logged` puts a notice in the
log; on the unit the first tells "wedged" from "on a page I did not expect". Enaga's log ring
keeps 16 lines, so a scenario that logs more loses its first lines.

### 3. Why the unit may read higher than the emulator

The emulator models no caches and no DDR latency. Its cl6x default beat is 19.17%
(`scenario_m3.sh`); the unit's LOAD showed up to about 26% at first boot. If MAX confirms that
ratio, every headroom figure in `docs/BUDGET.md` shrinks by it. Do not plan voice counts from
emulator figures alone, and do not take "hardware is faster" from anywhere until MAX says so.
High-pitch GRAIN and SAMPLE are where the gap should be widest (U3).

### 4. `tests/fakehost.cpp` is why `app/` is testable

Enaga's C++ API is header-only: every call is an inline function over `enaga::host()`, one C
struct of function pointers that the app runtime defines. `host/Makefile` does not link that
runtime, so `tests/fakehost.cpp` defines `enaga::host()` instead, and `app/engine.cpp`,
`app/storage.cpp` and `app/app.hpp` compile for the tests exactly as for the device, with no
`#ifdef` in `app/`. The fake adds what the real host cannot: a programmable clock
(`deliver_tick(24, 37)` puts a tick on frame 37 of the next block; `set_running`, `set_source`),
loads that complete only on `complete_loads()`, failure injection (`set_card(false)`, `set_full`,
`set_read_error`, `set_audio_context`), an in-memory card, and `read_prefix()` (because
`read_file` refuses a buffer smaller than the file, as the ABI does).

`tests/apprig.{hpp,cpp}` sits on top: one shared `Engine` and `Storage` (tens of MiB each, so
file-scope, never locals), `fresh()`, `pump()`, `pump_until_idle()`, `settle()`, `post()`,
`post_delta()`. **Traps it has already fallen into**, fixed and commented in place:

- `fresh()` posts `Cmd::stop` with `a = 1` (cut the tails), and clears the performance layer: a
  macro left at full position by an earlier test overrides the kit and makes a knob look dead.
- `settle()` waits for 400 silent blocks: the delay is silent between its repeats. A test that
  leaves a send open never settles, because `Mixer::add_track` restarts the delay's tail.
- A kick's block peak oscillates with its phase against the block. Compare **energy over a
  span**, never one block's peak.
- `tests/test_ui.cpp`'s `fresh()` puts every UI cursor, page and setting back to a fresh start
  (`UiProbe::view_to_defaults`), the factory scenes and macros, and the engine's performance
  layer. Still walk to a named page (`mixer_page(app, n)`) rather than count key presses, and use
  `knob_result` for encoder A on EDIT's first page (TUN, a semitone a click).
- `Cmd::stop` cuts with `a = 1`; `post_cmd`'s first byte is `a`. Both rigs once posted `c = 1`, a
  plain release, and a pad note rang into the next test.
- Thousands of commands without a pump overflow the command ring, and the drops land on the next
  test.

### 5. The standard this code is held to

- **Mutation testing, not just coverage.** A surviving mutation is a missing test or an
  equivalence argued in a comment in the owning file. docs/MUTATION.md has every file's score,
  the equivalent classes and the accepted gaps; dsp/freeze, lfo and tapestop are not run yet.
- **Golden renders pin every machine's sound** (tests/test_golden.cpp). A deliberate sound
  change regenerates them after listening (`STRATA_GOLDEN=print build/host/strata-tests >
  /tmp/g.txt; python3 tools/golden_update.py /tmp/g.txt`), and the diff of
  tests/golden_data.inc goes in the same commit.
- **AddressSanitizer and UBSan over the whole suite** (`make -C host test SANITIZE=1`, also in
  CI), with `detect_leaks=0`: `BeatRepeat::init` allocates its ring with `new[]` and nothing
  frees it, by design.
- **Bit-exactness gates a DSP refactor**, pinned with `==`, not a tolerance. Every optimization
  merged today (prepare-on-change, cached VA increments, the anti-aliasing loop in
  `core/pcmloops.ti.c`) has a test that runs old and new side by side.
- **A feature with no gesture is dead code.** `tools/audit_commands.sh` lists every `Cmd` with a
  poster but no handler, or a handler but no poster. Run it after any change to
  `app/commands.hpp`. Its KNOWN list holds only `none`, the ring's empty slot.
- **Every screen passes the layout test** in `tests/test_ui.cpp`: Enaga's 5x7 font, 21 characters
  a line, and its charset (no `*`, `,` or `_`). First boot showed overlapping text; this is why.

### 6. Decisions in the code not to re-open without reading them

- **The sample swap protocol.** `clear_sample`, then wait for `Status::sample_epoch[track]` to
  change, then release the range, load, `set_sample`. `SamplePool::release` makes memory reusable
  at once and `core/` cannot see whether a voice still reads it; releasing early sounds like a
  broken sampler, not a broken lifetime.
- **Saving never reads a torn step**: drain the command ring first and defer by a frame if it is
  not empty. A load also needs the transport stopped. `Engine::project_for_load()` is the one
  deliberate hole in "the main context never writes pattern memory": a project is 1.03 MiB.
- **The queue protocol** (`core/queue.hpp`): a queued pattern change lands at the end of the
  playing pass, at once when the transport is stopped (there is no pass), and at once when the
  transport jumps back (PR #22).
- **Sends and master are encoder bytes, commanded by deltas.** A byte cannot hold a NaN and needs
  no float serializer; a delta, not the next byte, because the authoritative byte lives on the
  audio side and sixteen fast clicks would read the same stale byte. `PROJECT.BIN`'s third section
  is optional on read, and absent means the defaults, never "leave the struct alone".
- **Gate length on the `Trig`, release by note for KEYS.** `Trig::gate_frames` carries a step's
  length. A KEYS key-up posts `Cmd::release_note`, which releases by note, so it also lets go of
  the sequencer's same note on that track (decided 2026-11-22).
- **Machines are mono and the insert chain spreads them**; a stereo machine would double its
  track's insert work.
- **`VaFilterCache` is keyed on a float by exact equality**, on purpose: the question is "the same
  bits", and `VaPoly::prepare` clearing it is what makes a stale coefficient impossible.
- **The shared voice budget** (PR #30): past 12 VA POLY voices across all tracks, a new note
  chokes the quietest started voice on any POLY track (a 64-frame fade); a note is dropped only
  when every counted voice is still waiting for its frame. GRAIN and VA MONO are not counted.
- **A SAMPLE or GRAIN track with no file plays the placeholder** (Roland, 2026-11-22): `Cmd::clear_sample`
  gives both readers the static placeholder rather than nothing, so NEW and a reopened factory
  project keep the LOOP part, as a card-less boot does.
- **The factory content is one function**, `factory_project` in `core/factory.cpp`, used by both
  `Engine::init` (no card) and `Storage::start_new` (NEW). Change sounds and patterns there only.
  `project_init` stays blank because the tests' `fresh()` builds on it. It ships macros B ACID,
  C DUB and D DRIVE and leaves A empty, because pad pressure bends macro A on whichever track is
  selected.
- **A macro sweeps from the parameter's live value** (kit, then morph) to its high end, so at 0
  it is exactly the knob and EDIT and the morph keep working (PR #34). Before that, an assigned
  macro froze its parameters at `low`, which is why no macros shipped at first.
- **Navigation follows Enaga's menus**: UP/DOWN move between rows, LEFT/RIGHT sideways, OK picks,
  CLR goes back. New list-like settings go on the arrows, not new chords. `docs/CHEATSHEET.md` has
  the full map; on-screen hints name keys as printed on the panel (FUNC, PROJECT, EDIT).
- **Settled by Roland on 2026-11-22**: note names use sharps everywhere; the scale fold applies to
  both VAs and GRAIN, and to SAMPLE only while unsliced (SLC = 1); one p-lock is removed by
  holding the step and pressing the knob or tapping the parameter; FUNC+STOP also puts a running
  chain back on its first entry; CHAIN repeats are capped at 16; a sample may be 30 s of the
  file at up to 96 kHz (PR #31).
- **`core/` never includes an Enaga header**, which is why `kSendFields` / `kMasterFields` are
  stated in `core/project.hpp` and `static_assert`ed against the enum in `dsp/buses.hpp`.

### 7. Small things noticed and deliberately not fixed

- `dsp/va.cpp`'s flop table predicts ~1,660 cycles a voice; the measurement is ~18,900. The table
  is kept for the order of its stages, which held; `docs/BUDGET.md` has the real costs.
- `core/project.cpp`'s `project_read` carries `#pragma GCC optimize("no-unroll-loops")`: GCC
  13.3.0 for tic6x-elf crashes on it with `-O2 -funroll-loops` (`haifa-sched.cc:1597`). To be
  reported upstream with that file as the failing case.
- Two comments say float division is hardware on this core (`Engine::process`,
  `Granular::fit_length`). It is not; both are at control rate, so harmless.
- `tests/test_engine.cpp` and `tests/scenario_m4.sh` cite older versions of this file for two
  fixed bugs and the queue protocol. The bugs are in commit `1108ee0`; the queue
  protocol is in section 6 above.

- **Test order.** The host suite passes in registration order, with `--only` on any one test,
  as contiguous shards (`--shard K/N`, what `make -C host test JOBS=N` and CI run), and since
  2026-11-26 in shuffled orders: 37 seeds clean with `tools/test_shuffle.sh BIN [SEEDS...]`,
  where they had failed 28-55 checks each. What leaked, now fixed: the scenes and macros (they
  load beside `Project`, so `project_init` left them), every UI cursor and page, the take
  counter (tests named `STRATA_1.WAV`), the stream late counts (they only ever grow), the
  ambient rig's settings, a FREE LFO's phase, the CPU MAX, a stale card error and its notice, a
  held MUTE pad, the track meters' fall, and the UI rig's STOP flag. A failing seed keeps its
  order in `$TMPDIR/strata-shuffle-SEED.txt`; replay it with `strata-tests --names FILE` and cut
  the file in halves to find the test that leaks. Run the tool after adding tests to a rig.
  Known open (2026-11-27): `app_a_motion_recorded_lock_survives_the_block_copy` fails in
  `tools/test_shuffle.sh BIN 10` (`after_lock < unlocked`); halving finds no single culprit, so it
  takes two or more earlier tests together. The
  shards are still contiguous blocks; round robin would now work but nothing needs it.

### 8. Upstream, and moving the Enaga pin

[seawaffle/enaga#1](https://github.com/seawaffle/enaga/pull/1) is open: it keeps libgcc's unwinder
out of an app's link. It is not in v0.4.0 either. **Never modify `third_party/enaga`**; Enaga bugs go
upstream with a failing case.

The pin is **release v0.4.0 (`7487bbf`)** (moved from v0.3.0 on 2026-11-27, Roland's call for the
v0.5 release candidate; v0.3.0 had come from v0.2.0 on 2026-11-25 and added a `battery` entry to the
host table). v0.4.0 keeps the host table and ABI 0, and brings firmware fixes STRATA gains from:
MIDI clock over USB MIDI and no wrong tempo after a long pause, Enaga's own share of an audio block
down from about 7,000 to 2,600 cycles, FAT32's FSInfo written correctly, multi-sector SD reads and
writes, the watchdog fed while a large package loads, and a USB debug port with remote key input
(`tools/usb_debug.py`), a candidate for scripting unit checks. The unit needs 0.4.0 firmware. The rule is to pin release
tags only, and the one that matches the firmware on the unit: Enaga's loader checks nothing but `ENAGA_ABI_VERSION` (still 0),
so headers and firmware can drift apart with nothing failing. On the unit, v0.2.0 logged one
unreproduced crash on CHECK's MIDI page; the fix for a stalled USB host holding up DIN is in
v0.3.0. Do not add a package icon (`ENAGA_PACKAGE_ICON`) while any unit runs firmware older
than 0.2.0: that loader refuses non-zero flags.

#### Moving the Enaga pin

`tools/enaga_check.sh --upstream` reports newer releases and the commits since the pin that touch
`include/`, `runtime/`, `loader/` or `build_app.py`, and exits 3 when there is something to look
at. Without `--upstream` it only checks that the checkout, the docs and the emulator match their
pins. Moving the pin is Roland's decision; once made, one commit, in this order:

1. `git -C third_party/enaga fetch --tags && git -C third_party/enaga checkout vX.Y.Z`, then
   `git add third_party/enaga`.
2. `git -C third_party/enaga diff <old> vX.Y.Z -- include runtime loader tools/build_app.py`, and
   re-check every signature in `docs/ENAGA_API.md` against it. Any `ENAGA_ABI_VERSION` change
   means the unit's firmware must move in step.
3. `make -C host test` (`tests/test_platform.cpp` catches a changed platform constant), then
   `tools/build.sh` with GCC **and** with cl6x, then `tools/scenarios.sh`. Record any load change
   in `docs/BUDGET.md`.
4. Update the pin in `CLAUDE.md`, `docs/ENAGA_API.md`, `THIRD_PARTY_NOTICES.md` (the check fails
   until those three name the new tag and hash), and this section.
5. `tools/enaga_check.sh` passes.

The emulator moves the same way, by `STRATA_EMULATOR_COMMIT` in `tools/env.sh`, then
`tools/fetch_emulator.sh` and `tools/scenarios.sh`.

## Running commands on Roland's Windows host

The repo is on `D:` and every build runs in WSL. `CLAUDE.md` has the working form; the short
version is `MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c '...'`, single
quotes, and a script file in the scratchpad for anything longer than one command. In a Linux
container none of that applies.

**Threads work in worktrees** (`D:\DEV\STRATA-wt\<name>`, each with its own `ENAGA_BUILD`), made
by Windows git, because several threads share `D:\DEV\STRATA` and none may switch its branch. A
new worktree needs `git submodule update --init`. WSL's git cannot read such a worktree, so
`tools/env.sh` sets `STRATA_GIT` / `STRATA_GH` to Windows' `git.exe` / `gh.exe` there (WSL runs
them), and `tools/enaga_check.sh` and `tools/publish_release.sh` use those; gh and the GitHub
credentials exist only on Windows anyway. The emulator clone is untracked and lives in the main
checkout only: `tools/env.sh` finds it from a worktree, and `tools/scenarios.sh` refuses to run
without one rather than report every scenario's SKIP as a pass.
