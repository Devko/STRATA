# STRATA controls concept

The reference the key handlers in `app/app.hpp` (and its `app/ui_*.hpp` parts) follow. It starts from the SmplTrek's panel as
it is printed and laid out, not from a list of features, so that each button does the same
kind of thing in every mode. [CHEATSHEET.md](CHEATSHEET.md) lists every gesture;
this file says why they are where they are. Agreed with Roland on 2026-11-22, after the first
evening on the unit showed that the old key names and positions were the hardest part of
using STRATA.

Change a gesture here first, then in the code, the cheatsheet, the manual and
`docs/UNIT_CHECKLIST.md`.

## The panel

```
                 [   SCREEN   ]        (A) (B)
                 [SEL<]   [SEL>]       (C) (D)   (VALUE)
   (UP)
(LEFT)(RIGHT)  [FUNC][REC ][PROJECT][HOME]   CLR  OK
  (DOWN)       [PLAY][STOP][MIXER  ][EDIT]

  STEP  1  2  3  4  5  6  7  8
  STEP  9 10 11 12 13 14 15 16

[OCT<]  TRK1 TRK2 TRK3 TRK4 TRK5 TRK6 TRK7  [OCT>]      upper pads: performance
     TRK8 TRK9 TRK10 GLB1 GLB2 GLB3 USB  EXT            lower pads: tracks 1-8
```

Positions are from the panel drawing in the emulator (smpltrek-re `emulator/ui/stock.js`,
measured control centres) and Enaga's `docs/hardware.md`. Three pairs of left/right keys, named
by where they are:

| Name here | Where | Code name |
| --- | --- | --- |
| **arrow pad** | the four-way pad, far left | UP DOWN LEFT RIGHT |
| **screen keys** | the pair just under the screen | SELECT_LEFT / SELECT_RIGHT |
| **pad-row keys** | at the two ends of the upper pad row, printed as octave | OCTAVE_LEFT / OCTAVE_RIGHT |

Each function key has a second label printed under it: REC "sampling", PROJECT "pool",
HOME "system", PLAY "◀◀", STOP "▶▶", MIXER "fx", EDIT "rec src".

## The rules

