# STRATA cheatsheet

Built from the key handlers and draw code in `app/app.hpp`, so it says what the build does, not
only what docs/SPEC.md planned. Keys are named as printed on the SmplTrek. `FUNC+X` means hold
FUNC and press X; "hold STEP" means a STEP key held down while you do the rest.
[MANUAL.md](MANUAL.md) explains the concepts behind these and walks through a first beat.

## Key legend

| Panel label (SmplTrek) | Code / SPEC name | Where |
| --- | --- | --- |
| FUNC, REC, PROJECT, HOME | F1, F2, F3, F4 | Function block, upper row, left to right |
| PLAY, STOP, MIXER, EDIT | F5, F6, F7, F8 | Function block, lower row, left to right |
| OK, CLR | OK, CLR | Right of the function block |
| UP, DOWN, LEFT, RIGHT | the arrows | Cursor keys |
| Octave left / right | OCTAVE_LEFT / RIGHT | The two octave buttons |
| Selection left / right | SELECT_LEFT / RIGHT | The two buttons under the screen |
| A, B, C, D | encoders A-D | Four knobs; each also pushes |
| VALUE | encoder 4 | The VALUE knob; it also pushes |
| STEP 1-16 | STEP keys | The 16 step buttons |
| Pads | lower 1-8, upper 1-7 | Lower row = tracks, upper row = performance |

The Lofi-12 XT and CyDrums print other names on the function block; this sheet uses the SmplTrek's.
The F-key labels are from Enaga's `docs/hardware.md`, taken from the original firmware's test page.
The on-screen hints use the same names (`FUNC+PROJECT TO SAVE`, `FUNC+EDIT: MACHINE`).

## Modes: how to get there

| Mode | Keys | What it is for |
| --- | --- | --- |
| GRID | HOME | All 8 tracks; STEP keys edit the selected track |
| KEYS | FUNC+HOME | STEP keys play 16 scale degrees on the selected track |
| EDIT | EDIT | The selected track's sound: pages SRC, FLTR, AMP, FX |
| MACHINE | FUNC+EDIT | Pick the machine on each track, and its MIDI OUT channel |
| KIT | FUNC+EDIT twice | Save and recall whole kits: 16 slots on the card, shared by all projects |
| MIXER | MIXER | Levels, delay, reverb, routing, master |
| PERFORM | FUNC+MIXER | Scenes, morph, macros, mutation |
| PROJECT | PROJECT | Banks and patterns, key and scale, the card |
| CHAIN | PROJECT, then octave RIGHT | The pattern chain as a list |
| PATTERN | PROJECT, octave RIGHT twice | Master length (LEN) and swing (SWG) of the playing pattern |
| SETUP | PROJECT, octave RIGHT three times | Record quantize; shows clock and MIDI settings |
| RESAMPLE | FUNC+REC | Record the output to a file or a track |
| Step page | Hold STEP + OK | One step's own fields (opens over GRID) |
| Enaga menu | FUNC+POWER | CLOCK SRC, CLOCK/SYNC OUT, MIDI CH |

## Keys that work everywhere

| Keys | Action |
| --- | --- |
| PLAY / STOP | Play / stop. STOP again cuts the tails. Refused under an outside clock (EXT CLOCK) |
| FUNC+PLAY | Restart from step 1, playing or not (RESTART) |
| FUNC+STOP | Stop, cut the tails, drop a queued change, back to step 1, a running chain back to its first entry (RESET) |
| REC | Live record on/off (REC on the edit line, REC lit red) |
| FUNC+PROJECT, FUNC+PROJECT (within 2 s) | Save the project |
| FUNC+CLR / FUNC+OK | Undo / redo, in every mode and page |
| VALUE turn | Tempo, 20-300 BPM in 0.1 steps (PERFORM: morph) |
| VALUE push + turn | Pattern swing |
| FUNC + VALUE | Master DJ filter (LP n / OFF / HP n) |
| FUNC + octave L/R | Selected track's length (polymeter); not in KEYS, PROJECT, PERFORM |
| FUNC + selection L/R | Selected track's speed: 2, 1, 3/4, 1/2, 1/4, 1/8 |
| Arrows | UP/DOWN move through rows, LEFT/RIGHT sideways, OK picks, CLR goes back |
| FUNC + UP / DOWN | The mode's second action (copy/paste while editing) |

## Pads

