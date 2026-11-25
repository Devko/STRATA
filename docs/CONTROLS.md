# STRATA controls concept

The reference the key handlers in `app/app.hpp` follow. It starts from the SmplTrek's panel as
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
     EDIT, MIXER, MACHINE, RESAMPLE), the bank A-H in PROJECT and CHAIN. Where the STEP keys
     always mean the same sixteen things (KIT, SAMPLES, SOUNDS, the menu) they do nothing.
     FUNC + pad-row key = the selected track's length, on every screen but the menu.
   - **Knobs A-D:** the four values on screen. Push = that value's switch. With a STEP held, turn
     = lock, push = remove the lock. In PERFORM, push = MACRO LEARN for that macro: EDIT opens,
     and every knob turned there goes on the macro, its start as the macro's 0 and where it was
     left as its 127 (learn by turning, D11). OK, CLR, HOME or leaving EDIT ends it (`B: 3
     PARAMS`) and the knobs snap back. A push there takes a parameter the macro holds off it. TUN's switch (PIT on SAMPLE and GRAIN), on EDIT, GRID and
     KEYS, is NOTE view: the value reads as a note, where TUN puts C-4 (`G-4`, `A-1+` when the
     byte is between notes), and a click is a semitone; push again for the number and one count
     a click. On at every start, one setting for all tracks (2026-11-23, core/tuning.hpp).
     On PROJECTS, where only A turns, push B renames and push C deletes the project under the
     cursor (delete then asks for OK twice).
   - **VALUE:** tempo (PERFORM: morph; CHAIN/PATTERN: the field under the cursor). Push + turn =
     swing, FUNC + turn = DJ filter. **Push + STEP 1-16 picks a performance effect, and while one
     is picked VALUE is its amount on every screen** (the performance mixer, below). **A press
     of VALUE alone** (let go within half a second, with nothing turned or pressed meanwhile) resets what it is on to 0:
     a picked effect's amount, the looper's mix, PERFORM's morph; with FUNC, the DJ filter to
     OFF. Never the tempo, swing or a field, and nothing while a STEP is held.
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
| CHAIN | | ON / OFF |
| CHAIN END | | SONG (play once) / LOOP |
| AUTO-FILL | | OFF 4 8 16 bars |
| QUANTIZE | | record quantize, by 5 |
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
| FUNC + MUTATE (hold) | delay freeze |
| FUNC + VALUE | DJ filter |
| FUNC + push A / B / C | kill LOW / MID / HI |
| FUNC + push D (hold) | tape stop |
| FUNC + pad-row key | selected track's length; held, it repeats (after 0.4 s, 20 steps a second) |
| FUNC + STEP | trigless step (PERFORM: STEP 5-7 put the kill EQ on the macro) |
| FUNC + A / B / C on GRID | Euclidean fill |

## The performance mixer (push VALUE + STEP)

Roland, 2026-11-23, after modbang's Octatrack Performance Mixer: VALUE as a crossfader from the
dry mix into one of sixteen effects, the sixteenth a master looper. `core/perffx.hpp` has the
slots, `dsp/masterfx.hpp` the five new effects, `dsp/looper.hpp` the looper.

| Gesture | Does |
| --- | --- |
| Push VALUE + STEP n | Pick effect n (`4 ECHO RISER`), at amount 0. It replaces the effect that was up, which fades out |
| Push VALUE + the same STEP | Let go (`FX OFF: VALUE TEMPO`): VALUE goes back to tempo / morph / its field |
| VALUE (an effect picked) | Its amount, 0 dry ... 127 the whole effect, on every screen (`ECHO RISER 64`). At 0 it is the dry mix to the bit, the rest rule of the morph and the macros |
| Push VALUE + LEFT / RIGHT | The previous / next slot, round the sixteen, keeping the amount: a scene change with the crossfader left up. Reaching 16 picks the looper without recording |
| Push VALUE + STEP 16 | The looper: records the finished mix from the next bar line for LOOP LEN bars (`LOOP 4 BAR: NEXT BAR`, `LOOP ARMED: PLAY` when stopped), then plays it. Again: leaves the looper, VALUE back to the tempo (`LOOP OFF: VALUE TEMPO`); the loop is kept silent, STEP 16 picks it up, VALUE + CLR throws it away. Again while armed or recording: `LOOP CANCELLED` |
| VALUE (looper picked) | The loop's mix, 0 live ... 127 only the loop (`LOOP MIX 64`) |
| Press VALUE alone | The picked effect back to 0, dry (`ECHO RISER 0`), or the looper's mix to 0 (`LOOP MIX 0`); the effect stays picked |
| Push VALUE + CLR | Every effect off and the loop's mix to 0 (`FX OFF`); again, with all of that off, throws the loop away (`LOOP CLEARED`) |
| VALUE held | The STEP keys are the sixteen slots: dim amber; the effect that is up bright amber; the picked one bright green; STEP 16 red blinking armed, red recording, green holding a loop |

