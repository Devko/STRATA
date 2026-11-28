# Design notes, feature by feature

What a feature's code does not say on its own: the protocol between the UI and the audio side,
which bytes on disk it owns, the traps the tests fell into, and what was left out on purpose.
Read the notes for a feature before changing it. They moved here from `docs/HANDOFF.md` on
2026-11-28; the cross-cutting knowledge (where things run, the test rigs, the standard, the
decisions not to re-open) stays there.

When a note goes stale, fix it or delete it. Do not append a history: that is the CHANGELOG's job.

## The UI class and the controls

**The UI class is split over files** (2026-11-25): `app/app.hpp` holds the helpers, `init`, the
private helpers and all the state, and includes sixteen `app/ui_*.hpp` fragments inside the class
body (input, gestures, lights, live, screen, edit, editview, listview, pickview, modslots, mixer,
mixview, browsers, perfmix, perform, project). What each screen draws, row by row, is docs/SCREENS.md. They are not
headers of their own. `tools/audit_commands.sh` reads all of them.

**Controls.** docs/CONTROLS.md is the reference the key handlers follow; change a gesture there
first, then in the code, `core/help.cpp`, the cheatsheet, the manual and the unit checklist.

## LFO

dsp/lfo.hpp has the detail. Other features cite "TRIG's reason" from here.

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

## RESAMPLE writes only inside /RECORD

RESAMPLE had never written a take on Enaga until 2026-11-25: Storage made `/RECORD` first, and
Enaga writes only strictly inside `/RECORD`, so every take failed `DENIED`. Fixed, and
`tests/fakehost.cpp` now refuses writes outside the data folder, `/SAMPLES` and `/RECORD` as the
firmware does (`write_check`; `seed_folder` lays out a PC-made card). No take has been heard on the
unit yet: UNIT_CHECKLIST's resample items are the first real test.

## REL ALL, LEN ALL and FEEL

REL ALL and LEN ALL on FUNC + turn A / B in PERFORM
(`core/moveall.hpp`; live, rest 0 with a detent, in a LOCK, `RL`/`LN` in the live strip) and FEEL
on the PATTERN page (`core/timing.hpp` feel_offset, pattern file section 5, D turns it).
`scenario_moveall` turns REL/LEN ALL in the emulator (`move_all_notice` logs each click);
`scenario_feel` walks FEEL. UNIT_CHECKLIST W29 walks both.

## LIVE RND