1. **One job per group, in every mode.**
   - **Function block:** plain = go to that screen (or transport); **FUNC + key = its printed
     second label.**
   - **Arrow pad:** the cursor on the screen. UP/DOWN rows, LEFT/RIGHT sideways. OK picks, CLR
     goes back (or clears, where there is something selected to clear).
   - **Screen keys:** the screen's pages. EDIT SRC FLTR AMP FX LFO ARP ENV OSC SCAN (SCAN on
     the WAVE machines only; a page a machine lacks is stepped over); MIXER's nineteen;
     PROJECT, CHAIN, PATTERN, SETUP, PROJECTS; the step page's field pages; SAMPLES' and KIT's pages of
     eight. On GRID and KEYS, where A-D turn the current EDIT page, they turn that page. The
     one-page screens give them their one sideways setting: MACHINE and SOUNDS the track,
     RESAMPLE the destination, PERFORM the mutate amount.
   - **Pad-row keys:** which sixteen the STEP keys mean. Step page 1-4 (GRID, KEYS, PERFORM,
     EDIT, MIXER, MACHINE, RESAMPLE), the bank A-H in PROJECT and CHAIN. Both go round: past
     page 4 is page 1, as LEFT/RIGHT walk the step page on GRID. Where the STEP keys
     always mean the same sixteen things (KIT, SAMPLES, SOUNDS, the menu) they do nothing.
     FUNC + pad-row key = the selected track's length, on every screen but the menu.
   - **Knobs A-D:** the four values on screen. Push = that value's switch. With a STEP held, turn
     = lock, push = remove the lock; with FUNC too, turn = the lock's random range, push = its
     slide (below, "Random ranges and slides"). In PERFORM, push = MACRO LEARN for that macro: EDIT opens,
     and every knob turned there goes on the macro, its start as the macro's 0 and where it was
     left as its 127 (learn by turning, D11). LEARN goes on onto MIXER, where LOW, MID and HI
     on the EQ page are learnt the same way: the kill EQ's bands. OK, CLR, HOME or any screen
     but EDIT and MIXER ends it (`B: 3 PARAMS`) and the knobs snap back. A push there takes a
     parameter the macro holds off it (on the EQ page; any other push there is the kill). TUN's switch (PIT on SAMPLE and GRAIN), on EDIT, GRID and
     KEYS, is NOTE view: the value reads as a note, where TUN puts C-4 (`G-4`, `A-1+` when the
     byte is between notes), and a click is a semitone; push again for the number and one count
     a click. On at every start, one setting for all tracks (2026-11-23, core/tuning.hpp).
     On the LFO page, RAT's switch (push B, on EDIT, GRID and KEYS) is LFO TRIG: FREE, RETRIG,
     ONE-SHOT and round (`LFO TRIG RETRIG`); the kit's, per track (S2, 2026-11-25).
     **MOD slots** (two per track, the kit's; backlog Phase 5): on the LFO page push C (DEP) for
     slot 1 or D (DST) for slot 2 (EDIT opens from GRID or KEYS), push it again to walk the
     source LFO ENV VEL PRS RND OFF, then turn the knob the slot should move on any EDIT page of
     the track: where it is left, less where it started, is the depth, and it snaps back (learn
     by turning, as D11). OK, CLR or leaving EDIT ends it (`M1 VEL>CUT +40`); no knob turned keeps
     the slot's knob and depth with the new source, OFF clears it. Not on the LFO's or the
     arp's own knobs (`MOD: NOT LFO OR ARP`). No routing screen: the LFO page lists the two
     slots and every EDIT bar a slot moves has its range drawn under it (the font has no `~`).
     On the ARP page the four switches are the arp's kit settings (core/arp.hpp), each pushed
     once round its list and back to OFF (`ARP RHY E5/16`): push A (MOD) VAR, push B (RAT)
     RHYTHM, push C (OCT) ACCENT, push D (GAT) RATCHET. Per track, the kit's, as LFO TRIG; on
     EDIT, GRID and KEYS. OK on the ARP page opens ARPS, the factory arps, as OK on the sound
     pages opens SOUNDS (same keys, AS WAS first).
     On PROJECTS, where only A turns, push B renames and push C deletes the project under the
     cursor (delete then asks for OK twice). SOUNDS does the same for your own saved sounds
     (core/usersounds.hpp); on a factory sound or in ARPS they say `YOUR OWN SOUNDS ONLY`.
   - **VALUE is the crossfader.** Push VALUE + STEP picks what it fades: an effect, the morph
     (13) or the looper (16), on every screen (the performance mixer, below). With nothing
     picked it is the tempo (PERFORM: the morph; CHAIN/PATTERN: the field under the cursor).
     Push + turn = swing. FUNC + turn = **VariSpeed** (below): the tempo with the pitch tied to
     it. It was the DJ filter until D1 (2026-11-25), then free. **FUNC + push + turn = the tempo
     nudge** (2026-11-28, core/taptempo.hpp): each detent plays 4% faster or slower for 250 ms,
     then the set tempo comes back (`NUDGE FASTER`, `NUDGE SLOWER`); not saved, `EXT CLOCK` under
     an outside clock.
     **A press of VALUE alone** (let go within half a second, with nothing turned or pressed
     meanwhile) resets what it is on to 0: a picked effect's amount, the looper's mix, the morph
     (PERFORM, or slot 13 picked). Never swing or a field, and nothing while a STEP is held.
     With FUNC: a cut of the picked effect, or with none picked VariSpeed back to 0.
     **Where VALUE is the tempo, the press is tap tempo** (2026-11-28): from the second tap the
     tempo follows the last four taps (`TAP TEMPO`, then `TAP 128.0`), set and saved as a turn
     sets it; a gap over 2 s or a tap at half or twice the pace starts a new count.
   - **STEP keys:** the sixteen of whatever the pad-row keys chose. **On every playing screen,
     PERFORM and KEYS included, they are steps** (D12, 2026-11-25): tap, hold + knob, hold + OK
     work as on GRID. MUTE MODE (STEP 1-8 the mute states), a held scene pad, push VALUE (the
     performance mixer) and the list screens (PROJECT, CHAIN, KIT, SAMPLES, SOUNDS, the menu)
     keep theirs.
   - **Lower pads:** the eight tracks: select and play. FUNC + pad = mute on the next bar.
   - **Upper pads:** the seven performance tools, the same in every mode but KEYS.
   - **KEYS is the exception: both pad rows are a piano**, being the pressure-sensitive ones and
     laid out like a keyboard (Roland, 2026-11-23, after playing KEYS on the STEP keys). Lower
     pads C D E F G A B C, upper pads C# D# above them, F# G# A#; the upper pad between E and F
     is octave down and the one after B octave up. A pad plays at its own velocity, holds its
     note while down, and its pressure bends macro A. The track is UP/DOWN, as in KEYS anyway.
     FUNC + a pad keeps its usual job (mute on the bar, the performance moves). The pads light as
     a keyboard: the root bright in the track's colour, the scale's other notes dim, the rest
     dark, a held pad white, the two octave pads dim white. The octave is LEFT/RIGHT and the two
     octave pads; the STEP keys are steps (D12), so you play on the pads and edit at once.
2. **FUNC + something is either the key's printed second label or a performance move.** The
   rare setup actions are in the FUNC menu, named on the screen, not on chords that change from
   page to page.
3. **What is lit says where you are.** A mode key is green on its own screen and red on its FUNC
   screen. The performance pads are always lit, dim, in their own colours, and bright while held
   or on.
4. **The print on the upper pads is wrong for STRATA, so the screen and the lights correct it.**
   The upper row is printed TRK 1-7. Roland preferred tracks on the lower row and tools above, so
   the pads stay as they are and every performance pad names itself on the screen when pressed;
   FUNC held alone shows the whole row.
5. **A step that cannot be undone asks for a second press, always in the same words** (U3,
   2026-11-28): `<KEY> AGAIN: <WHAT>`, the key to press again and what it will do, within 2 s:
   `OK AGAIN: DELETE`, `OK AGAIN: OPEN`, `CLR AGAIN: REMOVE`, `DOWN AGAIN: REPLACE`, `A AGAIN:
   HEAR`. A delete that starts on a push asks `OK TWICE: DELETE` first. tools/check_rules.sh
   holds the wording.

## Function block

| Key | Plain | FUNC + key (printed second label) | Lit |
| --- | --- | --- | --- |
| FUNC | modifier | — | green while held |
| REC | live record on/off | RESAMPLE ("sampling"); again JAM REC on/off | red recording, green on RESAMPLE; a dim red blink while JAM REC records |
| PROJECT | PROJECT: banks, patterns, chain, card. Opens on the release; held, STEP n queues slot n from any screen (below) | SAMPLES, the selected track's browser ("pool") | green PROJECT, red SAMPLES |
| HOME | GRID, always. Double-tap (within 0.4 s, on GRID) = KEYS | **FUNC menu** ("system"); again closes it | green GRID, amber KEYS, red menu |
| PLAY | play | restart from step 1 ("◀◀") | green running |
| STOP | stop; again cuts the tails | reset: back to step 1, queue dropped, chain to its start ("▶▶") | red stopped |
| MIXER | MIXER | PERFORM ("fx") | green MIXER, red PERFORM |
| EDIT | EDIT | MACHINE; again KIT ("rec src" is the nearest: the sound's source) | green EDIT, red MACHINE/KIT |
| OK | pick / confirm. On PROJECT's slots a tap queues, a hold (0.5 s) opens the pattern menu: COPY, PASTE, CLEAR, DOUBLE (doubles every track up to 64 steps, copying the steps) | redo | lit when redo has somewhere to go |
| CLR | back / clear | undo | lit when undo has somewhere to go |

## The FUNC menu (FUNC + HOME)

A list over whatever screen was up, of the project-wide rows only: the selected track's rows are
in GRID's track menu (below). UP/DOWN or A scroll, OK runs the row, LEFT/RIGHT or B change
a setting row, CLR or FUNC + HOME closes it and returns to the screen it opened over. STEP
keys, screen keys and pad-row keys do nothing in it. FUNC + CLR / OK stay undo and redo.
The rows run by how often a jam reaches for them: the live toggles first, the file actions at
the bottom. The cursor starts on SAVE every time, so **FUNC + HOME, OK saves** and
**FUNC + HOME, DOWN, OK starts or stops JAM REC**; the list wraps, so the file rows are a few UPs
away. JAM REC has a key of its own as well: **FUNC + REC twice** (RESAMPLE, then again), as
FUNC + EDIT twice is KIT.

| Row | What OK does | Value (LEFT/RIGHT, B, or OK steps) |
| --- | --- | --- |
| SAVE | saves the project, closes the menu | |
| JAM REC | starts recording the finished master to `/RECORD/JAM_<n>.WAV` and closes the menu (`JAM REC JAM 3`); OK again stops it (`JAM STOPPING`, then `SAVED JAM 3` once the file is closed). FUNC + HOME, DOWN, OK. FUNC + REC on RESAMPLE does the same and stays on RESAMPLE, whose last line shows the time and `FUNC+REC: STOP JAM` (`JAM OFF` when none runs) | `OFF`, or how long it has run, `4:07`, `1:02:33` |
| ARR REC | starts a take (`ARR REC ON`, or `ARR: PRESS PLAY` with the transport stopped) and closes the menu; OK again ends it (`ARR 23 SECTIONS`). FUNC + HOME, DOWN, DOWN, OK. See ARR REC below | `OFF`, `ON`, or `ON 7`: the sections written so far |
| CLICK | | the metronome, `ON` / `OFF`: a click on every beat while the transport runs, higher on a bar's first. Not saved |
| COUNT-IN | | `OFF`, `1 BAR`, `2 BAR`: a PLAY from stop with REC on clicks that many bars first (`COUNT-IN 4` .. `1`), then the pattern starts on step 1. Internal clock only. Not saved |
| REC MODE | | `REPLACE` (default) / `ADD`: what a note recorded onto a chord track's step does. REPLACE: the first note of a pass on a step takes it over (its notes and chord type go), the rest of that pass's notes join it. ADD: every note joins the step's notes. Saved with the project |
| QUANTIZE | | record quantize, by 5 |
| AUTO-FILL | | OFF 4 8 16 bars |
| FX GLIDE | | how long a button's jump in the performance mixer rides: `OFF`, `1/16`, `1 BEAT` (default), `1 BAR`, at the tempo. Not saved |
| LOOP LEN | | the performance mixer's looper: `4 BAR` (1 2 4 8), what push VALUE + STEP 16 records |
| CHAIN | | ON / OFF |
| CHAIN END | | SONG (play once) / LOOP |
| ARR UNDO | swaps the chain the last ARR REC take replaced back in (`ARR UNDONE`); OK again brings the take back (`ARR REDONE`). `NO ARR TO UNDO` with none, or during a take | |
| SAVE AS | PROJECTS page with the name entry open | |
| OPEN PROJECT | PROJECTS page; OK twice there opens | |
| KITS | the kit browser | |
| NEW PROJECT | OK twice within 2 s: a new, empty project (the factory sounds, every pan centred, no steps, scenes or macros) | |
| RELOAD PROJECT | OK twice within 2 s: read the project back from the card | |
| CLEAR PATTERN | OK twice within 2 s: every track's trigs and locks gone, one undo level (FUNC + CLR). The second-last row: FUNC + HOME, UP, UP | |
| LOAD DEMO | OK twice within 2 s: the factory demo project (the boot beat), under a new name. The last row: FUNC + HOME, UP | |

Moved here from: FUNC + PROJECT twice (save), PROJECT's FUNC + UP/DOWN/LEFT/RIGHT (chain,
reload, new), PROJECTS' FUNC + DOWN (save as, still there too), CHAIN's FUNC +
RIGHT (song), PATTERN's FUNC + UP (chain), PERFORM's FUNC + UP/DOWN (auto-fill), FUNC + EDIT
twice (kits, still there too). Sample reload (from PROJECT's FUNC + DOWN), the track speed (from
FUNC + screen keys), TRACK PLAY and FREEZE were rows here too, until 2026-11-28 moved them to the
track menu with the rest of the selected track's settings. CHAIN's FUNC + LEFT twice (remove an
entry) became CLR twice: CLR is what clears.

## FUNC chords that stay

Printed labels (above), and the moves made while playing:

| Chord | Does |
| --- | --- |
| FUNC + CLR / OK | undo / redo |
| FUNC + UP / DOWN | copy / paste (editing screens; on PROJECT's slots a whole pattern, the paste twice). Card lists: read again / store here (KIT save, PROJECTS save as). SOUNDS: copy the track's sound (the PASTE row under AS WAS loads it, on any track) / store it as your own, named (on one of your own: replace it). ARPS: `ARPS: NOT SAVED`. FUNC + LEFT twice on PROJECT's slots clears the pattern under the cursor |
| FUNC + lower pad | mute that track on the next bar (at once while stopped). Several pads before the bar all land; the same pad twice takes it back. In PERFORM: that track in or out of LIVE RND (below) |
| FUNC + FILL / REPEAT / THROW | note repeat on/off / pitched-down repeat / reverb throw |
| FUNC + push A / B / C | kill LOW / MID / HI |
| FUNC + push D | kill all three bands; with all three out, all back (`KILL ALL`, `ALL BANDS BACK`). On every screen, PERFORM included |
| FUNC + pad-row key | selected track's length; held, it repeats (after 0.4 s, 20 steps a second) |
| FUNC + STEP | trigless step, on every playing screen, PERFORM included |
| FUNC + A / B / C on GRID | Euclidean fill |
| FUNC + MUTATE, held half a second | **LOCK**: store the live set as the playing pattern's lock (`LOCK A01 STORED`); again later overwrites it |
| FUNC + turn A / B in PERFORM | **REL ALL** / **LEN ALL** (below) |
| FUNC + turn C in PERFORM | **ENERGY**: from a sparse breakdown to the peak (below) |
| FUNC + turn D in PERFORM | **LIVE RND**: the unit (below) |
| FUNC + turn VALUE | **VariSpeed**: the tempo and every machine's pitch together, on every screen (below) |
| FUNC + MUTATE, tapped | Jump to that lock at once (`BACK TO LOCK A01`); tapped again, swap back to the state from just before the jump (`SWAPPED BACK A01`). No lock: `NO LOCK: HOLD STORES` |

Retired 2026-11-25 (backlog D1, D5; CHANGELOG "Gestures that moved"): FUNC + VALUE (DJ filter,
now slots 1-2), FUNC + hold push D (tape stop, slot 8) and FUNC + hold MUTATE (delay freeze,
slot 5). Each now only names its slot (`TAPE STOP: SLOT 8`, `FREEZE: SLOT 5`), apart from
FUNC + MUTATE, which is LOCK since the next change (below), FUNC + push VALUE + turn, the tempo
nudge since 2026-11-28, and FUNC + push D, the kill for all bands since 2026-11-28 (D2); FUNC +
push D with a STEP held toggles the slide of that step's D lock, as FUNC + push A-C do theirs.

## LOCK (FUNC + MUTATE)

Backlog D3, D4, D13, D14 (2026-11-25), `core/lockstore.hpp`. Store a moment you like, change a
lot, and jump back in one tap; tap again to compare with where you were.

| Holds | Does not hold |
| --- | --- |
| The playing pattern whole: steps, p-locks, conditions, lengths, swing | Other patterns, the bank, the chain |
| Every track's sound bytes (every EDIT page) and fader | The samples, the machines: a lock taken before a machine switch restores the parameters only |
| The send and master bytes | The kill EQ's switches, the tempo (not STRATA's under an outside clock) |
| The mutes (D4) | The eight stored mute states |
| The morph position, the four macro positions, REL ALL, LEN ALL, LIVE RND and ENERGY | The scenes and the macro setup |

- **One lock per pattern (D14), in RAM only**: not saved, and a project load or NEW clears them.
- **At once (D3)**, on the next audio block. The performance mixer is not in a lock: the jump
  rides the picked effect to 0 over FX GLIDE (it stays picked) and a loop plays on.
- **A second tap swaps back (D13)** to the state captured at the jump, sound included: undo
  covers only patterns (R1), so it could not. Any later tap jumps to the lock again. A pattern
  change, or a new store, ends the swap-back.
- **The kit is shared by every pattern (R2)**: jumping to A01's lock puts A01's sound back for
  every pattern. The steps are per pattern; the sound comes from whichever lock you jumped to.
- **A MUTATE waiting for OK or CLR is discarded** by a jump or swap: its pattern is gone.
- On screen, in the top line: `L` while the playing pattern has a lock, `L+` once
  anything it holds has changed (the font has no `*`). The MUTATE pad glows half bright while
  the playing pattern has a lock.

## REL ALL and LEN ALL (FUNC + turn A / B in PERFORM)

CyDrums' P4 (backlog Phase 5 item 5, 2026-11-25), `core/moveall.hpp`. Two global controls in
the Moves layer, beside the macros and the morph, on the one screen that is that layer's home.
FUNC + turn on PERFORM's A and B only duplicated the plain turn (the macro) before, so no gesture
changed meaning. C and D with FUNC are ENERGY and LIVE RND (below).

| Control | Moves | Range | Says |
| --- | --- | --- | --- |
| REL ALL, FUNC + A | Every track's release bytes by the same count: AMP's DEC on every machine, and ENV's REL on the machines that read it. A step's lock on them too | -64 .. +64 counts | `REL ALL -12` |
| LEN ALL, FUNC + B | Every sequenced note's gate, x 2^(n / 32) | 25% .. 400% | `LEN ALL 71%` |

- **Rest is 0, and 0 is a detent**: a turn that would cross it stops on it. At rest the sound is
  the programmed one to the bit (the rule the macros and the morph keep).
- **Two counts a click**, like the macros. Not saved, not undone; a LOCK holds both (D4), and a
  jump or swap puts them where the lock had them.
- **On screen**: PERFORM's line under the macros reads `FUNC+A/B REL/LEN ALL` at rest and
  `REL -12 LEN 71%` away from it; the live strip adds `RL-` / `RL+` and `LN-` / `LN+`.
- **While FUNC is held** in PERFORM the four gauges are this layer (U1, 2026-11-28): `REL`,
  `LEN`, `NRG` and `RND` under the letters, each gauge with its value (`-12`, `71`, `+40`, `4`,
  `OFF`), lit away from rest; the hint line gives way to them. Let go, and the macros are back.
- **LEN ALL leaves a note with no gate alone** (a drum, a pad hit, a held key, the arpeggiator's
  own GATE): REL ALL is what shortens drums.

## LIVE RND (FUNC + D in PERFORM)

CyDrums' P1, the live half (backlog Phase 5; the controls concept: "the PATTERN page plus one
PERFORM knob gesture"), `core/liverand.hpp`. The pattern half is the track menu's PLAY. FUNC
+ turn D in PERFORM was a duplicate of the macro, so no gesture that did something changed
meaning. The armed set was on FUNC + push D until 2026-11-28 (D2), when that became the kill for
all bands on every screen.

| Gesture | Does | Says |
| --- | --- | --- |
| FUNC + turn D | The unit for every armed track: OFF, 1, 2, 4, 8, 16, one a click, stopping at both ends | `LIVE RND4 7 TRK`, `LIVE RND OFF` |
| FUNC + lower pad | That track out of the armed set, or back in. No mute, no track selected | `T2 RND OUT 6 TRK`, `T2 RND IN 7 TRK` |

- **The kick stays** (D3, 2026-11-28): turned on from OFF, LIVE RND arms every track but those
  playing KICK, so the floor holds while the rest jumps (`LIVE RND4 7 TRK` on the factory kit).
  Once a FUNC + lower pad (or a LOCK jump) has chosen the set, it stays as chosen.

- **What it plays**: exactly the track menu PLAY's RND mode of that size, from the same seeded draw (the
  pattern's seed, the track, the block, the pass), over the track's own mode while it is on.
  Blocks keep their steps in order and their place in the bar.
- **Recoverable**: the pattern is never written. OFF is the programmed order from the next slot;
  a track's own mode (REV, PING ...) comes back with it.
- **Not saved, not undone**, like REL ALL. A LOCK holds the unit and the armed set (D4); a jump
  or swap puts both back. Live recording lands a hit on the step that was sounding.
- **On screen**: PERFORM's line under the macros reads `LIVE RND4 FUNC+D`, or `REL -12 LEN 71% R4`
  with REL/LEN ALL away from rest; the track meters mark the armed tracks with a dot; the live
  strip adds `RN4`. The FUNC help's second PERFORM page lists both gestures.

## ENERGY (FUNC + turn C in PERFORM)

Roland's pick of the bold ideas (backlog, After 1.0 > Performance), `core/energy.hpp`. One
bipolar control from a sparse breakdown to the peak, in the Moves layer beside REL ALL and LEN
ALL. FUNC + turn C in PERFORM only duplicated the macro before, so no gesture changed meaning.

| Direction | Does |
| --- | --- |
| Down (-2 .. -64) | Every track drops its weakest hits first: 16ths, then 8ths, then beats, the one last, an accent counting for more. The tracks go in an order from their machines: HAT at once, then PERC, SNARE and SAMPLE (and MID), the MONO lines, the pads (POLY, WAVE POLY, FM4, GRAIN), and KICK from -40. At -64 the drums and lines are silent and the pads keep only their strongest hits |
| Up (+2 .. +64) | Empty steps may get a quiet ghost (velocity about 40-86) with the note, length and locks of the trig before it on that track, off-beats first: HAT fills most, then PERC, SNARE and SAMPLE, the MONO lines; KICK only above +32; pads and MID never. A track with no trig stays silent. A ghost is one note with the arp on, and lights no step or trig glow. Step probabilities lift towards 100 (a 50% step plays every pass at +64) |

- **Rest is 0, and 0 is a detent**, two counts a click, like REL ALL. At rest the part is the
  programmed one to the bit.
- **The same groove at the same position**: every draw is fixed per step (the pattern's seed,
  the track, the step), not per pass. Each click down drops one more set of hits and never a
  different set; turning back brings each one back where it was.
- **What it never touches**: the pattern (nothing is written, saved or undone), trigless steps,
  conditions and the PRE/NEI chain (a thinned hit still counts as a pass), the recorder. A kept
  step keeps its ratchet; a thinned one loses it whole.
- **A LOCK holds it**, and a jump or swap puts it back.
- **On screen**: the notice `ENERGY -20` / `ENERGY +12`, `EN-20` on PERFORM's status line (the
  `BAR` line while stopped), `EN-` / `EN+` in the live strip. The FUNC help's third PERFORM page
  lists it.

