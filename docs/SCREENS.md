# The screens: pixel spec

What every STRATA screen draws, where, and from what. The drafts are `tools/visuals_mockup.py`
(PERFORM, GRID, AMP, FLT, LFO, WAVE OSC, GRAIN SRC, MASTER); this file turns them into rows and
columns and says how the screens the drafts never drew follow them. Change a screen here first,
then in the code, then in `tests/test_ui.cpp`.

The screen is 128 x 128, 16 greys, drawn at most every 50 ms (Enaga's main loop). Two fonts:
Enaga's 5 x 7 (`canvas.text`, 6 px a character) and STRATA's 3 x 5 (`paint::small_text`, 4 px a
character). Both are held to the screen by the layout test (`layout_faults`): no string crosses
another, small labels end by x 125, two small labels on a line keep 2 blank pixels, Enaga's text
4.

## Rules every screen keeps

- **Drawn from what the audio side publishes** (`Status`, the scope ring) or from settings. Nothing
  in draw() walks a pattern's memory beyond the page on screen, a sample, or a file. PROJECT's
  slots are the one exception: the shown bank's sixteen and one other bank's "holds a pattern"
  bits a frame, into a cache (below).
- **Stopped, a screen holds still and still looks right**: pictures are drawn from their knobs;
  analysers (spectrum, scope, meters) sit at rest; nothing moves on a timer of its own. Moving
  parts come from the audio side (an LFO's phase, a grain, a note's envelope) or from the running
  transport. The control audit (`ui_audit_every_input_in_every_screen_shows_it_did_something`)
  fails a screen that moves with no input.
- **A picture that plays a note through** (a drum's hit, the filter's sweep, the duck's dip)
  starts from the moment the UI saw the track's trigger count move (`update_track_triggers`,
  every frame on every screen). A count that moved while nothing was drawn for 200 ms is an old
  note and is not played through.
- **draw() stays under 5 ms** on the emulator with cl6x (`scenario_visuals`), figures in
  `docs/BUDGET.md`.

## The frame (every screen; unchanged since #171)

| Rows | What |
| --- | --- |
| 0-8 | the live pattern as a lit badge (0-20), bar.beat (24, 5x7), tempo or MIDI/SYNC and `>` (45, small), load % (80, small), LOCK mark (96), the track badge (106-125) |
| 8 | progress: the pass lit from the left, beats and bars marked ahead |
| 9-17 | a toast (notice, LEARN, the MUTATE verdict) lit over whatever is there; else the live strip (small, y 11) only while something is off rest |
| 18-117 | the screen |
| 121-127 | the tabs: the screen's pages in the small font, the one showing lit; DROP / GUARD at the right |
| x 126-127, rows 10-117 | the master meter |

## Displaced information: where it went

| Was | Now |
| --- | --- |
| The screen's title line (`MIXER DCK 5-8`, `SETUP`) | the lit tab; lists keep a caption in the small font at y 18 |
| Knob values as two rows of 5x7 text with bars under them | EDIT's knob line (small, y 18) and footer (y 105); a held step's lock lit on its value |
| MOD slots' range marks under the bars | the footer's second line names what moves the page's knobs (`LFO>CUT 40`, `VEL>RES +20`); the LFO page lists both slots there |
| `FUNC+EDIT: MACHINE` and the other hints | the footer's second line (y 113), when nothing moves the page's knobs |
| MIXER's PEAK line | the frame's meter (and its limiter light); MST shows GR and LUFS |
| Notices, scenes, MUT, LEARN | toasts in the frame (agreed 2026-11-25) |
| TRK, MAX, device | SETUP (agreed 2026-11-25) |

## EDIT (app/ui_editview.hpp)

| Rows | What |
| --- | --- |
| 18-22 | the knob line: `A ATK 004  B DEC 048  C SUS 057` at x 0, 42, 84, letter dim (6), name grey (10), value bright (15); a lock or range on the held step lights the value. Moved right only as far as the one before needs (3 blank px), then the longest loses its letter's space |
| 26-101 | the picture (below) |
| 105 | knob D at x 0; the page's state at the right (`TRIG FREE`, `AUTO 5`, `X2 OCT`) |
| 113 | what modulates the page's knobs, or the page's hint |
| 103-111 | holding a STEP and turning: the lock readout, 5x7, dark on a lit bar |

Pictures by page and machine:

| Page | Machines | Picture |
| --- | --- | --- |
| SRC | KICK SNARE HAT PERC | one hit worked out from the knobs (pitch sweep, tone and noise, clap bursts, metal, FM index), min/max columns about y 62 (amp 30), the envelope dotted round it, the length in ms; after a trig the played part lights in real time |
| SRC | SAMPLE | the overview about y 62 (amp 28), START..END bright (9) and the rest dim (4), the window's ends full height with STA's and LEN's letters, slices dotted (8), each voice a column and a dot |
| SRC | GRAIN | the draft: overview about y 46 (amp 20), the bracket lit (15) with each grain's tick on its edges; `GRAINS n/8` at y 74; the cloud rows 78-101, each grain a spark rising as it fades, afterglow 184/256 a frame |
| SRC | WAVE | the draft's waterfall: 16 frames from (6, 74), 3.4 px right and 2.6 up a frame, 40 points 1.6 px apart, amp 11, POS's frame 15, its neighbours 8, the rest 4, hidden lines blacked; LIVE at y 87-101 |
| SRC, OSC | VA MONO, VA POLY, WAVE (OSC) | two cycles across, y 62, amp 28: the first oscillator, the second (detuned, drifting while the beat plays, or synced), the sub, dim; their sum through the drive bright |
| SRC | FM4 | the algorithm's boxes and wires (op 4's feedback a loop), carriers on the OUT bus; two cycles of the carrier at IDX and RAT, the index falling after each note |
| FLT | drums, SAMPLE, GRAIN | the draft's filter: 30 Hz-12 kHz across (x 1-119), 0 dB at y 48, 1.1 rows a dB, rows 26-96, guides at 100 / 1K / 10K with labels at y 98; the output's spectrum behind (grey 3, every other column, falling a fifth a frame); the curve (15) with a fill (2); the cutoff line (6) with knob B's letter; the mode at the top right |
| FLT | VA, WAVE | the same frame at the synth's own CUT, RES and mode (SRC's MOD); a dim curve where ENV opens it; after each note the bright curve sweeps back down over FDE |
| AMP, ASR | all with an envelope | the draft's envelope: rows 34-100, dotted guides every 8 px and at quarters, the curve (9) with a fill (2), the letters lit over the points they move; while the note sounds its travelled part bright (15, fill 4) with a dot at its head |
| FX | all | a sine and what LOF makes of it (rows 30-56); the pan field as an arc with the track's dot (centre 36, 96); SNA and SNB as two columns into DLY and REV, their tops lit while the track sounds |
| LFO | all | the draft: the shape's box (0-77, rows 28-88) with the dot riding it and a fading trail, the output strip (82-119), `OUT`; the destination bar (rows 93-97) lit to what it sounds at with a mark at its knob; off, `LFO OFF: SET DEP+DST` there |
| ARP | all | sixteen slots of the run over a three-note chord through OCT octaves (VAR's order), each GAT long; RHYTHM's rests a dot on the floor, ACCENT's hits taller and brighter, a RATCHET slot split into its repeats; while the beat plays the slot it is on lit. The info line names the kit settings that are set |
| OSC | FM4 | harmonics 0-8 across, CAR tall and bright, MOD at RAT + FIN, the detuned pair, the sidebands the index makes |
| OSC | GRAIN | the file small with SCN's travel as an arrow; PSP's spray as a fan with the grains on it |
| ASR, OSC | a machine without them | `NO ASR` / `NO OSC` and which machines have them |

## MIXER (app/ui_mixview.hpp)

| Page | Picture |
| --- | --- |
| TRK 1-4, TRK 5-8 | the desk: eight strips 15 px apart. The page's four with their knob's letter lit at y 19, the others' numbers dim; pan as a tick (rows 27-29); SNA and SNB as two thin bars (31, 33); the meter (x+1..x+5, rows 36-97, top 36 dB, peak held a second); the fader's slot (x+10) and cap at the level (0-1.2); the level (y 102) and the machine (y 110); a muted strip dim with a hollow cap |
| DCK 1-4, DCK 5-8 | the desk with each track's duck depth hanging from the top, lit as far as the duck pulls now (the source's trigs through ATK, HLD and REL); the source's strip says `SRC` and flashes on its trigs |
| DUCK | knob line; the gain over time (0 dB at the top, the floor at the bottom) through ATK, HLD and REL, the letters on its corners, a dot riding it after each source trig; `DUCKS UNDER Tn` |
| DLY, DLY+ | one hit and its echoes over 2 s (8 s LONG), each as tall as FBK leaves it, wider as TON darkens, alternating up and down with ping-pong, wobbling with WOW, smeared with DIF; a head sweeping in time with the transport; the time in ms |
| REV, REV+ | the tail over a log time axis (10 ms-30 s): early reflections spaced by SIZ, the dense tail falling over DEC, smoother as DMP darkens it, wobbling with MOD, the shimmer's sparks; ROOM or HALL |
| ROUTE | TRK into DLY and REV, both into OUT; D>R's wire as bright as it sends; ping-pong's L R; LONG's low cut; dots running along the wires while the beat plays |
| MST | the draft: knob line (DJ CMP LIM); the spectrum, 24 bands of 5 px (4 lit) in rows 26-59, every other row, bright at the top, peaks held 12 frames; 100 / 1K / 10K at y 63; the vectorscope 42 x 42 at (0, 70), mid up and side across, afterglow; CMP's reduction and LIM falling from the top of two bars (x 50, 68; rows 72-109); GR and LUFS (momentary, K-weighted, 400 ms, from the scope history at 24 kHz) at x 88; LVL at (88, 106) |
| EQ | FLT's frame with the kill EQ's curve (a shelf under 250 Hz, a bell, a shelf over 2.5 kHz; KILL 30 dB down), A B C on it, a lit `KILL` over a thrown band |