| Pad | Plain | With FUNC / held |
| --- | --- | --- |
| Lower 1-8 | Select the track and play its root note; pressure bends macro 1 up | FUNC: mute or unmute on the next bar |
| Upper 1 FILL | Fill while held | |
| Upper 2 REPEAT | Beat repeat while held; VALUE picks 1/4 ... 1/16T | FUNC at the press: pitch drops |
| Upper 3 MUTE | Toggles MUTE MODE: lower pads mute at once | In MUTE MODE: STEP 1-8 recall a mute state, FUNC+STEP on the next bar, push VALUE+STEP stores |
| Upper 4/5 SCENE A/B | Hold + STEP 1-16 picks that side's scene | In PERFORM: hold + OK captures the selected track only, hold + CLR erases the scene |
| Upper 6 THROW | Delay throw while held | FUNC: reverb throw |
| Upper 7 MUTATE | Mutates the selected track (type and amount set in PERFORM) | Then OK keeps it (KEPT), CLR reverts it (REVERTED); any mode but PROJECT |

## Step editing (GRID, EDIT, MIXER, MACHINE, RESAMPLE)

| Gesture | Action |
| --- | --- |
| Tap STEP | Toggle trig. FUNC + tap: toggle trigless (locks, no note) |
| Octave L/R | Step page 1-4 (64 steps) |
| Hold STEP + turn A-D | Lock that parameter of the current EDIT page on the step |
| Hold STEP + push A-D | Remove that one lock (LOCK CLEARED) |
| Hold STEP + turn VALUE | Set the step's note |
| Hold STEP + OK | Open the step page on that step |
| Hold STEP + CLR | Clear the step |
| Hold STEP + UP / DOWN | Copy / paste the step |
| Hold two STEPs | Select the range between them (SEL a-b) |
| Range + CLR / FUNC+UP / FUNC+DOWN | Clear / copy / paste the range |
| Range + A / B | Transpose / nudge micro-timing of the range |
| CLR, nothing held | Clear the whole track (FUNC+CLR brings it back) |
| FUNC+UP / FUNC+DOWN, nothing held | Copy / paste the whole track |
| REC on, playing, turn A-D | Motion record: locks land on the playing step |
| FUNC + A / B / C (GRID) | Euclidean fill: hits, steps, rotation (EUC h/s Rn) |

**Step page.** A-D edit four fields; selection keys flip the three field pages; LEFT/RIGHT move
to the previous/next step; OK or CLR close it.
Page 1 NOTE VEL LEN MICR, page 2 PROB COND RTCH RAMP, page 3 ACNT SLID CHRD.

## Per mode

| Mode | UP / DOWN | LEFT / RIGHT | Selection L/R | A-D | OK / CLR | FUNC + arrow |
| --- | --- | --- | --- | --- | --- | --- |
| GRID | Track | Step page 1-4 | Step page | Current EDIT page params | — / clear | Copy / paste |
| EDIT | Track | Page SRC FLTR AMP FX | Page | The page's 4 params | — / clear | Copy / paste |
| KEYS | Track | Octave (OCT 0-7) | Track | Current EDIT page params | — | Copy / paste |
| MACHINE | Machine | Track | Track | A: machine, B: MIDI OUT channel (OFF, 1-16) | — | Copy / paste |
| KIT | Slot | Page of 8 slots | Page of 8 slots | A: slot | Recall (twice) / back to MACHINE | DOWN: save here, UP: read the card again |
| MIXER | — | Page (6) | Page | Levels or page fields | — / clear | Copy / paste |
| RESAMPLE | Bars 1/2/4/8 | Destination FILE, TRK 1-8 | Destination | A: bars | Arm / cancel | Copy / paste |
| PROJECT | Slot cursor by row | Slot cursor | Bank A-H | A: queue mode, B: key, C: scale | Queue slot / cancel queue | UP: chain on/off, DOWN: reload track's sample, LEFT: reload project, RIGHT: new project |
| CHAIN | Entry | Entry's pattern -/+ | Bank A-H | A: repeats X1-X16 | Add entry / back | LEFT: remove entry, UP: chain on/off |
| PATTERN | Change field -/+ | Field LEN / SWG | Bank A-H | A: LEN 1-64, B: SWG | — / back | UP: chain on/off |
| PERFORM | Mutation type | Macro A-D | Mutate amount (±10) | Macros A-D | Capture scene / clear macro | UP/DOWN: auto-fill OFF 4 8 16 |