The slots: 1 LP SWEEP and 2 HP SWEEP (the DJ filter), 3 HP RISER (high-pass, resonance climbing,
the whole mix into a longer reverb), 4 ECHO RISER (every track into the delay, feedback
climbing), 5 ECHO FREEZE (into the delay, frozen from 112 up), 6 REVERB WASH (every track into a
longer reverb), 7 STUTTER (beat repeat from 16 up: 1/4, 1/8, 1/16, 1/32 as it turns, each a
shorter slice of the same past), 8 TAPE STOP (from 16 up), 9 BASS KILL (the kill EQ's LOW),
10 PUMP (a dip on every beat), 11 CRUSH (bits and rate), 12 NOISE RISER, 13 FLANGER, 14 GATE
(sixteenths), 15 BUILD (high-pass, echo and noise), 16 LOOPER.

- **One effect at a time**, plus the looper: picking an effect leaves a loop playing at its mix,
  and the effect goes over the loop. That also bounds the CPU (docs/BUDGET.md).
- **Nothing is saved or undone.** The project's settings are untouched; amount 0 is exactly what
  was there. The FUNC chords for the same moves (FUNC + VALUE, FUNC + push A-D, FUNC + MUTATE, the
  REPEAT pad) keep working beside it; the pad and the chords win over the mixer's stutter, freeze
  and tape stop, and a picked LP/HP sweep overrides FUNC + VALUE's DJ filter while it is up.
- **MUTE mode keeps push VALUE + STEP 1-8** (store a mute state); 9-16 still pick effects there.
- **The looper uses RESAMPLE's 30 s buffer** (8 bars down to 64 BPM; LOOP LEN halves until it
  fits): no RESAMPLE or FREEZE while a loop is held (`LOOP HOLDS BUFFER`; push VALUE + CLR twice frees it), and no loop while a take is running (`RESAMPLE BUSY`), and a
  stop during the take throws it away. Not saved with the project.

## The performance pads (upper row, printed TRK 1-7)

| Print | STRATA | Colour | Bright while | Says |
| --- | --- | --- | --- | --- |
| TRK 1 | FILL | white | held, or NOTE RPT on | `FILL`; held + tap STEP writes a FILL trig (COND FILL), `FILL TRIG` / `FILL TRIG OFF` |
| TRK 2 | REPEAT | cyan | held | `REPEAT 1/16` |
| TRK 3 | MUTE | red | MUTE MODE on | `MUTE MODE` |
| TRK 4 | SCENE A | green | held | `SCENE A: STEP PICKS` |
| TRK 5 | SCENE B | blue | held | `SCENE B: STEP PICKS` |
| TRK 6 | THROW | amber | held | `THROW DELAY` / `THROW REVERB` |
| TRK 7 | MUTATE | violet | a mutation waiting for OK/CLR, or frozen | `MUTATE T1 DENSITY +` |

## FUNC help

FUNC held for 0.6 s with nothing else pressed shows help for **the screen that is up**
(Roland, 2026-11-23): what A-D do there, named as on the screen and said in words (`B FMA FM
AMOUNT` on a kick's SRC, `A SRC SOURCE TRACK` on MIXER DUCK), and the gestures that work there
(the step page, the hold-and-press LEN, MACRO LEARN, NOTE view, SLC AUTO, rename and delete,
KEYS on the pads ...). While FUNC is still held, **the screen keys (or LEFT/RIGHT) turn the help's
pages**, as they turn every screen's pages; the line above the bottom line says
`SCREEN KEYS: PAGE 1/9`. The last five pages are the same everywhere: FUNC + each function key,
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
and its undo: `tests/scenario_clear.sh`.