## PROJECT's slots (app/ui_pickview.hpp)

| Rows | What |
| --- | --- |
| 18 | the project's name; at the right what the card side is doing (`SAVING 40%`), NO CARD, the last error, or `UNSAVED` |
| 25 | `KEY` and the key (`C# MIXOLYDIAN`); at the right `Q` and the queue mode, and `CHAIN` while it plays |
| 32 | `SMP`, a mark a track (full: loaded; hollow: streamed; a dash: loading; a cross: failed; a dot: none) and the pool (`9/64M`); at the right `BANK A 5/16`, the shown bank and how many of its slots hold a pattern |
| 39 | the eight banks' letters, each over its block, the shown one lit, a bank holding nothing dim |
| 46-56 | the map: each bank a block of 4 x 4 cells (2 x 2 px, 3 apart) laid out as the tiles below. Dim (2): empty; grey (7, 9 in the shown bank): holds a pattern; white: playing; blinking with the clock's eighths: queued |
| 61-115 | the shown bank's sixteen tiles, 29 x 13, 30 and 14 apart: the number (a lit badge when it plays) and the pattern's steps as 16 columns by 8 track rows (a 64-step track folds four steps a column); the playing one brighter with a playhead column (the selected track's step) while running; a queued one's underline blinking; the cursor a white frame; an empty slot dotted with `--` |
| 121 | tabs: SLOT CHAIN PAT SETUP PROJS |

The map and the tiles draw from a cache (`refresh_slot_cache`): the shown bank's sixteen are
read every frame, the other banks' "holds a pattern" bits one bank a frame (400 ms for all seven),
and every bank in the first frame the screen is up after another, so a screen nobody touches
holds still.

## MACHINE (app/ui_pickview.hpp)

| Rows | What |
| --- | --- |
| 18 | `MACHINE T1`; at the right `B` and MIDI OUT (`OFF`, `CH 10`), lit when a MID track sends nowhere |
| 24-38 | the kit: a cell a track (15 px), its machine's picture (12 x 6) and name; the selected track framed, its name a badge |
| 42-94 | the twelve machines, 29 x 17 tiles in columns of three as the arrows walk them: a picture of the sound (25 x 8) over the name; the track's framed with its name lit |
| 98, 105 | what the track's machine is (`KICK: SINE, PITCH SWEEP, CLICK`) and what it costs (`DRUM, 2 VOICES, LIGHT ON CPU`, `6 VOICES OF THE SHARED 12`) |
| 113 | OK's action (`OK: SOUNDS`, `OK: PICK SAMPLE`); `FUNC+EDIT: KITS` at the right |

The pictures (`machine_icon`): KCK a sine sweeping down and dying; SNR a short tone and a burst of
noise; HAT a closed and an open tick of noise; PRC an FM ring; SMP bars of a recording; GRN a cloud
of grains; MNO a saw; PLY three saws stacked; MID a five-pin socket; WMN and WPL frames of a table
turning from sine to square (four for WPL); FM4 four operators, the last with its feedback loop.
They are drawn from their shapes and do not move.

## RESAMPLE, the browsers, CHAIN, KITS, KEYS, PATTERN (app/ui_listview.hpp)

The screens that were text lists until #175. Lists keep their rows in Enaga's font, which reads at
arm's length, with the cursor's row a lit bar (`list_row`); everything around them is the drafts'.
No caption repeats the screen's name: the tab says it, and so does the toast on the way in.

| Screen | Rows |
| --- | --- |
| RESAMPLE | 18 where the take goes (`TO FILE`, `TO TRK 3 SMP`, `PUT ON TRK 8 SMP`), its state at the right (`ARMED` blinking with the clock's eighths, `REC 45%` lit, `WRITING`); 25 B and the source, C and the input's level, A and the length; 31-37 the source's level as 26 segments over the top 48 dB, the dB (or CLIP lit) at the right, the scale at 40; 46-88 the take drawn as it is recorded, a column a 120th of it from the source's level (bright while recording, grey once written, a bright head), bar lines dotted; before the first take the source's last 120 frames roll by dim (only while the beat plays or the reading moves, so a silent stopped screen holds still); 92-95 the take's length as a block a bar, filled as it records; 99 the last take, SHORTENED or what is wrong with the input, the length in seconds at the right; 105 and 113 the keys |
| SAMPLES | 18 the folder's tail (or READING, an error, the loading), the cursor's place at the right; 26-71 six entries; the preview from 76: while a file is heard, `HEAR` lit, its name, `3/12S 44.1K`, and the file as a tape of seconds (rows 88-99) lit as far as it is heard under a bright head; else the track's file: its name, length and rate, and its picture about y 95 (amp 10), START..END bright with their lines, the voices as columns |
| SOUNDS | 18 `INTO T1 KCK ONLY`, the cursor's place at the right; 26-71 six presets; 77 the preset's name and whether it is the one playing; 81-104 its twenty knobs as bars, page by page (SRC FLT AMP ASR OSC, labels at 106), the track's own value a tick where it differs; a page the machine does not read dotted |
| CHAIN | 18 entries and SONG or LOOP, `ON` lit while the chain plays; 24-26 the whole song as a strip, each entry as long as its passes; 31-60 the lane: each entry a block as long as its passes (6 + steps/4 px a pass, 15-120), its pattern, `X2` and `M3` in it, the passes dotted, the cursor's framed bright, the playing one filled as far as it has got with the playhead; the song's end a double bar; 66 the cursor's entry (number lit, pattern and repeats in Enaga's font); 77-91 its pattern's steps (read once when the cursor moves onto it) and the mute state it recalls as eight boxes (hollow: muted); 105 and 113 the keys |
| KITS | 18 the page of slots, what the card is doing at the right; eight slots 11 px apart from 25: the number and the kit's eight machines as MACHINE's pictures (drawn once into a cache, `build_kit_icons`); the cursor's row lit with the machines' names; an empty slot a dotted line (the cursor's says how to save there); 113 the keys |
| KEYS | 18 the key and scale, OCT at the right; 25-54 the pads as a keyboard (held pads white); 61 the held notes, lowest first, and the chord they make lit at the right (`C MAJ`, `A MIN7`); 72 the EDIT page A-D turn here; 77-110 its four knobs as gauges, name under, value under that; 113 which sixteen steps the STEP keys are |
| PATTERN | 18 the live pattern; 26, 35, 44 LEN, SWG and FEEL as rows, SWG drawn at the right as a beat of sixteenths with the off-beats late; 55 `TRACK SWING`; 62-109 eight bars, 6 px a track: number, machine, a bar from 50% to the track's swing (75% at x 104), PAT ones dim at the pattern's swing, which is a dotted line down the bars; the value at the right; the cursor's row lit |

MENU, SETUP and PROJECTS have no caption that names them: MENU's rows start at y 20 (nine of
them), SETUP's caption is the device and `DRW` (Enaga's font, which the input audit masks as a
live reading), PROJECTS' is the open project (or the delete question, lit) with the cursor's place
at the right and `OPEN` on the open project's row. HELP's title is a lit band where the toasts go,
its page count small at 113. The step page draws the track's lane (y 84) with the step bracketed
and its note on it: LEN long, MICR off the grid, as bright as VEL, dimmer under PROB, RTCH's hits
as notches.

There is no boot screen of STRATA's own: Enaga's loader draws until init() returns, and the first
frame STRATA draws is GRID.

GRID and PERFORM are as #171 left them (Roland, 2026-11-26: "the sequencer is already good",
"performance also looks good").
