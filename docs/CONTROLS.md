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
   - **Screen keys:** the screen's pages. EDIT SRC FLTR AMP FX LFO ARP ASR OSC; MIXER's twelve; PROJECT,
     CHAIN, PATTERN, SETUP, PROJECTS; the step page's field pages; SAMPLES' and KIT's pages of
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
     On PROJECTS, where only A turns, push B renames and push C deletes the project under the
     cursor (delete then asks for OK twice).
   - **VALUE is the crossfader.** Push VALUE + STEP picks what it fades: an effect, the morph
     (13) or the looper (16), on every screen (the performance mixer, below). With nothing
     picked it is the tempo (PERFORM: the morph; CHAIN/PATTERN: the field under the cursor).
     Push + turn = swing. FUNC + turn is free (D1, 2026-11-25: it says `DJ FILTER: SLOT 1/2`).
     **A press of VALUE alone** (let go within half a second, with nothing turned or pressed
     meanwhile) resets what it is on to 0: a picked effect's amount, the looper's mix, the morph
     (PERFORM, or slot 13 picked). Never the tempo, swing or a field, and nothing while a STEP
     is held.
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

## Function block

| Key | Plain | FUNC + key (printed second label) | Lit |
| --- | --- | --- | --- |
| FUNC | modifier | — | green while held |
| REC | live record on/off | RESAMPLE ("sampling") | red recording, green on RESAMPLE |
| PROJECT | PROJECT: banks, patterns, chain, card | SAMPLES, the selected track's browser ("pool") | green PROJECT, red SAMPLES |
| HOME | GRID, always. Double-tap (within 0.4 s, on GRID) = KEYS | **FUNC menu** ("system"); again closes it | green GRID, amber KEYS, red menu |
| PLAY | play | restart from step 1 ("◀◀") | green running |
| STOP | stop; again cuts the tails | reset: back to step 1, queue dropped, chain to its start ("▶▶") | red stopped |
| MIXER | MIXER | PERFORM ("fx") | green MIXER, red PERFORM |
| EDIT | EDIT | MACHINE; again KIT ("rec src" is the nearest: the sound's source) | green EDIT, red MACHINE/KIT |
| OK | pick / confirm. On PROJECT's slots a tap queues, a hold (0.5 s) opens the pattern menu: COPY, PASTE, CLEAR, DOUBLE (doubles every track up to 64 steps, copying the steps) | redo | lit when redo has somewhere to go |
| CLR | back / clear | undo | lit when undo has somewhere to go |

## The FUNC menu (FUNC + HOME)

A list over whatever screen was up. UP/DOWN or A scroll, OK runs the row, LEFT/RIGHT or B change
a setting row, CLR or FUNC + HOME closes it and returns to the screen it opened over. STEP
keys, screen keys and pad-row keys do nothing in it. FUNC + CLR / OK stay undo and redo.
The cursor starts on SAVE every time, so **FUNC + HOME, OK saves**.

| Row | What OK does | Value (LEFT/RIGHT, B, or OK steps) |
| --- | --- | --- |
| SAVE | saves the project, closes the menu | |
| SAVE AS | PROJECTS page with the name entry open | |
| OPEN PROJECT | PROJECTS page; OK twice there opens | |
| NEW PROJECT | OK twice within 2 s: a new, empty project (the factory sounds, every pan centred, no steps, scenes or macros) | |
| RELOAD PROJECT | OK twice within 2 s: read the project back from the card | |
| CLEAR PATTERN | OK twice within 2 s: every track's trigs and locks gone, one undo level (FUNC + CLR) | |
| KITS | the kit browser | |
| RELOAD SAMPLE | load the selected track's sample again | |
| TRACK SPEED | | selected track's speed, 2 to 1/8 |
| TRACK PLAY | | selected track's play mode: FWD, REV, PING, RND1, RND2, RND4, RND8, RND16 (`T3 PLAY RND4`). Which step sounds in each slot; the pattern is not changed. One undo level a change |
| CHAIN | | ON / OFF |
| CHAIN END | | SONG (play once) / LOOP |
| AUTO-FILL | | OFF 4 8 16 bars |
| QUANTIZE | | record quantize, by 5 |
| FX GLIDE | | how long a button's jump in the performance mixer rides: `OFF`, `1/16`, `1 BEAT` (default), `1 BAR`, at the tempo. Not saved |
| FREEZE | freeze the selected track (its loop from the next bar line), or unfreeze it | `T3 1 BAR` (1 2 4 8), `T3 WAIT` / `REC` while recording, `T3 ON` frozen |
| LOOP LEN | | the performance mixer's looper: `4 BAR` (1 2 4 8), what push VALUE + STEP 16 records |
| LOAD DEMO | OK twice within 2 s: the factory demo project (the boot beat), under a new name | |

Moved here from: FUNC + PROJECT twice (save), PROJECT's FUNC + UP/DOWN/LEFT/RIGHT (chain,
sample reload, reload, new), PROJECTS' FUNC + DOWN (save as, still there too), CHAIN's FUNC +
RIGHT (song), PATTERN's FUNC + UP (chain), PERFORM's FUNC + UP/DOWN (auto-fill), FUNC + screen
keys (speed), FUNC + EDIT twice (kits, still there too). CHAIN's FUNC + LEFT twice (remove an
entry) became CLR twice: CLR is what clears.

