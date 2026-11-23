# Unit checklist

The first part is **tonight's walkthrough** (2026-11-22): one pass, top to bottom, with the unit
in hand. It covers everything merged today and is ordered so each mode is visited once. Tick the
box, write the value or a word on the line, and move on; anything odd goes in the notes line and
is chased tomorrow. The second part, "Reference", is the full per-milestone list it was drawn
from, with the background for each check.

**Time.** The whole pass is about three hours. If the evening runs short, do 0-3b, 13 and 16
first (safety, the MAX readings, save and restore), then the sections for today's new features
(8c, 8d, 13d-13l, 14b, 14c, 15b-15d), then the rest.

Keys are named as printed on the SmplTrek (see the legend in `docs/CHEATSHEET.md`), and so are
the on-screen hints. `FUNC+X` = hold FUNC, press X.

Build on the card: STRATA `________`  STRATA6 `________`  cl6x / GCC only: `________`

---

# The new controls (docs/CONTROLS.md, after 2026-11-22's first evening)

Do this first next time the unit is in hand (10 min). It checks the controls concept built from
the SmplTrek's panel: each button group has one job, FUNC + a function key is the label printed
under it, and the setup actions are in the FUNC menu. Notes on anything that still feels wrong
are the point of this section.

- [ ] **Performance pads lit.** On start the seven upper pads (printed TRK 1-7) glow dim: white,
      cyan, red, green, blue, amber, violet, left to right. Hold FILL: it brightens and the
      second line says `FILL`. Tap MUTE: red stays bright (`MUTE MODE`); again: dim.
- [ ] **Pads name themselves.** REPEAT says `REPEAT 1/16`, SCENE A `SCENE A: STEP PICKS`, THROW
      `THROW DELAY` (FUNC + THROW `THROW REVERB`), MUTATE `MUTATE T1 <type>` and stays bright
      violet until OK (`KEPT`) or CLR (`REVERTED`).
- [ ] **FUNC help.** Hold FUNC alone for a second: the screen lists FUNC + REC ... EDIT and the
      upper pads. Let go: the screen is back. FUNC + a pad (a mute) never shows it.
- [ ] **FUNC lights while held**, green. **Mode keys:** HOME green on GRID, amber in KEYS, red in
      the menu; MIXER green / PERFORM red; EDIT green / MACHINE red; PROJECT green / SAMPLES red;
      REC green on RESAMPLE, red while recording.
- [ ] **HOME.** From any screen HOME is GRID. HOME twice quickly: `KEYS`. HOME again: GRID. A slow
      second press on GRID stays on GRID.
- [ ] **FUNC + PROJECT** ("pool") opens SAMPLES on the selected track. **FUNC + REC** RESAMPLE,
      **FUNC + MIXER** PERFORM, **FUNC + EDIT** MACHINE, as before.
- [ ] **The FUNC menu.** FUNC + HOME ("system"): `FUNC MENU`, cursor on SAVE, rows SAVE ... QUANTIZE
      with values on the right, nothing overlapping. UP/DOWN and A scroll; CLR goes back to the
      screen it opened over. **FUNC + HOME, OK saves** (`SAVING`).
- [ ] Menu rows: TRACK SPEED with LEFT/RIGHT changes the grid's `X n`; CHAIN shows ON/OFF and OK
      flips it; AUTO-FILL and QUANTIZE step with LEFT/RIGHT; NEW PROJECT needs OK twice
      (`OK AGAIN FOR NEW`); KITS opens the kit browser.
- [ ] **CLEAR PATTERN.** On a playing beat: FUNC + HOME, DOWN to CLEAR PATTERN, OK: `OK AGAIN TO
      CLEAR` and nothing changes. OK again: `CLEARED FUNC+CLR UNDO`, the menu closes, every track
      goes silent and GRID is empty, but the sounds are the same when you tap a step back in.
      FUNC + CLR: the whole beat is back at once. One OK, wait 3 s, OK: only arms again.
- [ ] **Screen keys = pages.** EDIT and MIXER as before. On PROJECT they walk PROJECT, CHAIN,
      PATTERN, SETUP, PROJECTS. On GRID they change the EDIT page the knobs turn (the second line
      says `FLTR` etc.); LEFT/RIGHT still flip GRID's step page.
- [ ] **Pad-row keys = which sixteen.** GRID: step page. KEYS: octave. PROJECT: the bank
      (`BANK B`). PERFORM and KIT: nothing.
- [ ] **CHAIN:** CLR on an entry: `AGAIN TO REMOVE`; again: removed. On an empty chain CLR goes back.
- [ ] **KEYS on the pads** (HOME twice, track 7 VA POLY): the pads light as a keyboard (root bright,
      the scale dim, the rest dark, two dim white octave pads in the upper row). Lower pads play
      C to C, upper pads the sharps between them; soft and hard hits sound different; a held pad
      sustains; the upper pad between E and F is octave down, the last one octave up (`OCT n`).
      FUNC + lower pad still mutes on the bar. Does it play like a keyboard? `____`
- [ ] **Macro assignment.** PERFORM, macro A (LEFT/RIGHT), a STEP: `A T1 ... 1/4`. The same STEP
      again: `... OFF 0/4`. Macro D (factory, four used), a STEP: `D FULL: CLR CLEARS`. Reliable now? `____`
- [ ] **Macro learn.** PERFORM, push knob A: EDIT with `LEARN A n/4`. Push C on FLTR: `A T1 ... `
      with the count. Another track (DOWN), push again: added. CLR: `KNOBS EDIT AGAIN`; CLR again
      at once: the pattern stays. Learn again, HOME: ended. Turn macro A up, EDIT, turn that knob:
      `HELD BY MACRO A`. Back to
      PERFORM, turn A: both move. Easier than the STEP keys? `____`
- [ ] **Velocity on the STEP lights.** Four trigs, step page VEL 20, 60, 95, 120: four clearly
      different greens, dimmest to brightest? `____` A trig with PROB 50% blinks. Locks still amber.
- [ ] **TUN as a note** (2026-11-23). Track 1 (KICK), EDIT SRC: A reads `A-1`, not `064`. Turn A
      one click: `A#1+` (a few cents sharp of A#1: the `+`), and the kick is a semitone higher;
      twelve clicks from `A-1`: `A-2-`, an octave up. Push A:
      `TUN AS NUMBERS`, A reads a number and a click is one count. Push again: `TUN AS NOTES`. Hold
      a STEP + push A still clears that lock (`LOCK CLEARED`), and FUNC + push A is still KILL LOW.
      On GRID the bottom slot shows the same. Track 7 (VA): turn TUN to `C-4`, then twelve clicks
      up: `C-5`, exactly an octave. Legible, and is NOTE view the right default? `________`
- Notes on what still feels awkward: `____________________________________________`

---

# Tonight's walkthrough

## On the PC, before building the card (15 min, plus E2-E5 if there is time)

From `D:\DEV\STRATA` through WSL, as in CLAUDE.md (`wsl.exe -d Ubuntu -- bash -c 'cd
/mnt/d/DEV/STRATA && ...'`), on an up-to-date main with PR #17 (the MAX readout) merged.

- [ ] `make -C host test` passes. Checks: `________`
- [ ] `tests/scenario_factory.sh` passes with cl6x: it plays A01 GROOVE, queues A04 PEAK while
      running, and checks the switch, no overruns and peak under 60%.
      Emulator peak, cl6x `________ %`
- [ ] The same with `ENAGA_TI_CGT=none tests/scenario_factory.sh` (GCC only).
      Emulator peak, GCC `________ %`
- [ ] `tests/scenario_pad.sh` (new: queues A05 PAD, held chords all the time) with cl6x and GCC
      only; record both in docs/BUDGET.md, "A05 PAD". Peak `____ %` / `____ %`
- [ ] `tests/scenario_midi.sh` (new: track 1 to MID on the running beat, both STOPs) passes with
      cl6x and GCC only. It is part of E1's `tools/scenarios.sh` too. Peak `____ %` / `____ %`
- [ ] **E1.** `tools/scenarios.sh` (every scenario) passes, with cl6x and with GCC only, on main
      now and on `eb46d43` (before the DSP optimizations, PR #26): four columns for BUDGET's
      optimization review. `scenario_m3.sh` cl6x now: `________ %` (unit/emulator ratio in 3)
- [ ] For U5 (section 13b), the test file is ready in the project files, `unit-test/`:
      **`STRATA_1.WAV`**, a 15 s stereo 16-bit 96 kHz linear sweep from 100 Hz to 46 kHz at
      -6 dBFS. Also there, `SWEEP_30S_96K.WAV`, the same sweep at 30 s, which now loads too (13b).

The next four are BUDGET's emulator runs (`docs/BUDGET.md`, "Optimization review"). They are not
needed for tonight's unit session; do them if the PC is free.

- [ ] **E2.** VA POLY per voice after PR #26: MIN7 vs MIN11 in `groove()` (`core/factory.cpp`,
      where `build_default_beat` went), `scenario_m3.sh`, both compilers. Was 2.16 cl6x / 3.82
      GCC per voice. Now: `________` / `________`
- [ ] **E3.** The app's real worst case: four, then six, then eight tracks on `va_poly` with MIN11
      chords. Crosses 60% at `____` tracks, overruns at `____`.
- [ ] **E4.** `scenario_m5.sh` against `scenario_m3.sh`, before and after PR #26 (the prepare
      memo's share). Gap before `________`, after `________`
- [ ] **E6.** The master kill EQ's cost: `tests/scenario_killeq.sh` (kills all three bands, so
      the EQ runs) against `tests/scenario_m7.sh` (EQ bypassed), both compilers. Proxy estimate
      ~1.3 points cl6x, ~2.2 GCC-only (`docs/BUDGET.md`, "The master kill EQ").
      cl6x `________ %` vs `________ %`, GCC `________ %` vs `________ %`
- [ ] **E7.** Eight LFOs' cost: `tests/scenario_lfo.sh` as it is (every LFO off, should match
      `scenario_factory.sh`), then again after the temporary `factory_kit` edit in
      `docs/BUDGET.md` ("Per-track LFO", E7: every track `params[17] = 40`, `[18] = 127`,
      `[19] = 38`), both compilers, and revert the edit. Host estimate ~1.1-1.4 points.
      Off: cl6x `________ %` GCC `________ %`. Eight on: cl6x `________ %` GCC `________ %`
- [ ] **E8-E10.** The cl6x vectorisation pass (`docs/BUDGET.md`, "The cl6x vectorisation pass"):
      `python3 -B tools/ti_loops.py --enaga` (pipelining report), then `bash tools/ti_ab.sh --gcc`
      (about an hour; builds each variant in its own worktree, so main is untouched). Audio check
      all `identical`: yes / no `____`. `head` vs `base`, scenario_m3 cl6x: `____ %` vs `____ %`.
      VA POLY per voice on `head` (E2's method): `________`
- [ ] **E11.** The sends' ambient modes: `tests/scenario_ambient.sh` (DLY+ push A to LONG, REV+
      push A to HALL, on the default beat whose sends are open) against `scenario_m3.sh`, both
      compilers. Host estimate +1.9 points cl6x / +2.8 GCC-only once the classic tails have
      gone, +3.6 / +5.3 while they still ring (`docs/BUDGET.md`, "The sends' ambient modes").
      cl6x `________ %` vs `________ %`, GCC `________ %` vs `________ %`
- [ ] **E12.** The ASR page: `tests/scenario_asr.sh` (walks EDIT to ASR on track 1 and track 7;
      every ASR byte 0, so it should match `scenario_factory.sh` to within noise), both
      compilers. Then the held-chord cost: the temporary `factory_kit` edit in `docs/BUDGET.md`
      ("The ASR page", E12), `scenario_factory.sh` both compilers, and revert the edit.
      Off: cl6x `________ %` GCC `________ %`. Held: cl6x `________ %` GCC `________ %`
- [ ] **E5.** Load `STRATA_1.WAV` (15 s, 96 kHz) through the emulator's card, cl6x vs GCC.
      `________ s` / `________ s`
- [ ] Optional pre-listen: `make -C host render` writes the four factory patterns and A01's
      tracks soloed to `build/host/render/`. Anything that already sounds wrong: `________`
- [ ] STRATA6: in `core/factory.cpp`, `groove()`, both stabs `ChordType::min7` -> `min11`, then
      `tools/build.sh --name STRATA6`, and revert the edit. (The stab used to live in
      `build_default_beat`; that function is gone.)
- [ ] Both apps copied to the card.

## 0. Before power-on (2 min)

- [ ] SYSTEM INFO shows **BOOT 1.0.17**.
- [ ] A spare SD card with the official `SmplTrek.bin` is within reach.
- [ ] Power adapter in, **batteries out**. HOME held at power-on reaches BOOT MODE (try it once now).
- [ ] The card holds `/ENAGA/APPS/STRATA/APP.ENA` and `/ENAGA/APPS/STRATA6/APP.ENA`.

## 1. Start and first look (3 min)

Start STRATA from Enaga's app list. Do not press anything yet.

- [ ] The app starts and the screen is readable: no text over text, over a bar or off the edge.
- [ ] Top line reads like `A01 130.0 . 3.2%` (bank+slot, tempo, `.` stopped, load).
      Idle load: `________ %`
- [ ] Bottom line shows a **MAX** figure (like `TRK8 MAX23.5 SMPLTREK` once playing) and the
      device name SMPLTREK. No MAX means the build predates PR #17.
- [ ] No noise or clicks with the transport stopped.

## 2. Panel labels match the cheatsheet legend (3 min)

Press each key once from GRID (HOME) and check the screen agrees. Notes: `________________`

- [ ] **HOME** shows GRID. **EDIT** shows EDIT (title names the machine). **MIXER** shows MIXER.
      **PROJECT** shows PROJECT. HOME again.
- [ ] **PLAY** starts the beat (`>` on the top line), **STOP** stops it.
- [ ] **REC** toggles `REC ON` / `REC OFF` and lights red. Leave it OFF.
- [ ] **FUNC** held + **HOME** opens KEYS; **FUNC+EDIT** opens MACHINE; **FUNC+MIXER** opens
      PERFORM; **FUNC+REC** opens RESAMPLE (CLR backs out). HOME to return.
- [ ] The octave pair and the selection pair under the screen are where the legend says
      (in GRID, both flip the step page `PG1`...`PG4`).
- [ ] Encoders A, B, C, D in GRID move the four slots left to right, **in that order** (A and B
      not swapped).

## 3. A01 GROOVE (the factory default beat), and the MAX CPU readout (5 min)

A first start and every NEW project are the factory project (`core/factory.cpp`): bank A holds
four patterns. Tracks: 1 kick, 2 clap, 3 closed hat, 4 open hat, 5 rim (PERC), 6 bass,
7 chord (VA POLY), 8 blips (SAMPLE). Nobody has listened to it yet, so judge it.

- [ ] PLAY: all eight tracks play, at 130.0 with a little swing (54). The chord stab on track 7
      (steps 4 and 11) is **four notes together** (Cm7), not one note and not a flam.
- [ ] Kick: a tight 909-ish thump, tuned low (G, about 49 Hz). The third kick of the bar is shorter
      (a lock on step 9) and the next one reverts.
- [ ] Clap: a three-burst 909 clap. Hats bright, the open hat cut by the next closed hat.
- [ ] Bass on the offbeats with one slide up to Eb. **Loud enough against the kick?** Does the
      sidechain duck pump it rather than chop it? `________`
- [ ] The stab sits in a dotted-1/8 ping-pong delay and the reverb (dub chord).
- [ ] **U1.** STOP, then PLAY: MAX restarts from the new run. Let it play **60 s**, then read:
      top-line load range `________ %` to `________ %`, **MAX `________ %`**.
      Divided by E1's `scenario_m3.sh` cl6x figure = unit/emulator ratio `________`
      (about 1.35 would mean the cache and DDR costs the emulator cannot see are real).
- [ ] Turn VALUE: tempo follows smoothly. Push VALUE + turn: swing. Back to 130.0.

## 3b. The rest of the factory project: A02-A04, chain, scenes, mute states (10 min)

Queue each pattern with PROJECT + STEP 2, 3, 4 (it lands at the end of the bar), HOME to watch.
Fix anything by naming the track and what is wrong: each value is one number in core/factory.cpp.

- [ ] **A02 GROOVE+**: a kick pickup on step 15 every fourth bar, a hat roll into every bar, the
      rim on a 12-step loop drifting against the bar, Cm7 then Abmaj7, blips on a 7-step loop.
      Hold FILL (upper pad 1): an extra clap on step 16. `________`
- [ ] **A03 BREAK**: no kick or bass; Cm7 and Abmaj7 with long tails into the delay and reverb,
      a rim at 35% probability. Up to six chord voices ring here: STOP, PLAY, 60 s,
      **MAX `________ %`** `________`
- [ ] **A04 PEAK**: 16th hats with ratchets, an acid line with slides and accents, the 5TH OCT
      rave stab. Hold FILL for kick and clap rolls. The heaviest factory pattern: STOP, PLAY, 60 s,
      **MAX `________ %`** `________`
- [ ] **The factory chain** ships OFF. PROJECT, screen key RIGHT (CHAIN): A01 X2, A02 X2, A03, A04 X2,
      A02. the FUNC menu's CHAIN row (FUNC+HOME, scroll, OK) turns it on: it walks the set and loops; the `PLAY` marker follows. Leave it ON.
- [ ] **Scenes**: FUNC+MIXER (PERFORM), turn VALUE: the morph goes from the kit (scene 1) to DUB
      (scene 2): chord, rim and blips into the sends with longer tails. Hold SCENE B + STEP 3:
      DARK (filters close); STEP 4: TENSION (acid opens, hats and kick tighten). Back to B = 2.
- [ ] **Macros B-D ship** (PERFORM, encoders): B ACID (bass cutoff, resonance, envelope), C DUB
      (chord and rim into delay and reverb), D DRIVE (kick longer and driven, hats longer). Each
      audible from the first detent, no jump. A is empty; pad pressure bends it.