**LIVE RND** (CyDrums P1's live half, `core/liverand.hpp`, docs/CONTROLS.md "LIVE RND"): FUNC +
turn D in PERFORM sets a unit (OFF, 1-16) for the armed tracks, FUNC + push D arms or disarms the
selected one (all eight at start). It lives in `PerformState` (`rnd_unit`, `rnd_armed`) and the
scheduler asks `live_play_mode` for each track's mode every tick, so it reuses the track PLAY mode's RND
draw and never writes the pattern; the recorder asks the same function. In a LOCK (`LiveState`
grew two bytes), not saved. Default armed set is all eight: Roland may prefer the kick out by
default. `scenario_liverand` walks it; UNIT_CHECKLIST W36.

## MOD slots

core/modslot.hpp has the rules.

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

## CLICK and COUNT-IN

**CLICK and COUNT-IN** (CyDrums S6, docs/CONTROLS.md "CLICK and COUNT-IN"): dsp/click.hpp is a
recursive-sine click (loop in dsp/clickloops.ti.c) mixed after RESAMPLE's tap, beside the
audition. `Engine::metronome_tick` fires it from the clock's own ticks. A count-in is
`Cmd::count_in`, posted BEFORE `enaga::clock::start()` (and taken back if the start is refused):
the clock starts at once and `count_in_end_` ticks are subtracted from every tick the sequencer,
the rolls, the stutter and the arps see, from `Status::tick` and from the recorder's stamps
(`Status::count_in_ticks`). A STOP (seen as running going false) or a restart ends it. Host tests
in tests/test_click.cpp and test_ui; `scenario_click`; UNIT_CHECKLIST W47.

## Ratchets over LEN

**Ratchets over LEN** (Roland's RATE + span flag, 2026-11-26; core/pattern.hpp kRatchetSpan): bit 7
of `Step::ratchet` makes the low nibble a rate (kRatchetRateTicks) and the ramp bits 4-6 run
across the roll. The scheduler emits the first hit with `StepEvent::span_ticks/span_hits`;
`Engine::span_start` (from `fire`) keeps the run and `span_tick` hits it on the clock's ticks,
each hit through `fire` with ratchet_index 1. Every place that drops deferred ratchet hits also
calls `span_clear`. The step editor's RTCH walks 1-8 and then the eight rates. Recording a note
repeat as a roll (P3) is meant to write exactly this byte. tests/test_ratchetspan.cpp,
`scenario_ratchetspan`, UNIT_CHECKLIST W49.

## Choke groups

**Choke groups** (CyDrums S4, docs/CONTROLS.md "Choke groups"): `KitTrack::choke`, KIT.BIN's
optional section 11. `Engine::note_started` now takes the note's frame and calls `choke_group`,
which marks the other tracks of the group; after each such track renders, `run_choke` fades its
whole output from that frame over 64 frames (`strata_choke_fade` in dsp/chainloops.ti.c, the fade
carrying into the next block) and then `kill_all`. It is a fade on the TRACK, not a voice, so
every machine chokes alike, and a block renders whole: a note on the cut track at or after the
choke's frame in the same block cancels the choke, and two tracks of a group on one frame both
sound. The HAT machine's own track 3 to 4 pair (`rewire_machines`) is untouched. Host tests in
tests/test_choke.cpp; UNIT_CHECKLIST W46.

## Sample audition

**Sample audition** (backlog Phase 5, "sample audition in the browser"): push A in SAMPLES
streams the file under the cursor to the output (dsp/audition.hpp; app/storage.hpp "sample
audition" has the protocol). It is a fifth Enaga stream with its own 256 KiB ring, beside the job
machine, so it never waits for a load and takes none of the four track rings. Played at the file's
own rate with Catmull-Rom, after every effect and the RESAMPLE tap, at a fixed -6 dB. A late card
holds its place rather than skipping. `scenario_audition` is the one scenario that checks the
captured output (the emulator keeps only the run's last 0.68 s, hence its short run). UNIT_CHECKLIST
W36 walks it; the level against a real mix is the open question.

## Arp depth and ARPS

core/arp.hpp has the design.

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

## LOCK and FX GLIDE

The audio side keeps the 128 locks and the swap-back and does
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

## Step stutter and the repeat rate

docs/CONTROLS.md "REPEAT" has the gestures. One rate (`RepeatLength`, UI-owned, posted by `Cmd::set_repeat`) serves beat
repeat, the rolls and the stutter; LEFT/RIGHT walk it by ticks (`repeat_length_step`), VALUE in
the enum's order. The stutter is `Cmd::step_stutter`: the engine silences the masked tracks'
scheduled events and fires `stutter_event` on the roll's grid, while `Scheduler::tick` keeps
running, so the release lands in time. It is all tracks by default, FUNC + STEP the selected one
(the backlog proposed the opposite; Roland may want it flipped). The first STEP turns the audio
beat repeat off for the rest of that REPEAT hold. Not built: a two-STEP loop (two steps held
alternate). A NOTE RPT roll IS recorded now (P3): `record_note(..., roll)` puts a span ratchet byte in `Cmd::record_hit`'s `e` and the pad's release gives LEN through the RecHold path, on any machine. Host-tested only: the emulator cannot press pads.

## Random lock ranges and parameter slide

docs/CONTROLS.md "Random ranges and slides" has the gestures. Both live in the lock's param byte (bit 6 range, bit 7
slide; `core/pattern.hpp`), and every lock function matches by `lock_key`, so a slide never makes a
second lock. A range is its own entry (one of the step's 8 slots), drawn by `lock_spread` from the
seed, track, step, pass and parameter. The scheduler looks ahead for a slid step only
(`StepEvent::glide_*`, in play order); `Engine::start_slides` / `run_slides` glide at control rate
before the LFOs. Hold STEP + FUNC + push no longer clears a lock: it toggles the slide.
`scenario_locks` walks both gestures; UNIT_CHECKLIST W37.

## Track Launch

`core/launch.hpp` has the design and Roland's decisions (2026-11-27). The scheduler plays each
lane from its own pattern and its own tick (`Scheduler::configure` and `tick` take lane arrays;
the single-pattern calls wrap them). FEEL comes from the playing pattern, the probability seed
from the lane's. The engine keeps a `LaneQueue` (the pattern queue's timing) and an origin per
lane; `lane_follows_` marks the lanes that count from the pattern's origin. Launches due on a
tick land after a pattern change on the same tick. `Engine::launch_refuses` is the read-only
guard, listing every track-edit command. CAPTURE copies every lane's part and compacts the lock
pool into one free slot, then switches to it keeping each lane's origin, so nothing restarts.
LAUNCH is PROJECT's last page so the page walks in older tests and scenarios stay valid.
`tests/scenario_launch.sh` drives it in the emulator; CAPTURE and held pads are host-tested only.

## Your own sounds and copy / paste in SOUNDS

`core/usersounds.hpp` has the design and the file's bytes; `app/storage.hpp` ("your own sounds")
the protocol. The decisions behind it:

- **One file per machine, not per sound**: `SOUNDS/<MCH>.BIN`, sixteen slots, read when SOUNDS
  opens on that machine and written whole after each save, rename or delete (a few hundred bytes,
  one pass, through files::save's .TMP). One read per list, and no scan of sixteen files.
- **Beside the projects, never in them**, like the kit library: every project file is unchanged,
  so every older project opens as it did, and a sound saved once is in every project.
- **Versioned like every STRATA file**: the container's version, and in the record its own length
  and parameter count, so a later build can save more per sound (the WAVE scan page's bytes) and
  this one still reads it, and a record with fewer bytes takes the machine's defaults for the rest.
  A file this build cannot read is never written over (`failed`), so nothing a newer build or a
  damaged card holds is lost by saving.
- **The whole kKitParams bytes are kept; loading writes what a factory sound writes**
  (`user_sound_owns` = `sound_preset_writes`), so the FX page, LFO and ARP stay the track's, as
  Roland chose for the factory sounds. Kept whole so a later build may choose to load more.
- **Loading goes as bytes, not a preset index**: `Cmd::sound_bytes` carries eight a command (b-e
  and `u`), four commands a sound, posted together so they land in one block. No shared buffer
  between the contexts is needed.
- **Copy is a clipboard row, not a target picker**: FUNC+UP takes the track's sound into the UI's
  clipboard and OK on the PASTE row loads it, so "paste" is the list's own OK. Pasting onto
  another machine posts `Cmd::set_machine` first; `Cmd::sound_preset`'s AS WAS remembers the
  machine too and switches it back (`Engine::switch_machine`).
- `tests/scenario_usersounds.sh` walks save, load, copy, paste and AS WAS; UNIT_CHECKLIST W68.

## Line input as a mixer strip

The jam review's idea 4. `ProjectSettings::input` holds the strip (`InputField` order, PROJECT.BIN's
optional section 9, which grows on the end: a short section reads what it has). It is a project
setting, not a kit one, for MIDI OUT's reason: it is cabling. `Engine::run_input` runs after the
tracks, through `Mixer::add_input`, a ninth `InsertChain` prepared in TON mode (no drive), so the
strip has a fader (square law, 100 = 0 dB), PAN, the three sends, the kit's duck and TON. The MUTE
BUS scales its fader as it does the tracks'; the send raise and the bounce tap do not reach it.
Turning it on resets the chain so its gain ramps from 0; off, it runs one more block prepared at
level 0. The codec route is set by the UI (`service_input_route`, main context only), whenever the
strip is on and its jack or gain differs from what was last applied; RESAMPLE's own input sources
invalidate that and win while RESAMPLE is up. The IN pages sit before MST so the scenarios that go
round to MST and EQ still count the same; CHO moved two pages further (scenario_chorus, m7).
UNIT_CHECKLIST W69.

## Song sections

Roland approved the arrangement concept on 2026-11-28 (no section names, no kit per section, MOVE's D free; ARR REC and launch capture follow in their own PRs).
Each chain entry has a `SectionExtra` (core/project.hpp), saved in PROJECT.BIN section 10: a
layout byte, then 8 bytes for each of the 64 entries. A CAP entry writes `--` into section 5, so
a build without section 10 leaves the mutes alone rather than recalling a stale state. The reader
reads anything it does not understand as `--`. `chain_insert`, `chain_remove` and
`chain_normalize` move the extras with their entries.

`Engine::chain_entry_started` applies the fields on the audio side as an entry starts: the mute
recall or CAP mask, the scene on side B, the morph (SNAP at once; RIDE and BACK through
`morph_ride_*`, a rate worked out once and stepped in `resolve_tick`, since the C674x has no
integer divide), the energy. A BPM goes to the main context through `Status::tempo_due`,
because only it may call `clock::set_tempo`; under an outside clock the UI skips it and says so
once a run. Every edit is a `ChainEdit` the audio side applies as a delta to the authoritative
record. Quick fill (hold OK) can only write SNAP or `--` for the morph: there is no "snap to A".
`tests/scenario_sections.sh` drives the view and a section in the emulator.