## FUNC chords that stay

Printed labels (above), and the moves made while playing:

| Chord | Does |
| --- | --- |
| FUNC + CLR / OK | undo / redo |
| FUNC + UP / DOWN | copy / paste (editing screens; on PROJECT's slots a whole pattern, the paste twice). Card lists: read again / store here (KIT save, PROJECTS save as). FUNC + LEFT twice on PROJECT's slots clears the pattern under the cursor |
| FUNC + lower pad | mute that track on the next bar (at once while stopped). Several pads before the bar all land; the same pad twice takes it back |
| FUNC + FILL / REPEAT / THROW | note repeat on/off / pitched-down repeat / reverb throw |
| FUNC + push A / B / C | kill LOW / MID / HI |
| FUNC + pad-row key | selected track's length; held, it repeats (after 0.4 s, 20 steps a second) |
| FUNC + STEP | trigless step, on every playing screen, PERFORM included |
| FUNC + A / B / C on GRID | Euclidean fill |
| FUNC + MUTATE, held half a second | **LOCK**: store the live set as the playing pattern's lock (`LOCK A01 STORED`); again later overwrites it |
| FUNC + turn A / B in PERFORM | **REL ALL** / **LEN ALL** (below) |
| FUNC + turn D / push D in PERFORM | **LIVE RND**: the unit / the selected track in or out (below) |
| FUNC + MUTATE, tapped | Jump to that lock at once (`BACK TO LOCK A01`); tapped again, swap back to the state from just before the jump (`SWAPPED BACK A01`). No lock: `NO LOCK: HOLD STORES` |

Retired 2026-11-25 (backlog D1, D5; CHANGELOG "Gestures that moved"): FUNC + VALUE (DJ filter,
now slots 1-2), FUNC + hold push D (tape stop, slot 8) and FUNC + hold MUTATE (delay freeze,
slot 5). Each now only names its slot (`DJ FILTER: SLOT 1/2`, `TAPE STOP: SLOT 8`,
`FREEZE: SLOT 5`), apart from FUNC + MUTATE, which is LOCK since the next change (below); FUNC +
push D with a STEP held toggles the slide of that step's D lock, as FUNC + push A-C do theirs.

## LOCK (FUNC + MUTATE)

Backlog D3, D4, D13, D14 (2026-11-25), `core/lockstore.hpp`. Store a moment you like, change a
lot, and jump back in one tap; tap again to compare with where you were.

| Holds | Does not hold |
| --- | --- |
| The playing pattern whole: steps, p-locks, conditions, lengths, swing | Other patterns, the bank, the chain |
| Every track's sound bytes (all eight EDIT pages) and fader | The samples, the machines: a lock taken before a machine switch restores the parameters only |
| The send and master bytes | The kill EQ's switches, the tempo (not STRATA's under an outside clock) |
| The mutes (D4) | The eight stored mute states |
| The morph position, the four macro positions, REL ALL, LEN ALL and LIVE RND | The scenes and the macro setup |