- [ ] **A macro at 0 lets go.** ACID at 0, EDIT track 6, turn CUT: heard. Back in PERFORM, ACID up:
      the cutoff opens from where you left it, no jump; back to 0, your value is back.
- [ ] **Mute states**: MUTE (upper pad 3), then STEP 1-8: all, no kick, drums only, no hats,
      no bass, kick + bass, melodic only, groove without rim and blips. STEP 1, then MUTE to leave.
- [ ] Overall: does the factory project sound good? First thing to change: `________________`

## 4. Screen layout, mode by mode (4 min)

The beat can keep playing. One look per screen; note any overlap or illegible text.

- [ ] GRID: four encoder slots at the bottom, name over value, the fourth fully on screen; the
      track line (`T1 ... L16 X1`) ends inside the screen.
- [ ] EDIT: rows read like `A CUT 064` with a bar; hint `FUNC+EDIT: MACHINE` is legible.
- [ ] MACHINE (FUNC+EDIT): the nine machines read as three columns of three, with `B: MIDI OUT`
      below, no overlaps.
- [ ] MIXER: screen keys walk TRK 1-4, TRK 5-8, DCK 1-4, DCK 5-8, DUCK, DLY, DLY+, REV, REV+,
      ROUTE, MST, EQ; every page draws.
- [ ] PERFORM (FUNC+MIXER): macro bars stop above the two hint lines; selected macro shows `>A`.
- [ ] PROJECT: the KEY line sits below the 16 slots, not through the last row.
- [ ] Dim greys (empty steps, bar backgrounds) are visible but clearly darker than text.
      Notes: `________________`

## 5. Arrow keys (3 min)

- [ ] GRID and EDIT: UP/DOWN walk the track **without playing it**; LEFT/RIGHT flip the step page
      (GRID) or SRC/FLTR/AMP/FX/LFO/ARP (EDIT).
- [ ] Hold STEP 1 + OK opens the step page; LEFT/RIGHT walk to the previous/next step and wrap;
      CLR closes it and erases nothing.
- [ ] PERFORM: LEFT/RIGHT pick the macro, UP/DOWN walk the mutation type.
- [ ] RESAMPLE: UP/DOWN pick the bars, LEFT/RIGHT where the take goes; CLR backs out.
- [ ] RESAMPLE to FILE, then while the screen says WRITING pick TRK 1 and press OK: `TAKE STILL
      WRITING`, and track 1 keeps its own sample once the write ends.
- [ ] Notices (e.g. an octave change) appear on the second line in every mode.

## 6. Step editing: locks, clearing one lock, the note, undo/redo (6 min)

In GRID on track 1 (KICK), playing.

- [ ] Hold STEP 5 and turn A: that step is locked (LED red) and the trig does **not** toggle when
      you let go.
- [ ] Hold STEP 5 and **push A**: `LOCK CLEARED`, the lock is gone. Repeat for B, C, D (lock with
      a turn, clear with that knob's push): each push clears **its own** knob.
      A `__` B `__` C `__` D `__`
- [ ] EDIT, RIGHT twice to the AMP page, HOME. Hold STEP 9 and push A (DEC): the third kick goes back
      to the long body. FUNC+CLR puts the lock back.
- [ ] Hold a STEP and turn VALUE: the step's note moves (`NOTE`), the tempo does not, the trig is
      unchanged on release. FUNC+CLR undoes the whole turn at once.
- [ ] FUNC+CLR = `UNDO`, FUNC+OK = `REDO`; the CLR and OK keys light when there is something to
      undo / redo. Sweep a locked knob 40 clicks, undo once: the whole sweep goes.
- [ ] CLR with nothing held clears the track (`CLEARED FUNC+CLR UNDO`); FUNC+CLR brings it back.

## 7. Key, scale and sharps (2 min)

- [ ] PROJECT: B walks the key, C the scale. Walk the key through C#, D#, F#, G#, A#: always
      sharps, and the `#` is legible. It wraps B to C; the scale stops at both ends.
- [ ] Hold a STEP on track 7 + OK: the step page's NOTE (A) spells the same way, e.g. `C#3`.
- [ ] Set the key back to **C MINOR**.

## 8. Machine select, GRAIN, and the scale fold (8 min)

The fold test: one step plays a C#, then switch PROJECT's scale between CHROMATIC (C# stays) and
MINOR (C# is not in C minor, so a folding track moves it). Repeat on each track below.

- [ ] Select track 7. FUNC+EDIT: UP/DOWN change the track's machine at once (`MACHINE`),
      LEFT/RIGHT walk the track. Discoverable? `________`
- [ ] **VA (track 7):** set a step's NOTE to C#3. CHROMATIC vs MINOR: the pitch **changes**.
- [ ] **Kick (track 1):** NOTE to C#3. CHROMATIC vs MINOR: the pitch does **not** change.
- [ ] **GRAIN:** track 8 to GRN (FUNC+EDIT, UP/DOWN). It sounds (the first time GRAIN has been
      heard on a unit). Turn POS SIZ DEN SPR: each audibly does something.
      Load with GRAIN playing, MAX `________ %`
- [ ] GRAIN with a C#3 step: CHROMATIC vs MINOR **changes** the pitch.
- [ ] **SAMPLE, sliced:** track 8 back to SMP. SLC (EDIT SRC, D) is at 8: the C#3 step does
      **not** move with the scale.
- [ ] **SAMPLE, one-shot:** turn SLC fully down (1). Now the step **does** move with the scale.
- [ ] Set SLC back to 8 (or FUNC+CLR) and the scale to MINOR.

## 8b. U3: GRAIN on a long sample, at high pitch (5 min)

Resampling gives track 8 a multi-second sample and checks RESAMPLE too. (A WAV picked in the
sample browser, 15b, would also do.)

- [ ] FUNC+REC: UP/DOWN to `RESAMPLE 4 BAR`, LEFT/RIGHT to `TO TRK 8`, OK arms. Playing, it records from
      the next bar line, writes the take (`WRITING`, then `LAST STRATA_n`) and track 8 loads it.
      Take name: `________`. Does the take sound like the mix and loop cleanly? `________`
- [ ] **Take written in chunks (new tonight).** During `WRITING` the screen keeps moving (PLAY
      cursor, LOAD) instead of freezing for seconds, and a streamed track (15c) plays on with no
      `STREAM LATE`. An 8-bar take writes in about 3 s: `________ s`
- [ ] Track 8 to GRN (FUNC+EDIT). EDIT SRC: DEN 127, SPR 127. AMP: PIT 64.
      **MAX `________ %`**
- [ ] AMP PIT 127, and a step on track 8 at a high note (step page NOTE, e.g. C6).
      **MAX `________ %`** Over PIT 64 by `________` points (the DDR cost of high-pitch grains).
- [ ] Leave track 8 on GRN for section 13.

## 8c. Sampling from the input (5 min)

New 2026-11-22. A synth, a phone or anything with a line out into INPUT L/R.

- [ ] FUNC+REC, then B: `FROM LINE NORM` and a meter. Play the source: the meter moves; turn C
      until it stays short of `CLIP`. Level used: `________`
- [ ] `TO TRK 8` (or FILE), OK, PLAY: the take starts on the bar, writes, and track 8 plays the
      source alone, not the beat. Clean start and end? `________`
- [ ] B through LINE L, MIC, DYN MIC, HI-Z: each routes (no `INPUT REFUSED`); MIC with no
      headphones says `MIC NEEDS HEADPHONES` and records silence, with headphones it records.
      Which routes worked: `________`
- [ ] B back to MASTER: a take is the beat again. MAX while recording from the input `________ %`

## 8d. MIDI out and in (8 min, needs a synth or a MIDI monitor on MIDI OUT)

First time STRATA has sent MIDI, and probably the first time Enaga's MIDI OUT has run on a unit,
so a fault here may be Enaga's (see "Expect to find Enaga bugs"). **Unplug USB first**: on Enaga
0.2.0 a computer that is not reading USB MIDI stops the DIN jack too (fixed upstream after the
release, `192f03f`). Synth or monitor on channel `____`.

- [ ] Track 6 (bass), FUNC+EDIT: the page shows nine machines in three columns and
      `B: MIDI OUT OFF`. Turn B to your synth's channel: `B: MIDI OUT CH n`. The bass line plays on
      the synth **and** on STRATA, note for note. Each note ends (no hanging notes); long LEN steps
      hold longer on the synth.
- [ ] UP/DOWN to `MID`: STRATA's bass stops, the synth keeps playing it. LOAD drops (one track
      fewer). EDIT on track 6: SRC shows `C74 C71 C73 C72`; turning A changes the synth's cutoff
      if it answers CC 74 (or watch the monitor). Switching to MID sent no CCs by itself: `____`
- [ ] Lock C74 on one step: the synth's cutoff jumps on that step and comes back after it.
- [ ] Mute track 6 (FUNC+pad): the synth stops at once, no hanging note. Unmute.
- [ ] STOP once: the synth stops. PLAY, then STOP twice: the monitor shows CC 123 on the channel.
      FUNC+STOP the same.
- [ ] KEYS on track 6: a held key holds the synth's note until you let go.
- [ ] Turn B back to OFF while a long note sounds: it ends.
- [ ] KEYS on track 6, hold a key, and with it still down open the Enaga menu (FUNC+POWER) and
      start another app (or power off): the synth's note ends. Before this fix it hung there.
- [ ] EDIT ARP on track 6 (MID), MOD UP, and give one step a MIN chord (step page 3, CHRD): the
      synth arpeggiates the chord's three notes, not the root alone.
- [ ] **LFO on MID (new tonight).** EDIT LFO on track 6 (MID): DST to C74, DEP 127, RAT about
      `1.00`. The synth's cutoff sweeps with it; the monitor shows CC 74 about 50 times a second,
      no more. DEP back to 0: the CC returns to the knob's value.
- [ ] MIDI IN, a keyboard on channel 6 (track 6, back on MNO): a held key sustains and ends on
      release. CC 17 from the keyboard moves track 6's SRC B. A program change 5 on your MIDI CH
      (SETUP shows it) queues A05.
- [ ] Timing: record the synth and STRATA's own kick together; the synth's notes are within about
      2 ms of the kick: `________ ms`

## 9. KEYS: a held key sustains (3 min)

Select track 7 (VA POLY), EDIT AMP page, DEC up high. HOME twice quickly for KEYS.

- [ ] The screen shows sixteen note names under the keys, roots brighter, all sharps.
- [ ] Hold three STEP keys: a chord. Let one go: **that** note fades, the other two keep sounding
      until released.
- [ ] Hold a key, change octave (LEFT/RIGHT), let go: the note stops, nothing hangs.
- [ ] Hold a key and press HOME: the note stops.
- [ ] On track 1 (KICK) letting go of a key changes nothing.
- [ ] On a GRAIN track (FUNC+EDIT, track 8 to GRN) with DEC at 127: the cloud thins out when the
      key comes up. Leave track 8 on GRN.

## 10. MUTATE keep and revert; PERFORM undo (4 min)

Back to GRID, track 2 selected, playing.

- [ ] Press MUTATE (upper pad 7): the track changes. CLR: it sounds as before, `REVERTED`.
- [ ] MUTATE again, then OK: it stays, `KEPT`. Same in EDIT and in PERFORM.
- [ ] MUTATE, then FUNC+CLR (undo), then plain CLR: CLR does its normal job in that mode, with no
      `REVERTED` and no second undo.
- [ ] PERFORM: FUNC+CLR undoes and FUNC+OK redoes; plain CLR still clears the selected macro.
- [ ] Hold SCENE A (upper pad 4) + CLR: `SCENE ERASED`.
- [ ] MUTATE, then hold a STEP that has a trig + CLR: `STEP CLEARED`, only that step goes and the
      mutation stays. MUTATE, then hold a STEP + OK: the step page opens.

**10b. Every press shows something (3 min).** Roland's first pass: upper pad 7 "did nothing". It had
mutated the selected track's velocities (the default type), which on a drum part is subtle and
invisible on the grid, and the screen said only `VELOCITY` for a second and a half.

- [ ] The upper row is lit dimly, each pad in its own colour, before anything is pressed; a held FILL, REPEAT, SCENE or
      THROW lights in its own colour while held; MUTE MODE and NOTE RPT stay lit while on.
- [ ] Track 2 selected, MUTATE (upper pad 7): `MUTATE T2 VELOCITY`, then `OK KEEPS CLR REVERTS`
      stays on screen and the pad glows bright violet until OK or CLR. Is the bright pad visible? `____`
- [ ] PERFORM, DOWN to `ROT RIGHT` (a change you can see on the grid), back to GRID, MUTATE: the
      track's trigs move one step right. CLR: back.
- [ ] Track 8 cleared (CLR with nothing held), MUTATE: `T8 NOTHING TO MUTATE`, the pad stays dim.
- [ ] EDIT, FUNC+octave RIGHT: `T2 LEN ..` on the second line; FUNC menu, TRACK SPEED, RIGHT: the speed.
- [ ] KEYS, turn A: `T2` and the knob's name and value. PERFORM, turn B: `MACRO B` and its value.
- [ ] FUNC+lower pad 3: `T3 MUTE AT BAR`. CHAIN, pad-row RIGHT (octave): `BANK B`. STOP twice while
      stopped: `STOP`, then `TAILS CUT`.
- [ ] Anything else that seemed to do nothing, by mode and key: `________________________`

## 11. Transport: FUNC+PLAY and FUNC+STOP (2 min)

- [ ] Playing, FUNC+PLAY: everything jumps to step 1 on the beat and keeps playing; `RESTART`.
- [ ] In PROJECT queue another slot, then FUNC+STOP before it lands: stops, tails cut at once,
      the queued change is gone (`RESET`), the next PLAY starts at step 1 of the old pattern.
- [ ] With a chain running and on its second entry, FUNC+STOP: the chain goes back to its first
      entry and that pattern is live; PLAY starts the set from the top. With the chain off the
      pattern stays where it was.

- [ ] STOP right after the third kick (the locked step 9), then turn D DRIVE in PERFORM and play
      a kick pad: the kick follows the macro. A stop used to leave that step's lock holding it.

## 12. PROJECT pages: slots, CHAIN, PATTERN, SETUP (6 min)

PROJECT, then the screen key RIGHT walks CHAIN, PATTERN, SETUP; the screen key LEFT walks back; CLR on any of
them returns to the slots.

- [ ] Slots: the arrows move a frame over the 16 slots, OK queues the framed slot; a STEP key
      queues and moves the frame there.
- [ ] CHAIN (the factory chain, ON from 3b): UP/DOWN move the cursor, STEP 3 sets the entry to
      A03, VALUE its repeats, OK adds a copy below (`ENTRY ADDED`), CLR twice removes one
      (`ENTRY REMOVED`).
- [ ] While it plays, add an entry ABOVE the playing one: the chain carries on with the entry it
      was playing and the `PLAY` marker moves with it. Remove the playing entry: that pattern
      finishes its pass and the NEXT entry plays, none skipped.
- [ ] Queue a pattern (END), then STOP and PLAY before it lands: it lands as the transport
      starts, not bars late.
- [ ] PATTERN: LEN to 8 (`LEN`): a queued change lands every 8 steps. SWG moves swing exactly
      as VALUE push + turn does.
- [ ] Still in PROJECT: FUNC+CLR undoes that LEN change, FUNC+OK redoes it.
- [ ] SETUP: shows CLOCK SRC, tempo, MIDI CH, QUANTIZE. A sets QUANTIZE in fives; B C D do nothing.

## 13. Save, power cycle, restore (5 min)

Set up something to recognise: the chain left ON, tracks 3 and 5 muted (FUNC + lower pad),
scene 7 in slot B (hold SCENE B + STEP 7), tempo **142.0**, key **D# MINOR**, track 8 on GRN.

- [ ] FUNC+HOME: the FUNC menu opens on SAVE, nothing written yet. OK: `SAVING`, the menu
      closes, and `UNSAVED` disappears from PROJECT.
- [ ] Turn a knob you will recognise (track 1 AMP LVL right down), then without saving power
      off and on: the AUTOSAVE offered at start has that knob where you left it.
- [ ] Power off, power on, start STRATA. It reopens with: the chain on (the saved pattern plays
      its pass, then the chain carries on from its first entry), tracks 3 and 5 muted,
      scene 7 in B, 142.0, D# MINOR, track 8 on GRN. Missing: `________________`

## 13b. U5: loading a long 96 kHz WAV (5 min)

Track 8's kit names the take from 8b (`/RECORD/STRATA_n.WAV`, `STRATA_1` on a card with no
earlier takes). Swap that file for the 96 kHz sweep.