## VariSpeed (FUNC + turn VALUE)

CyDrums' P7 (backlog, After 1.0 > Performance), `core/varispeed.hpp`. VALUE is the tempo; FUNC +
VALUE is the tempo with every machine's pitch tied to it, like a turntable. On every screen, an
effect picked or not (a picked effect owns VALUE's plain turn, not FUNC's).

| Gesture | Does | Says |
| --- | --- | --- |
| FUNC + turn VALUE | A tenth of a semitone a click, -12.0 .. +12.0; the clock runs at the project's tempo times 2^(n/12), and every machine plays n semitones off | `VARISPEED +1.2 140.3` |
| FUNC + tap VALUE, no effect picked | Back to rest at once | `VARISPEED 0 130.0` |

- **Rest is 0, and 0 is a detent**: a turn that would cross it stops on it. At rest the tempo and
  the sound are the project's to the bit.
- **The project keeps its tempo**: a save stores the unmoved tempo, and VALUE alone turns it with
  VariSpeed riding on top. A turn that would take the clock past 20-300 BPM stops where it fits,
  and a faster project tempo pulls VariSpeed back towards 0 rather than past 300.
- **What moves**: the clock (so the delay's division follows); the drums' base frequency and
  SAMPLE's and GRAIN's read rate; the synths' and FM4's pitch by semitones, as the LFO on TUN
  does. Held notes and ringing hits follow. Envelope times do not, and MID's notes go out as
  played.
- **Internal clock only** (Enaga owns the clock): under MIDI or SYNC the turn says `EXT CLOCK`,
  and a change of CLOCK SRC takes VariSpeed to rest (`VARISPEED 0 EXT CLOCK`).
- **Not saved, not undone, not in a LOCK**, like the tempo.
- **On screen**: the top bar shows the tempo the clock runs at; the live strip adds `VS+` / `VS-`.
  The FUNC help's FUNC + KNOB OR PAD page lists it.

## The track menu: GEN, EVOLVE and the track's settings (hold OK in GRID or KEYS)

Everything that belongs to the selected track. Roland, 2026-11-27: generate a drum or melodic
part, random but in the key, and let it mutate on its own every few loops. The track's SPEED,
PLAY, FREEZE and RELOAD SAMPLE joined it from the FUNC menu (usability review, 2026-11-28).
`core/generate.hpp`, `core/evolve.hpp`, `app/ui_trackmenu.hpp`. OK in GRID with nothing held did
nothing, so the hold costs no gesture; a tap says where the menu is (`HOLD OK: TRACK MENU`). KEYS
has the same hold and tap (usability review, 2026-11-28): GEN on a melodic track while playing
the pads. There the pads stay the piano, and the arrows are the menu's until CLR closes it
(`KEYS`).

| Row | Does | Says |
| --- | --- | --- |
| GENERATE | OK: a new part on the selected track, one undo level (CLR in GRID, or FUNC + CLR, takes it back) | `GEN T1 BOTH` |
| DENSITY | 0-100 in tens: about 2 hits a bar at 20, 6 at 50, 11 at 100 | |
| RANGE | 1-3 octaves above the track's own root (melodic tracks) | |
| LEAP | 0-100 in tens: how often the melody jumps 2-5 degrees instead of stepping | |
| WHAT | BOTH, RHYTHM (notes kept), NOTES (rhythm kept) | |
| EVOLVE | OFF, 1, 2, 4 or 8 loops: this track mutates on its own every that many passes | `EVOLVE T1 4 LOOPS` |
| MODE | ORBIT (every turn starts again from HOME) or DRIFT (the changes add up), every track | |
| MUTATION, AMOUNT | What EVOLVE plays: PERFORM's MUTATE setting and amount, the same two values. **MIX** (after NOTES) draws a different mutation each turn, and on the MUTATE pad each press | `MUTATION MIX` |
| KEEP | OK: what plays becomes HOME, EVOLVE stops | `KEPT: EVOLVE OFF` |
| HOME | OK: back to HOME (one undo level), EVOLVE stops | `HOME: EVOLVE OFF` |
| SPEED | the track's speed: 2, 3/2 (16th-note triplets), 1, 3/4, 1/2, 1/4, 1/8. The arrows stop at the ends, OK goes round. One undo level a change | `T1 LEN 16 X3/2` |
| PLAY | the track's play mode: FWD, REV, PING, RND1, RND2, RND4, RND8, RND16. Which step sounds in each slot; the pattern is not changed. One undo level a change | `T3 PLAY RND4` |
| FREEZE | OK: freeze the track (its loop from the next bar line), or unfreeze it; the menu stays open. LEFT/RIGHT: the loop's length. The row reads `1 BAR` (1 2 4 8), `WAIT` / `REC` while recording, `ON` frozen | `FREEZE ARMED`, `UNFROZEN` |
| RELOAD SAMPLE | OK: load the track's sample again, the way back from a `FAIL` or a card that was out | `LOADING`, `NO SAMPLE` |
| CAPTURE | OK: Track Launch's CAPTURE (LAUNCH, below) into the playing bank: what plays, launched lanes and all, becomes a pattern of its own in the first free slot, so a READ ONLY lane edits again. The row reads `TO A`, the bank; the menu stays open | `CAPTURING`, `CAPTURED A05` |

UP/DOWN or A pick the row, LEFT/RIGHT or B change it, OK runs (and steps a setting forward), CLR
closes. A lower pad still selects the track, so one menu walks GEN over a kit; the menu keeps its
row between openings. The list wraps: from GENERATE, UP reaches CAPTURE, RELOAD SAMPLE, FREEZE,
PLAY and SPEED. While the menu is open in GRID, notices show as a toast over the page tabs' line.

- **Drum or melodic** is the machine's: the VAs, WAVE, FM4, GRAIN, an unsliced SAMPLE and MID get
  notes; the drums and a sliced SAMPLE get a rhythm with accents and ghosts, their notes kept.
- **What GEN leaves**: locks, conditions, ratchets, micro-timing and probability, as Euclid does.
- **HOME** is taken when EVOLVE goes on, in RAM only. An edit made while a track evolves (a trig,
  a recorded note, a chord) goes into HOME at the next turn, so ORBIT never throws it away.
- **A pattern change**: in ORBIT the pattern left goes back to HOME; every evolving track then
  carries on from the new pattern. A load stops EVOLVE.
- **Saved** in the project: each track's EVOLVE, the mode, the mutation and the amount (PROJECT.BIN
  section 7). HOME is not: a project opens evolving from the pattern as it was saved, and a save
  writes what plays. A project from before this opens with nothing evolving.