- **One lock per pattern (D14), in RAM only**: not saved, and a project load or NEW clears them.
- **At once (D3)**, on the next audio block. The performance mixer is not in a lock: the jump
  rides the picked effect to 0 over FX GLIDE (it stays picked) and a loop plays on.
- **A second tap swaps back (D13)** to the state captured at the jump, sound included: undo
  covers only patterns (R1), so it could not. Any later tap jumps to the lock again. A pattern
  change, or a new store, ends the swap-back.
- **The kit is shared by every pattern (R2)**: jumping to A01's lock puts A01's sound back for
  every pattern. The steps are per pattern; the sound comes from whichever lock you jumped to.
- **A MUTATE waiting for OK or CLR is discarded** by a jump or swap: its pattern is gone.
- On screen, at the right end of line 2: `L` while the playing pattern has a lock, `L+` once
  anything it holds has changed (the font has no `*`). The MUTATE pad glows half bright while
  the playing pattern has a lock.

## REL ALL and LEN ALL (FUNC + turn A / B in PERFORM)

CyDrums' P4 (backlog Phase 5 item 5, 2026-11-25), `core/moveall.hpp`. Two global controls in
the Moves layer, beside the macros and the morph, on the one screen that is that layer's home.
FUNC + turn on PERFORM's A and B only duplicated the plain turn (the macro) before, so no gesture
changed meaning; C and D with FUNC still turn their macros.

| Control | Moves | Range | Says |
| --- | --- | --- | --- |
| REL ALL, FUNC + A | Every track's release bytes by the same count: AMP's DEC on every machine, and ASR's REL on the machines that read it. A step's lock on them too | -64 .. +64 counts | `REL ALL -12` |
| LEN ALL, FUNC + B | Every sequenced note's gate, x 2^(n / 32) | 25% .. 400% | `LEN ALL 71%` |

- **Rest is 0, and 0 is a detent**: a turn that would cross it stops on it. At rest the sound is
  the programmed one to the bit (the rule the macros and the morph keep).
- **Two counts a click**, like the macros. Not saved, not undone; a LOCK holds both (D4), and a
  jump or swap puts them where the lock had them.
- **On screen**: PERFORM's line under the macros reads `FUNC+A/B REL/LEN ALL` at rest and
  `REL -12 LEN 71%` away from it; the live strip adds `RL-` / `RL+` and `LN-` / `LN+`.
- **LEN ALL leaves a note with no gate alone** (a drum, a pad hit, a held key, the arpeggiator's
  own GATE): REL ALL is what shortens drums.

## LIVE RND (FUNC + D in PERFORM)

CyDrums' P1, the live half (backlog Phase 5; the controls concept: "the PATTERN page plus one
PERFORM knob gesture"), `core/liverand.hpp`. The pattern half is the FUNC menu's TRACK PLAY. FUNC
+ D in PERFORM was a duplicate of the macro (turn) and the tape stop's hint (push), so no gesture
that did something changed meaning; FUNC + push D still names slot 8 on every other screen.

| Gesture | Does | Says |
| --- | --- | --- |
| FUNC + turn D | The unit for every armed track: OFF, 1, 2, 4, 8, 16, one a click, stopping at both ends | `LIVE RND4 8 TRK`, `LIVE RND OFF` |
| FUNC + push D | The selected track out of the armed set, or back in. All eight at start | `T1 RND OUT 7 TRK`, `T1 RND IN 8 TRK` |

- **What it plays**: exactly TRACK PLAY's RND mode of that size, from the same seeded draw (the
  pattern's seed, the track, the block, the pass), over the track's own mode while it is on.
  Blocks keep their steps in order and their place in the bar.
- **Recoverable**: the pattern is never written. OFF is the programmed order from the next slot;
  a track's own mode (REV, PING ...) comes back with it.
