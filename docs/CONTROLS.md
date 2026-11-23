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
   - **Screen keys:** the screen's pages. EDIT SRC FLTR AMP FX LFO ARP; MIXER's twelve; PROJECT,
     CHAIN, PATTERN, SETUP, PROJECTS; the step page's field pages; SAMPLES' and KIT's pages of
     eight. On GRID and KEYS, where A-D turn the current EDIT page, they turn that page. The
     three one-page screens give them their one sideways setting: MACHINE the track, RESAMPLE the
     destination, PERFORM the mutate amount.
   - **Pad-row keys:** which sixteen the STEP keys mean. Step page 1-4 (GRID, EDIT, MIXER,
     MACHINE, RESAMPLE), the octave in KEYS, the bank A-H in PROJECT and CHAIN. Where the STEP
     keys always mean the same sixteen things (PERFORM, KIT, SAMPLES, the menu) they do nothing.
     FUNC + pad-row key = the selected track's length, on every screen but the menu.
   - **Knobs A-D:** the four values on screen. Push = that value's switch. With a STEP held, turn
     = lock, push = remove the lock.
   - **VALUE:** tempo (PERFORM: morph; CHAIN/PATTERN: the field under the cursor). Push + turn =
     swing, FUNC + turn = DJ filter.
   - **STEP keys:** the sixteen of whatever the pad-row keys chose.
   - **Lower pads:** the eight tracks: select and play. FUNC + pad = mute on the next bar.
   - **Upper pads:** the seven performance tools, the same in every mode but KEYS.
   - **KEYS is the exception: both pad rows are a piano**, being the pressure-sensitive ones and
     laid out like a keyboard (Roland, 2026-11-23, after playing KEYS on the STEP keys). Lower
     pads C D E F G A B C, upper pads C# D# above them, F# G# A#; the upper pad between E and F
     is octave down and the one after B octave up. A pad plays at its own velocity, holds its
     note while down, and its pressure bends macro A. The track is UP/DOWN, as in KEYS anyway.
     FUNC + a pad keeps its usual job (mute on the bar, the performance moves). The pads light as
     a keyboard: the root bright in the track's colour, the scale's other notes dim, the rest
     dark, a held pad white, the two octave pads dim white. The STEP keys stay the 16 scale
     degrees, so KEYS has both a chromatic piano and an in-key keyboard.
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
| OK | pick / confirm | redo | lit when redo has somewhere to go |
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
| NEW PROJECT | OK twice within 2 s: a new project | |
| RELOAD PROJECT | OK twice within 2 s: read the project back from the card | |
| CLEAR PATTERN | OK twice within 2 s: every track's trigs and locks gone, one undo level (FUNC + CLR) | |
| KITS | the kit browser | |
| RELOAD SAMPLE | load the selected track's sample again | |
| TRACK SPEED | | selected track's speed, 2 to 1/8 |
| CHAIN | | ON / OFF |
| CHAIN END | | SONG (play once) / LOOP |
| AUTO-FILL | | OFF 4 8 16 bars |
| QUANTIZE | | record quantize, by 5 |

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
| FUNC + UP / DOWN | copy / paste (editing screens). Card lists: read again / store here (KIT save, PROJECTS save as) |
| FUNC + lower pad | mute that track on the next bar |
| FUNC + FILL / REPEAT / THROW | note repeat on/off / pitched-down repeat / reverb throw |
| FUNC + MUTATE (hold) | delay freeze |
| FUNC + VALUE | DJ filter |
| FUNC + push A / B / C | kill LOW / MID / HI |
| FUNC + push D (hold) | tape stop |
| FUNC + pad-row key | selected track's length |
| FUNC + STEP | trigless step (PERFORM: assign the LFO or kill EQ to the macro) |
| FUNC + A / B / C on GRID | Euclidean fill |

## The performance pads (upper row, printed TRK 1-7)

| Print | STRATA | Colour | Bright while | Says |
| --- | --- | --- | --- | --- |
| TRK 1 | FILL | white | held, or NOTE RPT on | `FILL` |
| TRK 2 | REPEAT | cyan | held | `REPEAT 1/16` |
| TRK 3 | MUTE | red | MUTE MODE on | `MUTE MODE` |
| TRK 4 | SCENE A | green | held | `SCENE A: STEP PICKS` |
| TRK 5 | SCENE B | blue | held | `SCENE B: STEP PICKS` |
| TRK 6 | THROW | amber | held | `THROW DELAY` / `THROW REVERB` |
| TRK 7 | MUTATE | violet | a mutation waiting for OK/CLR, or frozen | `MUTATE T1 DENSITY +` |

## FUNC help

FUNC held for 0.6 s with nothing else pressed shows FUNC + each function key and what the
upper pads are, over the current screen. Any key, knob or pad, or letting go of FUNC, takes it
away, so a chord never sees it.

## Checked by

`tests/test_ui.cpp` (every gesture above, the lights, the help, and the layout check over the
menu and the help screen) and `tests/scenario_controls.sh` (HOME twice, the screen keys on
PROJECT, the FUNC menu, FUNC + PROJECT and the menu's save, on the C674x build). CLEAR PATTERN
and its undo: `tests/scenario_clear.sh`.