## FEEL (the PATTERN page)

CyDrums' P8, `core/timing.hpp`. A pattern setting like swing, so its home is the PATTERN page
(PROJECT, screen key RIGHT twice), the row after SWG; **D** turns it from anywhere on the page,
and UP/DOWN or VALUE when `>` is on it. One line: `OFF`, `DRIFT 1-6`, `WAVE 1-6`.

- **DRIFT n**: each track on its own smooth random curve (a new aim every 64 ticks), at most n
  ticks of 96 PPQN early or late. **WAVE n**: one four-bar triangle for every track, 0 at bar 1,
  +n (late) at the end of bar 1, 0 at bar 3, -n at the end of bar 3.
- On top of swing and micro-timing; the steps never change, so OFF is the grid exactly. Seeded
  from the pattern and the bar position, so a pass plays the same after a reload.
- Saved in the pattern (an optional section; older files load OFF), undoable, held by a LOCK.

## Queue a pattern from any screen (hold PROJECT + STEP)

Hold PROJECT and press STEP n: slot n of the shown bank (the bank PROJECT last showed) is queued,
and the screen stays where it is. The toast names the slot and when it lands, `A05 QUEUED END`,
by the QUEUE mode on PROJECT's knob A. Several STEP keys while PROJECT is held queue each in
turn; the last one is what lands. PROJECT lights green while held.

- PROJECT opens on its release, and only when no STEP was pressed while it was held. With a STEP
  held as a modifier (a lock, the step page), PROJECT opens at once and ends the hold.
- The chord means the same on every screen, PROJECT's own pages and KIT included. FUNC +
  PROJECT stays SAMPLES.
- To queue from another bank, pick the bank on PROJECT first (the pad-row keys); the chord
  keeps using it.

## LAUNCH: one track from another pattern (PROJECT twice)

Track Launch, Roland's decisions of 2026-11-27, `core/launch.hpp`, `app/ui_launch.hpp`. Each track
is a lane that plays its own part of the playing pattern, or the same track's part of another
pattern of the project. PROJECT pressed twice quickly opens LAUNCH, PROJECT's last page (tab
`LNCH`): a row for each lane with its 16 slots of the shown bank.