- **Not saved, not undone**, like REL ALL. A LOCK holds the unit and the armed set (D4); a jump
  or swap puts both back. Live recording lands a hit on the step that was sounding.
- **On screen**: PERFORM's line under the macros reads `LIVE RND4 FUNC+D`, or `REL -12 LEN 71% R4`
  with REL/LEN ALL away from rest; the track meters mark the armed tracks with a dot; the live
  strip adds `RN4`. The FUNC help's third PERFORM page lists both gestures.

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
step one field of that step round a short cycle instead of performing, and the bottom line says
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
| 0-6 | Top line: live pattern, **bar.beat** in its pass, tempo, `>` playing / `.` stopped, load in whole percent | `A01 3.2 130.0 > 24%` |
| 8 | Progress: one pixel across the pattern's pass (its master length), its head brightest, on a dim track with the beats (dim) and bars (brighter) still to come marked; empty while stopped | |
| 10-16 | Second line: a notice, LEARN or the MUTATE verdict while there is one; else the **live strip**; else `EXT MIDI` / `EXT SYNC` / `CLOCK REFUSED` | `K:L FX4 S40 M:AB` |
| 18-117 | The screen's own content | |
| 120 | Bottom line: tracks sounding, MAX, device, DROP, GUARD | `TRK3 MAX23.5 SMPLTREK` |
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

EDIT draws a picture under its four rows, rows 96-117 and x 0-123, on the pages where one says
more than numbers (Phase 3 item 4: CyDrums ideas V2, V3, V5). The page's one line of words moves
to y 87, over the picture: SLC on SAMPLE, `OK: SOUNDS` on GRAIN, `OK: PICK SAMPLE` or
`FUNC+EDIT: MACHINE`, what SUS does on ASR, `FDE 0: 0.55 X DEC` when the filter's decay is tied.
While a STEP is held and a knob turned, the lock line (`S5 CUT 64>90`) takes that line instead,
and the picture draws the lock.

| Page | Machine | Picture |
| --- | --- | --- |
| SRC | SAMPLE | The sample, each column's range about the middle, scaled so the loudest column fills the height. The START..END window bright, the rest dim; START and END as bright full-height lines; each slice start (SLC above 1, equal or AUTO) a dotted line; each playing voice a full-height column, as bright as its envelope. A streamed file shows its resident head and a dotted line for the rest |
| SRC | GRAIN | The sample, all bright; a bracket where the next grain starts (POS plus SCN's travel, one grain's span wide); a tick for each grain in the air, as bright as its window times the cloud's envelope, so the cloud breathes |
| AMP | every machine but MID | The amplitude envelope: ATK and DEC on SAMPLE (DEC at the top holds), DEC alone on the drums, ATK DEC SUS REL on the synths, FM4 and GRAIN |
| ASR | VA, WAVE, FM4, GRAIN | The same envelope |
| FLTR | VA and WAVE | The filter's: ATK, FDE (or 0.55 x DEC while FDE is 0), FSU, REL, and ENV as its height |
| LFO | every machine, while the LFO runs (DEP and DST set) | One cycle of the shape (x 0-79) with a dot riding it at the LFO's phase and a dotted line through it; beside it a strip of the last two seconds of output, newest at the right. The `NOW CUT 064` line sits over it at y 87. Off, the page keeps `LFO OFF: SET DEP+DST` at y 96 and draws nothing |

On an envelope, a segment's width follows its **knob**, not its time: every time curve is
exponential in its knob, so this is a log-time drawing, and 2 ms and 5 s share the width. A
handle that a knob **on this page** moves is a bright square with the knob's letter by it, as
CyDrums draws them; a handle moved from another page has no letter. On AMP and ASR a **hollow
square rides the curve** where the track's last note is (SAMPLE's loudest voice, GRAIN's cloud,
VA MONO and WAVE MONO; the POLY machines, FM4 and the drums draw no dot). Under every envelope a
dim fill; under the part the note has already travelled, a brighter one.

