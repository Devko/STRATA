# STRATA features and their status

Every feature STRATA has, grouped by area, with where it stands (`main`, 2026-11-25). How to play each one is in [MANUAL.md](MANUAL.md); this file only says what exists
and how sure we are of it. Update a row when its status changes.

**Columns**

- **Since:** `v0.1.0` = in the [v0.1.0 release](https://github.com/Devko/STRATA/releases/tag/v0.1.0);
  `#83` ... `#111` = merged after v0.1.0 and released in
  [v0.2.0](https://github.com/Devko/STRATA/releases/tag/v0.2.0); `#116` ... `#129` = released in
  [v0.3.0](https://github.com/Devko/STRATA/releases/tag/v0.3.0); `#133` ... `#134` = released in
  [v0.3.1](https://github.com/Devko/STRATA/releases/tag/v0.3.1); `#139` ... `#178` = released in
  [v0.4.0](https://github.com/Devko/STRATA/releases/tag/v0.4.0); `#179` and later = on `main`,
  not released yet; `WIP` = being built, not on `main`.
- **Unit:** what the SmplTrek itself has confirmed.
  - **✔** confirmed on the unit (recorded in BUDGET.md or ticked in UNIT_CHECKLIST.md).
  - **◐** used on the unit in the free-play session of 2026-11-22 (controls-remap build, cl6x,
    MAX 84.3%, no clicks or dropouts), but its own checklist box is still open.
  - **○** not checked on the unit yet; the part of UNIT_CHECKLIST.md that covers it is named:
    `W12` = item 12 of the walkthrough at the top, `13d` = a section of the full walkthrough
    under it, otherwise the reference section's title.
- Every feature has host tests; CI runs them on every PR. Emulator scenarios exist for most, but
  several are still owed on Roland's PC (see "Gaps" at the end).

**In one line:** about 80 user-facing features, all host-tested. On the unit: boot, the
default beat, the load readouts and a long stress session with no dropouts. Of
UNIT_CHECKLIST.md's ~550 boxes, 4 are ticked.

---

## Platform and display

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Runs as an Enaga app on the SmplTrek (Enaga 0.3.0 since #139; 0.2.0 for v0.1.0-v0.3.1; ABI 0, BOOT 1.0.17) | v0.1.0 | ✔ booted 2026-11-22; v0.3.1 runs on the unit (2026-11-24); a #139 build boots on 0.3.0 (2026-11-25) | Loader checks only the ABI number: a build for one Enaga release may load on another and misbehave. Lofi-12 XT / CyDrums untried |
| Everything on the SD card under `/ENAGA/DATA/STRATA/`; never writes the SPI flash | v0.1.0 | ○ (13) | |
| Audio load on the top line | v0.1.0 | ✔ about 0–26% on the default beat | Flickers every block |
| Held MAX readout (since PLAY), on SETUP since #171 (the bottom line before) | v0.1.0 | ✔ top line under 30% default beat; MAX 84.3% in heavy play | Clean A01 and A04 MAX figures (60 s each) still to record |
| **CPU guard**: sheds POLY voices (2 a level) and thins GRAIN under load, `GUARD1-4` on screen | #83 | ○ ("The CPU guard") | Thresholds (70% avg / 90% block / 55% fall) are provisional; nothing in the emulator reaches them |
| Shared budget of 12 VA POLY voices across all tracks; quietest voice choked with a 64-frame fade | v0.1.0 | ○ ("The shared VA POLY voice budget") | 12 is provisional until U1/U2 on the unit |
| Every screen fits Enaga's 5x7 font (layout test over every screen) | v0.1.0 | ◐ (overlaps seen at first boot, fixed in PR #7) | |
| **Screen frame**: top line with bar.beat, progress line, live strip of what is off rest, master meter with held peak and limiter light | #144 | ○ (W26) | The meter is 2 px wide; nothing may draw past x 124 |
| **RESAMPLE, SAMPLES, SOUNDS, CHAIN, KITS, KEYS and PATTERN redrawn** (docs/SCREENS.md): RESAMPLE's meter, live take and length bar; the browsers' previews; CHAIN as blocks on a lane; KITS' machine pictures; KEYS' held chord and knob gauges; PATTERN's swing bars; no caption repeats a screen's name | #176 | ○ (W45) | draw() see docs/BUDGET.md "The screen" |
| **PROJECT's slots and MACHINE redrawn to the drafts** (docs/SCREENS.md): a map of all 128 patterns (empty, used, playing, queued) and each of the shown bank's sixteen drawn as its steps; MACHINE's kit as pictures, every machine a picture of its sound, what it is for and what it costs | #175 | ○ (W44) | draw() 1.28 ms PROJECT, 2.50 ms MACHINE (emulator, cl6x) |
| **EDIT and MIXER redrawn to the drafts** (docs/SCREENS.md): EDIT's knob line and a picture on every page of every machine; MIXER's eight-strip desk, duck, DLY, REV, ROUTE, MST with LUFS, EQ curve; lists with a lit cursor, the step page's lane, CHAIN's strip | #174 | ○ (W43) | draw() see docs/BUDGET.md "The screen"; GRID and PERFORM unchanged |
| **Screens rebuilt to the mockups**: frame (badges, toasts, tabs), PERFORM, GRID, EDIT (graph rows 50-117, filter response, LFO trail, WAVE LIVE, GRAIN cloud), MIXER master (spectrum, vectorscope, CMP/LIM bars) | #171 | ○ (W42) | draw() 1.71 ms on PERFORM, 1.20 ms on MST (emulator, cl6x). Knob values in the small font and LUFS on MST came with #174 |
| **Live screens 6**: MIXER MST/EQ spectrum (24 bands, peak dots) and vectorscope; live meters beside the faders | #170 | ○ (W41) | draw() 1.21 ms on MST (emulator, cl6x) |
| **Live screens 5**: WAVE's SRC page draws the table as a waterfall (POS brightest); GRAIN's grains spark with a fading trail | #164 | ○ (W40) | |
| **Live screens 4**: the LFO page's shape with the phase riding it and an output strip; envelope fills lit as a note travels | #163 | ○ (W35) | |
| **Live screens 3**: GRID trigs glow and fade; PERFORM macro gauges over a scope, eight track meters (while running) | #162 | ○ (W34) | draw() 1.34 ms a frame on PERFORM (emulator, cl6x) |
| **Live screens 2**: audio taps for the screens: each track's level, each LFO's phase, the output into a ring for scopes and spectra (`core/scope.hpp`) | #161 | - | About +0.3 points of load, fixed |
| **Live screens 1**: the meter in decibels and segments, beat and bar marks on the progress line, draw time on SETUP; the drawing kit (`app/paint.hpp`: AA lines, arcs, dots, afterglow, 3 x 5 font) | #160 | ○ (W33) | Is the 3 x 5 font readable on the OLED? `paint::kSmallLabels` |
| **Locks on screen**: heard value + kit tick on GRID and EDIT bars, `S5 CUT 64>90`, GRID cell marks (locks, condition, probability, ratchet, trigless) | #146 | ○ (W27) | |
| **Waveforms and envelopes in EDIT**: SAMPLE and GRAIN draw the sample with window, slices and playheads or grains; AMP and ENV draw the envelope (FLTR the filter's curve since #171) | #150 | ○ (W28) | The picture is made at load (250 bytes a track); a streamed file shows its resident head |

## Controls and UI

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Controls concept from the panel: one job per button group ([CONTROLS.md](CONTROLS.md)) | v0.1.0 | ◐ (2, 5) | |
| FUNC + function key = its printed second label (sampling, pool, system, ◀◀, ▶▶, fx, rec src) | v0.1.0 | ◐ (2) | |
| FUNC menu (FUNC+HOME): save, save as, open, new, reload, kits, sample reload, track speed, chain, song/loop, auto-fill, quantize; freeze (#109), loop length (#116), load demo (#140), track play (#148), FX glide (#149) | v0.1.0 | ◐ (14, W12, W13, W20c) | |
| NEW PROJECT is empty (factory sounds and mixer, pans centred, no steps, scenes or macros); LOAD DEMO loads the factory demo | #140 | ○ (W12) | |
| **CLEAR PATTERN** in the FUNC menu: every track in one press, OK twice, one undo | #86 | ○ (W16) | Keeps sounds, lengths, speeds and swing |
| **Help for the screen that is up**: its knobs in words and its gestures, every screen and EDIT/MIXER page, then five shared pages (FUNC + key, FUNC + knob or pad, upper pads, the performance mixer); screen keys turn its pages while FUNC is held | #107 | ○ (W19) | Text only; it says what the build does, so a changed gesture changes `core/help.cpp` too |
| HOME = GRID, HOME twice = KEYS | v0.1.0 | ◐ | |
| Lit, self-naming performance pads; mode keys green/red by screen | v0.1.0 | ○ (W8, W20c) | Upper pads are printed TRK 1-7 but are performance pads (Roland's choice) |
| Arrow-key navigation model (UP/DOWN rows, LEFT/RIGHT sideways, OK picks, CLR back) | v0.1.0 | ○ (5, "Arrow keys") | |
| Screens: GRID, step page, EDIT (6 pages, 8 on the VA, WAVE, FM4 and GRAIN machines), MACHINE, KIT, SOUNDS, KEYS, MIXER (12 pages), PERFORM, PROJECT (5 pages), SAMPLES, RESAMPLE, menu | v0.1.0 | ◐ (4) | |
| Fixes: the FUNC menu never shows a MUTATE verdict OK would not give; OK in RESAMPLE no longer retargets a take being written | #84 | ○ | |

## Engines (machines)

Eight tracks, any machine on any track (MACHINE, FUNC+EDIT). Each track: 16 parameters on four
pages (SRC, FLTR, AMP, FX), plus LFO and ARP pages, and ENV and OSC pages on the VA, WAVE,
FM4 and GRAIN machines.

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| KICK (909-style, 2 voices, follows the note) | v0.1.0 | ✔ plays in the default beat | One-shot; DEC is the length |
| SNARE with clap mode (2 voices) | v0.1.0 | ✔ default beat | One-shot |
| HAT, 808 recipe (2 voices), track 3 chokes track 4 | v0.1.0 | ✔ default beat | Choke pair is fixed to tracks 3→4; the CHK knob only shows it |
| PERC, 2-operator FM (2 voices) | v0.1.0 | ✔ default beat | One-shot |
| SAMPLE: one-shot or 1-16 equal slices by note, 4 voices, 4 play modes (fwd, loop, rev, rev loop) | v0.1.0 | ◐ default beat's loop track (placeholder sample) | Unit sample loads: 13b, 15b-15d |
| SAMPLE **SLC AUTO**: slices at up to 15 detected hits | v0.1.0 | ○ (15c2) | Hit points are found at load, not editable |
| A file picked on a SMP track plays whole from its start (STA 0, LEN 127, one slice, DEC 127) | #133 | ○ | Only a pick; a project or kit load keeps its saved values |
| Switching a track's machine loads that machine's default sound (a switch to GRN loads CLOUD) | #133 | ○ | |
| **SAMPLE holds notes**: SUS and REL on the ENV page (SUS up holds a sample, a looped one becomes a pad, for LEN or a held pad; REL 30 ms-8 s) | #173 | ○ (13k2) | ATK stays on AMP, so ENV's A and D read `---` on SAMPLE |
| GRAIN: up to 8 grains in one voice | v0.1.0 | ○ (8, 8b / U3) | Most expensive machine; outside the POLY budget (thinned by the CPU guard); no tempo sync of its own |
| **GRAIN rework**: +6 dB, PIT in whole semitones, SIZ 8-680 ms, ENV page (ATK SUS REL: pads that hold for LEN), OSC page (SCN scan/time-stretch, PSP detune or octave shimmer), values in units, 8 SOUNDS (OK in EDIT), a switch to GRN loads CLOUD | #134 | ○ (W25) | SCN is free-running, not tempo-locked; older projects' GRN tracks play 6 dB louder with slightly different SIZ and PIT snapped to the nearest semitone |
| VA MONO: saw or pulse, OSC 2 (detune or sync), sub, noise, drive, own filter and envelope, accent, slide, glide | v0.1.0 | ✔ default beat bass | At SUS 0 (default) DEC is the note's length and LEN is not heard; see the ENV row |
| VA POLY: up to 6 notes, 13 chord types with inversions, DET | v0.1.0 | ✔ default beat chord | At SUS 0 LEN only shortens a note; see the ENV row. No stereo spread or unison 2 (in SPEC) |
| **Held notes on the VAs: ENV page** (ATK SUS REL FSU, seventh EDIT page; named ASR until #174); with SUS up a note holds while the key is held or for its step's LEN (up to 64 steps) | #90 | ○ (13k) | Held voices count against the 12-voice POLY budget for as long as they hold; MONO counts LEN only once SUS is up |
| **VA sound rework: OSC page** (WAV OS2 SUB DRV, eighth EDIT page): saw or pulse width, a detuned or hard-synced OSC 2, sub level, drive into the filter; RES self-oscillates near the top, LP24 squelches like LP12 | #118 | ○ (13p, W22) | Older projects lose the old fixed synced pulse (load `SAW OFF 102 000`). No key tracking to the filter |
| **One filter on the synths (D8)**: on the VA and WAVE machines FLTR is the voice's own filter (CUT RES ENV FDE; FDE its own decay, or 0.55 x DEC at 0), the insert filter one TON knob; **velocity opens the filter (D15)**; **cutoff floor (D9)**: a swept cutoff stops at about 100 Hz | #139, #147 | ○ (13q, 13e) | Older projects' insert filter on these tracks loads as the nearest TON, without its drive and resonance |
| **WAVE MONO and WAVE POLY**: the VA voice with a wavetable (16 built-in tables of 16 frames: TBL POS, MOT envelope to position, WRP phase bend, DET second reader), 8 + 7 factory sounds | #122 | ○ (W23) | CPU per voice is a host estimate until the emulator and the unit measure it (BUDGET "WAVE"); a table is built when a track first sounds it (8 slots, 1.25 MiB; the sub plays alone for a moment). No user tables from the card yet, though the slots leave room for them |
| **FM4**: four-operator FM, eight algorithms, operator 4 feedback, an index envelope, six voices with chords in the shared 12-voice POLY budget; 8 factory sounds | #126 | ○ (W24) | CPU per voice is a host estimate (about 1.7x a VA POLY voice) until the emulator and the unit measure it (BUDGET "FM4", E15). |
| MID: plays an outboard synth over MIDI; SRC/AMP knobs send CC 74/71/73/72, 70/75/76/77 | v0.1.0 | ○ (8d) | No sound or CPU of its own; no scale fold |
| Key and scale per pattern (15 scales), fold for the VA, WAVE and FM4 machines, GRAIN and unsliced SAMPLE; sharps everywhere | v0.1.0 | ○ (7) | |
| **NOTE view**: TUN / PIT read as a note (where TUN puts C-4) and turn a semitone a click; push the knob for 0-127 | #88 | ○ (W3) | Chromatic, not by scale (TUN applies after the fold). 128 bytes per range, so many notes are a few cents off (`+`/`-`). Not saved: on at every start |
| Golden renders pin every machine's sound | v0.1.0 | — | Host only |

## Sequencer

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Up to 64 steps a track, per-track length and speed (2x ... 1/8): polymeter | v0.1.0 | ○ (M3) | |
| Steps, trigless steps (FUNC + tap), STEP LED colours | v0.1.0 | ◐ | |
| Step page: NOTE, VEL, LEN, MICR (±11 ticks), PROB, COND, RTCH 1-8, RAMP, ACNT, SLID, CHRD | v0.1.0 | ○ (6) | LEN is the note length on VA POLY, on VA MONO once SUS is up, and for MIDI out; ignored by drums, SAMPLE and GRAIN |
| Trig conditions: a:b, FILL, PRE, NEI, 1ST and their NOTs; seeded | v0.1.0 | ○ (M3) | |
| **Play modes per track**: FWD, REV, PING, RND1-RND16 (FUNC menu TRACK PLAY); seeded, the pattern untouched | #148 | ○ (W13) | Live on PERFORM too: LIVE RND, below |
| Parameter locks: 8 a step, 1,024 a pattern; push the knob to remove one | v0.1.0 | ○ (6) | `FULL n` when the pool is full (never reached on the unit) |
| **Random lock ranges** (hold STEP + FUNC + turn): each pass a new value within the range, drawn from the pattern's seed | #167 | ○ (W37) | Takes one of the step's 8 lock slots |
| **Parameter slide** (hold STEP + FUNC + push): a lock glides to the next step that plays, in play order | #167 | ○ (W37) | Ignores the target step's condition and probability, swing and micro-timing; not on a step stutter |
| Pattern swing and **per-track swing** (PATTERN page) | v0.1.0 | ○ (13e) | |
| **FEEL**: DRIFT 1-6 (each track its own curve) or WAVE 1-6 (one four-bar curve), over swing and micro (PATTERN page, D) | #152 | ○ (W29) | Up to 6 ticks of 96 PPQN; seeded |
| Euclidean fill (FUNC + A/B/C in GRID) | v0.1.0 | ○ (M3) | |
| Copy/paste a step, a range or a track; range transpose and nudge | v0.1.0 | ○ (M3) | |
| **Pattern menu** (hold OK on a PROJECT slot): COPY, PASTE, CLEAR, DOUBLE; FUNC + UP / DOWN / LEFT on the slots | #140 | ○ (W12) | DOUBLE stops at 64 steps |
| **Ratchets over LEN**: RTCH past 8 is a roll at a rate (1/64 to 1/4) for the step's whole LEN, its RAMP across the roll | Unreleased | ○ (W48) | Roland's RATE + span flag (2026-11-26): bit 7 of the ratchet byte, no file change; an older build plays such a step as a one-step ratchet. A new trig on the track, STOP, a restart or a pattern change ends a roll. Under the arp, the arp takes the step |
| **Hold STEP + upper pad edits the step (D6)**: FILL condition, REPEAT ratchet, MUTE A:B, MUTATE probability, THROW a delay (FUNC: reverb) send lock; FILL + STEP writes a FILL trig | #140, #142, #153 | ○ (W9) | SCENE A/B still perform with a STEP held |
| 16 levels of undo and redo | v0.1.0 | ○ (6) | Chain edits and kit recalls are not undoable |
| Live recording (pads, KEYS, MIDI) with latency correction and QUANTIZE 0-100%; **held length recorded as LEN** (#90) | v0.1.0 | ○ (M3, 13k) | Note-repeat rolls are not recorded; a held chord on one step keeps only its last note (as before) |
| Motion recording of knob moves as locks | v0.1.0 | ○ (M3) | MIDI CCs are not motion-recorded |
| **KEYS on the pads: two rows as a piano**, velocity, held, octave pads; the STEP keys are steps there (D12, #142; until then they played the scale) | #85 | ○ (W8) | |
| Per-track LFO: 6 shapes, free 0.06-32 Hz or synced 8 bars-1/32, any of the 16 params; lockable, scene-able, macro-able | v0.1.0 | ○ (13e) | Steps at about 750 Hz; on VA and WAVE an LFO on TUN glides instead (S3) |
| LFO TRIG: FREE, RETRIG, ONE-SHOT (push RAT); ONE-SHOT shapes are envelopes | S2 | ○ (W30) | A kit setting, not lockable; no new shapes (see the PR) |
| Cutoff floor (~100 Hz) for LFO, morph and macros; LFO depth on a cutoff about +-3 octaves | D9 | ○ (W30) | The knob and locks still close a filter fully |
| **Two MOD slots per track**: LFO, ENV (the LFO's shape once per note), VEL, PRS (pad pressure), RND (per note) to any sound, ENV or OSC parameter, signed depth; learnt by push C / D on the LFO page and a turn | #168 | ○ (W38) | A kit setting, not lockable; not on LFO or ARP knobs; on MID the slots reach the CCs |
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
| Mutes on the bar (FUNC + track pad; several pads in one bar since #139), MUTE MODE, 8 stored mute states | v0.1.0 | ○ (3b, W12) | |
| 16 scenes, A/B morph on VALUE, capture whole or one track | v0.1.0 | ◐ (3b) | Morph position is not saved; locks are not morphed (parked) |
| 4 macros of up to 4 targets, sweep from the live value; demo B ACID, C DUB, D DRIVE | v0.1.0 | ○ (3b) | |
| **Macro learn by turning (D11)**: push a macro's knob in PERFORM, turn EDIT knobs, OK | #141 | ○ (W10) | |
| The kill EQ's bands learnt by turning on MIXER's EQ page during MACRO LEARN (FUNC + STEP 5-7 retired) | #156 | ○ (W32) | |
| **LOCK** (FUNC + MUTATE: hold stores, tap jumps, tap again swaps back), one per pattern, RAM only | #149 | ○ (W20c) | Not saved; the kit is shared, so a jump brings that lock's sound to every pattern |
| **REL ALL / LEN ALL**: every track's release (FUNC + A) and every note's length (FUNC + B) in PERFORM | #152 | ○ (W29) | Live only; LEN ALL leaves gateless notes (drums, arp) alone |
| **LIVE RND**: FUNC + turn D in PERFORM shuffles every armed track in blocks of 1-16 steps, FUNC + push D arms the selected track; OFF is the grid at once | #166 | ○ (W36) | Live only, in a LOCK; the pattern is never written |
| Pad pressure bends macro A | v0.1.0 | ○ ("Pad feel") | |
| THROW to delay or reverb | v0.1.0 | ◐ | |
| MUTATE (rotate, density, velocity, micro, chance, notes), OK keeps, CLR reverts | v0.1.0 | ○ (10) | |
| Note repeat on the pads, pressure as velocity | v0.1.0 | ○ (13f) | Not recordable |
| Step stutter: hold REPEAT + STEP loops that step on every track (FUNC: one track) | #151 | ○ (W11) | Sequencer, not audio: sounds stay live. Host-tested; the emulator cannot press pads |
| Roll rate on LEFT/RIGHT under REPEAT or a rolling pad, on the live strip | #151 | ○ (W11) | |
| Tape stop (performance mixer slot 8; FUNC + hold push D until #145) | v0.1.0 | ○ (13g) | |
| Delay freeze (performance mixer slot 5, ECHO FREEZE; FUNC + hold MUTATE until #145) | v0.1.0 | ○ (13g) | |
| **Tap VALUE to reset**: the effect's amount, the loop's mix or the morph back to 0, riding over FX GLIDE; FUNC + tap: a cut | #121, #124, #149 | ○ (W20, 13o) | A hold of 0.5 s or more resets nothing; never the tempo |
| **Performance mixer on VALUE**: push VALUE + STEP picks one of 14 effects (sweeps, risers, echo freeze, wash, mute bus, tape stop, bass kill, pump, crush, noise, gate, build) or the morph (13), VALUE is its amount; hold the STEP to push an effect in | #116, #145, #149 | ○ (W20, W20b, 13o) | One effect at a time. Not saved. CPU host-estimated only (about 2 points worst case with the looper) |
| **FX GLIDE** (FUNC menu): the performance mixer's button jumps ride over OFF, 1/16, 1 BEAT or 1 BAR; back to rest in at most a 1/16 | #149, #157 | ○ (W20c, W31) | Not saved |
| **Master looper** (VALUE + STEP 16): 1-8 bars of the mix from the bar line, VALUE crossfades live/loop, effects go over it; STEP 16 again leaves it and keeps the loop | #116, #120, #139 | ○ (W20, 13o) | Shares RESAMPLE's 30 s buffer: no RESAMPLE or FREEZE while a loop is held. No overdub yet. Not saved |

## FX and mixer

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Insert chain per track: drive, filter (LP/BP/HP 12/24), lo-fi, amp, duck, pan, level | v0.1.0 | ◐ | Off stages cost nothing |
| **Sidechain duck**, editable: depth per track, source track, attack/hold/release | v0.1.0, editable since #89 | ○ (13l) | MIXER pages DCK 1-4, DCK 5-8, DUCK. Saved in the kit (KIT.BIN section 6; older kits open with the v0.1.0 duck: track 1, 35%, 3/40/120 ms). Not p-lockable, not in scenes or macros: a lock slot would change the pattern and scene file layouts |
| **CLICK and COUNT-IN**: a metronome, and 1 or 2 bars clicked before a PLAY with REC on (CyDrums S6) | #182 | ○ (W47) | FUNC menu rows. STRATA's own click (Enaga has none), 1 kHz / 1.6 kHz on a bar, after RESAMPLE's tap, so never recorded. A count-in starts the clock at once and holds the sequencer, rolls, stutter and arps for its bars. Internal clock only. Not saved |
| **Choke groups** 1-4 for any track: a note cuts the other tracks of its group (CyDrums S4) | #181 | ○ (W46) | Push A-D on MIXER's TRK pages; `C1`-`C4` on the strip. A 64-frame fade on the track's output, then its voices are cut. Two notes of a group on the same frame both sound (a block renders whole). Saved in the kit (KIT.BIN section 11; older kits have no groups) |
| Send A: tempo-synced ping-pong delay | v0.1.0 | ◐ (13c / U4) | |
| Send A **LONG** mode: up to 12 s, wow, diffusion, low cut, true freeze | v0.1.0 | ○ (13j) | ~2x the send's CPU while it rings; load host-estimated only (E11 owed) |
| Send B: reverb | v0.1.0 | ◐ | |
| Send B **HALL** mode with shimmer | v0.1.0 | ○ (13j) | As LONG |
| Delay into reverb (D>R) routing | v0.1.0 | ○ ("The sends, the master and SETUP") | Routing page has one empty slot |
| Master: DJ filter, glue compressor, limiter, output level | v0.1.0 | ◐ | |
| Master **kill EQ**: 3 bands at 250 Hz / 2.5 kHz, kill switches (FUNC + push A/B/C), scene-able, macro-able | v0.1.0 | ○ (13d / U6) | |
| MIXER: 8 faders, 12 pages (3 for the duck since #89) | v0.1.0 | ◐ | |

## Samples and resampling

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| WAV 8/16/24-bit or 32-bit float, mono or stereo, any rate (converted to 48 kHz: 96 kHz low-passed and halved, other rates through a windowed sinc since #143) | v0.1.0 | ○ (13b / U5) | More than two channels refused |
| Background loading, the beat keeps playing | v0.1.0 | ○ (13b) | |
| Long files at 48 kHz up to 349 s, read in 2 MiB chunks (4 MiB until #124) | v0.1.0 | ○ (15c) | 64 MiB sample memory shared by all tracks (32 until 2026-11-23) |
| Other rates: up to 30 s at 96 kHz; longer files stream (#124) | v0.1.0 | ○ (13b) | |
| Streaming of files too big for memory (2 s head + card stream, 4 tracks max) | v0.1.0 | ○ (15d) | Forward only, one voice: no STA, slices or reverse (not feasible, below); GRAIN plays only the head; CPU unmeasured |
| Streamed samples honour LEN (end point, loop back to the head) | #91 | ○ (15d) | A LEN loop inside the 2 s head never touches the card |
| Sample browser (`/SAMPLES`, `/RECORD`), load while playing | v0.1.0 | ○ (15b) | 256 entries a folder; path ≤ 111 characters |
| Sample audition: push A in SAMPLES hears a file, whole, without loading it | #169 | ○ (W39) | Streamed from the card at the file's own rate, -6 dB, after every effect; 8-96 kHz WAVs; one at a time |
| Sample browser reaches the stock SmplTrek folders (`POOL` = `/SmplTrek/Pool/Audio`, `SMPLTREK` = `/SmplTrek`), read-only | #117 | ○ (W21) | shown only when the card has `/SmplTrek` |
| Resample the master, 1-8 bars, to a file or straight onto a track | v0.1.0 | ○ ("Resampling") | Takes ≤ 30 s |
| Bounce: resample ONE track alone (B = `TRK 1`-`TRK 8`), mono, before its duck, fader, pan and sends | #109 | ○ (13m) | No emulator scenario: B is an encoder, the emulator presses keys only |
| **Bounce to track**: a take to FILE is offered to a SAMPLE or GRAIN track (`PUT ON TRK 3 GRN`), one OK puts it there | #153 | ○ (13m) | `scenario_bounce` runs a take through Enaga's real file layer. Takes failed `DENIED` on the unit before #153 (Storage made `/RECORD`) |
| **Freeze**: FUNC menu FREEZE, a track plays a 1-8 bar loop of its machine from RAM; chain, mix and sends stay live; instant unfreeze | #109 | ○ (13n), E (scenario_freeze.sh) | Not saved: a reload, NEW or kit recall unfreezes. Measured (cl6x): A05 held pad 20.3% to 10.1% average; A01 peaks 7 points lower |
| Sample from the input: LINE, LINE L, MIC, DYN MIC, HI-Z, with level and meter | v0.1.0 | ○ (8c) | No monitoring through STRATA; MIC needs headphones in |

## MIDI and sync

| Feature | Since | Unit | Known limits / notes |
| --- | --- | --- | --- |
| Follows the unit's clock: internal, MIDI clock, SYNC IN; refuses tempo/transport under external clock | v0.1.0 | ○ (M7) | STRATA cannot set CLOCK SRC (Enaga has no setter). SYNC polarity not selectable (Enaga) |
| MIDI OUT per track (OFF/1-16): steps, chords, ratchets, arp, note repeat, pads, KEYS, MIDI IN | v0.1.0 | ○ (8d) | **On Enaga 0.2.0 (v0.1.0-v0.3.1):** with USB connected to a computer not reading, DIN out stalls; unplug USB. Fixed in Enaga 0.3.0, which `main` needs |
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
| Reload project, new project (empty since #140; LOAD DEMO for the factory content) | v0.1.0 | ○ (14, W12) | All three stop the transport |
| Project browser: open, save as with a name | v0.1.0 | ○ (14b) | |
| Project rename (push B) and delete (push C, OK twice), safe against a pulled card | #91 | ○ (14c, W17) | Rename is a copy then a delete (Enaga has no rename). The open project cannot be deleted |
| Kit library: 16 kits on the card, recall while playing, store | v0.1.0 | ○ (15) | Recall is not undoable |
| Damaged files refused, never crash | v0.1.0 | ○ (M4) | Card pulled mid-save not tried on the unit |
| Factory (demo) project: 130 BPM C minor kit, A01-A05, chain, 3 scenes, 3 macros, 8 mute states; a first start and LOAD DEMO | v0.1.0 | ✔ A01 plays; ◐ the rest (3b) | Track 8 plays a placeholder until given a sample |
| Factory pattern A05 PAD: held VA POLY chords from the ENV page, in the pattern's locks | #100 | ○ (13k) | The kit's chord stays A01's SUS 0 stab |
| Factory sounds: SOUNDS (OK in EDIT or MACHINE) loads one of 76 ready-made sounds (KCK 6, SNR 6, HAT 7, PRC 7, MNO 12, PLY 7, WMN 8, WPL 7, FM4 8, GRN 8) into one track, AS WAS puts it back | #102, #118, #122, #126, #134 | ○ (15e) | Leaves FX, LFO, ARP, fader and HAT's choke group; not undoable except by AS WAS |

---

## Gaps and next ideas

**Still owed before the status above can say ✔**

1. **The unit walkthrough.** Almost every row is ○. The top of UNIT_CHECKLIST.md orders it; the
   first numbers needed are the clean A01/A04 MAX, STRATA6 (six voices), U1-U9.
2. **Settle the provisional numbers from those readings:** `kPolyVoiceBudget = 12`, the CPU
   guard's thresholds, the duck's factory depth (now editable, so only the default is open).
3. **Emulator runs on Roland's PC:** E1-E11 (BUDGET.md), `scenario_controls.sh`,
   `scenario_midi.sh`, `scenario_ambient.sh`, and the cl6x figures for the post-v0.1 features.
4. **The fused VA loop is off** (cl6x renders it differently, BUDGET.md): ~1.7 points to win back
   once the cause is found.

**Known limits worth building next**

1. **More engines:** WAVE MONO and WAVE POLY (#122) and FM4 (#126) are in.
2. ~~An editable sidechain duck~~: done (depth per track, source, attack/hold/release in the
   kit). Left: making the depth lockable or scene/macro-able.
3. **MIDI:** pitch bend and aftertouch in, CC motion recording, slide as legato out. (The DIN
   fix came with the move to Enaga 0.3.0, #139.)
4. ~~A per-project sample memory above 32 MiB~~ **Done 2026-11-23: every project gets 64 MiB**, so
   fewer files need to stream. Streamed LEN is built; streamed STA, slices and reverse were looked at on 2026-11-23
   and are not feasible on Enaga 0.2.0's card path ("Why streamed samples play forward only",
   below).
5. ~~Resample one track~~ **Built 2026-11-23 as the bounce** (RESAMPLE, B = `TRK n`).
6. **VA POLY stereo spread and unison** (SPEC promise), and the 6-to-8 voice question once U1/U2
   are in.
7. **Undo for chain edits and kit recall.** (Copying a whole pattern to another slot came with
   the pattern menu, #140.)
8. ~~Delete and rename projects on the unit~~: built (push B / push C on PROJECTS), as a copy
   and a delete since Enaga has no rename.

**From the ideas list, not built**

- A chorus as a third send (`enaga::dsp::Chorus`, 1-3 points, ~100 KiB).
- More scenes or per-pattern scenes.
- Scene morphing of locks (parked: no single sensible blend).
- Skipping KICK's click path after its transient (a CPU win, needs a listening test).

**Budgets that limit new work:** heap 97.04 of 120 MiB at init with LOCK's 1.06 MiB (22.96 MiB spare,
against an 8 MiB headroom rule), and an estimated ~63% unit load with everything on at once (BUDGET.md
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