| Key | Does | Says |
| --- | --- | --- |
| STEP 1-16 | launch that slot of the shown bank on the selected lane, or on every held lower pad. The same STEP again while it is queued drops it | `T3 < B07 AT END`, `LAUNCH OFF` |
| Lower pads | select the lane, held for a launch of several. **Silent** on this page; the first on a visit says why | `PADS PICK LANES` |
| UP / DOWN | the lane | |
| LEFT / RIGHT | the slot cursor | |
| OK | tap: launch the cursor's slot. Hold (0.5 s): **CAPTURE** | |
| CLR | the lane back to its own part, and only that: on a lane already on its own part it says so and the page stays (U2); the screen keys leave | `T3 < OWN`, `T3 PLAYS ITS OWN` |
| push B | PIN the lane: a pattern change leaves it playing what it plays | `T3 PINNED` |
| FUNC + STEP | a whole-pattern change, as on SLOT | |
| The bank keys | the shown bank | |

- **When it lands**: on the queue mode's boundary (END, 16, 8, 4, NOW), as a pattern change does;
  stopped, or NOW, at once. The launched part starts from its own step 1 on that tick, and keeps
  its own length and speed. `T3 B07` when it lands. A jump back, CLR on SLOT and STOP drop queued
  launches.
- **A pattern change** sends every lane back to its own part of the new pattern, unless pinned.
  A pinned lane on its own part keeps playing the pattern being left. Launches queued for the same
  boundary land after the change, over the new pattern.
- **Read only**: a launched lane plays another pattern's part, and edits to it (a trig, a lock,
  LEN, speed, GEN, recording ...) are refused with `T3 B07 READ ONLY`, which on GRID and KEYS
  takes turns with `HOLD OK: CAPTURE` (U2): the track menu's CAPTURE row makes it a pattern of its
  own. EVOLVE pauses on it.
  Sounds, mutes and the mixer are the track's as ever: only the steps come from elsewhere.
- **CAPTURE** (hold OK, or the CAPTURE row of the track menu in GRID and KEYS, which writes to the
  playing bank): what plays, every lane's part, is written to the first empty slot of the
  shown bank and plays from there with every lane on its own part, so it edits again.
  `CAPTURED A02`; `NO FREE SLOT IN BANK` or `LOCKS FULL` when it cannot. Not undoable; the source
  patterns are untouched. Master length, swing, key, FEEL and the seed come from the playing
  pattern.
- **LOCK** (FUNC + MUTATE) stores what you hear, launched parts included, as the playing
  pattern's own.
- **GRID** draws each lane's playing part; a launched lane's number is underlined, a queued one
  blinks, and the info line ends `<B07`.
- **Saved** in the project (PROJECT.BIN section 8): each lane's source and the pins. Not in the
  chain. A project from before this opens with every lane on its own part.

## ARR REC: a jam written into the chain (the FUNC menu)

The arrangement concept, Roland's decisions of 2026-11-28, `core/arrrec.hpp`. ARR REC records the
structure of a jam, not its audio (JAM REC does that; the two are independent and can run
together).

| Key | Does | Says |
| --- | --- | --- |
| FUNC + HOME, DOWN, DOWN, OK | ARR REC on: the chain is kept for ARR UNDO and emptied, and the chain stops being followed; the menu closes. Recording starts with the next PLAY, or at once with the transport running | `ARR: PRESS PLAY` / `ARR REC ON` |
| the same row, OK (or LEFT/RIGHT, B) | ARR REC off: the take is over | `ARR 23 SECTIONS` |
| STOP | ends the take too | `ARR 23 SECTIONS` |
| FUNC menu CHAIN ON | ends the take and plays what it wrote | `ARR 23 SECTIONS` |
| FUNC menu ARR UNDO, OK | the chain from before the take; again, the take | `ARR UNDONE` / `ARR REDONE` |

- **A pass at a time.** At the end of each pass of the playing pattern, the pass is compared with
  the chain's last entry: the same pattern with the same mute mask, scene on side B, morph side
  (past half way is B), ENERGY and tempo adds a repeat; anything else starts a new entry holding
  them as section fields (MUT CAP, SCN, MRP SNAP or SNAP A, NRG, BPM). A pass is what played at
  its start: a mute or an ENERGY move inside a pass shows from the next one, except in the pass's
  first 12 ticks (half a sixteenth, the queue's grace), which still count for it.
- **X16** is an entry's most: a seventeenth pass of the same starts a new entry of it. **64
  entries** end the take when a new one is needed (`ARR FULL 64 SECTIONS`).
- **Passes cut short** (a pattern change at once or on a 16 / 32 grid, a LOCK jump, STOP, ARR REC
  off, a take turned on mid-pass) are written when at least half of the pass was recorded.
- **A take that kept nothing** puts the old chain back (`ARR NOTHING KEPT`), with nothing to undo.
- **ARR UNDO** keeps one chain: the next take replaces it, and opening a project drops it.
- **Under an outside clock** the sections carry no BPM, so the song follows the clock it plays to.
- **While a take runs** the live strip starts with `ARR` and the sections written (`ARR7`), the
  menu row reads `ON 7`, and the REC key blinks dim red, in time with JAM REC's blink.
- Editing the chain by hand during a take is allowed; the next pass then starts a new entry.
- The take's passes mark the project for the next save, as an edit would.

## SECTION: a chain entry as a song section (push A on CHAIN)

The arrangement concept, Roland's decisions of 2026-11-28, `core/queue.hpp` (`SectionExtra`,
`section_step`), `app/ui_section.hpp`. A chain entry is a section: its pattern, repeats and mute
state, plus a scene, a morph move, an ENERGY and a tempo applied as the entry starts. **Push A** on
a CHAIN entry opens its SECTION view; the list's own keys are unchanged.

| Key | Does | Says |
| --- | --- | --- |
| A-D | the page's four fields: **SECT** PAT REP MUT SCN, **MOVE** MRP NRG BPM (D free) | |
| push A-D | the field back to `--` (PAT and REP have none) | `SCN --`, `PAT HAS NO --` |
| Screen keys | SECT / MOVE | |
| UP / DOWN | the previous / next entry, staying in the view | `SECT 3/8` |
| LEFT / RIGHT | the entry's pattern, as on the list | |
| STEP 1-16 | that slot of the shown bank into the entry, as on the list | |
| VALUE | the entry's repeats, as on the list | |
| OK | tap: back to the list. Hold (0.5 s): **quick fill** from what plays | `SECTION FILLED` |
| CLR | back to the list | |

- **The fields.** MUT `--`, M1-M8 or **CAP** (an exact mute mask the entry stores; turned to CAP
  it takes the mutes as they stand). SCN `--` or 1-16: that scene goes on morph side B. MRP `--`,
  **SNAP** (the morph jumps to B), **RIDE** (it rides to B over the whole entry), **BACK** (it
  rides to A over the whole entry), **SNAP A** (it jumps to A). NRG `--` or -64..+64: ENERGY on the boundary. BPM `--` or
  20-300, a whole BPM a click: the tempo on the boundary.
- `--` leaves that thing as it is, so an entry with every field `--` plays exactly as a chain
  entry always has. NRG and BPM leave `--` on any turn, at ENERGY 0 and the tempo playing.