**Both files load now.** The loader's limit is 30 s of the FILE's own length, up to 96 kHz
(`kSrcFrames` in `app/storage.hpp` is 30 s of 96 kHz frames, 2,880,000; changed 2026-11-22 with
Roland's OK). It used to be 30 s of 48 kHz frames, which refused the 30 s file `TOO LONG`. The
15 s file is the timing run; the 30 s one checks that a full-length 96 kHz file loads.

- [ ] Power off. Copy `unit-test/STRATA_1.WAV` from the project files over `/RECORD/STRATA_1.WAV`
      on the card (rename it if 8b showed another number). Card back, power on, start STRATA.
- [ ] It loads once as the project opens. Then track 8 selected, FUNC+HOME, RELOAD SAMPLE, OK
      (reload the track's sample) to time it: from `LOADING` until
      the SMP line shows `O` for track 8: `________ s`
- [ ] While it loads (`RESAMPLING`), the screen keeps drawing and the keys answer. Stalls? `________`
- [ ] Play it on SMP (FUNC+EDIT, track 8 to SMP, SLC down to 1): the sweep rises for about 8 s
      to 24 kHz and fades out of hearing well before that. In the second half it must stay
      silent: no falling tone coming back down. Back to GRN.
- [ ] Copy `SWEEP_30S_96K.WAV` over the same name instead and reload. It LOADS (no `TOO LONG`),
      in about twice the 15 s file's time: `________ s`. The sweep now rises for about 16 s to
      24 kHz and the second half stays silent, as above.

## 13c. U4: the delay tail (5 min, a good break)

Section 14 reloads the saved project, which puts the delay back afterwards.

- [ ] STOP. MIXER, DLY page: A (TIM) to `1/1`, B (FBK) to the top. EDIT FX on track 2 (clap): C
      (SNA) up. Idle LOAD `________ %`
- [ ] Tap lower pad 2 once, then nothing. LOAD while the tail rings: `________ %`. It drops back
      after `________` (at 130 BPM and feedback 0.95 this can take several minutes; the tail
      must keep decaying and never grow).

## 13d. The master kill EQ (5 min)

New after the walkthrough was written: a DJ isolator on the master, LOW / MID / HI split at
250 Hz and 2.5 kHz. Play A01 GROOVE.

- [ ] **FUNC + push A** (hold FUNC, push encoder A): `KILL LOW`, and the kick and bass are gone
      within a few milliseconds, **no click or thump**. Push FUNC+A again on the one: `LOW BACK`,
      they slam back, no click.
- [ ] FUNC + push B kills the mids (clap body, chord), FUNC + push C the highs (hats). With all
      three killed it is almost silent; bring them back one at a time.
- [ ] The same from PERFORM (FUNC+MIXER): FUNC + push works, and turning A-D is still the macros.
- [ ] MIXER, screen keys to the last page: `MIXER EQ`, LOW MID HI on A B C. Centre reads `0`,
      bottom `KILL`, top `+63`. Sweep B down slowly: the mids fade out smoothly, no zipper noise.
      Turn A to the top: the lows +6 dB, the limiter holds it (PEAK at most 1.0).
- [ ] On that page a plain push of A, B or C throws the switch; a killed band reads `KILL`
      whatever its knob says, and comes back to where the knob is.
- [ ] **U6.** Kill LOW and let it play **60 s**: **MAX `________ %`**, against U1's
      `________ %`. Difference = the EQ's cost on the unit: `________` points.
- [ ] Bring everything back to flat (`0`, no `KILL`): after a moment MAX should fall back to
      U1's figure (the EQ switches itself off).
- [ ] **Kills are not saved (changed tonight).** Kill HI, save (FUNC+HOME, OK), power
      cycle: it reopens with every band back (no `KILL` on the EQ page). The EQ knobs are kept.
- [ ] **A macro on the EQ.** PERFORM, select macro A (LEFT/RIGHT), FUNC + STEP 5: `A EQ LOW`.
      Turn A up slowly over a few bars: the lows fade down to nothing; back to 0 and they are
      exactly back. CLR clears macro A.
- [ ] **A scene with the EQ.** In PERFORM put empty scene 9 on side A (hold SCENE A + STEP 9).
      MIXER EQ: LOW to `KILL`. PERFORM: OK captures the sound, EQ knobs included, into scene 9.
      EQ page: LOW back to `0`. PERFORM: scene 9 to side B (SCENE B + STEP 9), empty scene 10 to
      side A (SCENE A + STEP 10). Sweep VALUE towards B: the lows fade out; back towards A, they
      return. The EQ page's LOW knob stays at `0` throughout (a morph is not an edit).

## 13e. The per-track LFO and per-track swing (8 min)

New after the walkthrough was written. Play A01 GROOVE, select track 7 (the chord stab).

- [ ] EDIT, then screen key RIGHT four times: a fifth page box lights, `LFO`, with WAV RAT DEP
      DST on A-D. The line below reads `LFO OFF: SET DEP+DST`. The sound has not changed.
- [ ] D (DST): one click is one destination, `OFF`, then the sound's names (`CUT`, `DRV`, ...).
      Pick CUT (or DRV if the track's filter is open). C (DEP) up to about 100: the stab sweeps,
      and the line reads `NOW CUT nnn`, moving. **No clicks or zipper noise** on a fast rate.
- [ ] A (WAV): one click is one shape, SIN TRI RMP SAW SQR S+H. S+H steps randomly, SQR jumps.
- [ ] B (RAT): below the middle reads Hz (`0.06` up to `32`), above it note lengths (`8B`
      down to `1/32`, `.` dotted, `T` triplet). Set `1B`: the sweep repeats once a bar, **in time
      with the beat**, and FUNC+PLAY restarts it on the one.
- [ ] Hold a STEP + turn C on the LFO page: a DEP lock on that step only (the sweep deepens on
      that step). Clearing it works as for any lock.
- [ ] PERFORM: hold FUNC + STEP 3 assigns DEP to the selected macro (the notice names `DEP`).
      Turn the macro: the sweep fades in from wherever DEP was.
- [ ] Store a scene with the LFO on, another with it off; the morph fades the depth between them.
- [ ] Undo (FUNC+CLR) takes back an LFO edit like any other.
- [ ] **U7.** LFOs on four tracks (DEP 127, DST DRV or CUT, a fast free RAT), 60 s: **MAX
      `________ %`** against U1's `________ %`. Difference: `________` points.
- [ ] Save (FUNC+HOME, OK), power cycle: the LFO settings come back, still in time.
- [ ] **Track swing.** PROJECT, screen key RIGHT twice (PATTERN): below LEN and SWG, eight track
      swings, `PAT` by default. LEFT/RIGHT walk the fields, C changes the one under the cursor.
      Set the hats' track to 66 with SWG at 50: only the hats swing. Back to `PAT`: they follow
      SWG again. It saves with the pattern, and undo takes it back.

## 13f. Note repeat on the pads (4 min)

New after the walkthrough was written. The emulator cannot press pads, so this is the first time
it runs anywhere but the host tests.

- [ ] **FUNC + FILL** (hold FUNC, tap FILL): `NOTE RPT`, no fill, and PERFORM's last line reads
      `BAR n NRPT`.
- [ ] Transport running, hold lower pad 1: the kick rolls in sixteenths, locked to the beat. Let
      go: it stops at once. Hold REPEAT and turn VALUE to 1/32, let REPEAT go, roll again: 1/32.
- [ ] Hold pad 7 (CHORD) and lean on it: the roll gets louder with pressure, softer as you ease
      off. Macro 1 does not move.
- [ ] Transport stopped: a held pad still rolls, at the tempo's sixteenths.
- [ ] FUNC + FILL again: `PADS PLAY`, and a pad plays one note. REC on: a roll records nothing.

## 13g. Tape stop and delay freeze (4 min)

New after the walkthrough was written. Both are held key combinations, which the emulator
cannot press, so this is their first run outside the host tests. Play A01 GROOVE.

- [ ] **Hold FUNC, then hold push D**: `TAPE STOP`. The beat slows and drops in pitch to silence
      over about two beats, no click at the start. Let go of FUNC first, keep holding D: still
      stopped. Let go of D: the beat is back at once, in time, **no click**.
- [ ] Let go of D half way through a stop: it comes straight back from the slowed sound.
- [ ] **U8.** Hold a tape stop for 10 s: **MAX `________ %`** against U1's `________ %`.
- [ ] **Changed tonight.** Lock D on a step (hold STEP, turn D). Hold that STEP, then FUNC, push
      D: `LOCK CLEARED`, and **no** tape stop. Without a STEP held, FUNC + push D is still the stop.
- [ ] Turn the chord's SnA (delay send) up and play a few chord stabs. **Hold FUNC, then hold
      MUTATE**: `FREEZE`, `FRZ` on PERFORM's bar line. The echo keeps looping at the same level;
      new stabs play dry and are not echoed. Let go: the loop fades into the delay's tail, then
      new stabs echo again. FUNC + MUTATE mutated nothing (no `KEPT`/`REVERTED` prompt).

## 13h. The arpeggiator (5 min)

New after the walkthrough was written. The emulator can only walk to the page
(`tests/scenario_lfo.sh` checks `scr EDIT ARP`), so the sound is first heard here. Play A01 GROOVE.

- [ ] Select track 6 (MNO), EDIT, screen key RIGHT five times: a sixth page box lights, `ARP`, with
      MOD RAT OCT GAT on A-D, and `ARP OFF: TURN MOD` below. The page boxes still fit.
- [ ] Turn A one click: `UP`, and the line reads `UP 1/4 X1 50%`. A run lasts as long as the
      step's LEN, so a one-step bass note still plays once at 1/4. Turn B to `1/32` (`UP 1/32 X1`):
      every bass note doubles. C to `X2`: the second note is an octave up. Turn D: the notes get
      longer. Turn A back to `OFF`: the bass is exactly as before.
- [ ] Track 7 (PLY), MOD UP, RAT `1/16`, then set a chord step's LEN to `4 STP` on the step page:
      the chord plays as four single notes going up it.
- [ ] MOD `RND`: the run is random but the **same every bar**.
- [ ] KEYS (HOME twice) on track 6 with MOD UP: hold two keys, the run alternates them in time
      with the beat; let one go, it plays the other; let both go, it stops. Stop the transport and
      hold a key: the run starts at once.
- [ ] Save, power cycle: the ARP settings come back.

## 13i. Song mode: mute recalls and SONG (5 min)

New after the walkthrough was written. Chain on/off and SONG/LOOP are FUNC menu rows
(FUNC+HOME, scroll, OK; CLR closes the menu). Play A01 GROOVE.

- [ ] PROJECT, screen key RIGHT: CHAIN. Build three entries (OK on an empty chain, then OK twice):
      A01, A02, A01, each X1. Turn B on entry 2: `M2` at the end of its row (no kick, factory).
- [ ] FUNC menu, CHAIN, OK: chain on. Entry 2 starts **without the kick**, on the bar line, no click. Entry 3
      (no recall) keeps it muted: `--` leaves the mutes as they are.
- [ ] FUNC menu, CHAIN END, OK: `SONG: PLAY ONCE`, the header ends `SONG`. Unmute the kick, FUNC+STOP (reset), PLAY:
      the chain plays A01, A02 without kick, A01, then **stops** with `SONG END`, nothing after
      the last bar, no hanging note. PLAY: it starts again from entry 1.
- [ ] CHAIN END, OK again: `LOOP CHAIN`, and the chain goes round as before.
- [ ] **Changed tonight.** On entry 3, CLR once: `AGAIN TO REMOVE`, nothing removed. Wait
      3 s and press again: still `AGAIN TO REMOVE`. Press twice quickly: `ENTRY REMOVED`.
- [ ] Save, power cycle: SONG and the `M2` come back. The page boxes and rows fit the screen.

## 13j. The ambient delay and reverb: LONG and HALL (8 min)

New: send A has a LONG mode (up to 12 s, wow, diffusion, low cut) and send B a HALL mode (a
bigger, swept tail with shimmer). Both are on MIXER pages beside their send: DLY+ after DLY,
REV+ after REV; the LONG delay's low cut is C on ROUTE. Play A01 GROOVE; the chord and the
clap already feed both sends.

- [ ] MIXER, DLY+: push A (or turn A): `DLY LONG`, the slot reads `LONG`. The echoes carry on
      with no click, the classic ones ringing out underneath. B (MUL) to `X4`: the bottom line
      reads `TIME 1385 MS` at 130 BPM (dotted 1/8 times four), and the echoes land on the beat.
- [ ] B to `X8`, then DLY's TIM to `1/1`: TIME reads `12000 MS` (clamped). One clap, then
      listen: the first echo comes 12 s later, **on the beat**, as clean as the dry clap.
- [ ] Back to `1/8D X4`. Turn the time while it rings: the echoes bend in pitch like tape and
      settle, no clicks. C (WOW) up: a slow tape wobble, wider with the knob. D (DIF) up: each
      repeat softer-edged, washing out after three or four. ROUTE, C (LCT) up: the repeats thin
      out at the bottom.
- [ ] FBK to the top with DIF and WOW up, feed it for 20 s: it builds to a dense wash and
      **stays at a steady level**, never running away or distorting harshly.
- [ ] **Hold FUNC, then hold MUTATE** in LONG: `FRZ` on PERFORM's bar line, the wash holds
      exactly as it is, new hits play dry. Hold it for 30 s: no fading, no growing. Let go: it
      decays at the FBK setting again.
- [ ] MIXER, REV+: push A: `REV HALL`. A big, soft, slow-moving tail. DEC (REV, A) to the top:
      a long wash that never rings metallic. B (SIZ) down then up: smaller then bigger, a short
      pitch bend while it moves. C (MOD) at 0 against the top: still against slowly swirling.
- [ ] D (SHM) up on a held chord: an octave-up shimmer rises out of the tail. With DEC at the
      top and SHM at the top, play 30 s of chords: it shimmers and swells but **stays below
      clipping** (PEAK under 1.0); stop playing and it dies away.
- [ ] Push A on both pages again: `DLY CLASSIC`, `REV ROOM`, and the beat sounds as it did
      before. Save (FUNC+HOME, OK), power cycle, reload: the modes and knobs come back as saved.
- [ ] **U9.** A01 GROOVE with LONG (`X4`, DIF and WOW up) and HALL (SHM up) for 60 s:
      **MAX `________ %`** against U1's `________ %`.

## 13k. Held notes: the ASR page, LEN and recorded lengths (8 min)

New: VA MONO and VA POLY have a seventh EDIT page, ASR (ATK SUS REL FSU). SUS 0 is the old
one-shot; SUS up holds a note while a pad is held or for the step's LEN. REC records how long a
note was held into its step's LEN. The emulator only walks to the page (`tests/scenario_asr.sh`),
so this is the first time it is heard. Play A01 GROOVE.

- [ ] Track 7 (PLY), EDIT, screen key RIGHT six times: a seventh page box, `ASR`, with ATK SUS REL
      FSU on A-D reading `3MS 0% 60MS 0%`, and `SUS 0: ONE-SHOT` below. The seven boxes fit. On
      track 1 the same page reads `---` and `VA MONO/POLY ONLY`.
- [ ] **Old sound kept.** With all four at 0 the chord stab sounds exactly as before (compare with
      A01 on the v0.1.0 card if in doubt).
- [ ] **A pad.** SUS to about 70%, ATK to about `400MS`, REL to about `2.0S`, FSU up a little. Hold
      a STEP (a chord step), step page, LEN to `16 STP`: the chord swells in, holds for the bar
      and fades into the next. Line reads `HOLDS: PAD OR LEN`. No clicks at the start or the end.
- [ ] **A05 PAD** (a NEW project, or a card without one; an older project keeps its own bank A).
      PROJECT + STEP 5: at the end of the bar, a soft beat under four held chords, Cm9, Abmaj7,
      Ebmaj7, Gm7, a bar each. Each chord swells in over about half a second, holds, and rings
      into the next; the notes two chords share carry on rather than restarting. No clicks at a
      chord change, no note jumping to another pitch, no `GUARD`. Is the pad level right against
      the kick? Then MIXER, `REV+`, A to HALL: better or worse? STOP, PLAY, 60 s,
      **MAX `________ %`** (A05) `________`
- [ ] **KEYS on the pads** (HOME twice) on track 7: hold a chord on the pads for five seconds: it
      holds at the SUS level the whole time and fades over REL when let go. Let go of one note of
      three: only that one fades.
- [ ] **The track pad.** Still on the ASR page (EDIT, not KEYS), hold track 7's own pad for five
      seconds: it holds, and fades over REL when let go. SUS back to 0: a quick tap rings on over
      DEC after the finger lifts, as before.
- [ ] **Twelve voices.** Hold six pads with the pattern's chords playing: the oldest/quietest notes
      fade out to make room, nothing clicks or hangs, no `GUARD` unless the load is genuinely high.
      MAX meanwhile: `________ %` (**U10**)
- [ ] **MONO.** Track 6 (MNO) ASR page: SUS 0, the bass line is unchanged. SUS up: each bass note
      now holds for its step's LEN (one step) and REL is its tail; a slid step bends the held note
      into the next pitch without restarting it. Set a step's LEN to `8 STP`: a long drone note.
      Back to SUS 0: the 303 bass is back exactly.
- [ ] **Recorded length.** REC on, KEYS on track 7, hold a pad for about a bar, let go. REC off.
      Open that step's step page: LEN is near `16 STP`. Tap a pad quickly on another step: its
      LEN is short (`1/4 STP` or so). On track 1 (KICK), a held key leaves LEN at `1 STP`.
- [ ] **MIDI in**, if a keyboard is at hand: a held key on channel 7 holds the pad the same way,
      and is recorded with its length.
- [ ] Save, power cycle: the ASR settings come back. Open a project saved before tonight: every
      VA sounds as it did (ASR reads all 0).

## 13l. The sidechain duck, editable (5 min)

New: the duck is set from three MIXER pages after the faders: DCK 1-4 and DCK 5-8 (each track's
depth), DUCK (source, attack, hold, release). It is saved in the kit. Play A01 GROOVE.

- [ ] MIXER, screen key right twice: `MIXER DCK 1-4`, T1 reads `SRC`, T2-T4 `35%`, the bottom
      line `DUCKS UNDER T1`. Right again: DCK 5-8, all `35%`. Right again: `MIXER DUCK`,
      `SRC T1`, `ATK 3`, `HLD 40`, `REL 120`, `TIMES IN MS`. Nothing overlaps.
- [ ] DCK 5-8: turn the chord's and the bass's depth to `100%`: they **pump hard** on every kick,
      no click at the dip or the return. Turn them to `OFF`: they stop pumping, the kick still
      plays. Back to `35%`.
- [ ] DUCK: REL (D) to about `500`: a slow breathing pump. HLD (C) to `200`: the dip lasts
      longer before it comes back. ATK (B) to `50`: the dip comes in softer. Back to 3/40/120.
- [ ] DUCK: SRC (A) to `T2` (the snare): the pump now follows the snare, the kick ducks under it
      at its own depth, and DCK 1-4 shows `SRC` on T2. Back to `T1`.
- [ ] Set T6 to `80%` and REL to `300`, save (FUNC+HOME, OK), power cycle, reload: DCK 5-8 and
      DUCK show the same values, and it sounds the same. Store the kit in the kit library and
      recall it into a NEW PROJECT: the duck comes with it.
- [ ] Open a project saved with an older STRATA: the duck reads T1, 35%, 3/40/120.
      Notes: `________________`

## 14. Reload and new: the FUNC menu's RELOAD PROJECT and NEW PROJECT (3 min)

- [ ] Change the tempo (unsaved). FUNC+HOME, RELOAD PROJECT, OK once: `OK AGAIN TO RELOAD`,
      nothing changes. Again within 2 s: `RELOADING`, and the tempo is back to 142.0.
- [ ] NEW PROJECT, OK once: `OK AGAIN FOR NEW`, nothing changes. Twice: `NEW PROJECT`, and it is the
      factory project again (A01 GROOVE, all eight tracks audible; NEW used to be silent).
      Track 8's blips play too: a SAMPLE track with no file now plays the placeholder (PR #42). A single OK on either row never loses work.
- [ ] Both also work from the SETUP page.

## 14b. Projects: open and save as (4 min)

New 2026-11-22. PROJECT, screen key RIGHT four times: `PROJECTS`.

- [ ] The list shows `DEFAULT` (and any other project folders), with the cursor on the open one
      and `NOW DEFAULT` at the top. OK on it says `ALREADY OPEN`.
- [ ] FUNC+DOWN (or the FUNC menu's SAVE AS): `SAVE AS` with `DEFAULT` in the entry. Make it `LIVE 1` with A and VALUE, OK:
      `SAVING`, then `LIVE 1` appears in the list and `NOW LIVE 1`. The text is readable and the
      underline sits under the right character? `________`
- [ ] Change something, then OK twice on `DEFAULT`: `AGAIN TO OPEN`, then `OPENING`; the change
      is gone and DEFAULT plays as it was saved.
- [ ] Power cycle: STRATA comes back with `DEFAULT`, the one opened last.
- [ ] SAVE AS `LIVE 1` from DEFAULT says `NAME TAKEN` and writes nothing.

## 14c. Projects: rename and delete (4 min)

New 2026-11-23. On PROJECTS, after 14b (`DEFAULT` and `LIVE 1` on the card, DEFAULT open).
The second-to-last line says `PUSH B REN  C DELETE`.

- [ ] Cursor on `LIVE 1`, push B: `RENAME`, `FROM LIVE 1`, the name in the entry. Make it
      `LIVE 2`, OK: `RENAMING`, then the list shows `LIVE 2` and no `LIVE 1`. Open `LIVE 2`
      (OK twice): it is what LIVE 1 was? `________`
- [ ] Rename the open project the same way: `NOW` shows the new name. Power cycle: STRATA comes
      back with it.
- [ ] Rename onto a name in the list: `NAME TAKEN`, nothing changes.
- [ ] Cursor on another project, push C: `DELETE? OK TWICE`. CLR: `DELETE OFF`, still there.
      Push C, OK, wait 3 s, OK: only `OK AGAIN TO DELETE` again. Push C, OK, OK: `DELETING`,
      and it leaves the list. On a computer its folder is gone from
      `/ENAGA/DATA/STRATA/PROJECTS/`.
- [ ] Push C on the open project: `PROJECT IS OPEN`, nothing else.
- [ ] While a pattern plays, rename a project: no click, no LOAD jump? `________`
- [ ] Pull the card during a rename (push B, OK, pull at once): card back in, both names or the
      old one open, never a project with patterns missing. What was there: `________`

## 15. Kit browser: FUNC+EDIT twice (6 min)

Kits (machines, parameters, faders, sample paths) live on the card in 16 slots shared by every
project. Back in STRATA's project from 14 (or any project), playing.

- [ ] FUNC+EDIT opens MACHINE; its last line says `FUNC+EDIT AGAIN: KITS`. FUNC+EDIT again opens
      KIT: `KITS 01-08    OK LOAD`, eight rows, `EMPTY` on a fresh card after a moment. LEFT/RIGHT
      (or the screen keys) show slots 09-16. CLR goes back to MACHINE. No overlaps.
- [ ] Change a few knobs and put track 8 on GRN. On slot 1, FUNC+DOWN: `SAVING KIT`, the row reads
      `KIT` and the two lines below show the eight machines (GRN last).
- [ ] FUNC+DOWN on slot 1 again: `AGAIN TO OVERWRITE`; only a second press writes.
- [ ] Change the sound (track 8 back to SMP, a knob or two). Still playing, OK on slot 1:
      `OK AGAIN TO LOAD`, nothing changes. OK again: the slot-1 sound is back and **the beat
      does not stop**. Click or dropout at the recall? `________`
      STOP, PLAY, recall again: MAX `________ %`
- [ ] STEP keys only move the cursor; they never load.
- [ ] PROJECT now says `UNSAVED`. FUNC+HOME, OK, then the menu's RELOAD PROJECT, OK twice: the
      recalled kit is what comes back.
- [ ] Kits are shared: the menu's NEW PROJECT, OK twice, then FUNC+EDIT twice: slot 1 is still there.

## 15b. Sample browser: OK on a SMP or GRN track (5 min)

New 2026-11-22. Put a few WAVs in `/SAMPLES` on the card first (the two test files from
`unit-test/` will do, plus one in a subfolder, and a file whose name has `_` in it).

- [ ] EDIT on track 8 (SMP): the page says `OK: PICK SAMPLE`. OK opens `SAMPLES T8 SMP` with
      the folder's WAVs and folders (`/` after a folder), no `._` files, no overlaps.
- [ ] PLAYING, DOWN to a file and OK: `LOADING`, the line under the title shows the load, then
      track 8 plays the new file and **the beat never stops**. Click at the swap? `________`
- [ ] DOWN, OK again: the next file replaces it. The `NOW` line names the file.
- [ ] OK on a folder opens it; CLR goes up with the cursor on that folder; CLR at `/SAMPLES`
      shows `SAMPLES/` and `RECORD/`; CLR there goes back to EDIT. Same from MACHINE (OK).
- [ ] Reopen the browser: it starts in the track's folder with the cursor on its file.
- [ ] PROJECT says `UNSAVED`; save, reload (the menu's RELOAD PROJECT, OK twice): track 8 has the picked file.
- [ ] A folder of many files: listing time `________` (about how long `READING FOLDER` shows).

## 15c. A long stem (4 min)

New 2026-11-22: files over the old 30 s limit load whole, in chunks. Put a 48 kHz stereo WAV of
2-2.5 minutes (25-30 MB) in `/SAMPLES`.

- [ ] Pick it for track 8 in the sample browser while playing: `LOADING LONG` with a rising
      percentage, the beat never stops. Seconds to playable `________` (expect about 4 s).
      Keys and screen responsive while it loads? `________`
- [ ] It plays from the start, all the way through (STA 0, LEN 127, PLY LOOP); on GRN, POS
      sweeps the whole file. Clicks at 4 MiB boundaries (every ~22 s of stereo 24-bit, ~44 s of
      16-bit)? `________`
- [ ] PROJECT's SMP line shows the memory used, about `27/32M`. A second one on another track:
      `NO ROOM`, and the first keeps playing.

## 15c2. SLC AUTO on a played break (4 min)

New 2026-11-22. Put a drum break that was played, not programmed (any loop that is not on a
grid), in `/SAMPLES` and pick it for track 8 (SAMPLE).

- [ ] EDIT, SRC page: the line under the knobs says `PUSH SLC: AUTO SLICE`. Push D:
      `SLICES AUTO`, then `SLC AUTO: n HITS` with n about the number of hits you can hear
      (up to 15). n = `________`
- [ ] SLC 8, trigs on notes 60-67 on steps 1, 3, 5 ... 15: each slice starts on a hit, with no
      flam or cut-off attack at its start? `________` Compare with push D again (even).
- [ ] Load time of the break against before (the hits are found while it loads): any slower to
      play? `________`
- [ ] Save, power cycle: track 8 is still AUTO, same slices.

## 15d. A stem bigger than memory, streamed (5 min)

New 2026-11-22: a file the sample memory cannot hold streams from the card. Put a 48 kHz stereo
16-bit WAV of 4-5 minutes (45-55 MB) in `/SAMPLES`.

- [ ] Pick it for track 8 while playing: `LOADING LONG` only briefly (the first chunk), then the
      SMP line shows `S` for track 8 and about `0/32M` more used (its 2 s head is 0.4 MiB).
- [ ] With STA 0, PLY LOOP and one trig on step 1, it plays the whole file and loops: no gap
      at 2 s (where the head ends and the card takes over)? `________` At the loop? `________`
- [ ] Trig it every bar for 2 minutes: every restart starts clean, no `STREAM LATE`? `________`
      (Each restart reopens the stream, which walks the file's cluster chain on the card:
      docs/FEATURES.md, "Why streamed samples play forward only". This reading decides whether
      reverse streaming is worth building.)
- [ ] LEN halfway (new 2026-11-23): it stops halfway through the file, and with PLY LOOP loops
      back from there to the start, no gap? `________` LEN very short (under 2 s): loops inside
      the head with no `STREAM LATE`.
- [ ] Two more streamed tracks (copies of the file on tracks 6 and 7) at once: `STREAM LATE`
      ever? `________` LOAD range `________ %` with three streams against without `________ %`.
- [ ] A fifth streamed track says `STREAMS FULL`.
- [ ] Pull the card while it streams: silence and `STREAM LATE`, no crash. Card back in: it
      streams again after the reload.

## 16. Six voices: the STRATA6 app (4 min)

STRATA6 is the same build with the chord stab set to MIN11 (six notes). Leave STRATA through
Enaga's menu and start STRATA6.

- [ ] The stab is six notes together, nothing crackles.
- [ ] PLAY, let it run **60 s**: top-line load range `________ %` to `________ %`,
      **MAX `________ %`**. Difference to STRATA's MAX = cost of two more voices:
      `________` points.
- [ ] **U2, and the shared voice budget.** At most **12 VA POLY voices** sound at once across all
      tracks (`Engine::kPolyVoiceBudget`, new today). Switch tracks 5 (rim) and 6 (bass) to PLY
      (FUNC+EDIT) and give their trigs MIN11 chords (hold STEP + OK, third page, CHRD): 18
      voices asked for, 12 allowed. **MAX `________ %`**. Crackle or overrun? `________`
- [ ] Put a track-5 chord on the same step as the stab: only two chords' worth sounds, the third
      is dropped, no glitch. Move it one step later: it sounds, and the oldest, quietest voices
      fade without a click.
- [ ] From this MAX: is 12 the right budget (largest that keeps the worst case under 60%)?
      Proposed value `________`

## 17. Write the numbers down

| Reading | Value |
| --- | --- |
| Idle load (1) | |
| scenario_factory.sh emulator peak, cl6x / GCC (PC) | |
| E1 `scenario_m3.sh` cl6x, emulator (PC) | |
| U1 A01 GROOVE MAX, 60 s (3), and ratio to E1 | |
| A03 BREAK MAX, 60 s (3b) | |
| A04 PEAK MAX, 60 s (3b) | |
| A05 PAD MAX, 60 s (13k) | |
| GRAIN on track 8 MAX (8) | |
| U3 GRAIN on the take, DEN/SPR 127: PIT 64 / PIT 127 (8b) | |
| U5 96 kHz WAV load time, 15 s and 30 s (13b) | |
| U4 delay tail: LOAD while ringing, time to drop (13c) | |
| E6 kill EQ, `scenario_killeq.sh` vs `scenario_m7.sh`, cl6x / GCC (PC) | |
| U6 A01 GROOVE MAX with LOW killed, 60 s, and the difference to U1 (13d) | |
| E7 eight LFOs, `scenario_lfo.sh` off / on, cl6x / GCC (PC) | |
| U7 A01 GROOVE MAX with four LFOs, 60 s, and the difference to U1 (13e) | |
| U8 A01 GROOVE MAX holding a tape stop, 10 s (13g) | |
| E11 ambient modes, `scenario_ambient.sh` vs `scenario_m3.sh`, cl6x / GCC (PC) | |
| U9 A01 GROOVE MAX with LONG and HALL, 60 s, and the difference to U1 (13j) | |
| E12 ASR page, `scenario_asr.sh` vs `scenario_factory.sh`; held-chord edit, cl6x / GCC (PC) | |
| U10 MAX with six pads held on VA POLY, SUS up, over A01 (13k) | |
| MAX straight after a kit recall (15) | |
| STRATA6 MAX, 60 s (16) | |
| U2 STRATA6 + tracks 5 and 6 as MIN11 POLY, capped at 12 voices (16) | |
| Voice budget to set (16) | |

These go into `docs/BUDGET.md` as the first unit rows (U1-U5 and E1-E5 in "Optimization review"), and the ticks into the reference list below.

## 18. If it passed: the v0.1.0 release

Only after the walkthrough passes and Roland says go. `docs/release/RELEASING.md` is the
procedure and wins where this summary differs.

- [ ] **The card is what ships.** It was built from `main` as it is now, the walkthrough passed
      on it, and `SmplTrek.bin` on it is Enaga's own v0.2.0 release file. If `APP.ENA` is a cl6x
      build, TI's licence text was read for conditions on its output and recorded in
      `docs/BUDGET.md`; if in doubt, release a GCC-only build.
- [ ] **One command** (WSL, on `main`, clean tree), with the build and the two MAX readings
      from section 3:
      `tools/publish_release.sh v0.1.0 --build gcc --max-a01 __._ --max-a04 __._`
      (`--build cl6x --ti-licence-read` for a cl6x card). Add `--dry-run` first to read the
      release text without publishing, or `--draft` to publish as a GitHub draft.
- [ ] Merge the `release/v0.1.0` PR it opens, then update README's install section and badge.

---

# Reference: the full list by milestone

Only the unit settles CPU load, timing and feel. Everything here needs the hardware, so it
cannot be closed from a development machine. Record figures in `docs/BUDGET.md`.

## Before powering anything on — safety

From `docs/SPEC.md`, "Unit safety checklist". Do not skip these: a hung image leaves the power
key dead.

- [ ] SYSTEM INFO shows **BOOT 1.0.17**. All the recovery evidence is for that bootloader.
- [ ] A second SD card holds an official `SmplTrek.bin`. One is already at
      `D:\DEV\smpltrek\SmplTrek v3.2.174\SmplTrek.bin`.
- [ ] Confirm **HOME (F4) held at power-on** reaches BOOT MODE, before it is needed in anger.
- [ ] Run on the power adapter **without batteries**, so a hung image can be power-cycled.
- [ ] Confirm the build writes no SPI flash and ships no BOOT or MCU sections.

## M0 — skeleton

- [x] `/ENAGA/APPS/STRATA/APP.ENA` on the card; the app appears in Enaga's app list and starts.
      Booted 2026-11-22 on Enaga 0.2.0 and played the default beat.
- [ ] The screen shows `STRATA`, the device name, LOAD, PEAK, CLK, BPM and PPQN.
- [ ] Device name reads **SMPLTREK** (it comes from the bootloader, so it proves
      `system::device()` works rather than a hardcoded string).
- [ ] Idle load and peak recorded in `docs/BUDGET.md`. Expect roughly Enaga's own ~3%: the
      skeleton outputs silence.
- [ ] No audible noise or clicks with the app running and the transport stopped.
- [ ] **Clock readout, the M1 foundation.** With CLOCK SRC = INTERNAL, confirm BPM tracks the
      unit's tempo setting and `RUN`/`STOP` follow the transport.
- [ ] With CLOCK SRC = MIDI and a clock arriving at MIDI IN, confirm the line shows `MIDI`,
      `EXT` and the incoming tempo. **This is the first time Enaga's MIDI DIN has run on real
      hardware** — see the note below.
- [ ] With CLOCK SRC = SYNC and pulses at SYNC IN, confirm the line shows `SYNC` and `EXT`.
- [ ] Leave it running for 10 minutes: no underruns, no watchdog reset, load stable.

## M1 — scheduler and drums

The default kit plays a four-track beat at 130 BPM as soon as PLAY is pressed; nothing needs
setting up first.

- [ ] PLAY (F5) starts the beat; the screen's top line shows `>` and the tempo.
- [ ] **The load criterion.** The emulator gives **12.02% peak with cl6x** for the four-track
      beat, against M1's bar of 15% — it passes there, after the `*.ti.c` extraction took it
      down from 20.14%. Measure it on the unit anyway: pure computation runs about 25% faster
      there, and this is the peak block rather than the average, so the real figure should be
      lower still. Record it in `docs/BUDGET.md`.
- [ ] **The same beat built GCC-only** (`ENAGA_TI_CGT=none`) for comparison: the emulator says
      21.64%. The gap is the thing to confirm on hardware, because the eight-track worst case
      is 23.61% with cl6x and 44.26% without — i.e. **GCC-only misses M2's 40% bar**, and that
      is what makes the cl6x redistribution question matter.
- [ ] STOP (F6) stops with tails; STOP twice cuts them.
- [ ] VALUE changes the tempo and the beat follows it smoothly.
- [ ] STEP 1-16 toggles trigs on the selected track and the change is heard on the next pass.
- [ ] The lower pads 1-8 select a track and play it with velocity; F1 + pad mutes.
- [ ] Encoders A-D edit the selected track's four parameters. **Check A and B are not swapped** —
      the encoder event ids are alphabetical while the push-button codes are not, and getting
      that wrong is silent (`docs/ENAGA_API.md`).
- [ ] Pad velocity feels right across all 15 pads; note any crosstalk.
- [ ] **Timing.** Record line out at 48 kHz and measure the spacing of the kick's onsets over
      5 minutes. M1's bar is jitter under 0.1 ms.
- [ ] The hats choke: the closed hat on track 3 cuts the open one on track 4.
- [ ] 10 minutes of playing: no underruns, no watchdog reset, load stable.

## M2 — the full kit

All eight track slots are filled: KICK, SNARE, HAT closed, HAT open, PERC, BASS, a mono stab
standing in for CHORD, and the LOOP sampler. The default pattern plays all of them.

- [ ] **The load criterion.** The emulator gives **17.00%** for the default kit and **23.61%**
      worst case (drive, LP24, lo-fi and both sends on all eight tracks) against M2's bar of 40%.
      Measure both on the unit and record them in `docs/BUDGET.md`.
- [ ] **The GCC-only comparison matters here.** The emulator says 44.26% worst case without
      cl6x, which misses the bar. Confirm the gap on hardware — it is the evidence behind the
      redistribution question in `docs/BUDGET.md`.
- [ ] PERC on track 5: the default is a rimshot. Check the ratio knob's lower half steps between
      *characters* (hollow at integer ratios, metallic at irrational ones) rather than just
      getting brighter.
- [ ] BASS on track 6: the filter should squelch with resonance up without self-destructing.
      **Accent should be audibly more than volume**, and the slide on step 11 should glide into
      its note rather than retrigger.
- [ ] LOOP on track 8 plays a **synthesised placeholder**, not a file — eight slices built in
      `Engine::init`. Check successive notes pick successive slices, and that the boundaries land
      on the blips (a slice-count mismatch puts them mid-blip).
- [ ] EDIT (F8) and MIXER (F7): the screen keys move the parameter page and the track bank.
      Check the MIXER's faders and the master peak behave, and that EDIT's four values match
      what the encoders do.
- [ ] Listen for aliasing on PERC and BASS at the top of their ranges, and for a click when a
      voice is stolen on any machine.
- [ ] **Parameter locks, and listen properly.** The default pattern (A01 GROOVE) locks the third
      kick of the bar to a shorter body (KICK DEC on step 9); A02 pitches the rim with TUN locks
      on steps 8 and 11, A03 opens the chord's decay and sends, A04 opens the acid bass's
      resonance on steps 8 and 15. Each must be *audible*, and the step after each must revert to
      the kit value.

      This needs an ear rather than a test. `app/` cannot be host-tested — it is the only part
      of the project that includes Enaga headers, so there is no unit test standing behind this
      path. It was also broken once: locks were applied around `trigger()` but the render loop
      then prepared from the unlocked kit every block, so a lock on a cutoff or a decay did
      nothing at all while a locked *note* worked fine. If locks ever look right on screen and
      sound wrong, that asymmetry is the first thing to suspect.
- [ ] **Clearing one lock.** Hold STEP 9 on the kick and push encoder A on the AMP page: the
      screen says LOCK CLEARED, the step's LED loses its red if it had no other lock, and the
      third kick goes back to the kit's long body. Check every knob, not just A -- Enaga numbers
      the pushes B, A, C, D, and a push that clears the wrong parameter is exactly what that
      ordering invites. F1 + CLR puts the lock back.
- [ ] While there: turn a knob on a track whose current step carries a lock on that same
      parameter. The knob should be heard at once, and the lock should win again from the next
      step.

## M3 — sequencer depth

Everything here is a gesture, and gestures are the part of this project with the least test
cover: `app/` is the only code that includes Enaga headers, so none of it is reachable from
`make -C host test`. The logic behind each one is host-tested in `core/` (undo, recording's
arithmetic, the step fields, copy and paste); what needs the unit is whether the gesture itself
is findable and whether it feels like an instrument.

`tests/scenario_m3.sh` covers one path end to end in the emulator — toggle a step, undo, redo,
each verified by its effect rather than by the absence of a crash — so if that scenario passes,
the command ring and the undo stack are known to work before you touch the hardware.

**Step editing**

- [ ] Tap STEP: the trig toggles. Hold STEP and turn A-D: that parameter is **locked on that
      step only**, and the trig does *not* toggle when the key comes up. The second half is the
      one to watch: if a held step toggles its trig on release, every lock edit also flips a trig.
- [ ] F1 + STEP makes a step **trigless**: its locks apply and no note sounds. The LED is red
      alone, where a trig is green. An empty step and a trigless step must be distinguishable.
- [ ] Hold STEP + OK opens the step page. All eleven fields reachable on three pages with the
      screen keys, and each one does what its name says. **Check the condition field
      specifically**: NONE, then the eight named conditions, then the 35 A:B pairs, clamping at
      both ends rather than wrapping.
- [ ] Hold two STEP keys: the pair is a selection (the screen says `SEL n-m`). CLR erases it; F1 + UP
      copies it, F1 + DOWN pastes it (plain UP and DOWN still do while a STEP is held); A transposes it and B nudges its micro-timing.
- [ ] CLR with nothing held erases the **whole track**. Confirm F1 + CLR brings it straight back:
      that is the only thing that makes the bare gesture acceptable.

**Undo**

- [ ] F1 + CLR undoes, F1 + OK redoes, 16 levels deep. The CLR and OK keys light when there is
      somewhere to go, so the panel says whether undo will do anything.
- [ ] **One sweep is one level.** Hold a step and turn an encoder through forty clicks, then
      undo once: the whole sweep goes, not one click of it. If each click took a level, the
      fortieth would have thrown away everything else in the history.
- [ ] A live-recording pass is one level, and a motion pass is one level, however many steps
      they touched.
- [ ] Undo after changing a track's length or speed restores those too, and the sequencer keeps
      playing without a glitch at the moment it happens.

**Recording**

- [ ] F2 arms REC (the key lights red). With the transport running, pad hits land on the nearest
      step with velocity. F1 + VALUE sets the quantize amount, shown as `Q nnn`.
- [ ] **Quantize 100 vs 0.** At 100 every hit snaps dead to the step. At 0 it keeps where you
      played it, as micro-timing. Play deliberately late and listen for it.
- [ ] **Swing and recording must agree.** Set the pattern swing to 66 and play dead on the swung
      offbeat: it must record as micro 0, not as an offset. This is the one piece of recording
      arithmetic that is wrong in a way you can only hear — `core/record.cpp` tests it against
      `core/timing`, but the two agreeing on the unit is what counts.
- [ ] **Pad latency.** `kPadLatencyMs` in `core/record.hpp` is **6 ms, a placeholder** — the
      spec says "about 4-8 ms" and nobody has measured it. To measure: set quantize to 0, play
      along to the kick as precisely as you can for a bar, then read the recorded `micro` values
      on the step page. A consistent bias one way is the latency figure; put it in that constant
      and record it here. Everything else about recording is correct before this is done; hits
      just sit slightly late.
- [ ] **Quantize 0 on a slow track.** `Step::micro` is ±11 ticks, but a 1/4x step is 96 ticks and
      1/8x is 192, so on those tracks an off-grid hit mostly *cannot* be stored where it was
      played: it clamps, and the screen says `OFF GRID`. Confirm that message appears rather than
      the hit silently moving. (This is `Step::micro`'s range from `docs/SPEC.md`, not a bug in
      the quantizer. If it proves annoying in practice, the fix is a design decision about what
      grid a slow track records on.)
- [ ] **Motion recording.** With REC on and the transport running, turn an encoder: locks are
      written on the steps the playhead passes over, and **the knob is audible as you turn it**.
      If the sound only changes on the next pass of the loop, the effective-parameter mirror in
      `Engine::apply` has been lost and motion recording will read as doing nothing.
- [ ] MIDI notes at MIDI IN record exactly as pad hits do, channels 1-8 addressing tracks 1-8.
      This shares one stamping mechanism with the pads deliberately; if pads record correctly and
      MIDI does not, suspect Enaga's MIDI DIN rather than STRATA (see the note at the end).

**Polymeter**

- [ ] FUNC + pad-row keys set the selected track's length, the FUNC menu's TRACK SPEED its speed; both show
      on the grid line as `L nn` and `X n`. Set track 2 to 15 steps against track 1's 16 and
      listen for the phase walking — that is the feature working.
- [ ] A track at 2x with micro-timing at the extremes still fires every step exactly once per
      pass (the case `core/timing.cpp`'s scan window was written for).

**Two things that are deliberately not finished**

- [ ] **Gate length is audible on VA POLY only.** `Step::length` is stored, edited on the step
      page, saved and restored, and `Engine::fire` now converts it to frames and sends it as
      `Trig::gate_frames`; VA POLY counts it down per voice and releases on its exact frame
      (9 host tests, 11 of 14 mutations caught, the three survivors recorded in
      `tests/test_vapoly.cpp`). Every OTHER machine is a one-shot built on a sustain-0 envelope
      and ignores the field, so on a drum track there is still nothing to hear: check it survives
      a save and reload there rather than listening for it. On a VA POLY track, listen: a short
      length should shorten the note, and a long one should not clip the decay.
- [ ] The conversion itself (`Engine::gate_frames_for`) is **not host-tested** — nothing in
      `app/` is, because the host suite builds only `core/` and `dsp/`. It reads
      `ticks_per_step`, `samples_per_tick_` and `length_sixteenths`, all of which ARE tested, but
      the multiplication is not. Listen for a plausible length rather than trusting it: at 120
      BPM a 1/16 step on speed x1 should gate for about 125 ms.
- [ ] **A full lock pool.** 1,024 locks per pattern. `lock_delta` compacts and retries before
      refusing, so this should take real work to reach; when it is reached the screen says
      `FULL n`. Try to reach it (eight locks on every step of several tracks) and confirm the
      message appears and nothing else misbehaves.

## M4 — projects and storage

Written from the cloud, so **none of it has run**: no device build, no emulator, no listening.
Every box here is open, and the first three are the ones that decide whether the rest is worth
checking.

The exit criterion from `docs/PLAN.md` is the hard one: *"a power cycle restores the last state;
pulling the card during a save, during sample loading and during resampling never corrupts the
project and never needs a restart."*

**Does it even run**

- [ ] `tools/build.sh` links. Everything compiles and every symbol resolves on a host, but no
      cross linker has seen it.
- [ ] `tests/scenario_m3.sh` still passes: `app/engine.cpp` changed underneath it.
- [ ] The app starts with no card in the slot. A first run with nothing to open must keep the
      built-in default pattern, not fail `init()`.
- [ ] Memory: `Storage::init` takes 64 MiB for the region plus **27.5 MiB of staging** (`raw_`
      16.5 MiB for one whole file, `src_` 11 MiB for a resample's source, both sized from
      `docs/SPEC.md`'s own 30 s in-RAM limit, counted at up to 96 kHz). Host total 101.2 MiB
      of a 120 MiB heap. That is a fixed cost, not per sample. Confirm it
      against the ~124 MiB app heap and record it in `docs/BUDGET.md`.

**Projects**

- [ ] F3 opens PROJECT. The bank grid shows A, the live pattern is boxed, and the top line's
      pattern now follows the chain rather than reading `A01` for ever.
- [ ] FUNC+HOME, OK saves (the menu opens on SAVE). `/ENAGA/DATA/STRATA/PROJECTS/<NAME>/` holds `PROJECT.BIN` (277 bytes),
      `KIT.BIN` (1,076 bytes) and a `BANK_x.BIN` per touched bank.
- [ ] Only dirty banks are written. Edit one pattern in bank A, save, and confirm no
      `BANK_B.BIN` appears.
- [ ] Power cycle. The project reopens on the pattern it was left on, not A01.
- [ ] The FUNC menu's RELOAD PROJECT (OK twice) reloads from the card and discards unsaved edits
      (it was F1 + OK, then FUNC + LEFT twice in PROJECT, until 2026-11-22).
- [ ] The FUNC menu's NEW PROJECT (OK twice) starts a new project under a NEW name (`NEW1`, `NEW2`...), so
      saving it cannot overwrite the project it came from (it was F1 + CLR).
- [ ] A load with the transport RUNNING stops it first. Under CLOCK SRC = MIDI or SYNC, where
      `clock::stop()` returns false, the load is REFUSED and the screen says `EXT CLOCK` — it
      must not load anyway.
- [ ] Hand-damage a `BANK_x.BIN` byte: the other 15 patterns still load and the screen says
      `PATTERN LOST`.

**Autosave**

- [ ] Stopping the transport writes `/ENAGA/DATA/STRATA/AUTOSAVE/`. A stop arriving from MIDI
      autosaves too, since it follows the transport and not the key.
- [ ] Leaving the app (Enaga's menu) runs `stop()` and the autosave lands inside the 3 s budget.
- [ ] With an autosave newer than the project, the next start opens the autosave. "Newer" is a
      counter in `SETTINGS.TXT` (`PROJSEQ` / `AUTOSEQ`), because `files::Info` carries no
      timestamp — confirm the counters move.

**Samples, and the swap protocol**

- [ ] A project whose kit names a WAV loads it at open, and the track stays silent while it
      loads rather than glitching.
- [ ] The `SMP` line shows `O` loaded, `.` loading, `X` failed, `-` empty, and the pool's used
      and budget in MiB.
- [ ] A 44.1 kHz file is resampled and plays at the right pitch. An 8-bit, 24-bit and 32-bit
      float file each load (`core/pcm` is host-tested; this is the wiring).
- [ ] A 96 kHz file of 30 s loads; one over 30 s says `TOO LONG` rather than loading a fraction
      of itself. (Below 96 kHz the same buffer holds more than 30 s, so 44.1 kHz loads up to
      about 65 s.)
- [ ] **The swap, while the track is sounding.** Hold a sample playing and load another on the
      same track. The protocol is clear → wait for `Status::sample_epoch` → release → load →
      hand over, and the failure it prevents is audible garbage rather than a crash. If it ever
      says `NO SWAP`, the epoch never moved and that is a real bug, not a slow card.
- [ ] A track whose new kit names no file loses its old sample on load instead of keeping the
      previous project's sound.

**Pulling the card — the exit criterion**

- [ ] Pull during a save. `files::save` writes `NAME.TMP` and replaces, so the project must be
      either the old one or the new one, never half.
- [ ] Pull during sample loading, and during resampling. The screen says `NO CARD`, nothing is
      written, and no restart is needed.
- [ ] Put it back: the samples the kit names are offered again automatically.
- [ ] Pull it, then save: refused, with `NO CARD` on screen.

**Queue and chains**

- [ ] STEP 1-16 in PROJECT queues that pattern; encoder A picks END / 16 / 32 / NOW.
- [ ] With the transport STOPPED a queued change switches at once, because there is no end of a
      pass to wait for.
- [ ] A change armed a few ticks late still lands on the boundary it just missed
      (`kQueueGraceTicks`), and the new pattern's step 0 has already gone by — `core/queue.hpp`
      says so and it should be audible at the seam.
- [ ] END, 16 and 32 start the new pattern on its step 1 at the boundary, whatever the track
      lengths (a blank pattern's 64-step tracks used to come in on step 16). NOW keeps the
      position. A second END change waits one full pass of the pattern from where it started.
- [ ] The FUNC menu's CHAIN row toggles the chain; starting one jumps to its first entry. CLR cancels a queued change.
- [ ] A pattern change clears the undo history (it belonged to the pattern left behind) and
      restarts the `1ST` conditions.

## M5 — performance layer

Also written in the cloud, so also unrun. `core/scale`, `core/mutate` and `core/perform` are
host-tested and mutation-tested; `dsp/repeat` is host-tested for correctness but its LOAD is
unmeasured, and that is the one figure that decides whether beat repeat ships.

The exit criterion from `docs/PLAN.md`: *"a 30-minute jam using only the performance row, F7
PERFORM and pattern changes, with no edit screens and no underruns."*

**Beat repeat — the load question first**

- [ ] `dsp/repeat` idle: the per-block cost of copying the block into the ring, with and without
      cl6x, into `docs/BUDGET.md`. It is always on, so it is a fixed tax on every block.
- [ ] `dsp/repeat` held: the per-block cost of the interpolated read. Worst case is the shortest
      loop at half rate.
- [ ] Worst case with eight tracks AND a repeat held still under 60%. If it is not, this is the
      effect to cut first -- `docs/SPEC.md` ranks cutting effects above cutting voices.
- [ ] The 2.93 MiB ring, plus M4's 64 MiB region and 27.5 MiB of staging, against the real heap.
      About 95 MiB before anything else, and the staging was never in the spec's table.
- [ ] Hold the pad through a tempo change: the loop length is derived from ticks, so it should
      follow without a click at the seam.
- [ ] Hold it for four seconds and release: the mix resumes where the music actually is, not
      where the repeat left off. That is intended (`dsp/repeat.hpp` says why) -- confirm it does
      not sound like a dropout.
- [ ] Each of the seven lengths, and the triplets against a metronome: 1/4T should be three in
      the time of two 1/4.
- [ ] Pitch-down an octave, by ear against a known tone.

**Mutation**

- [ ] MUTATE on a track, then F1 + CLR: the pattern comes back exactly. "OK keeps it, CLR
      reverts" is the existing undo and nothing else, so this is really a test that a mutation
      is exactly one undo level.
- [ ] Press MUTATE twice: two different results, not the same one twice.
- [ ] The same pattern, the same press count, after a save and a reload: the same result. Seeded
      from the pattern is the claim.
- [ ] Density up and down on a 16-step track: the hits redistribute evenly rather than one being
      added where there was room. Audible as the rhythm changing shape, which is intended.
- [ ] Notes on a melodic track with a scale set: everything stays in the key.

**Key and scale**

- [ ] F3 PROJECT, encoder B walks the key and C the scale; the screen reads "KEY C MINOR".
- [ ] The key wraps at B to C; the scale stops at both ends.
- [ ] Save, power cycle, reopen: the key and scale come back. **This is also the regression test
      for the root bug** -- `app/engine.cpp` used to store 60 in a field the loader reduces to
      `% 12`, so the default pattern's notes would have come back at the bottom of the MIDI
      range.
- [ ] The note names read correctly as sharps: C#, D#, F#, G#, A#, and the '#' is legible at
      this size. The key on PROJECT and the NOTE field on the step page spell the same pitch the
      same way (sharps everywhere since 2026-11-22).

**Scenes, morph and macros**

- [ ] Capture a scene, change the sound, capture a second, then sweep VALUE: the sound crosses
      from one to the other.
- [ ] A scene that stores only one track: sweeping it must not drag the other seven.
- [ ] A parameter only one scene stores fades to the KIT value at the far end, not back to
      itself. This is the morph rule most likely to feel wrong in the room, so listen for it.
- [ ] A macro with two targets, one inverted: one knob opening a filter while it closes a send.
- [x] ~~Assign a macro with its knob half way up: the parameter does not move on assignment.~~
      Superseded 2026-11-22 (PR #34): a macro at 0 lets go, a raised one sweeps from the live
      value, and assigning with the knob up moves the parameter at once. See "Factory content"
      at the end and walkthrough 3b.
- [ ] Macro setup and scenes survive a save and a reload (PERFORM.BIN, 2,424 bytes, next to
      PROJECT.BIN).
- [ ] A project with no PERFORM.BIN -- anything M4 wrote -- opens with empty scenes rather than
      refusing.

**Mutes and fill**

- [ ] A lower pad mutes with F1 in any mode, and MUTE mode (upper pad 3) makes the lower pads
      mutes outright. **With F1 the mute waits for the bar**, which is the deviation to check:
      the spec says "instantly or at the next bar" without saying which gesture picks.
- [ ] A mute armed for the next bar lands on the bar, and pressing three pads before it means
      the third, not all three.
- [ ] **The eight stored states are on STEP 1-8 while MUTE mode is on**, not the upper row.
      docs/SPEC.md says "recalled from the upper row", but that row is seven FIXED pads and
      there are eight states, so it cannot be literal. Recall on a tap, F1 for the next bar,
      VALUE held to store. If the row is what was meant, this is the thing to change.
- [ ] They survive a save: the states live in PROJECT.BIN.
- [ ] Auto-fill at 4, 8 and 16 bars fires on the LAST bar of the period, and FILL conditions
      follow it. The FUNC menu's AUTO-FILL row picks the period.
- [ ] **Stop and start the transport, then check auto-fill.** The bar counter resyncs on a
      backwards jump; without that it would have been wrong for ten bars after every stop, which
      is the bug this box exists for.
- [ ] Holding FILL with the transport STOPPED still reads as held.

**The panel, and what is reachable at all**

Every tool is reachable from the panel, which is worth confirming one by one: a feature with no
gesture is the mistake this milestone made twice before it was caught.

- [ ] Upper row, left to right: FILL (hold), REPEAT (hold, VALUE picks the length, F1 the
      pitch-down), MUTE mode (toggle), scene A (hold + STEP picks), scene B, THROW (hold, F1 for
      reverb), MUTATE.
- [ ] FUNC + MIXER opens PERFORM: encoders A-D are the macros, VALUE is the morph.
- [ ] In PERFORM: OK captures a scene (the selected track only while a scene pad is held),
      scene pad + CLR erases one, **STEP 1-16 assigns that parameter of the selected track to the
      selected macro**, LEFT/RIGHT pick which macro, CLR clears it, UP and DOWN walk the
      mutation, the screen keys its amount.
- [ ] A macro assigned in one press is immediately usable, and since PR #34 it sweeps from the
      parameter's live value (the kit, a morph or the EDIT knob), letting go again at 0. Check it
      feels right; the ranges are stored and a richer editor can refine them later.
- [ ] A throw released puts the stored send back by itself. Nothing was written to the kit, so
      there is nothing to undo and nothing to save.
- [ ] A lock still wins over a morph: lock a cutoff on one step, sweep the morph, and that step
      should keep its locked value while the others move.

- [ ] **The per-track LFO** (added 2026-11-22): EDIT's fifth page, WAV RAT DEP DST; zeroed bytes
      are OFF, so older kits and scenes sound as they did. Free and synced rates, every shape,
      p-locks, macros, scenes and the morph on the four bytes, save and reload (KIT.BIN's LFO
      section, PERFORM.BIN's scene LFO section). A kit or project from an older build loads with
      every LFO off. Section 13e of the walkthrough.
- [ ] **Per-track swing** (added 2026-11-22): PROJECT's PATTERN page, PAT or 50-75 per track,
      saved in the pattern, undoable. Section 13e.

## M6 — full concept (started)

The Euclidean generator and KEYS mode so far. `core/` is host-tested and mutation-tested; the gesture is
on encoders, which the emulator cannot drive.

- [ ] F1 + A, B, C in GRID walk hits, the cycle and the rotation, and the screen reads
      "EUC 5/16 R2". **Encoders cannot be driven by the emulator** (`emulate.py` has `--key`
      only, and the core exposes `key_input` and nothing else), so this binding has never been
      exercised at all.
- [ ] A sweep of any of the three is ONE undo level, not forty. They share a gesture so
      core/undo coalesces them; F1 + CLR once should put the old rhythm back.
- [ ] 3 hits in 8 on a 16-step track plays the figure twice. That is the point of hits and the
      cycle being two numbers.
- [ ] Generating over a part you have shaped by hand replaces the rhythm and keeps the sound:
      locks, conditions, ratchets, micro-timing and probability all survive. Host-tested, but
      worth hearing once.
- [ ] A rotation of a whole cycle is a no-op and reports no change, so it costs no undo level.

### The three M6 interface decisions, and what each one settled

Three M6 features and one M5 leftover all wanted something `dsp/track.hpp`'s `Machine` did not
have. All three were decided before any of them was wired in, so the audio behaviour changes
once and `app/engine.cpp` was edited once. What was chosen, and what is still open:

Two of them are now also WIRED: VA POLY is on the CHORD track and `Engine::fire` expands a
step's chord byte onto its voices.

| The question | Decided | Still open |
|---|---|---|
| A per-note note-off, so `Step::length` can be heard | **A duration on the `Trig`.** `Trig::gate_frames` is additive — no new virtual, no note-to-voice map in the engine — and the machine releases its own voice when the count runs out. `Step::length` has been stored since M3 and makes a sound for the first time | A held KEYS key and `Cmd::release_note` still have no known duration, so they still cannot be released. A `release(note)` virtual can land later without disturbing the gate; it is not needed for a sequenced part |
| `render_add_stereo`, promised by a comment and declared nowhere | **The comment was wrong, and was corrected.** Machines are mono and the chain spreads them, which is what the rest of the code implements; VA POLY took pitch detune instead of stereo unison for the same reason | Whether a stereo machine is worth it at all. The cost is not the virtual but `InsertChain` being mono, so a stereo track doubles that track's insert work — on the two most expensive machines. That is a question for after a unit measurement, not before |
| Whether `Engine::fire` folds a MONO note into the scale | **FIXED: it does, and so does every melodic machine.** `machine_folds_note` (core/project.hpp) is the notion that was missing — the two VAs, GRAIN, and SAMPLE while unsliced — and machine switching is what supplied it. A POLY and a MONO track now play identical step data identically | Decided 2026-11-22: GRAIN folds, SAMPLE folds only at SLC = 1, a sliced sample never folds. Listen to confirm it (below) |

- [ ] **The fold, now that both sides do it.** Put the same note value on a MONO track and a
      POLY one in a scale that does not contain it. They must now AGREE, both in key. They
      disagreed until machine switching supplied `machine_folds_note`.
- [ ] And the drums do NOT fold: put a kick on a note the scale does not contain, in a minor
      key, and it must not change pitch. Folding it would retune it, which is the reason the
      predicate exists rather than a blanket fold.
- [ ] GRAIN played melodically in a minor key folds into it, like the VAs.
- [ ] SAMPLE with SLC at 1 (one-shot) folds; turn SLC up and the same steps play their slices
      as written, unmoved by the key. Lock SLC on one step and check that step follows its lock.

**VA POLY (`dsp/vapoly`)** — 41 host tests, 16 of 18 mutations caught on the machine and 11 of
14 on the gate (all five survivors honest, and named in `tests/test_vapoly.cpp`), and VaMono's
voice factored out and proven BIT-EXACT over 4,840 rendered values — still bit-exact after the
gate, which is why the countdown lives in VaPoly's `Slot` and not in the shared `VaVoice`.
**Registered on the CHORD track**, with chord expansion in `Engine::fire` and a MIN7 in the
default kit, so it sounds the moment the app starts: measured contributing a 0.09 peak to the
mix (-20.8 dBFS under the limiter) on every stab step, and 4.25 points of load per voice.

- [ ] Six notes sound together; a seventh steals the quietest started voice rather than being
      dropped.
- [ ] **The step page's CHRD field reads chord names** (MIN, MIN7/2, 5THOCT/2) and every click is a
      different chord: 43 positions, no dead bytes.
- [ ] **A chord's six notes on the same frame all sound.** A voice is busy from the moment it
      takes a trig, not from the frame that trig starts on — the subagent found the bug this
      prevents via a surviving mutation, and the test is
      `vapoly_a_chord_change_that_steals_keeps_all_of_its_notes`.
- [ ] A ratchet's repeated note takes a second voice, so both hits sound.
- [ ] Slot 10 is **DET** (voice detune, up to a quarter semitone) where VA MONO has GLD, since
      glide and slide are MONO-only. "Spread" is therefore pitch, not stereo, and stereo unison
      is not implemented: machines are mono by decision, not by oversight (see the table above).
- [ ] **A gated chord.** Give a POLY step a short length: all six notes should end together and
      FADE rather than cut. Then a chord whose notes carry different lengths via locks — the
      countdown is per voice, so they should end apart.
- [ ] A gate shorter than the attack. The note should still speak before it releases, not click.
- [ ] **The six-voice cost, with cl6x. Without it, it is measured and it is bad:** 4.25 points
      per voice, so 25.6% of the block for six, against SPEC's 11,208 cycles for the whole
      machine -- 11.3x. The derived estimate in this box used to read ~2.0% for all six; it was
      wrong by an order of magnitude. docs/BUDGET.md has the four-point measurement, the three
      possible explanations and what to reach for. **This is the measurement the project most
      needs from the unit**, because every option for fixing it is work that should not start
      until someone knows whether cl6x already has.

**Chords (`core/chord`)** — 27 host tests, 15 of 15 mutations caught, and `Engine::fire` now
calls it: a POLY step's chord byte is expanded onto VA POLY's voices on one frame. The expansion
itself has no host test, because nothing in `app/` does — so the boxes below are the only check
on it.

- [ ] **The default kit's stab is a MIN7.** It is in A01 (core/factory.cpp) so the chord path runs
      every time anyone plays the default beat, the way the parameter locks are. Four notes
      should sound, together, on steps 4 and 11 of track 7. One note means the expansion is not
      running; a flam means the notes are not landing on the same frame.
- [ ] A POLY step with each chord type sounds like its name. The table is semitone stacks folded
      into the key, which gives the diatonic triad on every degree of a mode — so MAJ on the root
      of a minor key correctly comes out MINOR. Verify that reads as a feature and not a bug.
- [ ] Walking the chord field in the step page reaches every chord: the packing is 6 bits because
      `core/stepedit.cpp` already publishes the field as 0..63, and 58 is the highest byte it
      produces.
- [ ] A cleared pattern plays single notes. Type 0 with inversion 0 is 0, so zeroed memory is one
      note rather than a chord nobody asked for.
- [ ] Inversions move the voicing without changing the notes. Depth is clamped to one less than
      the note count, because deeper would transpose rather than invert.
- [ ] **Both VAs fold now**, so a chord of `none` on a POLY track and the same note on a MONO
      track are the same pitch. `core/chord` folds the chord's root and `Engine::fire` folds a
      melodic track's note; the asymmetry between them is gone.

**KEYS mode** — the one M6 feature that IS scenario-tested (`tests/scenario_m6.sh` plays three
keys with REC on and watches them land in the pattern), so these are about feel rather than
whether it works.

- [ ] F1 + F4 opens KEYS; the screen shows all sixteen notes under the keys, roots brighter.
- [ ] The sixteen keys are sixteen DEGREES of the project's scale, not sixteen semitones: in a
      seven-note scale they should span just over two octaves and every one should be in key.
- [ ] The pad-row keys (and LEFT/RIGHT) move the octave; UP/DOWN change track without leaving the
      mode, and the screen keys turn the EDIT page that A-D play with.
- [ ] Change the key or scale in F3 and the KEYS layout follows it.
- [x] ~~A key has no release.~~ Superseded 2026-11-22: letting go of a key releases its note
      (see "KEYS: a key holds its note" at the end).

**GRAIN (`dsp/grain`)** — 46 host tests, 64,759 checks, and **not one of these boxes can be
ticked, because GRAIN cannot be reached.** docs/SPEC.md's default kit fills all eight tracks with
other machines, so there is no slot for it until a track's machine is switchable. That is M6/M7
work and it is not written. The code is verified to the extent host tests can verify anything:

- Compiles and LINKS for the C674x. Verified by forcing every entry point into the link
  (`machines_` leaves it unreferenced and `--gc-sections` drops the code, so a plain build proves
  only that it compiles): with the references in, APP.ENA goes from 422,600 to 439,568 bytes and
  `build_app.py`'s own "no undefined symbols" check passes — so no 64-bit divide, which on this
  toolchain drags in the ARM unwinder and stops the whole app linking (seawaffle/enaga#1).
- **No out-of-range read**, under AddressSanitizer rather than by argument: the whole suite built
  with ASan at -O1 reports zero buffer overflows over all 812 tests. That is the one check the
  machine's own oracle test cannot make, because a read that lands out of range with an
  interpolation weight of zero contributes nothing to hear.
- Allocates nothing — not even in `init()`. It reads PCM the track already holds.
- Mutation-tested twice, independently: 24 of 26 by the agent that wrote it, then 13 of 14 by a
  set chosen from reading `grainloops.ti.c` fresh. The second pass found three untested
  behaviours the first had not — the window's interpolation, the upper end of its index clamp and
  the stereo read's second tap — and two oracles were added that kill all three. One survivor
  remains in each set and both are the same honest one: removing the window phase clamp changes
  no sample today, because the table's padding and not the clamp is what bounds the read.

- [ ] **Everything about how it sounds.** POS, SIZ, DEN, SPR, PIT, SHP: every one of them is
      unheard. The parameters are documented in `dsp/grain.hpp` and nothing has listened to one.
- [ ] **Its cost on a REAL sample.** Now measured in the emulator on the placeholder:
      **+1.6 points at the default density, +9.2 with all eight grains**, against SPEC's 4.3% --
      so 2.1x over at the extreme, GCC-only. That is a floor and not a worst case: the
      placeholder is 38 KiB and sits in cache, so eight grains reading it are eight streams over
      one resident buffer. Load a multi-second sample, turn SPR up, and watch what happens.
      `docs/BUDGET.md` has the four-point table.
- [ ] The cache behaviour of eight concurrent read streams, which is the part of the estimate
      that could not even be bounded. Under heavy SPR every grain starts at a fresh random
      offset, so a sprayed cloud may be almost all L1D misses. If the measurement disappoints,
      this is the first thing to look at, and the lever is grain count rather than the
      interpolator.
- [ ] **SPEC's "tempo-locked stretch" is served by POS under a step lock or a tempo-synced LFO**,
      not by the machine knowing the tempo — no `dsp/` file includes `<enaga/clock.hpp>` and
      `Machine::prepare` carries only `TrackParams`. That is an interpretation of a SPEC line
      rather than something SPEC spells out, and it is worth confirming against SPEC before the
      machine is reachable.
- [ ] No reverse grains. Not in SPEC's GRAIN row, and they would double every case in the bounds
      argument.

### Ten bugs a review of `app/` found, all fixed, none of them testable here

`app/` is 5,142 lines and the host suite compiles only `core/` and `dsp/`, so a review is the
only verification it gets. These are the fixes, and each one needs a gesture on the unit to
confirm — that is what the boxes are.

**Three shared one root cause**: M5 added a per-block copy of `performed_` into `effective_` so a
morph could be heard between steps, and it quietly reverted anything else that wrote `effective_`.
**All three now have host tests** (`tests/test_app.cpp`), so the boxes below are a confirmation
rather than the only check there will ever be.

- [ ] **Turn a knob in EDIT mode. It should be heard at once.** `Cmd::set_param` mirrored into
      `effective_` but did not set `perform_dirty_`, so `apply_performance` returned early,
      `performed_` kept the old value and the block copy put it back. Knob edits were inaudible
      until some unrelated performance control fired.
- [ ] **Load a project. The kit should SOUND like the one on screen.** Same mechanism via
      `params_from_kit()`: the previous project's kit went on playing under the new project's
      patterns.
- [ ] **Motion-record a knob. It should be heard on the pass it is recorded**, not the next one.
      `lock_delta` mirrored into `effective_` without setting the `locked_` bit, so the copy —
      which rewrites everything the mask does not claim — undid it before a sample was rendered.
      The comment in the code promised exactly what the code then prevented.

**And seven more:**

- [ ] **The sidechain duck works at all.** Both arms of its depth ternary were `0.0f`, so an M2
      SPEC feature was dead while the curve was still being rendered on every kick. Now 0.35 —
      **a number to settle by ear**, since SPEC gives attack, hold and release and no depth.
- [ ] ~~The duck depth is not reachable from the panel.~~ It is now, with the source and the
      times: MIXER's DCK 1-4, DCK 5-8 and DUCK pages, saved in the kit (13l).
- [ ] **Generate a Euclidean rhythm, save, reopen.** It should still be there. `Cmd::euclid` was
      missing from `mutates_pattern`, so the bank stayed clean and an explicit save skipped the
      bank file entirely.
- [ ] **Change the key or scale, save, reload.** It should hold. Both commands write
      `project_.settings` AND `live().root`/`.scale`, and only the project was marked dirty — so
      the bank's untouched copy won, which is the copy `core/scale` reads.
- [ ] **A WAV with a trailing chunk** (LIST, cue, id3 — most editors write one) should play its
      audio and not its metadata. The frame count came from "everything after the header" rather
      than from the data chunk's own length.
- [ ] **Hold a STEP key, change page or mode, let go, then turn an encoder.** No parameter lock
      should appear. The hold was resolved against the CURRENT page on release and against the
      press's page on the way in, and KEYS, PERFORM and MUTE swallow the STEP keys entirely — so
      a hold could be stranded forever, and the next encoder move wrote a lock onto a phantom
      step.
- [ ] **Press PLAY while a project is loading.** It should refuse and say `LOADING`, not start
      the transport over pattern memory that `step_load` is halfway through rewriting.
- [ ] **The unsaved-changes `*`.** Six gestures — mutate, a live-recorded hit, a queued pattern,
      a macro assignment, a morph move and a mixer level — built their command by hand and so
      skipped `note_command` completely, leaving the screen claiming the project was saved. All
      posts now go through one function; check the `*` appears for **five** of them.
- [ ] **The morph is the exception, deliberately.** `Cmd::set_morph` is in none of
      `app/storage.cpp`'s `mutates_*` lists, because the morph position is "where the performer's
      hands are, not part of the project". So moving VALUE in PERFORM must NOT raise the `*`,
      and `tests/test_ui.cpp` asserts the pair: the morph moved **and** the project stayed clean.
      If that feels wrong on the unit — if someone sets a morph, saves, reloads and is surprised
      it did not keep — the fix is to add it to a list, not to the UI.

### `app/` is host-tested now, and the first thing that found

`tests/fakehost.cpp` is a fake Enaga: `enaga::host()` returns a struct of 38 function pointers
that the app runtime normally defines, and `host/Makefile` does not link that runtime — so the
tests can define it instead and `app/engine.cpp` and `app/storage.cpp` compile for the host
exactly as they compile for the device, with no `#ifdef` in either. The clock is programmable and
a `files::load` does not complete until a test says so.

It found a data-loss bug before it had a single assertion in it, and a second one an hour later.
Both are fixed, both have regression tests, and both are listed here because the fix should be
confirmed by hand once:

- [ ] **Save a brand-new project, then reopen it.** `request_save` marks only the KIT dirty and
      `step_save` skips every file whose flag is clear — the flags describe how RAM differs from
      a project ON THE CARD, and for a project never written there is nothing to differ from. So
      a first save stored `KIT.BIN` alone and reopening it failed with NOT FOUND: the user's own
      work, reported as saved, and gone. An explicit save now writes all ten files when the
      project is not known to be complete on the card.
- [ ] **Swap SD cards, then save.** The "project is complete on the card" flag is now cleared on
      any card transition, because after a swap it is a claim about a card nobody has looked at.
      Without that, a save onto a fresh card would write only the dirty files again.
- [ ] **Press SAVE, pull the card, put it back.** The save is CANCELLED — `on_card(false)` clears
      the pending request along with any job — so the project stays unsaved with `NO CARD` as the
      only sign. Nothing is lost and asking again works. Asking the other way round (card out
      first, then SAVE) does complete by itself when a card arrives. Both are pinned by tests;
      **whether the cancelled one should resume on its own is a judgement for the unit.**

### And then `app/app.cpp` became testable too

`ENAGA_APP(Strata)` defines the package's entry points and belongs to a device build, so the
whole `Strata` class moved to `app/app.hpp` and `app/app.cpp` is now that one line. The move was
pure -- the device image is byte-identical across it -- and `tests/test_ui.cpp` includes the
header and drives the same class the unit runs, with no `#ifdef` anywhere in `app/`.

19 tests, 26 of 27 mutations caught. They pin the held-STEP state machine, tap against hold
against range, the mode changes, KEYS as scale DEGREES, the GRID encoder routing, PLAY's refusal
during a load, and the unsaved-changes mark. What they cannot reach is said in comments rather
than faked: the LED language and all screen TEXT, because the fake host counts LEDs and ignores
text. The screen is checked by pixels instead.

**A third way to strand a held STEP key came out of it, and it is fixed.** MUTE mode and the
scene pads swallow the STEP keys as well, and neither is a mode -- the performance row works in
every mode on purpose -- so they needed the hold dropped without the rest of a mode change.
Before: hold STEP 1, tap the MUTE pad, let go, and the next encoder turn wrote a parameter lock
onto a step nobody was touching.

- [ ] **Hold a STEP key, tap MUTE or a scene pad, let go, turn an encoder.** No lock should
      appear. Then confirm the pad itself still works: dropping the hold must not have swallowed
      the gesture it was pressed for.
- [ ] A tempo clamp sign bug was fixed on the way past: turning VALUE down hard used to wrap an
      unsigned subtraction and clamp to 300 BPM instead of 20. Not reachable from the panel (an
      encoder event carries a click or two), so this is a glance rather than a test.

### Three more from a review of `core/` and `dsp/`

That review read 14,423 lines with 531,652 checks already behind them and found two real bugs,
one rule violation and one stale comment. The two bugs are fixed and have tests (the ratchet, and
the duck's dropped trigger); the other two are here.

- [x] ~~Two note spellings in one app~~ **Decided 2026-11-22: sharps everywhere.**
      `core/scale.cpp` now spells "D#" like the step page's "D#3", and a host test holds the two
      `note_name` overloads to one spelling. What is left to see on the unit is in the scale
      section above (the '#' legible at screen size).
- [ ] A 64-bit divide was sitting in the audio path (`dsp/hat.cpp`, each voice's start phase).
      On this toolchain that does not merely cost cycles, it does not LINK -- it drags in
      libgcc's ARM unwinder. GCC happened to fold it to six constants, so the rule was being kept
      by the compiler's goodwill; it is now a literal table, and the hat's own tests passing is
      what proves the table matches the fold. Nothing to check by ear.

### Resampling — written tonight, reachable from the panel, never heard

docs/SPEC.md's eighth performance tool, end to end: record the finished master for 1, 2, 4 or 8
bars, write it to `/RECORD/STRATA_<n>.WAV`, and load it back into a track in one action. The
gesture is **F1 + F2**, which is where SPEC's key table puts "Resample setup": encoder A walks
the four lengths, the screen keys walk the destination (the file only, or one of the eight
tracks), OK arms and CLR cancels.

Host-tested end to end through a fake card -- the written file is checked by PARSING it with
`<enaga/wav.hpp>` -- and the gesture is tested from the keys with nothing posted by hand. What no
test can tell you is whether a take sounds right, which is the whole point of the feature.

- [ ] **Record a bar and listen to it.** It should be the mix you just heard, including beat
      repeat if you played one into the take -- it records the FINISHED master, after the limiter.
- [ ] **It should LOOP.** The take begins on a bar line, not under your finger, which is the only
      reason a round number of bars is worth recording. If the loop has a gap or a stutter at the
      join, the bar alignment is the first thing to suspect.
- [ ] **Load it straight into a track and play it.** That is "in one action": OK with a track
      chosen writes the file, puts its path in that track's kit and loads it through the ordinary
      sample swap.
- [ ] 8 bars below about 64 BPM is longer than the 30-second buffer. The take is SHORTENED rather
      than refused -- a short loop is usable and a refusal mid-performance is not -- and the
      screen says `SHORTENED TO 30S`. Check it says that rather than silently giving you less.
- [ ] A second take does not overwrite the first: the number comes from the settings and survives
      a restart. Record three and check `/RECORD` holds `STRATA_1`, `_2` and `_3`.
- [ ] Arming with the transport stopped says `ARMED: PRESS PLAY` and waits. Nothing records until
      the clock crosses a bar line.
- [ ] A second arm during a take is refused and counted, because there is no undo for a lost
      take. Check the screen says something rather than quietly restarting.

### Machine switching — F1 + F8, and the thing that makes GRAIN audible

Every track owns one of every machine (72.6 KiB, measured) and switching is a pointer swap.
`KitTrack::machine` has been in KIT.BIN since M4 and nothing honoured it until now, so a project
saved with a different kit used to come back with the default one's machines.

- [ ] **Put GRAIN on a track and play it.** This is the first time it can be heard at all: it
      has been in the tree, host-tested and mutation-tested twice, with no way to reach it.
      Everything in its own section above — POS, SIZ, DEN, SPR, PIT, SHP — is now listenable.
- [ ] **The knobs keep their positions across a switch and the sound changes.** Nothing is
      translated: slot 10 is GLD on VA MONO and DET on VA POLY, and docs/SPEC.md gives every
      machine the same sixteen slots on purpose. Check that reads as a feature and not as a bug.
- [ ] **Switch a machine mid-note.** The one being left is silenced, because it will never be
      rendered again and so cannot release its own tail. A click there is expected; a note that
      never ends is the bug this prevents.
- [ ] **The hat choke follows the kit.** A closed hat cuts an open one, and that pair is now
      re-derived rather than wired: move either hat off tracks 3 or 4 and the choke should stop,
      put them back and it should return.
- [ ] **A new project is playable.** NEW gives the factory project (core/factory.hpp): docs/SPEC.md's
      eight machines with their sounds set, and bank A's four patterns. Until 2026-11-22 it gave
      a kit with LVL 0 on every track, so a new project was silent.
- [ ] **A GRAIN track plays the track's card sample**, not the placeholder, and a track switched
      SAMPLE → GRAIN → SAMPLE across a sample change plays the new file on both.
- [ ] **A track switched to SAMPLE or GRAIN before any card has been read still makes a sound**:
      every machine in the pool gets the placeholder at init.
- [ ] Save a project with switched machines, reopen it, and check it comes back the same. This is
      the first build in which that field means anything.
- [ ] **Eight tracks of VA POLY is four times the block** and nothing stops you. Worth trying
      once to know what it sounds like when it overruns, and worth knowing that the same is true
      of eight tracks of anything.

## M0.5 — budget spike

Folded into the M2 worst-case measurement above — see `docs/BUDGET.md` for why. These two lines
stand only if that measurement is ever disputed.

- [ ] (If needed) the four original configurations measured **on the unit**, not the emulator.
- [ ] (If needed) go/no-go recorded on 8 full insert chains plus both sends.

## Standing checks for every milestone

- [ ] Worst-case load under the milestone's target, recorded with and without cl6x.
- [ ] One-hour soak with pattern changes and the performance tools; peak load and underrun
      count recorded afterwards.
- [ ] Card tests: pull the card during a save, during sample loading and during resampling.
      The app must recover without a restart (`on_card(false)` closes the dead handles).
- [ ] Timing: record line out at 48 kHz and measure 16th-note click spacing over 5 minutes.
      M1's bar is onset jitter under 0.1 ms.

## MIDI out per track, MID, and MIDI in (after v0.1)

Host-tested (`tests/test_midiout.cpp`, `tests/test_midi.cpp`, the MIDI tests at the end of
`tests/test_ui.cpp`) and drawn on the emulator (`tests/scenario_midi.sh`, keys only: it cannot
turn the channel knob or send MIDI IN). Never heard. Section 8d of the walkthrough is the short
version; these are the rest.

- [ ] Two tracks on one channel playing the same pitch: one note on the synth, never a hanging
      one (the second note-on ends the first).
- [ ] A VA POLY MIN11 stab with a MIDI OUT channel: the synth gets all six notes even while
      STRATA's 12-voice budget chokes its own.
- [ ] A ratchet x4 on a MID track: four notes, evenly spaced.
- [ ] Kit recall (KIT, OK twice) with a MIDI note held: it ends. The channels themselves stay
      (they are the project's, not the kit's).
- [ ] Save, power cycle: the MIDI OUT channels come back. A project saved before this change opens
      with every track OFF.
- [ ] Fill the queue: eight tracks on MID with 1/16 ratchets x8 and CCs moving on all of them.
      No hanging note after STOP (a refused note-off is retried until it goes). LOAD: `____ %`
- [ ] MIDI IN on channel 10 (selected track): select track 3, hold a note, select track 5, let
      go -- the note on track 3 ends.
- [ ] Unplug a keyboard with a key held (active sensing stops): Enaga sends CC 123, the note ends.
- [ ] With USB plugged into a PC that is NOT reading the port: on 0.2.0 firmware, does DIN out
      stall? `____` On firmware with upstream `192f03f`: it must not.

## Expect to find Enaga bugs, not just STRATA bugs

Enaga's MIDI DIN, SYNC, USB MIDI, input routing and pad-pressure curve are all implemented but
**have never run on a unit** — they are emulator-tested only. Two further known gaps: the
original's SYNC polarity settings are not built (Enaga uses rising edges in, active-high out),
and its USB IDs are TinyUSB test values that its own design log says must not ship.

So when sync misbehaves, suspect the platform before the app. Report upstream with a failing
case; never patch `third_party/enaga` in this repo.

## Pad feel

- [ ] Velocity curve across all 15 pads: a light press should be playable, a hard press should
      reach 127 without clipping the feel. Enaga maps 127 at about 3,300 raw, with light
      presses around 1,500 and hard around 3,400.
- [ ] Crosstalk between neighbouring pads — Enaga lists this as an open item, so characterise
      it before tuning a per-pad curve.

---

## The sends, the master and SETUP

These were the last settings in the app that nothing could change: `Cmd::set_sends` and
`Cmd::set_master` had no handler and no caller, so every delay and reverb setting and the spec's
master DJ filter sat at a compile-time default for the life of the app, and PROJECT.BIN had no
section for them. All fourteen are now encoder bytes in the project file. `docs/REVIEW.md` 2a
records where each gesture went and why.

- [ ] F7, then the screen keys: MIXER walks TRK 1-4, TRK 5-8, DCK 1-4, DCK 5-8, DUCK, DLY, DLY+,
      REV, REV+, ROUTE, MST, EQ and back round. Every page draws, with its four names, values and bars.
- [ ] On DLY, encoder A walks the delay's DIVISION: `1/32`, `1/16T`, `1/16`, `1/8T`, `1/8`,
      `1/8D`, `1/4`, `1/4D`, `1/2`, `1/1`. The page's `TIME nnn MS` line follows it, and the
      ends HOLD rather than wrapping.
- [ ] **Hear the tempo-sync.** Set the delay to `1/4` and turn VALUE: the repeats stay on the
      beat. This is the one thing the host tests cannot check — they can only read the number.
- [ ] On DLY, B is feedback, C tone, D the return level. B at the top is a long tail that
      decays; it must NOT grow without bound (the field stops at 0.95).
- [ ] On REV, A is decay 0.2–30 s, B damping, C the input high-pass 20 Hz–2 kHz, D the return.
      A's travel should feel even — it is exponential, so a click at the bottom and a click at
      the top are the same musical step.
- [ ] On ROUTE, A throws the delay's ping-pong on and off (`ON` / `OFF`), B feeds the delay into
      the reverb. C and D are empty and must do NOTHING — in particular they must not fall
      through to the track faders underneath.
- [ ] On MST, A is the DJ filter, B the glue compressor, C the limiter threshold, D the output
      level. A reads `OFF` at centre, `LP nn` below it and `HP nn` above.
- [ ] **F1 + VALUE is the master DJ filter**, as `docs/SPEC.md`'s key table says, from anywhere.
      The notice shows `DJ OFF`, `DJ LP nn` or `DJ HP nn`. The tempo must not move with it.
- [ ] Save, power cycle, reopen: every one of the fourteen comes back. Before this they did not
      exist in any file.
- [ ] **The kill EQ** (added 2026-11-22): MIXER's seventh page, `EQ`, LOW MID HI on A B C, flat at
      `0`, `KILL` at the bottom, `+63` at the top; push A-C there, or FUNC + push A-C anywhere,
      latches that band's kill switch. The three bands and the switches save with the project
      (the mixer section grew from 14 to 18 bytes; a 14-byte section from an older build loads
      with the EQ flat). A kill and its return must not click on a full-scale kick.
- [ ] A project saved by an older build (two sections, 251 bytes) still loads, and its sends and
      master come up at the DEFAULTS rather than at the previous project's.
- [ ] **The tempo saves.** Set 142.0, save, power cycle: it reopens at 142.0, not 130.0. The
      clock follows on the first update pass.
- [ ] F3, then the screen key RIGHT three times: the SETUP page. It shows CLOCK SRC, the tempo and
      whose it is, MIDI CH and QUANTIZE, and says the first three are set in the Enaga menu. The
      screen key LEFT goes back.
- [ ] Encoder A on SETUP is QUANTIZE, 0-100 in fives, clamped at both ends. B, C and D do
      nothing there — and must not reach PROJECT mode's key and scale encoders underneath.
- [ ] Quantize survives a save and a power cycle. It used to live only in `app/`.
- [ ] Change CLOCK SRC to MIDI in the Enaga menu with nothing plugged in: SETUP says `MIDI`, the
      tempo line says `EXT`, and **turning VALUE is refused** — the number must not move. Before
      this it moved and Enaga threw the call away, so the two drifted apart with every click.

---

## VA POLY after the optimisation loop

docs/BUDGET.md: a voice went from 4.30 to 3.82 points, GCC-only, from a fused output tail
(`dsp/valoops.ti.c`) and filter coefficients shared between the voices of a chord. Both are
bit-exact by construction and by test, so there is nothing new to HEAR -- which is exactly what
wants confirming on the unit, because "bit-exact" was argued against GCC and the unit runs cl6x.

- [ ] Play the default beat and listen to the CHORD stab. It must sound as it did: same filter
      movement, same bite on the accent. A shared coefficient that was not actually shareable
      would show up as the voices of one chord opening at slightly different rates.
- [ ] Hold a six-note chord (MIN11 from the step page) and sweep CUT and RES on the stab. No
      zipper, no voice lagging behind the others, nothing crackling at full resonance.
- [ ] An accented step inside a chord: the accent must open ITS voice further and leave the
      others alone. The accent is part of the cache's key, and this is what proves it.
- [ ] A chord with a very short `Step::length`, so the gate cuts slices mid-block. The cache
      holds eight entries, one block's worth with room to spare; a short gate makes more slices
      than usual, and the worst case is simply that the extras miss.
- [ ] Record the unit's load with cl6x for a 4- and a 6-voice chord, and put both in
      docs/BUDGET.md next to the GCC-only table. That column is the one this container cannot
      produce, and the 60% target depends on it.

## The shared VA POLY voice budget (2026-11-22)

At most 12 VA POLY voices sound at once across every track (`Engine::kPolyVoiceBudget`). Past
that, a new note fades out the quietest voice on any POLY track over 1.3 ms; notes that arrive
while every counted voice is still waiting for its frame are dropped. Twelve is a starting point.

- [ ] Switch three tracks to VA POLY (FUNC+EDIT, UP/DOWN), give each a six-note chord (MIN11) on
      the same step, DEC high. Only two chords' worth sounds; the third track's notes are dropped
      on that step. No glitch, no overrun. MAX `________ %`
- [ ] Put the third track's chord one step later instead: it sounds, and the oldest, quietest
      voices of the first two fade without a click.
- [ ] With MAX from the line above and docs/BUDGET.md's per-voice cost on the unit, decide the
      budget: largest that keeps the worst case under 60%. New value `________`

## The CPU guard (2026-11-23)

When the block's load runs high, STRATA sheds the costliest sustaining work instead of letting
the next chord overrun (`core/cpuguard.hpp`). It rises a level when the load averaged over about
11 ms reaches 70%, or at once on one block at 90%; it falls a level after each full second under
55%. Each level takes two voices off the shared VA POLY budget (12, 10, 8, 6, 4) and shrinks
every GRAIN cloud's pool (8, 6, 4, 3, 2 grains). Shed POLY voices fade over 1.3 ms, quietest
first; GRAIN skips new grains rather than cutting old ones. While it is up, the bottom line shows
`GUARD1`-`GUARD4` where the device name was. A second STOP (cut the tails) resets it. The
thresholds are provisional, like the voice budget.

- [ ] Default beat (A01), PLAY for a minute: the bottom line never shows `GUARD`. If it does, the
      thresholds are too low for the unit: note the MAX `________ %`.
- [ ] Repeat the 84% session: big effects, three or four POLY tracks holding MIN11 chords, a GRAIN
      track at DEN and SIZ high. `GUARD` appears; MAX stays under `________ %` (it was 84.3%
      without the guard). The chords get thinner; no click, no crackle, no overrun.
- [ ] Let go of everything with the transport running: within a few seconds `GUARD` counts down
      and disappears, one level a second, and the chords come back full on the next hits.
- [ ] Listen for pumping: on a pattern that sits right at the edge, the voice count should not
      audibly swing up and down every second. Notes: `______________________________`
- [ ] Second STOP while `GUARD` is up: it disappears at once.

## Downsampled samples are low-passed at load

core/pcm now filters a file above 48 kHz before resampling it, so content above 24 kHz no longer
folds back as audible tones. It costs main-loop time at load, estimated and not measured.

- [ ] Load a 15 s and a 30 s stereo 96 kHz WAV (30 s of the file's own length now loads) and time them. Put the figure in docs/BUDGET.md, "Load time".
      The screen must keep drawing and the keys keep answering while it shows RESAMPLING.
- [ ] Load a 96 kHz sweep that runs past 24 kHz and play it. Nothing should come back down as a
      falling tone after the sweep passes 24 kHz.
- [ ] A 44.1 kHz file must sound exactly as before: it is not filtered.

## Screen layout (first boot showed overlapping text)

The first boot on a unit showed text drawn over other text and over the bars. The host suite now
records every string the app draws and holds every screen to the font's real metrics (5 x 7
glyphs, 6-pixel advance, 21 characters a line, the font's character set): no string may cross
another, run off the screen, or partly cover a bar (`tests/test_ui.cpp`, "the screen's layout").
What it cannot judge is whether the result reads well on the OLED.

- [ ] GRID: the four encoder slots at the bottom show the name over the value, four clean
      columns, the fourth fully on screen. The track line ("T1 ... X3/4") ends inside the screen.
- [ ] EDIT: each row reads "A CUT 064" with its bar; the title names the machine; the hint
      "FUNC+EDIT: MACHINE" is legible.
- [ ] PERFORM: the macro bars stop above the two hint lines; the selected macro shows as ">A".
- [ ] PROJECT: the KEY line sits below the 16 pattern slots, not through the last row; an
      unsaved project says UNSAVED on the second line.
- [ ] MACHINE: the kit reads as two lines of four; SETUP's three hint lines have no '?' in them.
- [ ] The dim greys (2-4) used for empty steps and bar backgrounds are visible on the OLED but
      clearly darker than text. The emulator cannot tell us this.

## Arrow keys (one navigation model)

The arrows follow Enaga's own menu and SETTINGS page: UP and DOWN move through the rows on
screen, LEFT and RIGHT move sideways (the page, wherever the screen keys flip pages), OK picks
and CLR goes back. F1 + an arrow is the mode's second meaning. The host suite pins every mapping
below (`tests/test_ui.cpp`, "the arrow keys"); what it cannot tell is whether it feels natural.

- [ ] GRID and EDIT: UP and DOWN walk the selected track without playing it; LEFT and RIGHT flip
      the step page (GRID) or SRC/FLTR/AMP/FX (EDIT). F1 + UP copies, F1 + DOWN pastes.
- [ ] Step page: LEFT and RIGHT walk to the previous and next step, wrapping at the track's
      length. CLR closes the page and erases nothing.
- [ ] KEYS: LEFT and RIGHT are the octave. PERFORM: LEFT and RIGHT pick the macro; UP and DOWN
      still walk the mutation.
- [ ] MACHINE (F1 + EDIT): the eight machines are a list; UP and DOWN change the track's machine
      at once, LEFT and RIGHT walk the track. EDIT's screen says `FUNC+EDIT: MACHINE`.
- [ ] RESAMPLE: UP and DOWN pick the length, LEFT and RIGHT where the take goes.
- [ ] PROJECT: the arrows move a frame over the 16 slots and OK queues the framed one; a STEP key
      queues and moves the frame there. The FUNC menu toggles the chain and reloads the sample.
      On SETUP, CLR goes back.
- [ ] Notices (an octave, a scene captured, `OK AGAIN TO RELOAD`) appear on the second line in
      every mode, not only in GRID.

## F1 + F5 restart, F1 + F6 stop and reset

- [ ] Playing, F1 + F5: every track jumps back to step 1 on the beat and keeps playing; held
      notes ring on. The screen says RESTART.
- [ ] F1 + F6 while playing with a pattern change queued: it stops, the tails are cut at once
      (no second press needed), the queued change is gone, and the next PLAY starts at step 1.
- [ ] With CLOCK SRC on MIDI or SYNC both show EXT CLOCK and change nothing.

## The live mutes and scene A/B are saved with the project

- [ ] Mute two tracks, pick scene 7 into slot B (hold scene B + STEP 7), save, power off, power
      on. The project comes back with the same two tracks muted and scene 7 in slot B.
- [ ] A project saved by an older build opens with everything unmuted on scenes 1 and 2.

## Hold STEP + VALUE sets the note

- [ ] Hold a STEP key and turn VALUE: the step's note moves (play it to hear), the tempo does
      not, and letting go leaves the trig as it was. F1 + CLR undoes the whole turn at once.

## MUTATE: OK keeps, CLR reverts; undo works in PERFORM

- [ ] Press MUTATE, then CLR: the track sounds as before and the screen says REVERTED. Press it
      again, then OK: it stays (KEPT). Works the same in GRID, EDIT and PERFORM.
- [ ] MUTATE, then F1 + CLR (undo), then plain CLR: CLR does what CLR does in that mode and does
      NOT say REVERTED or undo a second edit.
- [ ] In PERFORM, F1 + CLR undoes and F1 + OK redoes. Plain CLR still clears the selected macro.
- [ ] Hold scene A and press CLR: scene A's slot is erased (SCENE ERASED). This used to be F1 + OK.

## PROJECT: the CHAIN and PATTERN pages

The screen keys in PROJECT walk its pages: the slots, CHAIN, PATTERN, SETUP and PROJECTS (they
were the octave keys until the controls concept, docs/CONTROLS.md).

- [ ] CHAIN: OK on an empty chain adds the playing pattern. STEP 3 makes it A03, VALUE sets the
      repeats (X1 to X16), OK adds a copy below, LEFT and RIGHT step its pattern, UP and DOWN
      move between entries, F1 + LEFT removes one. Build A01 X2, A03 X1 and turn it on with
      the FUNC menu's CHAIN row: it plays A01 twice, A03 once, and loops. The PLAY marker follows.
- [ ] Save and reopen: the chain comes back, and so does whether it was on. With it on, the saved
      pattern plays its pass and then the chain carries on from its first entry (it used to
      reopen off, whatever the file said).
- [ ] While the chain plays, add an entry ABOVE the playing one (cursor up, OK): the chain carries
      on with the entry it was playing and the PLAY marker moves with it. Remove the playing
      entry (F1 + LEFT): the pattern finishes its pass and the NEXT entry plays, none skipped.
- [ ] Queue a pattern (END), then STOP and PLAY before it lands: it lands as the transport
      starts, not as many bars late as the clock had run.
- [ ] PATTERN: LEN at 8 makes a queued change land every 8 steps; SWG moves the swing exactly as
      VALUE pushed + turned does. F1 + CLR in GRID undoes a LEN change.

## KEYS: a key holds its note

- [ ] KEYS on a VA POLY track with DEC up high: hold three STEP keys for a chord, let one go. That
      note fades with its release and the other two keep sounding until let go.
- [ ] The same on a SAMPLE track with a long sample and DEC at hold: the sample plays while the
      key is down and tails out when it comes up.
- [ ] Hold a key, change octave, let go: the note stops (it does not hang).
- [ ] Hold a key and press F4 to leave KEYS: the note stops.
- [ ] Drum tracks behave as before: letting go changes nothing.
- [ ] The same on a GRAIN track with DEC at 127 (hold): the cloud thins out when the key comes up
      (it used to sound until STOP).

## PROJECT: undo and redo, and where reload and new went

- [ ] In PROJECT, F1 + CLR undoes and F1 + OK redoes (try a LEN change on the PATTERN page).
- [ ] F1 + LEFT once shows AGAIN TO RELOAD and changes nothing; twice reloads the project from the
      card. F1 + RIGHT twice starts a new project. A single press of either never loses work.

## KIT: the kit browser (FUNC+EDIT twice)

- [ ] FUNC+EDIT opens MACHINE, whose last line says `FUNC+EDIT AGAIN: KITS`. FUNC+EDIT again opens
      KIT: `KITS 01-08    OK LOAD`, eight rows, and on a new card every row reads `EMPTY` after a
      moment. CLR (or FUNC+EDIT) goes back to MACHINE. The layout reads cleanly, no overlaps.
- [ ] Turn a few knobs and switch a machine, then FUNC+DOWN on slot 1: `SAVING KIT`, the row reads
      `KIT` and the two lines below show the eight machines. The card has
      `/ENAGA/DATA/STRATA/KITS/KIT_01.BIN`.
- [ ] Change the sound, then with the beat PLAYING press OK on slot 1: `OK AGAIN TO LOAD`, nothing
      changes. OK again: the stored sound comes back and the beat does not stop. Listen for a
      click or a dropout at the recall, and read LOAD/PEAK straight after it.
- [ ] FUNC+DOWN on slot 1 again says `AGAIN TO OVERWRITE`; only the second press writes.
- [ ] A kit with a sample on track 8: recall a kit with the SAME sample while playing -- the loop
      does not stop. Recall one naming another WAV -- the track reloads (PROJECT shows `.` then `O`).
- [ ] After a recall PROJECT says `UNSAVED`; FUNC+HOME, OK saves it, and the reopened project
      has the recalled kit.
- [ ] Kits are shared: start a NEW project (the FUNC menu's NEW PROJECT, OK twice) and slot 1 is still there.

## SAMPLES: the sample browser (OK on a SMP or GRN track)

- [ ] OK in EDIT or MACHINE on a SMP or GRN track opens the browser in the track's folder (or
      `/SAMPLES`), cursor on its file; on any other machine OK does nothing there.
- [ ] Only WAVs (any case of `.wav`) and folders are listed, sorted folders first; macOS `._`
      files and `.Trashes` are not. A folder of 300 files shows 256 and `+44`.
- [ ] OK loads with the transport running; repeated OKs on different files never wedge the
      loader, and the last one picked is what plays. Pulling the card mid-listing: `NO CARD` or
      an error, and the browser still answers CLR.
- [ ] `/SAMPLES` missing on the card: an error line (`NOT FOUND`), CLR still reaches `RECORD/`.
      (The host fake answers NOT FOUND for a missing folder; Enaga's real answer is unverified.)
- [ ] A long name is cut to the line, and `_` shows as `-`.

## RESAMPLE from the audio input (B: source, C: level)

- [ ] Every route Enaga offers records what is plugged into it: LINE stereo keeps L and R apart,
      LINE L puts INPUT L on both sides, HI-Z and DYN MIC take their jacks, MIC needs headphones.
      Enaga's input routes "have not been checked route by route" (SPEC), so a route that fails
      is an Enaga report, with the route and the level.
- [ ] The meter tracks the input and reads `CLIP` at full scale; the recorded file is the input
      at the level shown, nothing of STRATA's own output in it.
- [ ] Turning B or C during a take does not change the take's source (it is fixed at the arm),
      and the 10 ms gap of a route change is not in a take armed afterwards.
- [ ] After leaving STRATA, Enaga's own input setting is back to LINE, NORMAL.

## Factory content: audition list (2026-11-22)

A first boot with no card and a NEW project (PROJECT, F1 + RIGHT twice) now hold the same thing:
core/factory.hpp. Chosen from the references in docs/SOUND.md and balanced by rendering on the
host (`make -C host render`, which writes the four patterns and A01's eight tracks soloed to
`build/host/render/`), but **nobody has listened to it**. Levels measured there, A01, RMS over
8 bars: kick -18.7 dBFS, bass -26.0, clap -27.8, hats -28.1 / -26.8, chord -28.2 (with its
sends), rim -35.6, blips -37.7; the mix -17.0 RMS with peaks at the limiter.

- [ ] **A01 GROOVE**, PLAY on a fresh start. The kick should be a tight 909-ish thump tuned to G
      (about 49 Hz, the fifth of C), the clap a three-burst 909 clap, the hats bright (closed
      band-pass at 3.8 kHz, open at 3.5 kHz, where the old default's 1.2 kHz was dull), the
      open hat cut short by the next closed hat. The bass rolls on the offbeats with one slide
      up to Eb; the Cm7 stab sits in a dotted-1/8 ping-pong and the reverb. A little swing (54).
- [ ] Is the **bass loud enough against the kick**, and does the sidechain duck pump it musically
      rather than chop it? It renders 7 dB under the kick, with LVL at the top and the fader at
      1.25.
- [ ] **A02 GROOVE+**: a kick pickup on step 15 every fourth bar, a hat roll into every bar, the
      rim on a 12-step loop (it drifts against the bar for three bars), Cm7 then Abmaj7, blips
      on a 7-step loop. Hold FILL: an extra clap on step 16.
- [ ] **A03 BREAK**: no kick or bass; Cm7 and Abmaj7 with long tails into the delay and reverb, a
      rim ticking at 35% probability. Up to six chord voices ring here, so read LOAD on it.
- [ ] **A04 PEAK**: 16th hats with ratchets, an acid line with slides and accents, the 5TH OCT rave
      stab. Hold FILL for the kick and clap rolls. The highest-load factory pattern: read LOAD
      and MAX here.
- [ ] **A05 PAD**: four held chords, a bar each, swelling in and ringing into the next, over a soft
      beat. The pad is the pattern's locks; the kit's chord is still A01's stab. Up to six chord
      voices sound all the time, so read LOAD and MAX here too.
- [ ] **The chain**: the FUNC menu's CHAIN row (FUNC+HOME) turns it on. A01 x2, A02 x2, A03, A04 x2, A02, then loops.
      It ships OFF on purpose, so PLAY on a fresh project jams on A01 rather than walking off.
- [ ] **PERFORM, VALUE**: morphs from the kit (scene 1) to DUB (scene 2): chord, rim and blips into
      the sends with longer tails. Scenes 3 (DARK: filters close) and 4 (TENSION: the acid opens,
      hats and kick tighten) are there to assign to A or B.
- [ ] **PERFORM, B-D**: B ACID (bass cutoff, resonance, envelope), C DUB (chord and rim into the
      delay and reverb), D DRIVE (kick longer and driven, both hats longer). Each must be audible
      from the first detent, with no jump. A is empty on purpose: pad pressure bends macro A.
- [ ] **A macro at 0 lets go, and sweeps from where the sound is** (2026-11-22). With ACID at 0,
      turn BASS CUT in EDIT: it is heard. Turn ACID up: the cutoff opens from where you left it,
      with no jump. Back to 0: your value is back. The same with the morph: morph fully to DUB,
      then turn C up one detent: the chord's delay send must not drop.
- [ ] **Assigning with the macro knob up** (STEP in PERFORM while the macro is not at 0): the
      parameter moves at once to where the knob puts it -- the macro taking effect. The EDIT
      value is not rewritten, and back at 0 the parameter is where it was.
- [ ] **A project saved before this change** with macros may play them differently: a resting
      macro used to hold its targets at `low`, and a moving one swept from `low`; now both start
      from the kit value. Re-check any project you saved with macros before 2026-11-22.
- [ ] **Mute states** 1-8: all, no kick, drums only, no hats, no bass, kick + bass, melodic parts
      only, the bare groove without rim and blips.
- [ ] Anything that sounds wrong, say which and how: each value has its arithmetic in a comment
      in core/factory.cpp, so a change is one number.