**KEYS:** STEP 1-16 play the scale from the root and hold the note until released; octave keys also change the octave. Letting go also releases that note if the sequencer is playing it on the same track.
**PROJECT:** STEP 1-16 queue that slot of the shown bank. On the slots and SETUP pages, FUNC+LEFT
reloads the project from the card and FUNC+RIGHT starts a new one; both throw away unsaved work, so
each needs a second press within 2 s (AGAIN TO RELOAD, AGAIN FOR NEW). The octave keys walk the pages PROJECT,
CHAIN, PATTERN, SETUP; CLR on any of them goes back to the slots. In SETUP, A sets the record
quantize.
**CHAIN:** a STEP key puts that slot of the shown bank into the entry under the cursor (or starts
the chain); VALUE also sets repeats; OK adds a copy after the cursor (the playing pattern when
empty), up to 64 entries. Chain edits are not undoable.
**PATTERN:** VALUE also changes the field under `>`. LEN is the master length that a queued change
(END) and the chain wait for.
**PERFORM:** STEP 1-16 assign that parameter of the selected track to the selected macro. A macro
at 0 lets go: its parameters follow their EDIT knobs and the morph, and turning it up sweeps from
wherever they are. A new project has B ACID (bass filter), C DUB (chord and rim sends) and D DRIVE
(kick and hats); A is left for you, and pad pressure bends it. VALUE is the A/B morph; OK captures into scene A's slot (with a scene pad held: that scene, selected
track only); scene pad + CLR erases it. Mutes, mute states and the A/B scene choice are saved with
the project.
**KIT:** a kit is everything KIT.BIN holds: the eight machines, their parameters, the faders and
the sample paths. STEP 1-16 move the cursor to that slot (they never load). OK once shows OK AGAIN TO
LOAD; OK again within 2 s recalls the kit, and the transport keeps playing. A recall cuts the tails
and is not undoable, which is why it takes two presses. Tracks whose sample path changed reload
their sample in the background; the others keep playing theirs. FUNC+DOWN saves the current kit
into an empty slot at once, and over a stored kit on a second press (AGAIN TO OVERWRITE). The kits
live in `/ENAGA/DATA/STRATA/KITS/KIT_01.BIN` ... `KIT_16.BIN`. A recalled kit makes the project
UNSAVED until it is saved with FUNC+PROJECT twice.
**MIDI out:** a track with a MIDI OUT channel (MACHINE, encoder B) sends every note it plays
there: steps, chords, ratchets, pads, KEYS and MIDI IN. MID is the machine that plays nothing
itself; its SRC and AMP knobs send CC 74 71 73 72 and 70 75 76 77 when they move (locks too).
Notes end with their step length, a key or note let go, a mute, STOP; the second STOP and
FUNC+F6 also send All Notes Off. Enaga 0.2.0: unplug USB (or keep its MIDI port open on the
computer) when driving gear from the DIN jack.
**MIDI in:** channels 1-8 play tracks 1-8, other channels play the selected track. Notes are
held until their note-off. CC 16-31 set the track's 16 parameters (SRC, FLTR, AMP, FX in order);
CC 120/123 let go of held notes. Program change 1-128 on your MIDI CH queues pattern A01-H16.

## The screen

| Where | Example | Meaning |
| --- | --- | --- |
| Top line | `A01 130.0 > 23.4%` | Live bank+slot, tempo, `>` playing / `.` stopped, audio load |
| Line 2 | `EXT MIDI` `EXT SYNC` `CLOCK REFUSED` | Outside clock source; a transport call was refused |
| Bottom line | `TRK3 SMPLTREK` / `TRK3 DROP 2` | Tracks sounding, device; DROP = edits lost to a full command ring |
| GRID | `T1 MUTE PG1 L16 X1` | Track, muted, step page, length, speed |
| GRID edit line | `REC Q50 SEL 1-8 FULL 2 U3/R1` | Recording + quantize, selection, refused edits (lock pool full), undo/redo depth |
| Step page | `TRIG` `TRIGLESS` `OFF`, `LOCKS n` | What the step is and how many locks it holds |
| PROJECT | `BANK A   END CHAIN` | Shown bank, queue mode, chain running |
| CHAIN | `CHAIN 3/64 ON`, `>02 A05 X2 PLAY` | Entries used, chain on/off; cursor, entry, pattern, repeats, playing now |
| PATTERN | `>LEN 16 STEPS`, `SWG 54%` | Master length; swing (`>` = field VALUE changes) |
| PROJECT slots | `05>`, bright box | Queued slot; live slot (bright), has trigs (mid), empty (dark) |
| PROJECT | `SMP O.X- 12/32M` | Per track sample: O ready, . loading, X failed, - none; MiB used/budget |
| PROJECT | `UNSAVED`, `NO CARD` | RAM differs from the card; no usable card |
| KIT | `>03 KIT`, `EMPTY`, `UNREADABLE`, `...` | Cursor and slot: holds a kit, nothing, a damaged file, not read yet; the cursor's kit below as two lines of four machines |
| PERFORM | `A1 12P`, `>A`, `3P` | Scene slot and its parameter count; selected macro; macro targets |
| PERFORM | `MUT NOTES T1 50`, `FILL AF16 1/16- DLY`, `BAR n` | Mutation, track, amount; fill, auto-fill, repeat (`-` pitched down), throw; bar count |
| MIXER | `T1 M`, `ON`/`--`, `PEAK` | Track muted; machine present; master peak |
| RESAMPLE | `TO FILE`, `ARMED`, `REC 40%`, `WRITING`, `LAST name` | Destination and take state |