- **The hands win** until the next entry: a mute, the morph (VALUE in PERFORM, the mixer's slot)
  or ENERGY turned during a section takes over at once, and ends a RIDE or BACK where it is.
- **BPM under an outside clock** is skipped, with `EXT CLOCK: BPM SKIPPED` once per run of the
  transport; the song follows the outside clock. A section's tempo becomes the project's, as a
  turn of VALUE does.
- **Quick fill** (hold OK): MUT CAP with the mutes as they stand, SCN the scene on side B, MRP
  SNAP when the morph is past half way to B, else SNAP A, NRG the ENERGY, BPM the tempo. Dial the
  breakdown in by ear, then hold OK.
- **The list** marks an entry with fields of its own: under its mute state on the lane, `S4`
  (its scene) or `+` for any other field; the cursor's entry lists them under MUTES.
- **Saved** in PROJECT.BIN section 10. A project from before this opens with every field `--`; an
  older build plays pattern, repeats and mute recall, and reads a CAP entry as `--`.

## The performance mixer (push VALUE + STEP)

Roland, 2026-11-23, after modbang's Octatrack Performance Mixer: VALUE as a crossfader from the
dry mix into one of sixteen effects, the sixteenth a master looper. `core/perffx.hpp` has the
slots, `dsp/masterfx.hpp` the five new effects, `dsp/looper.hpp` the looper.

| Gesture | Does |
| --- | --- |
| Push VALUE + STEP n | Pick effect n (`4 ECHO RISER`), at amount 0. It replaces the effect that was up, which rides out over FX GLIDE |
| Push VALUE + the same STEP | Let go, on the release (`FX OFF: VALUE TEMPO`): VALUE goes back to tempo / morph / its field; the effect rides out |
| Push VALUE + hold a STEP | **The momentary push**: held 0.3 s, the picked effect rises to 127 over FX GLIDE while the STEP is held (`ECHO RISER 127`), and rides out to 0 over a 1/16 on release, still picked. A quick tap still just picks |
| VALUE (an effect picked) | Its amount, 0 dry ... 127 the whole effect, on every screen (`ECHO RISER 64`). At 0 it is the dry mix to the bit, the rest rule of the morph and the macros |
| Push VALUE + LEFT / RIGHT | The previous / next slot, round the sixteen, keeping the amount: a scene change with the crossfader left up, crossfaded over FX GLIDE (the old one out in the first half, the new one in over the second). Reaching 16 picks the looper without recording |
| Push VALUE + STEP 13 | The morph (D2): the same A/B morph PERFORM's VALUE turns, on every screen (`MORPH 0`, then `MORPH 64` as it turns). A control, not an effect: the effect that was up stays as it is. Again: let go, the morph stays where it is (`MORPH KEPT: VALUE TEMPO`) |
| Push VALUE + STEP 16 | The looper: records the finished mix from the next bar line for LOOP LEN bars (`LOOP 4 BAR: NEXT BAR`, `LOOP ARMED: PLAY` when stopped), then plays it. Again: leaves the looper, VALUE back to the tempo (`LOOP OFF: VALUE TEMPO`); the loop is kept silent, STEP 16 picks it up, VALUE + CLR throws it away. Again while armed or recording: `LOOP CANCELLED` |
| VALUE (looper picked) | The loop's mix, 0 live ... 127 only the loop (`LOOP MIX 64`) |
| Press VALUE alone | The picked effect rides back to 0 over at most a 1/16 (`ECHO RISER 0`), or the looper's mix (`LOOP MIX 0`); the effect stays picked. **FUNC + press VALUE** cuts at once, for a drop |
| Push VALUE + CLR | Every effect off and the loop's mix to 0, riding (`FX OFF`); again, with all of that off, throws the loop away (`LOOP CLEARED`) |
| VALUE held | The STEP keys are the sixteen slots: dim amber; the effect that is up bright amber; the picked one bright green; STEP 16 red blinking armed, red recording, green holding a loop |

The slots: 1 LP SWEEP and 2 HP SWEEP (the DJ filter), 3 HP RISER (high-pass, resonance climbing,
the whole mix into a longer reverb), 4 ECHO RISER (every track into the delay, feedback
climbing), 5 ECHO FREEZE (into the delay, frozen from 112 up), 6 REVERB WASH (every track into a
longer reverb), 7 MUTE BUS (every fader down, (1 - amount) squared; the sends are post-fader,
so their feeds fade too and the delay and reverb ring out: CyDrums' P5), 8 TAPE STOP (from 16
up), 9 BASS KILL (the kill EQ's LOW), 10 PUMP (a dip on every beat), 11 CRUSH (bits and rate),
12 NOISE RISER, 13 MORPH (the scene morph, above), 14 GATE (sixteenths), 15 BUILD (high-pass,
echo and noise), 16 LOOPER. Until 2026-11-25 slot 7 was STUTTER (the REPEAT pad keeps it) and
13 FLANGER.

- **FX GLIDE** (FUNC menu): a jump that comes from a button (press VALUE, push VALUE + CLR, a
  slot change, leaving a playing loop, a LOCK jump) rides to its target over OFF, 1/16, 1 BEAT
  (the default) or 1 BAR at the tempo. A ride back to rest (to 0: a VALUE tap, VALUE + CLR, the
  old effect on a slot change, a momentary push let go, a LOCK jump) is shorter: at most a 1/16,
  so the dry sound is back for the next beat. Turning VALUE is already a ride your hand makes and never
  glides; a turn during a ride takes over from where it got to. FUNC held makes any of them a cut.
  Every depth is also smoothed over about 11 ms on the audio side, so OFF is a click-free cut.
- **One effect at a time**, plus the looper and the morph: picking an effect leaves a loop
  playing at its mix and the morph where it is, and the effect goes over both. That also bounds
  the CPU (docs/BUDGET.md).
- **Nothing is saved or undone.** The project's settings are untouched; amount 0 is exactly what
  was there. The kill EQ's FUNC + push A-C and the REPEAT pad keep working beside it; a picked
  LP/HP sweep overrides the MST page's saved DJ filter while it is up.
- **MUTE mode keeps push VALUE + STEP 1-8** (store a mute state); 9-16 still pick effects there.
- **The looper uses RESAMPLE's 30 s buffer** (8 bars down to 64 BPM; LOOP LEN halves until it
  fits): no RESAMPLE or FREEZE while a loop is held (`LOOP HOLDS BUFFER`; push VALUE + CLR twice frees it), and no loop while a take is running (`RESAMPLE BUSY`), and a
  stop during the take throws it away. Not saved with the project.

## Hold STEP + an upper pad: edit the step (D6)

**STEP first edits, pad first performs** (2026-11-25). With a STEP held, five of the upper pads
step one field of that step round a short cycle instead of performing, and a toast says
where it landed (`STEP 5 PROB 50%`). The STEP's release then toggles nothing. A value outside
the cycle (set on the step page) goes to the cycle's first. Not with FUNC (the pad chords stay; FUNC + THROW is the reverb lock)
and not in KEYS, where the upper pads are the piano. `core/stepedit.hpp` has the cycles.

| Pad | Sets on the held step | Cycle |
| --- | --- | --- |
| FILL | the condition | FILL, NOT FILL, none |
| REPEAT | the ratchet | 2, 3, 4, off |
| MUTE | an A:B condition | 1:2, 2:2, 1:4, 4:4, none |
| MUTATE | probability | 75, 50, 25, 100% |
| THROW | a lock of the delay send (FUNC: the reverb send) | full, off (`STEP 5 DLY FULL`) |
| SCENE A / B | nothing yet: they perform (and drop the hold), as before | |

Pad first is unchanged: FILL held + tap STEP writes a FILL trig, a scene pad held + STEP picks
the scene, MUTE MODE on makes STEP 1-8 the mute states, REPEAT held + STEP stutters (below).

## REPEAT: beat repeat, step stutter and the rate

CyDrums' P2 and P3 (2026-11-25). One rate serves three things: beat repeat (the audio), note
repeat (a rolling lower pad) and step stutter (the sequencer). It is 1/16 at boot and not saved.

| Gesture | Does |
| --- | --- |
| Hold REPEAT | Beat repeat: the last 1/16 (the rate) of the finished master loops while held. FUNC at the press: pitched down |
| Hold REPEAT, then hold STEP n | **Step stutter**: every track retriggers its step n at the rate, from the press and then on the beat grid (`STUTTER 5 1/16`). The beat repeat gives way for the rest of the hold: the stutter plays live sound, so filters, LFOs, sends and tails keep moving. A step past a short track's length wraps into it; an empty step is silence for that track. The sequencer runs on silently underneath, so letting go lands back in the beat (`STUTTER OFF`). Played, not recorded; nothing is toggled or undone |
| ... FUNC + STEP n | The same on the selected track alone (`T3 STUTTER 5 1/16`); the other tracks play on |
| ... a second STEP | The stutter moves to it; let it go and it goes back to the one still held |
| Let go of REPEAT | Ends the stutter, STEP held or not; the STEP's release toggles nothing |
| REPEAT + LEFT / RIGHT | **The rate**, one slower / faster: 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, stopping at both ends (`REPEAT 1/8T`, `STUTTER 1/32`). Heard at once by the repeat, the stutter and any roll |
| A rolling pad + LEFT / RIGHT | The same, while NOTE RPT is on and a lower pad rolls (`ROLL 1/32`) |
| A rolling pad, REC on, transport running | **Recorded as one step** (CyDrums P3): the press's step gets a roll over LEN at the rate in force at the press (RTCH `ROLL/16` ...), the release its LEN, on any machine. 1/4T, which a step cannot hold, records a plain hit. The rolling pad plays on live over it for that pass |
| REPEAT + VALUE | The rate too, in the order the list names them (as before) |

The step plays as written, its locks included; its condition, probability and ratchet are not
drawn (the stutter is the ratchet now). While a stutter or the rate is under the hand, the live
strip starts with `ST5` and `R1/16`. STEP first still edits: hold STEP, then REPEAT, and the
ratchet steps (D6). VALUE pushed first keeps the mixer's STEP keys.

## Bounce to track: one OK after a take (RESAMPLE)

A take asked for `TO FILE` (the default) is offered to a track once it is all on the card: the
screen says `TAKE ON CARD`, `PUT ON TRK 3 GRN` and `OK PUTS  CLR KEEPS`.

| Key | Does |
| --- | --- |
| OK | The take becomes that track's sample and loads at once (`T3 GRN: TAKE ON`), as `TO TRK n` would have done before the take. Only on a SAMPLE or GRAIN track: anything else says `T1 NOT SAMPLE/GRAIN` and stays offered |
| LEFT / RIGHT (and the screen keys) | Walk the offered track round the eight; `TO FILE`/`TO TRK` is untouched |
| CLR | Leaves it a file (`TAKE STAYS A FILE`); the next OK arms a new take |

The first track offered is a SAMPLE or GRAIN track other than the one the take was bounced from
(the selected track if it is one, else the lowest), then the bounced track itself, then the
selected track. So bouncing two layered tracks into the GRAIN or sliced SAMPLE next to them is
B to `TRK n`, OK, PLAY, then OK. Pulling the card drops the offer; it is not saved.

## The performance pads (upper row, printed TRK 1-7)

| Print | STRATA | Colour | Bright while | Says |
| --- | --- | --- | --- | --- |
| TRK 1 | FILL | white | held, or NOTE RPT on | `FILL`; held + tap STEP writes a FILL trig (COND FILL), `FILL TRIG` / `FILL TRIG OFF` |
| TRK 2 | REPEAT | cyan | held | `REPEAT 1/16`; held + STEP stutters, held + LEFT/RIGHT the rate (below) |
| TRK 3 | MUTE | red | MUTE MODE on | `MUTE MODE` |
| TRK 4 | SCENE A | green | held | `SCENE A: STEP PICKS` |
| TRK 5 | SCENE B | blue | held | `SCENE B: STEP PICKS` |
| TRK 6 | THROW | amber | held | `THROW DELAY` / `THROW REVERB` |
| TRK 7 | MUTATE | violet | a mutation waiting for OK/CLR, or frozen | `MUTATE T1 DENSITY +` |

## The screen frame

Every screen keeps the same frame, so the eye learns where to look (D7: the frame and the live
strip first, pictures per page later). A screen draws only inside it.

| Rows | What | Example |
| --- | --- | --- |
| 0-7 | Top bar (the mockups' since the live-visuals rebuild): the live pattern as a lit badge, **bar.beat**, then in the small font the tempo (`MIDI` or `SYNC` under an outside clock) with `>` while playing, the load in whole percent (a lit dot before it while JAM REC records, blinking once a second, steady while its file closes) and the LOCK mark (`L`, `L+`); the selected track as a lit badge at the right | `[A01] 3.2 130.0 > 24% L [T1]` |
| 8 | Progress: one pixel across the pattern's pass (its master length), its head brightest, on a dim track with the beats (dim) and bars (brighter) still to come marked; empty while stopped | |
| 9-17 | A notice, LEARN or the MUTATE verdict as a lit **toast** while there is one; else, only while something is off rest, the **live strip** in the small font; else nothing (`CLOCK REFUSED` the same way) | `K:L FX4 S40 M:AB` |
| 18-117 | The screen's own content | |
| 121-127 | **Tabs**: the screen's pages in the small font, the one showing lit (EDIT's eight pages, three of MIXER's around the one showing, GRID's pattern pages, PERFORM with what is live); `DROP n` or `GUARDn` takes the right end when there is one. The tracks sounding, the held MAX and the device are on SETUP | `SRC FLT [AMP] FX LFO ...` |
| x 126-127, rows 10-117 | **Master meter**, in decibels (the top 48 dB, about 2 rows a dB) and in segments: the top 6 dB brightest, the next 12 dB bright, the rest dim. The level falls about 20 dB in a third of a second; a bright dot marks the highest reading of the last second; the top pixel is lit for a second after the limiter worked | |

The live strip lists only what is **off rest**, most telling first, and leaves out whole what does
not fit in 17 characters: `ST5` a step stutter on STEP 5 and `R1/16` the rate while REPEAT is
held or a pad rolls (where the hand is, so first), `K:LMH` kill EQ bands thrown, `FX4` the performance effect up (its STEP
number), `S40` the scene morph at 40%, `M:AB` macros away from zero, `RL-` / `RL+` REL ALL and
`LN-` / `LN+` LEN ALL away from rest, `LP` the master looper
armed, recording or playing. At rest it is empty. x 114-124 on that line is the LOCK mark: `L`
while the playing pattern has a lock, `L+` once the live set differs from it.

Two rules keep the frame cheap and whole. It is drawn only from words the audio side publishes
once a block (`Status`): nothing in `draw()` walks a pattern or a buffer. And no text may reach
x 126: the layout check holds every string to the screen, GRID's cells are five pixels wide, and
right-aligned values end at x 124.

The live screens draw with STRATA's own kit (`app/paint.hpp`): anti-aliased lines between pixels,
arcs, soft dots, an afterglow for scopes, and a 3 x 5 font for labels on graphs
(`paint::kSmallLabels` puts them back on Enaga's 5 x 7 if the small one is hard to read on the
OLED). SETUP shows what a frame costs to draw, `DRW 0.85MS`, averaged over 64 frames.

What moves with the music (`app/ui_live.hpp`), only while the transport runs, so a stopped
screen holds still:

- **GRID**: a trig that plays lights an outline round its cell, in the gap round it, which fades
  over about a third of a second, so the beat leaves a trail behind the playhead.
- **PERFORM**: the four macros are **gauges** (an arc from bottom-left round to bottom-right,
  bright up to where the macro is, a needle, the value under it; an empty macro's gauge dim),
  with the output's **scope** glowing dim behind them, and under the REL/LEN line **every
  track's level** in a row of eight short meters (decibels, a muted track dim). Stopped, that row
  shows the `AUTOFILL: FUNC MENU` hint again and the scope is gone.
- **MIXER**: on the fader pages a thin live meter beside each fader; on MST and EQ, under the
  bars, the output's **spectrum** in 24 bands (94 Hz to 12 kHz, peak dots held half a second;
  `app/spectrum.hpp`, a 256-point FFT a frame) and a **vectorscope** (mid up, side across, in an
  afterglow: a mono mix is a vertical line, a wide one a cloud). On EQ a thrown kill sinks its
  bands as you watch.

## Random ranges and slides (hold STEP + FUNC + A-D)

Backlog Phase 5 (CyDrums), 2026-11-25. Two kinds of lock beside the value, both in the lock's
param byte (core/pattern.hpp: bit 6 range, bit 7 slide), so no file layout changed and an older
build simply ignores them.

| Gesture | What it does | Shown |
| --- | --- | --- |
| Hold STEP + FUNC + turn A-D | The parameter's **random range** on the step, 0-127 counts either side. Each pass the step plays its lock (or the kit value with no lock) moved by a draw within the range. 0 removes it | `S5 CUT 64>90 R20`; a dim line under the bar over the reachable values |
| Hold STEP + FUNC + push A-D | The lock **slides**: from the step it glides to what the next step that plays holds for that parameter, arriving as that step starts. Again: off. No lock yet: the kit value is locked first | `SLIDE ON` / `SLIDE OFF`; `/` at the end of the lock line |
| Hold STEP + push A-D | Takes the whole parameter off the step: value, range and slide | `LOCK CLEARED` |

- **The draw** is a hash of the pattern's seed, the track, the step, the pass and the parameter
  (core/scheduler.hpp `lock_spread`), like probability: no running random number, so a pass
  plays the same after a reload and each pass differs. A ratchet's hits share their step's draw;
  a step stutter draws once.
- **The glide's target** is the next slot in play order (REV, PING and RND look ahead the way
  they will play) whose step has a trig or a trigless, at most a whole pass ahead: its lock and
  range, or the sound the track starts from. Its condition and probability are not drawn. The
  glide lasts the slots in between, swing, micro-timing and FEEL left out, and moves once an audio
  block at control rate. A ratchet glides across its own hits. Not on a step stutter.