SAMPLES (the sample browser) lists seven rows and draws what the track plays under them, from the
same picture, over the `NOW` line. **Push A hears the file under the cursor** without loading it
(dsp/audition.hpp): streamed whole from its start, straight to the output, after every effect.
Push A again stops it; while one plays, UP/DOWN, turning A and the pages hear the file the cursor
lands on; OK, CLR, another folder or leaving SAMPLES stops it. A one-pixel line under the heard
file's name grows as it plays, and line 2 reads `HEARING 3/12S` (`OPENING` until it sounds).

A SAMPLE or GRAIN track with no file plays the placeholder, and its picture is the placeholder's,
with `NO FILE` (or `LOADING` while its file loads) over it.

Nothing is computed in `draw()` that walks memory. The loader makes each sample's picture once,
as it writes the PCM into the pool (`core/overview.hpp`: the lowest and highest value of each of
124 columns, 250 bytes a track), in the slices it already works in, beside the slice-point
finder. The audio side publishes where each SAMPLE and GRAIN track is, in the same columns, every
fourth block (`Status::wave`: the window, the slice starts, up to eight playheads or grains with
a brightness each, and the amplitude envelope's stage and level for the dot). An envelope is
drawn from the knobs alone.

## FUNC help

FUNC held for 0.6 s with nothing else pressed shows help for **the screen that is up**
(Roland, 2026-11-23): what A-D do there, named as on the screen and said in words (`B FMA FM
AMOUNT` on a kick's SRC, `A SRC SOURCE TRACK` on MIXER DUCK), and the gestures that work there
(the step page, the hold-and-press LEN, MACRO LEARN, NOTE view, SLC AUTO, rename and delete,
KEYS on the pads ...). While FUNC is still held, **the screen keys (or LEFT/RIGHT) turn the help's
pages**, as they turn every screen's pages; the line above the bottom line says
`PAGE 1/10` (`SCREEN KEYS: PAGE 1/n` under ten pages). The last five pages are the same everywhere: FUNC + each function key,
FUNC + a knob or pad, the upper pads, and the performance mixer's gestures and slots
(`VALUE FX MIXER:`, `VALUE FX STEPS:`). Every hold starts on the screen's own page.

FUNC + a screen key or LEFT/RIGHT does nothing anywhere else, and the help only takes them once
it is up, so no chord changes meaning. Any other key, knob or pad, or letting go of FUNC, takes
the help away, so a chord never sees it. Not while a name is being typed.

The screens and their pages, from `core/help.cpp`:

| Screen | Its own pages |
| --- | --- |
| GRID | A-D (the EDIT page they turn), steps, range and track (CLEAR PATTERN), STEP lights |
| EDIT | its page's knobs on this machine (with NOTE view, SLC AUTO, ASR notes), EDIT keys and MACRO LEARN, steps, range and track |
| EDIT in MACRO LEARN | what a push does now, then the page's knobs |
| KEYS | the pads as a piano, the page's knobs, the pad lights |
| Step page | the three field pages, what each field is |
| MIXER | the page that is up (TRK, DCK, DUCK, DLY, DLY+, REV, REV+, ROUTE, MST, EQ) |
| PERFORM | macros, LEARN, scenes, morph, mutate |
| PROJECT, CHAIN, PATTERN, SETUP | one page each |
| PROJECTS | open, rename, delete, save as; typing a name |
| MACHINE, KIT, SAMPLES, SOUNDS, RESAMPLE, FUNC menu | one page each |

## Checked by

`tests/test_ui.cpp` (every gesture above, the lights, the help, and the layout check over the
menu and every page of the help on every screen; `tests/test_help.cpp` holds each row to the
font and the line; the emulator cannot hold FUNC long enough to show the help, so no scenario does) and `tests/scenario_controls.sh` (HOME twice, the screen keys on
PROJECT, the FUNC menu, FUNC + PROJECT and the menu's save, on the C674x build). CLEAR PATTERN
and its undo: `tests/scenario_clear.sh`. EDIT's pictures: `tests/test_ui.cpp` (`ui_wave_*`,
`ui_envelope_*`), `tests/test_overview.cpp`, and `tests/scenario_pictures.sh` on the C674x build.