## Abbreviations

**Machines:** KCK kick · SNR snare · HAT hat · PRC 2-op FM percussion · SMP sampler · GRN granular ·
MNO VA mono · PLY VA poly.

**EDIT pages:** SRC source · FLTR filter (insert chain) · AMP amplitude · FX effects (insert chain).

| Machine | SRC (A B C D) | AMP (A B C D) |
| --- | --- | --- |
| KCK | TUN tune, FMA FM amount, FMD FM decay, CLK click | DEC decay, TON tone, DRT dirt, LVL level |
| SNR | TUN, SNP snappy, TON noise tone, CLP clap mode | DEC, SPR clap spread, BDY body, LVL |
| HAT | TUN, TON tone, NOI noise, DTN detune | DEC, CHO choke time, CHK choke group, LVL |
| PRC | TUN, RAT ratio, IDX FM index, IDD index decay | DEC, SWP pitch sweep, SWT sweep time, LVL |
| SMP | PIT pitch, STA start, LEN length, SLC slices (1 = one-shot) | ATK attack, DEC, PLY play mode (fwd/loop/rev/rev loop), LVL |
| GRN | POS position, SIZ grain size, DEN density, SPR spray | PIT pitch, SHP grain shape, DEC, LVL |
| MNO | TUN, MOD filter mode, CUT cutoff, RES resonance | DEC, ENV filter env amount, GLD glide, LVL |
| PLY | TUN, MOD, CUT, RES | DEC, ENV, DET detune, LVL |

**Insert chain (every track):** FLTR = DRV drive, CUT cutoff, RES resonance, MOD filter mode
(OFF LP12 LP24 BP12 BP24 HP12 HP24). FX = LOF lo-fi, PAN pan, SNA send A (delay), SNB send B (reverb).

**Step fields:** NOTE · VEL velocity · LEN length (`1/16 STP` ... `64 STP`) · MICR micro-timing (±) ·
PROB probability % · COND condition · RTCH ratchet count · RAMP ratchet ramp (FLAT UP DOWN) ·
ACNT accent · SLID slide · CHRD chord.
**Conditions:** NONE, `a:b` (play on pass a of every b), FILL / NOT FILL, PRE / NOT PRE (this track's last
condition result), NEI / NOT NEI (the track to the left's), 1ST / NOT 1ST (first pass after a pattern change).
**Chords:** 1 NOTE, MIN, MAJ, SUS2, SUS4, DIM, MIN7, DOM7, MAJ7, MIN9, MIN11, 5TH, OCT, 5TH OCT; `/n` = inversion n.

**MIXER pages:** TRK 1-4, TRK 5-8 (levels) · DLY: TIM time (1/32 ... 1/1, T triplet, D dotted),
FBK feedback, TON tone, LVL · REV: DEC decay, DMP damping, HPF high-pass, LVL · ROUTE: PNG ping-pong,
D>R delay into reverb · MST master: DJ filter, CMP compressor, LIM limiter, LVL.

**Lists:** queue modes END (end of pattern), 16, 32 (steps), NOW · repeat 1/4 1/8 1/16 1/32 1/4T
1/8T 1/16T · auto-fill OFF 4 8 16 (bars) · mutations ROT LEFT, ROT RIGHT, DENSITY +, DENSITY -,
VELOCITY, MICRO, CHANCE, NOTES · scales CHROMATIC MINOR MAJOR DORIAN PHRYGIAN LYDIAN MIXOLYDIAN
LOCRIAN, HARM MINOR, MEL MINOR, MINOR PENT, MAJOR PENT, BLUES, WHOLE TONE, DIMINISHED · notes use
sharps (C# not Db).

---

<sub>STRATA is an independent project, not affiliated with or endorsed by Sonicware. SmplTrek is a
trademark of its owner. Provided as is, without warranty; use at your own risk (see LICENSE).</sub>