- A range takes one of the step's 8 lock slots; a slide takes none (a bit on the value lock).
- Not the SLID step flag (the step page, VA MONO): that bends the note into the next one.

## Locks on screen

While a STEP is held, every parameter shows what **that step plays**, and the kit value it moves
away from (Phase 3 items 2-3):

- **Bars.** On GRID's four slots and EDIT's four rows the solid bar is what you hear: the held
  step's lock if it has one, else the kit value. A **hollow tick** marks the kit value (the knob):
  a bright pixel above and below the bar, dark across it inside the solid part. No lock, no tick.
  The value text above or beside the bar is the heard value too.
- **The lock line.** Hold STEP + turn A-D (or push it to clear): `S5 CUT 64>90`, the step, the
  parameter, the kit value and then the lock, live while the STEP is down. `S5 CUT 64 KIT` once the
  lock is cleared. A random range adds ` R` and its width (`S5 CUT 64>90 R20`, or `S5 CUT 64 R20`
  with no value lock), and a slide a closing `/`. It takes GRID's edit line and EDIT's line under the rows; a notice still wins
  on GRID.
- **Macro gauges** (PERFORM): the arc is where the macro is, a pad's pressure included; when
  that differs from the knob, a bright dot just outside the ring marks the knob.
- **GRID cells.** A trig's 5 x 6 cell carries marks cut dark into it: a dot in the middle for
  locks, the top-left corner for a condition (FILL, A:B, PRE ...), the top-right corner for a
  probability under 100%, two teeth in the bottom edge for a ratchet. A trigless step (locks, no
  note) is an outline. The marks never overlap, so any mix reads.

All of it reads values the screen already has: the kit values, the cell's own step, and at most
eight locks of the one held step per slot. Nothing walks the lock pool.

## Waveforms and envelopes

