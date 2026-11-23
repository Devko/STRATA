# STRATA features and their status

Every feature STRATA has, grouped by area, with where it stands. Written 2026-11-23 from `main`
at `436a2f8`, docs/MANUAL.md, CHEATSHEET.md, CONTROLS.md, UNIT_CHECKLIST.md, BUDGET.md, SPEC.md
and the post-v0.1 ideas list. How to play each one is in [MANUAL.md](MANUAL.md); this file
only says what exists and how sure we are of it. Update a row when its status changes.

**Columns**

- **Since:** `v0.1.0` = in the [v0.1.0 release](https://github.com/Devko/STRATA/releases/tag/v0.1.0);
  `#83`, `#85` ... = merged to `main` after v0.1.0, in that PR; `WIP` = being built, not on `main`.
- **Unit:** what the SmplTrek itself has confirmed.
  - **✔** confirmed on the unit (recorded in BUDGET.md or ticked in UNIT_CHECKLIST.md).
  - **◐** used on the unit in the free-play session of 2026-11-22 (controls-remap build, cl6x,
    MAX 84.3%, no clicks or dropouts), but its own checklist box is still open.
  - **○** not checked on the unit yet; the section of UNIT_CHECKLIST.md that covers it is named
    (`13d` = the walkthrough at the top, otherwise the reference section's title).
- Every feature has host tests; CI runs them on every PR. Emulator scenarios exist for most, but
  several are still owed on Roland's PC (see "Gaps" at the end).

**In one line:** about 80 user-facing features, all host-tested. On the unit: boot, the
default beat, the load readouts and a long stress session with no dropouts. Of
UNIT_CHECKLIST.md's ~550 boxes, 4 are ticked.

---

## Platform and display

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Runs as an Enaga app on the SmplTrek (Enaga 0.2.0, ABI 0, BOOT 1.0.17) | v0.1.0 | ✔ booted 2026-11-22 | Loader checks only the ABI number: another Enaga release may load and misbehave. Lofi-12 XT / CyDrums untried |
| Everything on the SD card under `/ENAGA/DATA/STRATA/`; never writes the SPI flash | v0.1.0 | ○ (13) | |
| Audio load on the top line | v0.1.0 | ✔ about 0–26% on the default beat | Flickers every block |
| Held MAX readout on the bottom line (since PLAY) | v0.1.0 | ✔ top line under 30% default beat; MAX 84.3% in heavy play | Clean A01 and A04 MAX figures (60 s each) still to record |
| **CPU guard**: sheds POLY voices (2 a level) and thins GRAIN under load, `GUARD1-4` on screen | #83 | ○ ("The CPU guard") | Thresholds (70% avg / 90% block / 55% fall) are provisional; nothing in the emulator reaches them |
| Shared budget of 12 VA POLY voices across all tracks; quietest voice choked with a 64-frame fade | v0.1.0 | ○ ("The shared VA POLY voice budget") | 12 is provisional until U1/U2 on the unit |
| Every screen fits Enaga's 5x7 font (layout test over every screen) | v0.1.0 | ◐ (overlaps seen at first boot, fixed in PR #7) | |

## Controls and UI

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Controls concept from the panel: one job per button group ([CONTROLS.md](CONTROLS.md)) | v0.1.0 | ◐ ("The new controls") | |
| FUNC + function key = its printed second label (sampling, pool, system, ◀◀, ▶▶, fx, rec src) | v0.1.0 | ◐ ("The new controls") | |
| FUNC menu (FUNC+HOME): save, save as, open, new, reload, kits, sample reload, track speed, chain, song/loop, auto-fill, quantize | v0.1.0 | ◐ ("The new controls") | |
| **CLEAR PATTERN** in the FUNC menu: every track in one press, OK twice, one undo | #86 | ○ ("The new controls") | Keeps sounds, lengths, speeds and swing |
| FUNC held alone shows a help screen | v0.1.0 | ○ ("The new controls") | |
| HOME = GRID, HOME twice = KEYS | v0.1.0 | ◐ | |
| Lit, self-naming performance pads; mode keys green/red by screen | v0.1.0 | ○ ("The new controls") | Upper pads are printed TRK 1-7 but are performance pads (Roland's choice) |
| Arrow-key navigation model (UP/DOWN rows, LEFT/RIGHT sideways, OK picks, CLR back) | v0.1.0 | ○ (5, "Arrow keys") | |
| Screens: GRID, step page, EDIT (6 pages), MACHINE, KIT, KEYS, MIXER (9 pages), PERFORM, PROJECT (5 pages), SAMPLES, RESAMPLE, menu | v0.1.0 | ◐ (4) | |
| Fixes: the FUNC menu never shows a MUTATE verdict OK would not give; OK in RESAMPLE no longer retargets a take being written | #84 | ○ | |

## Engines (machines)

Eight tracks, any machine on any track (MACHINE, FUNC+EDIT). Each track: 16 parameters on four
pages (SRC, FLTR, AMP, FX), plus LFO and ARP pages.

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| KICK (909-style, 2 voices, follows the note) | v0.1.0 | ✔ plays in the default beat | One-shot; DEC is the length |
| SNARE with clap mode (2 voices) | v0.1.0 | ✔ default beat | One-shot |
| HAT, 808 recipe (2 voices), track 3 chokes track 4 | v0.1.0 | ✔ default beat | Choke pair is fixed to tracks 3→4; the CHK knob only shows it |
| PERC, 2-operator FM (2 voices) | v0.1.0 | ✔ default beat | One-shot |
| SAMPLE: one-shot or 1-16 equal slices by note, 4 voices, 4 play modes (fwd, loop, rev, rev loop) | v0.1.0 | ◐ default beat's loop track (placeholder sample) | Unit sample loads: 13b, 15b-15d |
| SAMPLE **SLC AUTO**: slices at up to 15 detected hits | v0.1.0 | ○ (15c2) | Hit points are found at load, not editable |
| GRAIN: up to 8 grains in one voice | v0.1.0 | ○ (8, 8b / U3) | Most expensive machine; outside the POLY budget (thinned by the CPU guard); no tempo sync of its own |
| VA MONO: saw + sync pulse + sub + noise, own filter and envelope, accent, slide, glide | v0.1.0 | ✔ default beat bass | **No sustain:** sustain-0 envelope, DEC is the note's length; step LEN is not heard; a held key only cuts it short |
| VA POLY: up to 6 notes, 13 chord types with inversions, DET | v0.1.0 | ✔ default beat chord | **No sustain** either; step LEN shortens a note but cannot lengthen it. No stereo spread or unison 2 (in SPEC) |
| **ADSR / ASR and note length for POLY and MONO** (ATK/SUS/REL page, step LEN as a real gate, held key length recorded) | WIP | — | Thread "Make POLY and MONO hold long notes" |
| MID: plays an outboard synth over MIDI; SRC/AMP knobs send CC 74/71/73/72, 70/75/76/77 | v0.1.0 | ○ (8d) | No sound or CPU of its own; no scale fold |
| Key and scale per pattern (15 scales), fold for VAs, GRAIN and unsliced SAMPLE; sharps everywhere | v0.1.0 | ○ (7) | |
| Golden renders pin every machine's sound | v0.1.0 | — | Host only |

## Sequencer

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Up to 64 steps a track, per-track length and speed (2x ... 1/8): polymeter | v0.1.0 | ○ (M3) | |
| Steps, trigless steps (FUNC + tap), STEP LED colours | v0.1.0 | ◐ | |
| Step page: NOTE, VEL, LEN, MICR (±11 ticks), PROB, COND, RTCH 1-8, RAMP, ACNT, SLID, CHRD | v0.1.0 | ○ (6) | LEN only audible on VA POLY (shortening) until the WIP above |
| Trig conditions: a:b, FILL, PRE, NEI, 1ST and their NOTs; seeded | v0.1.0 | ○ (M3) | |
| Parameter locks: 8 a step, 1,024 a pattern; push the knob to remove one | v0.1.0 | ○ (6) | `FULL n` when the pool is full (never reached on the unit) |
| Pattern swing and **per-track swing** (PATTERN page) | v0.1.0 | ○ (13e) | |
| Euclidean fill (FUNC + A/B/C in GRID) | v0.1.0 | ○ (M3) | |
| Copy/paste a step, a range or a track; range transpose and nudge | v0.1.0 | ○ (M3) | No one-press copy of a whole pattern to another slot |
| 16 levels of undo and redo | v0.1.0 | ○ (6) | Chain edits and kit recalls are not undoable |
| Live recording (pads, KEYS, MIDI) with latency correction and QUANTIZE 0-100% | v0.1.0 | ○ (M3) | Note-repeat rolls are not recorded |
| Motion recording of knob moves as locks | v0.1.0 | ○ (M3) | MIDI CCs are not motion-recorded |
| KEYS mode: STEP keys play the scale | v0.1.0 | ○ (9) | |
| **KEYS on the pads: two rows as a piano**, velocity, held, octave pads | #85 | ○ ("The new controls") | |
| Per-track LFO: 6 shapes, free 0.06-32 Hz or synced 8 bars-1/32, any of the 16 params; lockable, scene-able, macro-able | v0.1.0 | ○ (13e) | Steps at about 750 Hz: a slow deep pitch sweep can be heard stepping |
| Per-track arpeggiator: UP, DN, U/D, ORD, RND, 1/4-1/32T, 1-4 octaves, gate | v0.1.0 | ○ (13h) | On POLY it uses the shared voice budget |
| Banks A-H x 16 patterns, queued changes (END, 16, 32, NOW) | v0.1.0 | ◐ (3b) | |
| Chain of up to 64 entries, repeats X1-X16 | v0.1.0 | ○ (3b, 12) | |
| **Song mode**: per-entry mute recall, SONG plays once and stops | v0.1.0 | ○ (13i) | |
| Restart (FUNC+PLAY) and stop-and-reset (FUNC+STOP, also resets the chain) | v0.1.0 | ○ (11) | |

## Performance

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| FILL pad, auto-fill (every 4/8/16 bars) | v0.1.0 | ◐ (3b) | |
| REPEAT: beat repeat of the master, 1/4-1/32T, pitch-drop with FUNC | v0.1.0 | ◐ | |
| Mutes on the bar (FUNC + track pad), MUTE MODE, 8 stored mute states | v0.1.0 | ○ (3b) | |
| 16 scenes, A/B morph on VALUE, capture whole or one track | v0.1.0 | ◐ (3b) | Morph position is not saved; locks are not morphed (parked) |
| 4 macros of up to 4 targets, sweep from the live value; factory B ACID, C DUB, D DRIVE | v0.1.0 | ○ (3b) | |
| Pad pressure bends macro A | v0.1.0 | ○ ("Pad feel") | |
| THROW to delay or reverb | v0.1.0 | ◐ | |
| MUTATE (rotate, density, velocity, micro, chance, notes), OK keeps, CLR reverts | v0.1.0 | ○ (10) | |
| Note repeat on the pads, pressure as velocity | v0.1.0 | ○ (13f) | Not recordable |
| Tape stop (FUNC + hold push D) | v0.1.0 | ○ (13g) | |
| Delay freeze (FUNC + hold MUTATE) | v0.1.0 | ○ (13g) | |

## FX and mixer

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Insert chain per track: drive, filter (LP/BP/HP 12/24), lo-fi, amp, duck, pan, level | v0.1.0 | ◐ | Off stages cost nothing |
| **Sidechain duck** from the kick | v0.1.0 | ○ (3, "Ten bugs a review of `app/` found") | **Fixed and not editable:** depth 0.35, attack/hold/release 3/40/120 ms, source is the kick, set in `Engine::init` (`app/engine.cpp`, the block before `factory_project`). Making it editable needs a kit field |
| Send A: tempo-synced ping-pong delay | v0.1.0 | ◐ (13c / U4) | |
| Send A **LONG** mode: up to 12 s, wow, diffusion, low cut, true freeze | v0.1.0 | ○ (13j) | ~2x the send's CPU while it rings; load host-estimated only (E11 owed) |
| Send B: reverb | v0.1.0 | ◐ | |
| Send B **HALL** mode with shimmer | v0.1.0 | ○ (13j) | As LONG |
| Delay into reverb (D>R) routing | v0.1.0 | ○ ("The sends, the master and SETUP") | Routing page has one empty slot |
| Master: DJ filter, glue compressor, limiter, output level | v0.1.0 | ◐ | |
| Master **kill EQ**: 3 bands at 250 Hz / 2.5 kHz, kill switches (FUNC + push A/B/C), scene-able, macro-able | v0.1.0 | ○ (13d / U6) | |
| MIXER: 8 faders, 9 pages | v0.1.0 | ◐ | |

## Samples and resampling

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| WAV 8/16/24-bit or 32-bit float, mono or stereo, any rate (converted to 48 kHz, low-passed) | v0.1.0 | ○ (13b / U5) | More than two channels refused |
| Background loading, the beat keeps playing | v0.1.0 | ○ (13b) | |
| Long files at 48 kHz up to 349 s, read in 4 MiB chunks | v0.1.0 | ○ (15c) | 32 MiB sample memory shared by all tracks |
| Other rates: up to 30 s at 96 kHz | v0.1.0 | ○ (13b) | |
| Streaming of files too big for memory (2 s head + card stream, 4 tracks max) | v0.1.0 | ○ (15d) | Forward only, one voice: no STA, slices or reverse (not feasible, below); GRAIN plays only the head; CPU unmeasured |
| Streamed samples honour LEN (end point, loop back to the head) | WIP | ○ (15d) | A LEN loop inside the 2 s head never touches the card |
| Sample browser (`/SAMPLES`, `/RECORD`), load while playing | v0.1.0 | ○ (15b) | 256 entries a folder; path ≤ 111 characters; no audition before loading |
| Resample the master, 1-8 bars, to a file or straight onto a track | v0.1.0 | ○ ("Resampling") | Takes ≤ 30 s. Resampling one track alone (SPEC) is not built |
| Sample from the input: LINE, LINE L, MIC, DYN MIC, HI-Z, with level and meter | v0.1.0 | ○ (8c) | No monitoring through STRATA; MIC needs headphones in |

## MIDI and sync

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Follows the unit's clock: internal, MIDI clock, SYNC IN; refuses tempo/transport under external clock | v0.1.0 | ○ (M7) | STRATA cannot set CLOCK SRC (Enaga has no setter). SYNC polarity not selectable (Enaga) |
| MIDI OUT per track (OFF/1-16): steps, chords, ratchets, arp, note repeat, pads, KEYS, MIDI IN | v0.1.0 | ○ (8d) | **Enaga 0.2.0:** with USB connected to a computer not reading, DIN out stalls; unplug USB. Fixed upstream after 0.2.0 |
| Every note gets its note-off; second STOP sends All Notes Off | v0.1.0 | ○ (8d) | Slide is not sent as legato |
| MIDI IN notes (ch 1-8 = tracks, others = selected), held to note-off | v0.1.0 | ○ (8d) | No pitch bend or aftertouch |
| MIDI IN CC 16-31 = track parameters, CC 120/123 release | v0.1.0 | ○ (8d) | |
| MIDI IN program change queues a pattern | v0.1.0 | ○ (8d) | |
| Sending MIDI clock | — | — | Enaga's CLOCK OUT does it, not STRATA |

## Storage, projects and kits

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Save (FUNC+HOME, OK), only changed files, audio keeps playing | v0.1.0 | ○ (13) | |
| Autosave on every stop and on leaving the app; reopen last project or a newer autosave | v0.1.0 | ○ (13) | |
| Reload project, new project (factory content) | v0.1.0 | ○ (14) | Both stop the transport |
| Project browser: open, save as with a name | v0.1.0 | ○ (14b) | |
| Project rename (push B) and delete (push C, OK twice), safe against a pulled card | WIP | ○ (14c) | Rename is a copy then a delete (Enaga has no rename). The open project cannot be deleted |
| Kit library: 16 kits on the card, recall while playing, store | v0.1.0 | ○ (15) | Recall is not undoable |
| Damaged files refused, never crash | v0.1.0 | ○ (M4) | Card pulled mid-save not tried on the unit |
| Factory project: 130 BPM C minor kit, A01-A04, chain, 3 scenes, 3 macros, 8 mute states | v0.1.0 | ✔ A01 plays; ◐ the rest (3b) | Track 8 plays a placeholder until given a sample |

---

## Gaps and next ideas

**Still owed before the status above can say ✔**

1. **The unit walkthrough.** Almost every row is ○. The top of UNIT_CHECKLIST.md orders it; the
   first numbers needed are the clean A01/A04 MAX, STRATA6 (six voices), U1-U9.
2. **Settle the provisional numbers from those readings:** `kPolyVoiceBudget = 12`, the CPU
   guard's thresholds, the duck depth.
3. **Emulator runs on Roland's PC:** E1-E11 (BUDGET.md), `scenario_controls.sh`,
   `scenario_midi.sh`, `scenario_ambient.sh`, and the cl6x figures for the post-v0.1 features.
4. **The fused VA loop is off** (cl6x renders it differently, BUDGET.md): ~1.7 points to win back
   once the cause is found.
5. **CHANGELOG** "Unreleased" lists the CPU guard but not #84, #85 or #86.

**Known limits worth building next**

1. **Sustain and note length on the VAs**, in progress (ADSR/ASR page, LEN as a gate).
2. **An editable sidechain duck:** depth, and maybe source and release, as kit fields.
3. **MIDI:** pitch bend and aftertouch in, CC motion recording, slide as legato out; moving the
   Enaga pin once a release carries the DIN fix (`192f03f`).
4. **A per-project sample memory above 32 MiB** (SPEC allows up to 64), so fewer files need to
   stream. Streamed LEN is built; streamed STA, slices and reverse were looked at on 2026-11-23
   and are not feasible on Enaga 0.2.0's card path ("Why streamed samples play forward only",
   below).
5. **Resample one track** on its own (SPEC promise).
6. **VA POLY stereo spread and unison** (SPEC promise), and the 6-to-8 voice question once U1/U2
   are in.
7. **Undo for chain edits and kit recall**; one-press copy of a whole pattern to another slot.
8. ~~Delete and rename projects on the unit~~: built (push B / push C on PROJECTS), as a copy
   and a delete since Enaga has no rename.

**From the ideas list, not built**

- A chorus as a third send (`enaga::dsp::Chorus`, 1-3 points, ~100 KiB).
- More scenes or per-pattern scenes.
- Scene morphing of locks (parked: no single sensible blend).
- Skipping KICK's click path after its transient (a CPU win, needs a listening test).

**Budgets that limit new work:** heap 109.1 of 120 MiB at init (2.9 MiB above the 8 MiB
headroom rule), and an estimated ~63% unit load with everything on at once (BUDGET.md
"Everything at once").

## Why streamed samples play forward only

Looked at on 2026-11-23, against Enaga v0.2.0's file layer (`firmware/kernel/files.c` and its
FatFs configuration). CPU is not the problem: a streamed voice costs the same whichever way it
reads. The card is.

- **An Enaga stream reads forward only**, from the offset it was opened at. Anything else is a
  close and a reopen, or a `files::File` seek.
- **Every reopen or seek walks the file's cluster chain from its start.** Enaga builds FatFs with
  `FF_USE_FASTSEEK 0`, so `f_lseek` to a byte follows the FAT from the first cluster: one
  512-byte FAT sector per 128 clusters. A 300 MB file in 32 KiB clusters is about 75 sector
  reads, in one FatFs call that the main loop cannot slice. STRATA already pays this once per
  restart (a trigger or a loop wrap), which UNIT_CHECKLIST 15d measures.
- **Reverse** reads the file backwards, so it needs that walk for every block, all the time: with
  half-second blocks, two walks a second per streamed track, eight with four tracks. The stream's
  ring also reads ahead past each block, so a 256 KiB ring spends about 2.7 times the bandwidth a
  forward stream does. It would also need the file's last two seconds resident as well as its
  first, and a file only streams when the sample memory cannot take it.
- **START and slices** are lockable per step, so any trigger may ask for any place in the file.
  The 2 s head is what hides a reopen today; a start elsewhere would need two seconds resident at
  every place a trigger can start (sixteen slices of a 48 kHz stereo file: 6 MiB, the memory the
  file was streamed for want of), and a chain walk on every trigger: eight a second for 16th-note
  slices.
- **LEN** is an end point and needs nothing of the card: built.

What would change the answer: Enaga building FatFs with fast seek (a cluster table per open
stream makes a seek constant-time), or a unit reading for 15d showing restarts every bar with
no `STREAM LATE` and a short walk. Then reverse, at least, is worth building: the voice and the
fifo already count frames, so it is a second card-side reader and a resident tail.