Every EDIT page is a knob line at the top (A-C), a picture that takes the screen, and a footer
(D, the page's state at the right, and under it what moves the page's knobs or the page's own
line: `LFO>CUT 40`, `SLC AUTO: 12 HITS`, `HOLDS: PAD OR LEN`, `FDE 0: 0.55 X DEC`). How the
keys work is FUNC help's, not the footer's. While a STEP is held and a knob turned, the lock line
(`S5 CUT 64>90`) lights over the footer and the picture draws the lock. docs/SCREENS.md has the
rows and columns; this table says what each picture shows (Phase 3 item 4, CyDrums ideas V2, V3,
V5, then the redesign in #171 and #174).

| Page | Machine | Picture |
| --- | --- | --- |
| SRC | SAMPLE | The sample, each column's range about the middle, scaled so the loudest column fills the height. The START..END window bright, the rest dim; START and END as bright full-height lines; each slice start (SLC above 1, equal or AUTO) a dotted line; each playing voice a full-height column, as bright as its envelope. A streamed file shows its resident head and a dotted line for the rest |
| SRC | GRAIN | The sample, all bright; a bracket where the next grain starts (POS plus SCN's travel, one grain's span wide); a tick for each grain in the air, as bright as its window times the cloud's envelope, so the cloud breathes; and each grain a soft spark at a height of its own, leaving a fading trail |
| SRC | WAVE MONO, WAVE POLY | The table as a waterfall: its sixteen frames stacked back to front, up and to the right, each hiding what is behind it, the frame POS plays brightest and its neighbours brighter than the rest. `BUILDING` while the table is still being made |
| AMP | every machine but MID | The amplitude envelope: ATK and DEC on SAMPLE (DEC at the top holds), DEC alone on the drums, ATK DEC SUS REL on the synths, FM4 and GRAIN |
| ENV | VA, WAVE, FM4, GRAIN, SAMPLE | The whole note: the amplitude envelope, and on the synths the filter's beside it (`AMP FLT`) |
| FLTR | every machine but MID | The filter's response at CUT and RES (the mode at the right) over the spectrum of what the track plays; on VA and WAVE the voice's own filter, with `ENV>CUT` in the footer |
| LFO | every machine, while the LFO runs (DEP and DST set) | One cycle of the shape (x 0-79) with a dot riding it at the LFO's phase and a dotted line through it; beside it a strip of the last two seconds of output, newest at the right. TRIG (and SYNC) at the footer's right. Off, the page says `LFO OFF: SET DEP+DST` and the dot rests |

On an envelope, a segment's width follows its **knob**, not its time: every time curve is
exponential in its knob, so this is a log-time drawing, and 2 ms and 5 s share the width. A
handle that a knob **on this page** moves is a bright square with the knob's letter by it, as
CyDrums draws them; a handle moved from another page has no letter. On AMP and ENV a **hollow
square rides the curve** where the track's last note is (SAMPLE's loudest voice, GRAIN's cloud,
VA MONO and WAVE MONO; the POLY machines, FM4 and the drums draw no dot). Under every envelope a
dim fill; under the part the note has already travelled, a brighter one.

SAMPLES (the sample browser) lists seven rows and draws what the track plays under them, from the
same picture, over the `NOW` line. **Push A hears the file under the cursor** without loading it
(dsp/audition.hpp): streamed whole from its start, straight to the output, after every effect.
With the transport running it takes a second push on the same file within 2 s (the first says
`A AGAIN: HEAR`): there is no cue output, so it would go out to the room with the beat.
Push A again stops it; while one plays, UP/DOWN, turning A and the pages hear the file the cursor
lands on; OK, CLR, another folder or leaving SAMPLES stops it. A one-pixel line under the heard
file's name grows as it plays, and a toast reads `HEARING 3/12S` (`OPENING` until it sounds).

A SAMPLE or GRAIN track with no file plays the placeholder, and its picture is the placeholder's,
with `NO FILE` (or `LOADING` while its file loads) over it.

Nothing is computed in `draw()` that walks memory. The loader makes each sample's picture once,
as it writes the PCM into the pool (`core/overview.hpp`: the lowest and highest value of each of
124 columns, 250 bytes a track), in the slices it already works in, beside the slice-point
finder. The audio side publishes where each SAMPLE and GRAIN track is, in the same columns, every
fourth block (`Status::wave`: the window, the slice starts, up to eight playheads or grains with
a brightness each, and the amplitude envelope's stage and level for the dot). An envelope is
drawn from the knobs alone.

## CLICK and COUNT-IN (the FUNC menu)

CyDrums S6, 2026-11-26. Enaga has no click (checked at v0.3.0), so STRATA renders one
(dsp/click.hpp): a 30 ms decaying sine, 1 kHz on a beat and 1.6 kHz on a bar's first, about
-13 / -10 dB, straight to the output after RESAMPLE's tap (never recorded, looped or repeated, as
the sample audition). CLICK clicks on every beat while the transport runs. A COUNT-IN starts the
clock at once and holds the sequencer, the rolls, the stutter and the arps for its bars, clicking
them whether CLICK is on or not; then everything runs as from a plain START, step 1 on the beat
after the count-in. Nothing records during it, except a hit up to half a step before its end:
that is the downbeat played early, and it lands on step 1. STOP or FUNC + PLAY ends it. With CLOCK OUT on, an
outboard box hears START when the count-in begins, so it runs the count-in's bars ahead. Under an
outside clock there is no count-in (STRATA does not start the transport).

## Choke groups (push A-D on MIXER's TRK pages)

CyDrums S4, 2026-11-26. On MIXER's TRK 1-4 and TRK 5-8, a plain push of A-D steps that strip's
track through OFF, 1, 2, 3, 4, OFF (`T7 CHOKE GROUP 1`, `T7 CHOKE OFF`); the strip shows `C1`-`C4`
under its machine. The pushes were free on those pages (FUNC + push A-C stays the kill switches
everywhere). The step is counted on the audio side (`Cmd::set_choke` c = 1), so fast pushes are
not lost. A note on a track in a group cuts the group's other tracks (`Engine::choke_group`);
a kit setting, KIT.BIN section 11.

## FUNC help

FUNC held for 0.6 s with nothing else pressed shows help for **the screen that is up**
(Roland, 2026-11-23): what A-D do there, named as on the screen and said in words (`B FMA FM
AMOUNT` on a kick's SRC, `A SRC SOURCE TRACK` on MIXER DUCK), and the gestures that work there
(the step page, the hold-and-press LEN, MACRO LEARN, NOTE view, SLC AUTO, rename and delete,
KEYS on the pads ...). While FUNC is still held, **the screen keys (or LEFT/RIGHT) turn the help's
pages**, as they turn every screen's pages; the help's last line says
`PAGE 1/11`, and on the first page `< LAST`: the pages go round, so LEFT reaches the last. The
last five pages are the same everywhere: FUNC + each function key,
FUNC + a knob or pad, the upper pads, and the performance mixer's gestures and slots
(`VALUE FX MIXER:`, `VALUE FX STEPS:`). Every hold starts on the screen's own page.

A quick tap of FUNC alone (let go before the help shows, nothing else pressed) says
`HOLD FUNC: HELP`, unless another notice or a MUTATE waiting for OK or CLR is up: nothing else
on the panel says the help is there.

FUNC + a screen key or LEFT/RIGHT does nothing anywhere else, and the help only takes them once
it is up, so no chord changes meaning. Any other key, knob or pad, or letting go of FUNC, takes
the help away, so a chord never sees it. Not while a name is being typed.

The screens and their pages, from `core/help.cpp`:

| Screen | Its own pages |
| --- | --- |
| GRID | A-D (the EDIT page they turn), steps, range and track (CLEAR PATTERN), STEP lights |
| EDIT | its page's knobs on this machine (with NOTE view, SLC AUTO, ENV notes), EDIT keys and MACRO LEARN, steps, range and track |
| EDIT in MACRO LEARN | what a push does now, then the page's knobs |
| KEYS | the pads as a piano, the page's knobs, the pad lights |
| Step page | the three field pages, what each field is |
| MIXER | the page that is up (TRK, DCK, DUCK, WID, DLY, DLY+, REV, REV+, ROUTE, CHO, IN, IN+, MST, EQ) |
| PERFORM | macros, LEARN, scenes, morph, mutate; second, PERFORM LIVE: the layer FUNC + turn reaches (REL ALL, LEN ALL, ENERGY, LIVE RND) |
| PROJECT, CHAIN, PATTERN, SETUP | one page each |
| SECTION (push A on CHAIN) | one page |
| LAUNCH | one page |
| PROJECTS | open, rename, delete, save as; typing a name |
| MACHINE, KIT, SAMPLES, SOUNDS, RESAMPLE, FUNC menu | one page each |

## Checked by

`tests/test_ui.cpp` (every gesture above, the lights, the help, and the layout check over the
menu and every page of the help on every screen; `tests/test_help.cpp` holds each row to the
font and the line; the emulator cannot hold FUNC long enough to show the help, so no scenario does) and `tests/scenario_controls.sh` (HOME twice, the screen keys on
PROJECT, the FUNC menu, FUNC + PROJECT and the menu's save, on the C674x build). CLEAR PATTERN
and its undo: `tests/scenario_clear.sh`. EDIT's pictures: `tests/test_ui.cpp` (`ui_wave_*`,
`ui_envelope_*`), `tests/test_overview.cpp`, and `tests/scenario_pictures.sh` on the C674x build.
