# STRATA user manual

This manual explains how STRATA works and how to play it. It goes with
[CHEATSHEET.md](CHEATSHEET.md), which lists every gesture, screen and abbreviation on one page:
read this once, then keep the cheatsheet next to the unit.

It describes the current build, written from the code. STRATA is an early beta: most of it has
not been checked on the hardware yet, and where that matters this manual says so.

**Key names.** Keys are named as printed on the SmplTrek: FUNC, REC, PROJECT, HOME, PLAY, STOP,
MIXER, EDIT, OK, CLR, the arrow pad (UP, DOWN, LEFT, RIGHT), the **screen keys** (the pair just
under the screen), the **pad-row keys** (the pair at the ends of the upper pad row, printed as
octave), encoders A-D and VALUE, STEP 1-16 and the pads. `FUNC+X` means hold FUNC and press X.
"Hold STEP" means hold a STEP key down while you do the rest. The cheatsheet's key legend maps
these to the F1-F8 names used in the code and in docs/SPEC.md.

**How the controls are laid out** ([CONTROLS.md](CONTROLS.md)). Each group of buttons does one
kind of thing everywhere: the arrow pad moves the cursor (OK picks, CLR goes back), the screen
keys turn the screen's pages, and the pad-row keys choose which sixteen the STEP keys mean (step
page, octave, bank). FUNC + a function key does what is printed under that key: REC "sampling"
is RESAMPLE, PROJECT "pool" is the sample browser, HOME "system" is the **FUNC menu**, MIXER "fx"
is PERFORM. The rare setup actions (save as, open, new, reload, kits, chain on/off, song mode,
track speed, auto-fill, quantize) live in the FUNC menu, named on the screen. The upper pads are
printed TRK 1-7, but in STRATA they are the performance pads: each is lit in its own colour and
names itself on the screen when pressed. Hold FUNC alone for a moment for help on the screen you
are on: what the four knobs do there and the gestures it has. Keep holding FUNC and tap the screen
keys (or LEFT/RIGHT) for more pages; the last five are the same everywhere: FUNC + each function
key, FUNC + a knob or pad, the upper pads, and two pages on the performance mixer.

![The SmplTrek panel with STRATA's button groups: screen, knobs, screen keys, arrow pad, function block, OK and CLR, STEP keys, pad-row keys, performance pads and track pads](screens/panel.svg)

<p><img src="screens/help.png" width="256" alt="FUNC held alone on GRID: what A-D do on the EDIT page they turn, and GRID's keys"> <img src="screens/menu.png" width="256" alt="The FUNC menu, opened with FUNC+HOME, on SAVE"></p>

<sub>Left: FUNC held alone on GRID: A-D and their words, then GRID's keys; the screen keys turn to
the steps, the STEP lights and FUNC + each key.
Right: the FUNC menu (FUNC+HOME), which always opens on SAVE.</sub>

**Contents**

1. [Core concepts](#1-core-concepts)
2. [Your first beat](#2-your-first-beat)
3. [The machines](#3-the-machines)
4. [The sequencer in depth](#4-the-sequencer-in-depth)
5. [Performing](#5-performing)
6. [Saving, loading and the card](#6-saving-loading-and-the-card)
7. [Performance tips](#7-performance-tips)
8. [Troubleshooting](#8-troubleshooting)

![STRATA's screens: GRID, the step page, EDIT, MIXER, PERFORM, KEYS, MACHINE, KIT and PROJECT](screens/overview.png)

<sub>The factory project playing. Top: GRID, the step page, EDIT. Middle: MIXER, PERFORM (with the
factory macros B, C and D), KEYS. Bottom: MACHINE, the KIT browser, PROJECT. Every screenshot in
this manual is drawn by the app's own screen code on the host build, in Enaga's font
(`tools/screenshots.sh`), so it shows what the build shows.</sub>

---

## 1. Core concepts

### Tracks

STRATA has **eight tracks**. The eight lower pads are the tracks, left to right: tap one to select
it and hear it. Each track holds one **machine** (the sound source) followed by the same fixed
**insert chain**: drive, a multimode filter, lo-fi, the amplitude stage, the sidechain duck, pan
and level. From there each track goes to the mixer, the two send effects (delay and reverb) and
the master bus: the DJ filter, a three-band kill EQ, the glue compressor, the limiter and the
output level.

The selected track is the one the STEP keys, the encoders, KEYS and MUTATE work on. UP and DOWN
also walk the tracks in GRID, EDIT and KEYS.

### Machines

<p><img src="screens/edit.png" width="256" alt="EDIT, the kick's SRC page"> <img src="screens/edit_fltr.png" width="256" alt="EDIT, the FLTR page"></p>

<sub>EDIT on the kick: the machine's SRC page and the insert chain's FLTR page. The bars above the knobs are the EDIT pages; the screen keys turn them.</sub>

A machine is one of eleven sound engines, plus **MID**, which plays an outboard synth over MIDI
instead (section 3). Any track can host any machine (MACHINE mode, `FUNC+EDIT`). The default kit
uses eight of them, one per track (WAVE MONO and WAVE POLY are there to switch to).
**Switching a track to another machine loads that machine's default sound**: its SRC, FLTR and
AMP pages (and ASR and OSC where it has them) become the first entry of its SOUNDS list, and a
SMP track plays a sample whole from its start. The FX page (lo-fi, pan, sends), the LFO and the
arpeggiator stay as they were, and so does a MID track's page.

| Track | Machine | Default role |
| :---: | --- | --- |
| 1 | KCK (KICK) | Kick |
| 2 | SNR (SNARE) | Clap (the snare in clap mode) |
| 3 | HAT | Closed hat; it chokes track 4 |
| 4 | HAT | Open hat |
| 5 | PRC (PERC) | Rimshot |
| 6 | MNO (VA MONO) | Bass |
| 7 | PLY (VA POLY) | Chord stabs |
| 8 | SMP (SAMPLE) | Loop, cut into slices |

Every track has **16 parameters on four pages** of four, one parameter per encoder A-D, then a
fifth and a sixth page for its LFO and its arpeggiator (below). The VA, WAVE and FM4 machines add a
seventh and an eighth, ASR and OSC.

- **SRC** and **AMP** belong to the machine and change meaning with it (section 3).
- **FLTR** and **FX** belong to the insert chain and are the same on every track:
  FLTR = DRV drive, CUT cutoff, RES resonance, MOD filter mode (OFF, LP12, LP24, BP12, BP24,
  HP12, HP24); FX = LOF lo-fi, PAN, SNA send A (delay), SNB send B (reverb).

Parameters run 0-127. Every change is smoothed over one audio block, so turning a knob or
playing a locked step never clicks.

### The LFO

<p><img src="screens/edit_lfo.png" width="256" alt="EDIT, the LFO page"></p>

Every track also has **one LFO**, on a fifth EDIT page, **LFO**, after FX (EDIT, then the
screen keys or LEFT/RIGHT). It moves one of the track's 16 parameters up and down on its own:

- **A, WAV**: the shape. SIN, TRI, RMP (a rising ramp), SAW (a falling one), SQR and S+H (a new
  random value each cycle). One click is one shape.
- **B, RAT**: the rate. The lower half runs **free**, from `0.06` Hz (one sweep every 16 s) up to
  `32` Hz. The upper half is **synced** to the tempo, from `8B` (eight bars) down to `1/32`, with
  dotted (`1/4.`) and triplet (`1/8T`) lengths in between. A synced LFO stays in time with the
  bar wherever the song is, follows an outside clock, and starts on the one when you press PLAY.
- **C, DEP**: how far. At 127 the parameter swings about 64 either side of where its knob is.
- **D, DST**: which parameter, by name, or `OFF`. One click is one parameter.

The line under the knobs shows where the destination is right now (`NOW CUT 87`), or `LFO OFF:
SET DEP+DST` until both are up. A new track's LFO is off, and so is every track in a project
saved before the LFO existed.

The four LFO settings are parameters like any other: hold a STEP and turn one to lock it on that
step (a deeper sweep on one step, a different shape for a fill), store them in a scene and morph
between two LFO settings, or put DEP on a macro to fade the movement in. The LFO moves the
parameter in its 0-127 steps about 750 times a second: smooth on a filter or a pan, but a slow,
deep sweep of a pitch can be heard stepping.

### The arpeggiator

<p><img src="screens/edit_arp.png" width="256" alt="EDIT, the ARP page"></p>

Every track also has an **arpeggiator**, on a sixth EDIT page, **ARP**, after LFO. It turns a
note into a run of notes:

- **A, MOD**: OFF, UP, DN (down), U/D (up and back down), ORD (in the order played) or RND. One
  click is one mode.
- **B, RAT**: the spacing of the notes, `1/4` down to `1/32T`. Always in time with the tempo.
- **C, OCT**: how many octaves the run climbs, `X1` to `X4`.
- **D, GAT**: how long each note is, from `50%` of the spacing (staccato) to `99%` (legato).

The line under the knobs reads the whole setting (`UP 1/16 X2 75%`), or `ARP OFF: TURN MOD`.

**On the sequencer**, a step on a track whose arp is on plays its note as a run for the step's
length: set a step's length to 4 steps and a 1/16 arp plays four notes. On the PLY track the run
walks the step's chord, one note at a time. The track's next step starts a new run. RND plays the
same notes on every pass of the pattern, so a random arp is still a part you can keep.

**In KEYS**, hold pads and the arp plays them, adding each pad you press to the run and dropping
each one you let go; it stops when the last key comes up. With the transport running the notes
land on the beat; stopped, the run starts the moment you press. A pad hit plays one note.

The four settings are parameters like any other: hold a STEP and turn MOD to arpeggiate just that
step, or store two scenes with different arps. A new track's arp is off, and so is every track
in a project saved before the arp existed.

### Held notes: the ASR page

<p><img src="screens/edit_asr.png" width="256" alt="EDIT, the ASR page on VA POLY"></p>

On the VA, WAVE and FM4 machines a seventh EDIT page, **ASR**, after ARP, decides how a note is
held. With it, the POLY machines play pads and long chords, and the MONO ones long bass notes and
drones:

- **A, ATK**: the attack, `3MS` (snappy) up to `5.0S` (a slow swell). Above 0 the
  filter envelope swells with it.
- **B, SUS**: the sustain level, `0%` to `100%` of the note's peak. **At 0% a note is a one-shot**:
  it falls over DEC and ends. **Above 0%** it falls over DEC to SUS and
  **holds there**: for as long as you hold the pad in KEYS (or the MIDI note), or the
  track's own pad in any other mode, or for the step's **LEN** on the sequencer.
- **C, REL**: the release, the tail after the key comes up or LEN ends, `60MS` to `8.0S`.
- **D, FSU**: filter sustain. At 0% the filter envelope sweeps up and closes again over DEC, the
  acid shape; turned up, the filter stays open while the note holds, so a long pad does not go
  dull.

The line under the knobs says `SUS 0: ONE-SHOT` or `HOLDS: PAD OR LEN`. On any other machine the
page shows `NOT ON THIS MACHINE`.

A scene captured with SCENE stores the ASR page too, like every other knob. So if a note still
fades with SUS up, look at the morph: a scene stored at SUS 0 pulls SUS down as VALUE moves
towards it. Capture the scene again, or clear its ASR knobs.

**A pad in three moves:** on the PLY track, ASR page: SUS to about 70%, ATK to about `400MS`, REL
to about `2.0S`. Then on the step page give a chord step a LEN of `16 STP`: it holds for the whole
bar and fades into the next. Or queue **A05 PAD** to hear one ready-made.

A05's pad is in the pattern's **locks**, not the kit, so A01-A04's chord stab stays as it is:
each of its four chords locks ATK `400MS`, SUS 70%, REL `2.0S`, a longer DEC, more DET, a darker
CUT, a lower LVL and more reverb. A turned knob on a locked parameter is heard until the next
chord; to make a change stick, clear that lock on the four chord steps (hold the step, push the
knob), and the kit's value takes over. The reverb is in ROOM mode for the whole factory project;
switch it to HALL (MIXER, `REV+`, A) for the bigger tail the pad was made for.

On the MONO machines, LEN only counts once SUS is above 0, so a bass line written for decays
keeps them; a **slid** step carries the held note on into the next pitch
rather than letting it go.

Held notes cost CPU for as long as they hold: long chords on the POLY machines reach the shared 12-voice
budget sooner (the quietest note is faded to make room). The four settings lock, scene and macro
like any other parameter, and every track in a project saved before the page existed loads at 0,
which sounds exactly as it did.

### The oscillators and drive: the OSC page

<p><img src="screens/edit_osc.png" width="256" alt="EDIT, the OSC page on VA POLY"></p>

On **VA MONO** and **VA POLY** an eighth EDIT page, **OSC**, after ASR, picks what goes into the
filter (WAVE and FM4 have their own OSC pages, described with them in section 3):

- **A, WAV**: OSC 1's wave. `SAW` from 0 to 31; at 32 a square (`SQR`), hollow and woody;
  turned further the pulse narrows (`P30` ... `P08`), thinner and more nasal.
- **B, OS2**: the second oscillator, in two halves. **0** is off. **1-63** adds a second saw
  **detuned** against the first, shown in cents (`D0` to `D60`): a slow chorus low down, a wide,
  beating Reese bass at the top. The two are spread around the note, so it stays in tune.
  **64-127** is a pulse **hard-synced** to OSC 1 and tuned up from it, `S+0` to `S+24`
  semitones: the bright, tearing formant of a sync lead, and swept with a knob or a lock, the
  classic sync sweep. An accented step brings the synced pulse forward.
- **C, SUB**: the sub oscillator an octave down, from off (0) to heavy (127). The weight under a
  bass; turn it down for leads and plucks that should stay out of the kick's way.
- **D, DRV**: drive into the filter. The oscillators are saturated before the filter and a little
  more after it, so the sound gets thicker and dirtier rather than louder. With RES up, a gritty
  squelch.

The line under the knobs says `OS2: DET 1-63 SYN 64+`. On any other machine the page shows
`NOT ON THIS MACHINE`. Like every page, the four lock, scene and macro.

**RES** on SRC runs over the whole knob: each turn adds about the same amount of peak, and from
about 120 the filter whistles by itself at a steady level (self-oscillation; tune it with CUT).
LP24 squelches as hard as LP12. **A thick bass**: OSC SUB up, OS2 about 30 (`D14`), DRV about
40, SRC CUT low and RES under halfway. **An acid line**: SOUNDS, ACID, or by hand: MOD in LP24,
CUT low, RES about 100, ENV up, DEC short, accents on a few steps.

A project saved before the page existed loads `SAW OFF 102 000`: its VAs keep the saw and the sub
but lose the old voice's fixed synced pulse, so they sound a little rounder and darker than they
did. Turn OS2 to 114 (`S+19`) for something close to the old bite.

### Patterns, banks and the project

<p><img src="screens/project.png" width="256" alt="PROJECT: bank A's sixteen slots"></p>

A **pattern** holds all eight tracks: up to 64 steps per track, with their notes, parameter locks
and conditions, plus the pattern's master length, swing, key and scale. Patterns live in
**8 banks (A-H) of 16 slots**, 128 per project. The top line of every screen names the pattern
playing: `A01` is bank A, slot 1.

The **project** is everything: the 128 patterns, the kit (each track's machine, parameters,
level and sample), the tempo, the sends and master, the chain, scenes, macros and mute states.
One project is open at a time.

Each track has its own **length** (1-64 steps) and **speed** (2x, 1x, 3/4, 1/2, 1/4, 1/8), so
tracks can run against each other (polymeter). The pattern's **master length** decides when a
queued pattern change lands and when the chain moves on.

### Parameter locks (p-locks)

A **lock** is a parameter value stored on one step. Hold a STEP key and turn A-D: that parameter
of the current page now has its own value on that step, and goes back to the track's value on
the next. A step can hold up to 8 locks, from a pool of 1,024 per pattern. Locks are how a kick
gets one short hit in a bar, or a bass opens its filter on one accented note. STEP LEDs with
locks glow amber.

### The chain

<p><img src="screens/chain.png" width="256" alt="PROJECT's CHAIN page"></p>

The **chain** plays patterns in order: up to 64 entries, each a pattern and a repeat count
(X1-X16). It is a list, not a song timeline. You build it on PROJECT's CHAIN page and turn it on
and off with the FUNC menu's CHAIN row (FUNC+HOME). Each entry can also recall a **mute state** as it starts, and
**SONG** mode plays the chain once and stops, so a chain can be a whole arrangement (below).

### Scenes and morphing

<p><img src="screens/perform.png" width="256" alt="PERFORM: scenes, the morph bar and the macros"></p>

A **scene** is a snapshot of parameter values for any tracks. A project has 16. Two of them sit
on the **A** and **B** sides, and in PERFORM the VALUE knob **morphs** the live sound from A to
B. A parameter that only one side stores fades between that side's value and the kit's value, so
a scene that touches only the bass leaves the drums alone. Scenes change the kit values only;
a step's locks still win over the morph.

With VALUE at rest the morph **lets go**, the way a macro at 0 does: the EDIT knobs own the
sound, and scene A takes over from the first click up. So capturing a scene never leaves a knob
dead -- but parking the morph at 0 is not a way to recall scene A either.

### Macros

A **macro** is one knob that moves up to 4 parameters at once, each towards its own target value.
There are four, on encoders A-D in PERFORM. At 0 a macro does nothing, so its parameters still
follow their EDIT knobs and the morph; turned up, it sweeps them from wherever they are. A new
project has three: **B ACID**, **C DUB** and **D DRIVE**. Macro A is left for you, and pressure on
the selected track's pad pushes it up while you lean on it.

### MUTATE

<p><img src="screens/mutate.png" width="256" alt="GRID after MUTATE: MUTATE T1 VELOCITY"></p>

<sub>Right after the press: what was mutated, on which track. Then `OK KEEPS CLR REVERTS` until you answer, with the MUTATE pad lit bright.</sub>

**MUTATE** (upper pad 7) makes a seeded variation of the selected track: rotate it, add or
remove hits, or nudge velocity, timing, probability or notes within the scale. It is one undo
level: OK keeps it, CLR takes it back.

### Kits

A **kit** is the sound side of a project without the patterns: the eight machines, their 16
parameters each, the faders, the sidechain duck and the sample paths. Besides the project's own kit there is a
**kit library** of 16 slots on the card, shared by every project, so a sound you built in one
project can be recalled in another (KIT mode, `FUNC+EDIT` twice).

For one track rather than eight there are the **factory sounds**: OK in EDIT or MACHINE on a drum
or VA track lists ready-made sounds for its machine and loads one into that track only (section 3,
[Factory sounds](#factory-sounds-one-track-at-a-time)).

### On the card

Everything STRATA keeps is on the SD card, never in the unit's flash:

```text
/ENAGA/APPS/STRATA/APP.ENA                     the app
/ENAGA/DATA/STRATA/SETTINGS.TXT                which project was open last, counters
/ENAGA/DATA/STRATA/PROJECTS/<NAME>/PROJECT.BIN tempo, sends, master, chain, mute states
                                  /KIT.BIN     machines, parameters, faders, sample paths
                                  /PERFORM.BIN scenes and macros
                                  /BANK_A.BIN ... BANK_H.BIN   16 patterns each
/ENAGA/DATA/STRATA/AUTOSAVE/                   the working copy, same files
/ENAGA/DATA/STRATA/KITS/KIT_01.BIN ... KIT_16.BIN   the kit library
/RECORD/STRATA_<n>.WAV                         resampled takes
```

The first project is called `DEFAULT`. A new project is named `NEW1`, `NEW2` and so on. Give a
project its own name with SAVE AS and pick another one on the PROJECTS page (section 6). STRATA
reopens whichever was open last.

---

## 2. Your first beat

This walks through the factory project, which is what a first start and every new project
contain. It takes about fifteen minutes.

**What is in it.** Bank A holds five patterns in C minor at 130 BPM:

| Slot | Name | What it is |
| --- | --- | --- |
| A01 | GROOVE | Four on the floor, backbeat clap, 16th hats with offbeat opens, a rolling bass, a dub chord stab, a rim and a few loop slices |
| A02 | GROOVE+ | The same beat developing: a kick pickup every fourth bar, a hat roll, a clap on FILL, a 12-step perc line and a 7-step loop against the 16 |
| A03 | BREAK | Kick and bass out; long dub chords, a perc ticking at 35% probability |
| A04 | PEAK | Everything up: ratcheted hats, an acid bass line, a rave stab, kick and clap rolls on FILL |
| A05 | PAD | Held chords over a quiet beat: Cm9, Abmaj7, Ebmaj7 and Gm7, a bar each, swelling in and ringing into the next. The sound of the [ASR page](#held-notes-the-asr-page) |

It also ships a chain (GROOVE x2, GROOVE+ x2, BREAK, PEAK x2, GROOVE+, about two minutes) that
starts **off**, eight mute states, and scenes 2-4: **DUB** (chord, rim, loop and clap pushed into
the sends), **DARK** (filters closed) and **TENSION** (the bass opens and screams). Scene 1 is
empty, so the morph starts from the kit as it is. Three macros are set up: **B ACID** (the bass
filter), **C DUB** (the chord and rim into the sends) and **D DRIVE** (the kick and hats). Track 8 plays a built-in placeholder of eight
short blips until you give it a sample (section 6).

### 1. Play it

Press **PLAY**. A01 starts; the top line reads `A01 1.1 130.0 > 20%` (pattern, bar.beat, tempo, `>`
playing, audio load), a thin line under it runs across the pattern, and the column at the right
edge is the master level. Turn **VALUE** to change the tempo; push and turn VALUE for swing. **STOP** stops
and lets tails ring; STOP again cuts them.

### 2. Edit steps

<p><img src="screens/grid.png" width="256" alt="GRID: eight tracks, the selected one highlighted"></p>

<sub>GRID: one row per track, the playhead on the top row; under it the selected track (`T1`), its step page, length and speed, and the four knobs of the current EDIT page.</sub>

Press **HOME** for GRID: all eight tracks as rows, the selected one highlighted. Tap lower pad 2
to select the clap. Its steps show on the STEP LEDs; tap **STEP 8** to add a clap, tap it again to
remove it. The pad-row keys (or LEFT/RIGHT) move between the four 16-step pages of a 64-step track; `L16` on screen
is the track's length.

Clear the whole track with **CLR**, and get it back with **FUNC+CLR** (undo). FUNC+OK is redo.
There are 16 levels.

To start from an empty pattern, clear all eight tracks at once: **FUNC+HOME**, DOWN to **CLEAR
PATTERN**, then **OK** twice within 2 seconds (`OK AGAIN TO CLEAR`, then `CLEARED FUNC+CLR UNDO`).
Every trig and lock goes; the sounds, and each track's length, speed and swing, stay. FUNC+CLR
brings the whole pattern back in one undo.

### 3. Lock a parameter

Select the kick (lower pad 1) and press **EDIT**. The screen keys (or LEFT/RIGHT) flip the
pages SRC, FLTR, AMP, FX; go to **AMP**. Now hold **STEP 13** and turn **A** (DEC) down: the last
kick of the bar is shorter. That is a lock. GROOVE already has one on step 9; hold STEP 9 and push
**A** to remove it (`LOCK CLEARED`).

The encoders work the same way in GRID: they edit the current EDIT page of the selected track.

### 4. Change a note, open the step page

<p><img src="screens/step.png" width="256" alt="The step page for step 1 of track 1"></p>

Select the bass (pad 6). Hold a STEP that has a note and turn **VALUE**: that changes the step's
note. Hold the STEP and press **OK** to open the **step page**: A-D edit NOTE, VEL, LEN, MICR;
the screen keys flip to PROB, COND, RTCH, RAMP and then ACNT, SLID, CHRD. LEFT/RIGHT walk to
the neighbouring steps. Turn SLID on for a 303-style slide into the next note. OK or CLR closes
it. The page answers A-D whether or not you are still holding the STEP.

**A note's length from the STEP keys:** hold the STEP with the note and press a later STEP. The
screen says `LEN 4 STP` (or whatever reaches that far), and when you let go the note lasts up to
the end of the step you pressed. The table has 1, 1.5, 2, 3, 4, 6, 8, 12, 16 ... 64 steps, so a
span between two of them takes the shorter one: the note never runs past the key you pressed. The
pair is still a range (`SEL 1-5`): press CLR, copy or turn A/B while holding and it is only a
range, with no length set; copy or transpose it later and the old LEN goes back first. With SUS up
on a VA this is how a held pad is written in.

### 5. Play it in

<p><img src="screens/keys.png" width="256" alt="KEYS: the pads as a keyboard"></p>

Press **HOME twice quickly** for KEYS. The two pad rows become a piano keyboard: the lower pads
are the white keys C to C, the upper pads above them the black keys (C#, D#, F#, G#, A#), and the
two upper pads with no black key are octave down (between E and F) and octave up (after B). Pads
play at the strength you hit them and hold the note while down; the pads in the scale light in
the track's colour, the root brightest; the screen draws the same keyboard. LEFT/RIGHT change
octave, UP/DOWN the track, and FUNC + a pad still mutes on the bar. The STEP keys are the track's
steps, as on GRID, with the pad-row keys for the step page, so you can play a line on the pads and
place or fix steps at the same time. Press **REC** (it lights red) while playing and what you play is
recorded onto the selected track, quantized by the SETUP page's QUANTIZE amount (100% by
default). Pads record too, with their velocity. REC again stops. Turning A-D while recording
writes locks onto the steps as they pass (motion recording).

### 6. Change pattern

Press **PROJECT**. The 16 slots of bank A are on screen and on the STEP keys. Press **STEP 2**:
`A02 END` shows, and GROOVE+ takes over at the end of the current pattern. Encoder **A** picks when
a change lands: END, after 16 or 32 steps, or NOW. Go on to A03 and A04. In PEAK, hold the
**FILL** pad (upper pad 1) for the last bar to hear the kick and clap rolls.

To let STRATA play the arrangement itself, press **FUNC+HOME**, scroll to CHAIN and press OK
(`CHAIN ON`), then CLR to close the menu.
**FUNC+STOP** stops and resets everything, and puts a running chain back to its first entry.

### 7. Perform

<p><img src="screens/scene.png" width="256" alt="SCENE A held: STEP PICKS"></p>

<sub>Every performance pad names itself when pressed: here SCENE A, held.</sub>

Back in A01, press **FUNC+MIXER** for PERFORM. Turn **VALUE**: the bar at the top moves from
scene A (1, empty) to scene B (2, DUB), and the groove fades into a dub version of itself. Hold
the **SCENE B** pad (upper pad 5) and press **STEP 3** to put DARK on the B side instead, or
STEP 4 for TENSION.

Still in PERFORM, turn **B** up slowly: ACID opens the bass filter, adds resonance and filter
envelope. **C** pushes the chord and the rim into the delay and reverb, **D** lengthens and drives
the kick and lets the hats ring. Turn a macro back to 0 and the sound is exactly where it was.

Then try the rest of the upper row, in any mode:

- **FUNC + lower pad 1** mutes the kick on the next bar; again unmutes it.
- **MUTE** pad (upper 3), then **STEP 2**: recalls mute state 2, the kick-less breakdown.
  STEP 1 brings everything back. MUTE again returns the pads to playing.
- Select the rim (pad 5) and hold **THROW** (upper 6): the rim goes fully into the delay while
  held. FUNC + THROW throws into the reverb.
- Hold **REPEAT** (upper 2) for a beat repeat of the master; turn VALUE while holding it to pick
  1/4 to 1/16T.
- Select the bass and tap **MUTATE** (upper 7). Like it: OK. Don't: CLR.
- Hold **FUNC** and push encoder **A**: the lows are gone (`KILL LOW`). Push FUNC+A again on the
  one and they slam back (`LOW BACK`). FUNC+B and FUNC+C do the same for the mids and the highs.

### 8. Save

Press **FUNC+HOME** (the FUNC menu opens on SAVE), then **OK** (`SAVING`). The PROJECT screen
stops saying `UNSAVED`. STRATA also autosaves whenever the transport stops.

---

## 3. The machines

Each machine uses the SRC and AMP pages; FLTR and FX are the insert chain (section 1). On every
machine, AMP's first knob is the decay and its last is LVL, the output level (square law, so
the quiet end has room). Where a machine follows the trig note, note 60 (C4) plays the knob's own
pitch and other notes transpose from there.

**TUN as a note.** TUN (PIT on SAMPLE and GRAIN) shows that pitch as a note: where TUN puts C4,
spelled as on the step page (`C-4`, `F#2`). On a drum that is the drum's pitch: KICK at 64 reads
`A-1` (55 Hz). On a melodic machine it is the transposition: VA MONO at 64 reads `C-4` (none), an
octave up `C-5`. In this NOTE view a knob click moves TUN one
semitone, to the setting closest to that note, so tuning a kick or a tom to the key is a few
clicks. TUN has 128 settings across its range, so not every note falls exactly on one: a `+` or
`-` after the note means the setting is 5 cents or more sharp or flat of it (VA MONO's fifth,
`G-4+`, is 12.5 cents sharp). For SAMPLE and GRAIN the name assumes the sample itself is a C.
**Push the TUN knob** (on EDIT, GRID or KEYS) to see the number 0-127 instead and turn it a count at
a time, for tuning between notes; push again for notes. NOTE view is on at every start and
applies to every track. Locks follow it: hold a STEP and turn TUN in NOTE view, and the lock
moves in semitones.

The drum machines (KICK, SNARE, HAT) follow Mutable Instruments Plaits' models, reimplemented;
docs/SOUND.md has the details.

### KCK: KICK

A 909-style kick: an oscillator with a pitch sweep, a click, and a body that goes from sine to
distorted. Two voices, so a ratchet does not cut itself off.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** pitch, 25-120 Hz (55 Hz at 64) | **FMA** pitch-sweep amount, up to about +26 semitones | **FMD** sweep time, 8-128 ms | **CLK** click level |
| AMP | **DEC** decay, 20-640 ms time constant | **TON** body low-pass: thud to open | **DRT** dirt: clean sine to soft-clipped | **LVL** |

The trig note transposes the kick, so a kick follows the key. FMD in the middle of the knob is
where the classic click lives. Add DRV on FLTR for more 909 grit.

### SNR: SNARE

A 909-style snare: a tuned pair under filtered noise, with a clap mode. Two voices.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** 90-439 Hz (200 Hz at 64) | **SNP** snappy: body to noise | **TON** noise band, ±2 octaves | **CLP** clap: under 43 off, 43-84 three bursts, 85+ four |
| AMP | **DEC** decay | **SPR** clap spread, 2-40 ms between bursts | **BDY** longer body tail | **LVL** |

The factory clap is CLP 64 with SNP high and SPR around 10 ms.

### HAT

The 808 recipe: six square oscillators through a band-pass, plus optional noise. Two voices.
**Track 3 chokes track 4**: a hit on track 3 cuts track 4's ring short, which is how a closed hat
closes an open one.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** 180-940 Hz | **TON** band-pass, 150 Hz-9.6 kHz | **NOI** squares to noise | **DTN** detune: near unison, 808 (64), clangy |
| AMP | **DEC** decay, 7 ms-0.9 s | **CHO** how fast a choke cuts it, 2-120 ms | **CHK** choke group, 0-4 (shown; the choke itself is track 3 onto track 4) | **LVL** |

### PRC: PERC

Two-operator FM percussion: rims, toms, cowbells and blips. Two voices.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** carrier, 48-1822 Hz (300 Hz at 64) | **RAT** ratio: lower half steps through 16 useful ratios, upper half sweeps 4-16 | **IDX** FM depth: brightness | **IDD** how fast the brightness falls |
| AMP | **DEC** decay, 4-600 ms | **SWP** pitch sweep, up to +36 semitones | **SWT** sweep time, 1-200 ms | **LVL** |

Rough guide: toms around TUN 20-45 with a whole-number RAT and some SWP; rims and cowbells around
TUN 70-90 with an odd RAT; blips at TUN 100+ with high IDX and short IDD. Velocity also brightens
a hit.

### SMP: SAMPLE

Plays a sample from RAM, whole or in slices. Four voices.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **PIT** ±24 semitones (unity at 64) | **STA** start | **LEN** length of what is left after STA (127 = to the end) | **SLC** slices, 1-16 |
| AMP | **ATK** attack (0 = none) | **DEC** decay, 5 ms-16 s; 127 = hold | **PLY** forward, forward loop, reverse, reverse loop (four zones of 32) | **LVL** |

With **SLC 1** the sample is one-shot and the trig note transposes it. With **SLC 2-16** the
window is cut into equal slices and the **note picks the slice**: C4 (60) is slice 1, C#4 slice
2, and so on, so a chopped loop lies across the notes in order. The factory LOOP track uses 8
slices. Loop modes repeat the window or the slice.

**SLC AUTO.** Equal slices only fit a loop whose hits sit on an even grid. A played break does
not, so its slices start before or after the hit they were meant to hold. Push **D** (the SLC
knob) on the SRC page to turn **AUTO** on: SLC then cuts at the hits STRATA found in the sample
when it loaded, instead of evenly. SLC still sets how many slices: at SLC 8 the window is cut at
its 7 strongest hits, in order. The line under the knobs says `SLC AUTO: 12 HITS` (up to 15 are
found per sample), and with fewer hits inside the STA/LEN window there are fewer slices. With no
hit in the window (`NO HITS`, or a window between two hits), it cuts evenly. Push D
again for even slices (`SLICES EVEN`). AUTO is saved with the kit. A streamed file has no
slices either way.

### GRN: GRAIN

Granular playback of the track's sample: up to 8 overlapping grains in one voice. The most
expensive machine (section 7). Every knob shows its unit on screen: milliseconds, `X2.5` for
grains in the air, `HOLD`, a share in `%`.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **POS** position in the sample | **SIZ** grain size, 8-680 ms | **DEN** grains in the air, X0.3-X8.0: under X1 leaves gaps (a stutter), X2 and up is a smooth cloud | **SPR** spray, 0-500 ms: chorus, cloud, wash |
| AMP | **PIT** ±24 whole semitones | **SHP** grain shape: smooth to plucked | **DEC** 5 ms-15 s; 127 = `HOLD` | **LVL** |
| ASR | **ATK** attack, 0 (none) to 5 s | **SUS** sustain: 0 leaves DEC in charge; up holds the cloud while a KEYS pad or the step's LEN lasts | **REL** release, 180 ms-10 s | |
| OSC | **SCN** scan: the read head moves from POS while the note sounds, `OFF` to 200% of real time (100% at 90) | **PSP** pitch spray: 1-63 detunes each grain by up to a semitone (`D25`), 64-127 sends up to half the grains an octave up (`OCT25`, a shimmer) | | |

**Where to start:** press **OK** in EDIT for the GRN SOUNDS list (below), or switch a track to GRN,
which loads CLOUD. Then turn POS to the part of the sample you want. Rough guide: SIZ around 75 ms
and DEN X3 or more for a cloud, SIZ 250 ms and up with little SPR for a pad or a freeze, DEN under
X1 with SHP up for stutters and chops, SCN for a stretch.

SCN is free-running, not tempo-locked. To make a cloud move in time, **lock POS on steps**, or
motion record a sweep of POS. The trig note transposes, so a cloud plays chromatically.

### MNO: VA MONO

The bass and lead voice: a saw or pulse, a second oscillator (detuned or hard-synced), a sub an
octave down and a little noise, driven into its own resonant filter with an envelope. One voice,
with accent, slide and glide.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** ±24 semitones | **MOD** voice filter: 0-63 LP24, 64-95 LP12, 96-111 BP12, 112-127 HP12 | **CUT** cutoff, 35 Hz-12 kHz | **RES** resonance |
| AMP | **DEC** decay of both envelopes, 15 ms-2.4 s | **ENV** filter envelope amount, up to +66 semitones | **GLD** glide, 0-0.5 s | **LVL** |
| ASR | **ATK** attack | **SUS** sustain: 0 = one-shot | **REL** release | **FSU** filter sustain |
| OSC | **WAV** saw, square, pulse | **OS2** off, 1-63 detune, 64-127 sync | **SUB** sub level | **DRV** drive |

With SUS at 0 (the default) DEC is the note's length. Turn SUS up for long, held notes: see
[Held notes: the ASR page](#held-notes-the-asr-page).

This filter is the voice's own, on SRC; the FLTR page's filter is a second, utility filter after
it. ENV 0 is a static dub bass; ENV and RES up is the acid squelch; RES at the top
self-oscillates. **Accent** (ACNT on the step page) makes the note louder, opens the filter
further and brings the noise (and, with OS2 in sync, the synced pulse) forward; **slide** (SLID)
glides into the next note. The OSC page is described in
[The oscillators and drive](#the-oscillators-and-drive-the-osc-page).

### PLY: VA POLY

The same voice as VA MONO, up to six notes at once, for chords. Each step can play a chord
(CHRD on the step page: MIN, MAJ, SUS2, SUS4, DIM, MIN7, DOM7, MAJ7, MIN9, MIN11, 5TH, OCT, 5TH OCT,
each with inversions `/n`).

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** | **MOD** as MNO | **CUT** | **RES** |
| AMP | **DEC** with SUS at 0, the chord's length: short stab or long ring | **ENV** | **DET** voice detune, up to a quarter semitone | **LVL** |
| ASR | **ATK** attack | **SUS** sustain: 0 = one-shot | **REL** release | **FSU** filter sustain |
| OSC | **WAV** | **OS2** as MNO | **SUB** | **DRV** |

For pads, turn SUS up on the ASR page: a chord then holds for its step's LEN, or for as long as you
hold the keys ([Held notes: the ASR page](#held-notes-the-asr-page)).

Every note is a sustaining voice and costs CPU while it rings. All VA POLY, WAVE POLY and FM4 tracks
together share a budget of **12 voices**; past it, the quietest note is faded out quickly to make room. A longer
DEC, a long LEN with SUS up, or a long REL all mean more notes overlap.

### WMN and WPL: WAVE MONO and WAVE POLY

The VA voice with a **wavetable** where its two oscillators were: 16 tables of 16 frames each,
built into STRATA (no files needed), from PolyWerk's recipes. A table is computed the first
time a track plays it, which takes a moment: until then you hear only the sub. POS moves smoothly through a
table's frames; MOT lets the filter envelope move it, so a note's timbre changes as it plays.
Everything else is the VA's: the sub, the 24 dB filter with its envelope, the ASR page, accent,
slide and glide on WMN, chords and the shared 12-voice budget on WPL.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TBL** the table, eight counts each (shown by name) | **POS** position, frame 0-15 | **CUT** cutoff, LP24 | **RES** resonance |
| AMP | **DEC** as MNO | **ENV** filter envelope amount | **MOT** envelope to position: 64 off, up to +15 or -15 frames | **LVL** |
| ASR | **ATK** | **SUS** | **REL** | **FSU** as MNO |
| OSC | **WRP** bends the cycle (0 = none; high notes bend less, to stay clean) | **DET** a second, detuned copy: OFF, up to 60 cents | **SUB** sub level | WMN: **GLD** glide; WPL: **DRV** drive |

The tables, from TBL 0 up:

| TBL | Table | From frame 0 to 15 |
| --- | --- | --- |
| CLS | CLASSIC | sine, triangle, saw, square |
| PWM | PWM | a square narrowing to a thin pulse |
| SYN | SYNC | a hard-synced saw, the sync ratio rising |
| FMT | FORMANT | a vocal formant sweeping up |
| RSW | RESO SAW | a saw with a resonant peak sweeping up: a filter sweep with no filter |
| HRM | HARMONICS | organ-like, more harmonics drawn in |
| FLD | FOLD | a sine wavefolded harder and harder |
| PHD | PHASE DIST | CZ-style phase distortion, soft to sharp |
| FM1 | FM RATIO | two-operator FM, the index rising |
| TIN | FM TINE | an electric piano's tine: a sine gaining its metallic partials |
| DGT | DIGITAL | eight stepped digital spectra |
| VOW | VOWELS | "oo" to "ah" to "ee" |
| GRL | GROWL | a bass growl, darker to snarling |
| EPN | E-PIANO | mellow to barking |
| PLK | PLUCK | a plucked string, bright to dull (and quieter as it dulls) |
| MLT | MALLET | soft mallet, marimba, vibraphone, xylophone, glockenspiel, kalimba |

MOT above 64 moves the position up as the filter envelope rises and back as it falls; below 64 it
moves it down. With PLK at POS 127 and MOT 0, each note starts bright and dulls like a string.
The SOUNDS list (OK on MACHINE) has 8 WMN and 7 WPL starting points.

### FM4: four-operator FM

Four sine operators in the eight classic 4-operator algorithms: e-pianos, bells, glass, brass,
organs and FM basses. Six voices with chords, like PLY, in the same shared 12-voice budget. The
operators come in two pairs so it stays dialable: the carriers sit at **CAR**, the modulators at
**RAT** (plus **FIN**), and **DET** pulls the second of each pair sharp. **IDX** is how bright
the tone is; the index has its own envelope, faster than the volume's, so a note strikes bright
and mellows as it rings (**IEN** says how much).

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **ALG** algorithm `A1`-`A8` | **RAT** modulator ratio: 0.5 1 2 3 3.5 4 7 14 | **IDX** FM index (brightness) | **FBK** operator 4 feeding back on itself: a saw-like buzz, to noise |
| AMP | **DEC** decay, 15 ms to 8 s | **IEN** how much of IDX the index envelope moves (0 = a fixed timbre) | **VEL** velocity to index: harder is brighter | **LVL** |
| ASR | **ATK** | **SUS** (0 = one-shot) | **REL** | **ISU** index sustain: where the brightness settles while held |
| OSC | **DET** second pair sharp: `OFF`, up to `D25` cents | **FIN** fine ratio, ±0.5 around RAT (`0` at 64) | **BRT** keytrack: high notes get less index, so they stay sweet | **CAR** carrier ratio: 1, 0.5, 2, 3, 4 |

The algorithms: `A1` one stack 4>3>2>1 (the brightest), `A2` 3 and 4 into 2 into 1, `A3` 3>2>1
with 4 into 1, `A4` 4>3>1 with 2 into 1, `A5` two pairs 2>1 and 4>3 (the e-piano), `A6` 4 into
three carriers, `A7` one pair 4>3 over two plain sines, `A8` four sines (an organ: RAT sets the
second drawbar). The SOUNDS list has EPIANO, BELL, BASS, BRASS, ORGAN, MALLET, PAD and GLASS.
FBK costs a little more CPU than FBK 0.

### MID: MIDI

MID makes no sound of its own. It is for a track whose sound is an outboard synth, a drum
machine or a modular: the track's notes go to MIDI OUT and STRATA spends no voice and no CPU on
them. Give the track a channel first (MACHINE, encoder **B**); MACHINE shows `B: MIDI OUT OFF`
brightly on a MID track that has none, because then it does nothing at all.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **C74** CC 74 (brightness, cutoff) | **C71** CC 71 (resonance) | **C73** CC 73 (attack) | **C72** CC 72 (release) |
| AMP | **C70** CC 70 | **C75** CC 75 | **C76** CC 76 | **C77** CC 77 |

These are the General MIDI 2 sound controllers, which many synths answer without any setup; on
others, MIDI-learn them. A CC is sent when its value **moves** (a knob, a lock, a scene, a macro),
never just because a track became MID, so a synth you set up by hand keeps its sound until you
touch a knob. A step's locked CCs go out just before its note. FLTR and FX do nothing on MID.

MID sends its notes as written, without folding them into the scale (the receiver may be a drum
machine, where a note picks a sound). A step with a chord sends the whole chord.

### MIDI out on any track

Every machine can send, not only MID: give a KCK or PLY track a channel and its notes go to MIDI
OUT as well as sounding, to double a part on a synth. A track sends every note it plays: steps
(with their velocity; an accent sends 127), ratchets, chords (the whole chord, even past the
12-voice budget, which only counts STRATA's own voices), every arp hit and note repeat hit, pad
hits, KEYS and MIDI IN notes.

Every note gets its note-off. A step ends after its LEN (one step if it has none, and so does a
pad hit); a KEYS pad or a MIDI IN note when you let go. Muting the track, changing its channel or
machine, recalling a kit and STOP end its notes at once. The **second STOP** and **FUNC+STOP** also
send All Notes Off on every channel a track uses, for a synth that missed something. Notes leave
within one audio block (1.3 ms) of their step. The channels are saved with the project.

**With Enaga 0.2.0 firmware, unplug USB while driving gear from the MIDI OUT jack**, or keep a
program on the computer reading the SmplTrek's USB MIDI port. On 0.2.0 a computer that is not
reading fills the USB queue and stops the DIN jack with it; Enaga fixed this after the release.

### The key and the scale

Each pattern has a key (root) and a scale, set in PROJECT with encoders **B** (key) and **C**
(scale). The melodic machines **fold** their notes into the scale: the VA, WAVE and FM4 machines, GRAIN,
and SAMPLE when it is not sliced (SLC 1). Drums and sliced samples are not folded, since there the
note picks a pitch or a slice rather than a melody. Scales: CHROMATIC, MINOR, MAJOR, DORIAN,
PHRYGIAN, LYDIAN, MIXOLYDIAN, LOCRIAN, HARM MINOR, MEL MINOR, MINOR PENT, MAJOR PENT, BLUES,
WHOLE TONE, DIMINISHED. Notes are named with sharps (D#, not Eb).

TUN is applied **after** the fold: the sequencer folds each note into the key, then the machine
transposes it by TUN. So TUN moves the whole line chromatically, and in NOTE view it steps through
all twelve notes, not just the scale's. Moving a line by a scale step would not keep it in the key
(A minor moved up a whole step is B minor, with C# and F#); only an octave does, which is twelve
clicks. To keep a melodic track in key, leave TUN at `C-4` or an octave of it and write the notes;
on drums, which are not folded, TUN is simply the drum's pitch and can go to any note of the key.

### Changing a track's machine

<p><img src="screens/machine.png" width="256" alt="MACHINE for track 1"></p>

`FUNC+EDIT` opens MACHINE. The arrows move over the grid of machines (UP/DOWN one, LEFT/RIGHT a
column of three) and encoder A walks it too; you hear the change at once. The title says which
track you are changing (`MACHINE T1`). To change another track, press its pad or use the screen
keys, which say `TRACK 2` and so on. Encoder **B** sets the track's MIDI
OUT channel: OFF, or 1-16. `FUNC+EDIT` again opens the kit library (section 6).

### Factory sounds: one track at a time

<p><img src="screens/sounds.png" width="256" alt="SOUNDS on track 1, the kick"></p>

Every drum machine, both VAs, both WAVE machines, FM4 and GRAIN come with a short list of ready-made sounds.
On a KCK, SNR, HAT, PRC, MNO, PLY, WMN, WPL or FM4 track, press **OK** in EDIT or MACHINE: **SOUNDS** lists them.
On a GRN track it is **OK in EDIT** (OK in MACHINE opens the sample browser). UP/DOWN or A move
the cursor, **OK loads** the one under it into this track, with the beat playing, and the list
stays open so the next OK tries the next one. `NOW` marks the sound the track has. The first row,
**AS WAS**, is the track as it sounded when you opened the list: OK on it puts that back, so
trying sounds never loses yours. LEFT/RIGHT or the screen keys move to the next track, which gets
its own AS WAS. CLR goes back.

A factory sound sets the machine's SRC and AMP pages, the insert chain's FLTR page (drive and
filter) and, on the VA, WAVE, FM4 and GRAIN machines, the ASR and OSC pages. It leaves the FX page (lo-fi, pan and both sends), the LFO,
the ARP, the fader, HAT's choke group and GRAIN's POS and PIT alone, so the track keeps its place in the mix and a cloud keeps
reading the part of the sample you chose. The
first entry of each list is the factory kit's own sound.

| Machine | Sounds |
| --- | --- |
| KCK | 909 (the factory kick), 808 (long boom), DEEP (rounder, darker), PUNCH (short, hard, driven), DIST (industrial, distorted), SOFT (dub, no click) |
| SNR | CLAP (the factory clap), 909SN, 808SN, CLAP4 (four bursts, wide), RIMSN (a snare rim tick), DIRTY (noisy, driven) |
| HAT | CLOSED, OPEN (the factory pair), TIGHT, 808HH (pure squares), SHAKE (a shaker), RIDE (long, clangy), TRASH (noisy, metallic) |
| PRC | RIM (the factory rim), TOM, COWBEL, BELL, BLIP, CONGA, METAL |
| MNO | ROLL (the factory bass), ACID (squelching 303 line), ACIDSQ (the same on a square), FAT (detuned saws, sub and drive), REESE (wide detuned band-pass), SUB (deep, near-sine), PLUCK, GROWL (band-pass mid bass), GLIDE (portamento), SYNC (sync lead), LEAD (square lead with glide), DRONE (SUS up: holds each note for its LEN) |
| PLY | STAB (the factory stab), PAD (A05's pad), STRING, KEYS, RAVE (best with 5TH OCT chords), PLUCK, BRASS |
| WMN | SWEEP, RESO (a resonant sweep), GROWL, FORMNT (formant), DIGI (digital), VOWEL, WARP, FMBASS (FM bass) |
| WPL | PAD, EPIANO (electric piano), BELL, MALLET, PLUCK, CHOIR, PWMPAD (a PWM pad) |
| FM4 | EPIANO, BELL, BASS, BRASS, ORGAN, MALLET, PAD, GLASS |
| GRN | CLOUD (where to start), PAD (swells, holds for LEN), FREEZE (one moment held), SHIMMR (a third of the grains an octave up), DRIFT (a slow scan), STRTCH (half speed, own pitch), STUTTR (gapped plucked grains), CHOPS (short percussive grains for POS locks) |

The MNO list is longer than the screen: it scrolls as the cursor moves. PAD, STRING, KEYS, BRASS,
FAT, REESE, ACID, SYNC, LEAD and DRONE sustain (the ASR page's SUS is up), so their notes last as long as
each step's LEN: give the steps long LENs, or hear PAD on A05. A loaded sound is a change to the
kit like turning knobs: it is saved with the project, and FUNC+CLR does not undo it (AS WAS does).

---

## 4. The sequencer in depth

### Steps

Tap a STEP for a trig; tap again to remove it. **FUNC + tap** makes a **trigless** step: it plays
no note but its locks still apply, which is how you change a sound mid-note or set up the next
hit. The STEP LEDs show: green = trig, as bright as its velocity (four steps: soft, medium,
loud, accent; an accented step is brightest), blinking = trig with a condition or probability,
amber = trig with locks, red = trigless, bright red = the playhead.

The step page's fields:

| Field | Range | What it does |
| --- | --- | --- |
| NOTE | 0-127 | The note (also: hold STEP + turn VALUE) |
| VEL | 1-127 | Velocity |
| LEN | 1/16 step ... 64 STP | Note length: how long a POLY machine holds, a MONO one too once SUS is up (the ASR page), and the MIDI note-off |
| MICR | ±11 ticks | Micro-timing: 24 ticks is one 16th |
| PROB | 0-100% | Chance the step plays |
| COND | see below | Trig condition |
| RTCH | 1-8 | Ratchet: repeats within the step |
| RAMP | FLAT, UP, DOWN | Ratchet velocity ramp |
| ACNT | on/off | Accent |
| SLID | on/off | Slide (VA MONO) |
| CHRD | 1 NOTE, MIN ... 5TH OCT | Chord (VA POLY) |

### Trig conditions

- `a:b`: plays on pass a of every b (b = 2-8). `1:4` plays on the first of every four passes.
- `FILL` / `NOT FILL`: plays only while FILL is held (or auto-fill is on), or only while it is not.
- `PRE` / `NOT PRE`: follows this track's last condition result, to chain decisions.
- `NEI` / `NOT NEI`: follows the result of the track to the left (track n-1).
- `1ST` / `NOT 1ST`: plays only on the first pass after a pattern change, or never on it.

Conditions and probability are seeded from the pattern, so a pattern plays the same way after a
reload.

**Without the step page:** hold a STEP and press an upper pad. FILL steps the condition through
FILL, NOT FILL and none; MUTE through 1:2, 2:2, 1:4, 4:4 and none; REPEAT the ratchet through 2, 3,
4 and off; MUTATE the probability through 75, 50, 25 and 100%. The bottom line says where it
landed (`STEP 5 PROB 50%`). The order is the rule: STEP first edits the step, pad first performs
(FILL held, then a STEP, writes a FILL trig). The step page still has every value.

### Ranges, copy and paste

Hold two STEP keys to select the range between them (`SEL a-b`). Then CLR clears it, FUNC+UP
copies it, FUNC+DOWN pastes it, encoder A transposes it and encoder B nudges its micro-timing.
With nothing held, CLR clears the whole track and FUNC+UP / FUNC+DOWN copy and paste the whole
track. The FUNC menu's CLEAR PATTERN (OK twice) clears every track at once. Hold a STEP and press UP / DOWN to copy / paste one step. Everything goes through undo.

### Length, speed and swing

<p><img src="screens/pattern.png" width="256" alt="PROJECT's PATTERN page"></p>

- `FUNC + pad-row keys`: the selected track's length, 1-64 steps.
- The FUNC menu's TRACK SPEED row: its speed, 2x, 1x, 3/4, 1/2, 1/4, 1/8 (LEFT/RIGHT).
- `VALUE push + turn`: the pattern's swing, 50-75%, on the even 16ths.
- PROJECT, screen key RIGHT twice: the **PATTERN** page, where A sets the master length (LEN) and B
  the swing (SWG).
- **Each track can swing on its own**, on the same page: under LEN and SWG are eight TRACK SWING
  fields, `PAT` (follow SWG) or 50-75%. LEFT/RIGHT walk the fields, and C (or VALUE) changes the
  one under `>`; with `>` on LEN or SWG, C sets the selected track's. Hats at 66 over a straight
  kick and bass is the classic use. It is saved with the pattern and undoable.

A 12-step perc against a 16-step kick lands somewhere new each bar for three bars; GROOVE+ does
exactly this.

### Euclidean fill

In GRID, `FUNC + A / B / C` set hits, steps and rotation (`EUC 5/16 R2`), applied live to the
selected track. The steps number is a cycle, not the track's length: 3 in 8 on a 16-step track
plays the 8-step figure twice. Edit the result by hand afterwards.

### Pattern changes and the chain

In PROJECT, the pad-row keys pick the **shown** bank, which is not necessarily
the playing one; STEP 1-16 or the cursor plus OK queue a slot from it. CLR cancels a queued change
(`QUEUE OFF`). The slots show the playing pattern bright, patterns with trigs mid grey, empty ones
dark, and `05>` marks a queued slot.

The **CHAIN** page (PROJECT, screen key RIGHT) lists the entries, `>02 A05 X2 PLAY`. UP/DOWN move the
cursor, a STEP key puts that slot of the shown bank into the entry, LEFT/RIGHT step the entry's
pattern, A or VALUE set its repeats, OK adds a copy after the cursor (up to 64), CLR twice removes
the entry, and the FUNC menu's CHAIN row turns the chain on or off. Chain edits are not undoable.

**Song mode.** B sets the entry's **mute recall**: `--` leaves the mutes alone, `M1`-`M8` recalls
that stored mute state the moment the entry starts (the row reads `>02 A05 X2 M3`). Store an
intro without the kick in state 2 and a breakdown in state 3, point entries at them, and the
chain plays sections rather than just patterns. The FUNC menu's **CHAIN END** row switches the chain between `LOOP`
(round and round) and `SONG`: the chain plays through once, the transport stops at
the end of the last entry (`SONG END`), and the next PLAY starts it again from entry 1. Under an
outside clock that keeps running, STRATA goes quiet at the end and waits for PLAY or the outside
STOP. The header shows which (`CHAIN 3/64 ON SONG`). Both are saved with the project; a project
from before song mode loops, with no recalls.

### Recording

REC toggles live and motion recording. With the transport running, pad hits, KEYS and MIDI notes
are recorded with their timing, corrected for the pad's latency and quantized by QUANTIZE (SETUP
page, encoder A, 0-100%; 0 keeps your timing as micro-timing). A hit that cannot be stored where it
was played says `OFF GRID`. One recording pass is one undo level.

**How long you hold a note is recorded too**, on every machine but the drums: when the
pad, key or MIDI note comes up, its step gets the nearest LEN (from `1/16` of a step up to
`64 STP`), so with SUS up a held chord plays back as long as you played it. A drum ignores LEN,
so a drum hit keeps one step. Let go after REC is off, or with the transport stopped, and the step
keeps the length it had.

### MIDI in

MIDI IN (the DIN jack, or USB MIDI) plays STRATA like the pads:

- **Notes**: channels 1-8 play tracks 1-8; other channels play the selected track. A note is held
  until its note-off, so a pad or a chord on a VA, WAVE, FM4 or SAMPLE track sustains under your finger, and
  it is let go on the track it started on even if you have selected another one since. Notes are
  recorded like pad hits while REC is on.
- **CC 16-31** set the track's sixteen parameters in page order: 16-19 SRC, 20-23 FLTR, 24-27
  AMP, 28-31 FX. They change the kit, as turning the knob would (they are not motion-recorded).
- **CC 120 and 123** (All Sound Off, All Notes Off) let go of every note MIDI IN is holding on
  that channel. Enaga sends 123 by itself when a keyboard's active sensing stops.
- **Program change** on your MIDI CH (the Enaga menu, SETUP shows it) queues pattern 1-128,
  which is A01 to H16, in the queue mode PROJECT has set, exactly as pressing the slot would.

---

## 5. Performing

The seven upper pads are the same in every mode:

| Pad | Plain | With FUNC |
| --- | --- | --- |
| 1 FILL | FILL conditions on while held | NOTE RPT on/off |
| 2 REPEAT | Beat repeat of the master while held; VALUE picks the length | Pitch drops as it repeats |
| 3 MUTE | MUTE MODE on/off: lower pads mute at once | |
| 4 SCENE A | Hold + STEP 1-16 puts that scene on side A | |
| 5 SCENE B | The same for side B | |
| 6 THROW | Selected track's delay send to full while held | Reverb instead |
| 7 MUTATE | Mutate the selected track; OK keeps, CLR reverts | Held: delay freeze |

### Mutes

- `FUNC + lower pad`: mute or unmute that track **on the next bar**.
- In MUTE MODE a lower pad mutes **at once**, and STEP 1-8 are the eight stored **mute states**:
  STEP recalls one at once, FUNC+STEP on the next bar, VALUE pushed + STEP stores the current mutes
  there. The factory states are: 1 everything, 2 no kick, 3 drums only, 4 no hats, 5 no bass,
  6 kick and bass, 7 the melodic parts alone, 8 the bare groove.

Mutes and mute states are saved with the project.

### Scenes and morph

In PERFORM (`FUNC+MIXER`) the top shows `A1 0P` and `B2 9P`: which scene is on each side and how
many parameters it stores, and the morph bar between them. **VALUE** morphs.

- **OK** captures the whole current sound into the scene on side A, including the master kill
  EQ's three knobs (not its kill switches).
- Hold a scene pad + **OK** captures only the selected track into that side's scene.
- Hold a scene pad + **CLR** erases that side's scene.

Because a whole capture holds the kill EQ's knobs, a scene captured with LOW turned down to
`KILL` turns the morph into a slow low-end fade: morph towards it over a build.

A useful habit: leave side A on an empty scene, build a sound, capture it into a scene on side B,
then undo your knob moves and morph back and forth. The morph position itself is not saved.

At rest (VALUE fully down) the morph hands every parameter back to its EDIT knob, so a capture
cannot leave the sound stuck. One click up and side A has it.

### Macros

In PERFORM the four macros sit across the screen: `>A`, `B`, `C`, `D`, each with `---` (empty) or
the number of parameters it drives (`3P`), and a bar showing its position.

**How a macro moves the sound.** Each parameter on a macro has a target value. At **0** the macro
writes nothing: the parameter is wherever its EDIT knob and the morph put it, and you can keep
shaping it. Turning the macro up sweeps the parameter from that live value towards the target,
reaching it at the top. So the first detent is always a small nudge from the sound you have,
never a jump, even after you have turned the knob in EDIT or moved the morph. Back at 0, the
sound is exactly as it was. A target can lie below the starting value, so one macro can open a
filter while it closes a send.

**The factory macros**, in every new project:

| Macro | Name | What it drives |
| --- | --- | --- |
| A | (free) | Nothing: yours to assign, and the one pad pressure bends |
| B | ACID | Bass (track 6) CUT, RES and ENV up: the 303 move |
| C | DUB | Chord (track 7) SNA and SNB, rim (track 5) SNA: into the delay and reverb |
| D | DRIVE | Kick (track 1) DEC and DRV up, closed and open hat DEC longer |

**Assigning your own:**

1. Select a track (lower pad) and a macro (LEFT/RIGHT).
2. Press a **STEP** key: STEP 1-4 are that track's SRC parameters, 5-8 FLTR, 9-12 AMP, 13-16 FX.
   The screen confirms with the count, for example `A T6 CUT 2/4`. Repeat for up to four
   parameters, on any tracks. The same STEP again takes that parameter off (`A T6 CUT OFF 1/4`).
   A macro with four already says `A FULL: CLR CLEARS` and changes nothing: the factory macro D
   uses all four, B and C three.
   **FUNC + STEP 1-4** assign the track's LFO settings instead, WAV RAT DEP DST: a macro on DEP
   fades a wobble in.
   **FUNC + STEP 5, 6 or 7** puts the master kill EQ's LOW, MID or HI on the macro (`A EQ LOW`):
   from flat, the macro sweeps that band down to a kill.
3. Turn the macro's encoder (A-D).

**Or learn it from EDIT:** in PERFORM, **push** the macro's knob (A-D). EDIT opens with
`LEARN B 0/4` on the second line. Now **push** any knob on any EDIT page, on any track (the screen
keys turn pages, UP/DOWN or a lower pad the track): that knob's parameter goes on the macro
(`B T3 CUT 1/4`); pushed again, it comes off. **CLR** or **HOME** ends it (`KNOBS EDIT AGAIN`),
as does any screen other than EDIT; a second CLR straight after does nothing, so it cannot clear
the track. A parameter on a macro that is turned up follows the macro, not its EDIT knob: turning
that knob says `HELD BY MACRO B`; turn the macro to 0 (or CLR it in PERFORM) to edit it again.

The target is whichever end of 0-127 is further from the parameter's value when you assign it,
so the knob always has somewhere to go: a filter already open gets a macro that closes it. If the
macro is already turned up when you assign, the parameter moves straight to where the macro puts
it. **CLR** clears the selected macro. Macro setups are saved with the project (PERFORM.BIN).

**Pad pressure** pushes **macro A** up while you lean on the selected track's pad, never below
where its knob is, and lets go back to the knob's setting. This is why A ships empty: assign it
to the selected track's sound and the pads become expressive.

### Beat repeat, throws and auto-fill

REPEAT loops the last 1/4, 1/8, 1/16 or 1/32 (or triplets) of the finished master while held;
release and the music is where it would have been. The FUNC menu's **AUTO-FILL** row sets
**auto-fill**: OFF, or FILL on the last bar of every 4, 8 or 16. PERFORM's lower lines show what
is active: `MUT NOTES T1 50`, `FILL AF16 1/16- DLY`, `BAR n`.

### Note repeat

**FUNC + FILL** turns **NOTE RPT** on (`NOTE RPT`, and `NRPT` after the bar number in PERFORM);
again turns it off (`PADS PLAY`). While it is on, holding a lower pad rolls that track's note at
beat repeat's length: 1/16 until you pick another with REPEAT + VALUE. With the transport running
the hits land on the beat grid, so a roll is always in time; stopped, it rolls from the moment you
press. Leaning on the pad sets each hit's velocity, so pressure swells and fades the roll (macro A
is left alone while a pad rolls). Hold two pads for two rolls. Rolls are played, not written: REC
does not record them.

### The master kill EQ

<p><img src="screens/mixer_eq.png" width="256" alt="MIXER's EQ page"></p>

The master has a DJ-style **kill EQ** (an isolator) after the DJ filter: three bands split at
250 Hz and 2.5 kHz, LOW, MID and HI. Each band has a knob and a kill switch.

- **The switches**: FUNC + push encoder A, B or C, in **any mode** (PERFORM included, where
  turning the encoders is still the macros). The notice says `KILL LOW` or `LOW BACK`. A switch
  latches: push to take the band out, push again to bring it back. On the MIXER's EQ page a
  plain push of A, B or C does the same.
- **The knobs**: MIXER, then the screen keys (or LEFT/RIGHT) to the last page, `EQ`. A, B and
  C are LOW, MID and HI: centre (`0`) is flat, down to `KILL` at the bottom, up to +6 dB at the
  top (`+63`). D is empty.

A kill takes its band down by 30-50 dB within about 10 ms and comes back as fast, with no click.
Coming back puts the band where its knob is. While all three bands are flat and no switch is on,
the EQ is bypassed and costs nothing; turning a knob or throwing a switch starts it, and it
switches itself off again a few milliseconds after everything is flat. The knobs and the
switches are saved with the project, so a project saved with the lows killed reopens that way:
look for `KILL` on the EQ page.

### Tape stop and delay freeze

Two held master moves, in **any mode**:

- **Tape stop: hold FUNC, then hold push D** (`TAPE STOP`). The whole output slows to a standstill
  over two beats, its pitch and level falling together, and stays silent while you hold. Let go
  of push D (FUNC may already be up) and the music is straight back, in time: the pattern kept
  playing underneath. Let go half way and it comes back from wherever the slow-down had got to.
- **Delay freeze: hold FUNC, then hold MUTATE** (`FREEZE`, and `FRZ` after the bar number in
  PERFORM). Whatever is echoing in the delay loops for as long as you hold, and nothing new
  goes into the delay, so you can play over a frozen echo. It loops one delay time (two with
  ping-pong); the first loop starts one echo period after the press. Let go and it fades back to
  the delay's own tail. FUNC + MUTATE never mutates.

Neither is saved or undone; both cost nothing until pressed. With the delay in its LONG mode
(below) the freeze is the LONG delay's own: the whole wash holds exactly as it is, whatever its
length, from the moment you press.

### The performance mixer: VALUE as a crossfader

VALUE can be a crossfader from the dry mix into one of sixteen effects, the last of them a
looper (an idea taken from modbang's Octatrack Performance Mixer template).

**Pick an effect: hold VALUE pushed and tap a STEP key.** The screen names it (`4 ECHO RISER`).
While VALUE is held, the STEP keys show the sixteen slots: dim amber, the effect that is up
bright amber, the one VALUE is on bright green. Then **turn VALUE**: at 0 it is the dry mix, and turning up brings the effect in (`ECHO RISER 64`). This works on every
screen, so you can play an effect from GRID, EDIT or PERFORM; while an effect is picked VALUE
does not change the tempo or PERFORM's morph. Push VALUE + the same STEP again lets go
(`FX OFF: VALUE TEMPO`) and VALUE is VALUE again.

| STEP | Effect | What VALUE does |
| --- | --- | --- |
| 1 | LP SWEEP | the DJ filter's low-pass, closing |
| 2 | HP SWEEP | the DJ filter's high-pass, rising |
| 3 | HP RISER | high-pass with rising resonance, the whole mix into a longer reverb |
| 4 | ECHO RISER | every track into the delay, feedback climbing |
| 5 | ECHO FREEZE | into the delay; near the top (112) the echoes freeze |
| 6 | REVERB WASH | every track into a longer and longer reverb |
| 7 | STUTTER | beat repeat: from 16 up, 1/4, then 1/8, 1/16, 1/32, each a shorter slice of the same moment |
| 8 | TAPE STOP | from 16 up the tape stops; back under 16 and it is back |
| 9 | BASS KILL | the kill EQ's LOW band down to nothing |
| 10 | PUMP | a sidechain-style dip on every beat |
| 11 | CRUSH | fewer bits and a lower sample rate |
| 12 | NOISE RISER | white noise, louder and brighter |
| 13 | FLANGER | a slow jet sweep |
| 14 | GATE | sixteenth-note chopping |
| 15 | BUILD | high-pass, echo and a noise riser together |
| 16 | LOOPER | the loop's mix (below) |

One effect plays at a time: picking another replaces it, and the old one fades out. **Push VALUE
+ LEFT/RIGHT** moves to the previous or next slot and keeps the amount, so you can switch effects
with the crossfader still up. **Tap VALUE on its own** (push and let go within half a second,
without turning; a longer hold resets nothing) to snap back to 0: the effect goes dry but stays picked, and with the looper picked the mix goes back
to live. The same press resets PERFORM's morph (`MORPH 0`), and FUNC + a press turns the DJ
filter off. It never touches the tempo. **Push VALUE + CLR** turns everything off.

**The looper (STEP 16).** Push VALUE + STEP 16 with the beat running: from the next bar line the
whole mix is recorded for 4 bars (`LOOP 4 BAR: NEXT BAR`; set 1, 2, 4 or 8 in the FUNC menu's
**LOOP LEN** row). Then the loop plays, in time, and VALUE is its **mix**: 0 is the live beat,
127 only the loop, and in between both. Now change things underneath (mute tracks, switch
patterns, edit) and fade back when you are ready. Pick an effect after the looper and the loop
keeps playing at its mix with the effect over it, so you can filter or stutter the loop itself.
Push VALUE + STEP 16 again to leave the looper, as with any effect (`LOOP OFF: VALUE TEMPO`):
the loop falls silent but is kept, and VALUE + STEP 16 picks it back up. Pressed during the
count-in or the take, it cancels it (`LOOP CANCELLED`). With everything off, push VALUE + CLR
throws the loop away (`LOOP CLEARED`); then VALUE + STEP 16 records a new take.

The looper borrows RESAMPLE's memory, so while a loop is held RESAMPLE and FREEZE say
`LOOP HOLDS BUFFER`; clear the loop first. 8 bars fit down to 64 BPM (below that the take is
shortened to 4 bars). A STOP during the take throws it away, and the loop fades out while the
transport is stopped. Nothing of the mixer is saved or undone, and in MUTE MODE push VALUE +
STEP 1-8 still stores a mute state.

### The sidechain duck

<p><img src="screens/mixer_dck.png" width="256" alt="MIXER's DCK 1-4 page"> <img src="screens/mixer_duck.png" width="256" alt="MIXER's DUCK page"></p>

Every time the **source track** plays a step, the other tracks dip in level for a moment and come
back: the classic pump that lets the kick through a dense bass and pad. It is fired by the
source's steps, not by listening to the audio, so it is tight and costs almost nothing. By
default the source is track 1 (the kick) and every other track ducks 35%.

It has three MIXER pages, right after the faders:

- **DCK 1-4** and **DCK 5-8**: knobs A-D are how far each track ducks, `OFF` (0) to `100%`. The
  source track shows `SRC`: a track never ducks under itself. The bottom line says which track
  they duck under.
- **DUCK**: **SRC** (A) the source track, T1-T8. **ATK** (B) how fast the dip comes in, 1-50 ms.
  **HLD** (C) how long it stays down, 0-500 ms. **REL** (D) how long it takes to come back,
  10 ms to 1 s. Times are in milliseconds.

Short HLD and REL (0-40 ms, 60-120 ms) keep the groove tight; a long REL (300 ms and up) on
pads and the bass is the breathing house pump. To duck to a hat or a perc instead of the kick,
set SRC to that track. To turn the duck off, set every track's depth to `OFF`.

The duck is part of the **kit**: it is saved with the project and in the kit library, and a kit
recall brings its duck. A project or kit from before it was editable opens with the duck it
always had (track 1, 35%, 3/40/120 ms). Its settings are not p-lockable and not in scenes or
macros.

### Ambient delay and reverb: LONG and HALL

<p><img src="screens/mixer.png" width="256" alt="MIXER: the faders of tracks 1-4"> <img src="screens/mixer_dly.png" width="256" alt="MIXER's DLY page"></p>

<sub>MIXER's first page, the faders, and the DLY page. The screen keys walk all twelve: TRK 1-4, TRK 5-8, DCK 1-4, DCK 5-8, DUCK, DLY, DLY+, REV, REV+, ROUTE, MST, EQ.</sub>

Each send has a second mode for long, washy sounds. They sit on the MIXER pages beside their
send, `DLY+` after `DLY` and `REV+` after `REV`. **Push A** on either page (or turn A) flips the
mode; the old mode's echoes ring out underneath, so a switch never clicks.

- **LONG delay** (`DLY+`, A: `CLAS` / `LONG`). The echo time is DLY's division times **MUL**
  (B: `X1` to `X8`), up to **12 s**: `1/1 X4` is four bars. The bottom line shows the time in
  milliseconds. **WOW** (C) is a slow tape wobble on the repeats; **DIF** (D) smears each repeat
  a little more than the last, so echoes turn into a wash after a few passes. The first echo is
  always clean and on the beat. FBK, TON, LVL and PNG work as in the classic mode, and ROUTE's
  **LCT** (C) cuts the lows from the repeats so a long tail stays clear of the kick and bass.
  Turning the time while it rings bends the echoes like tape, rather than jumping.
- **HALL reverb** (`REV+`, A: `ROOM` / `HALL`). A bigger, softer tail that moves slowly, so even
  a 30 s DEC never rings metallic. **SIZ** (B) is the size of the space, **MOD** (C) how much the
  tail drifts, **SHM** (D) feeds the tail back an octave up: a rising shimmer over held chords.
  DEC, DMP, HPF and LVL work as in the classic mode, and D>R sends the LONG delay into the HALL
  for the biggest wash.

The modes and their knobs are saved with the project; a project from before they existed opens
with both sends classic. Both cost more CPU than the classic modes while they sound (about
twice the send cost), and nothing once they fall silent.

### Freeze

CPU is the SmplTrek's tight resource and memory is not, so a track can be **frozen**: it plays
a loop of itself from RAM instead of computing its sound. FUNC+HOME opens the menu; **FREEZE** is
near the end (UP twice from SAVE, since the list wraps). The row shows the selected track and the loop's length, `T7 1 BAR`;
LEFT/RIGHT or B pick 1, 2, 4 or 8 bars. OK arms it (`FREEZE ARMED`, or `ARMED: PRESS PLAY`
when stopped): from the next bar line the track's machine is recorded for that many bars, and
then the screen says `FROZEN` and the track plays the recording.

A frozen track keeps its **insert filter, drive, lo-fi, fader, pan, sends and duck**: they work
on the loop live. What stops is everything that feeds the machine: notes, steps, locks, chords
and the knobs on its SRC/AMP pages do nothing, and EDIT says `FROZEN: FUNC+HOME`. To change the
part, unfreeze it: the FREEZE row reads `T7 ON`, and OK gives the machine back at once
(`UNFROZEN`). The loop keeps in time with the bar lines; after a tempo change it jumps back into
time at the start of its next loop rather than stretching. A frozen track is silent while
stopped. Loading a project, NEW and recalling a kit unfreeze every track, and a freeze is not
saved: the project always opens live.

A freeze is a take, so it waits while a RESAMPLE take is running, and the RESAMPLE screen waits
for a freeze. The loop uses the sample memory (a bar at 120 BPM is about 190 KiB); if there is
not room, the screen says `NO ROOM` and the track stays live. MID tracks have no audio to
freeze. To keep a loop as a sample you can slice or reverse, bounce it instead (RESAMPLE, B on
`TRK n`).

### Mutation

In PERFORM, UP/DOWN pick the mutation and the screen keys the amount (0-100, in tens):

| Mutation | What it does |
| --- | --- |
| ROT LEFT / ROT RIGHT | Rotate the track by a step |
| DENSITY + / DENSITY - | One Euclidean hit more or fewer |
| VELOCITY | Velocities move by up to ±48 |
| MICRO | Micro-timing moves by up to ±11 ticks |
| CHANCE | Probability moves by up to ±40 points |
| NOTES | Notes move by up to ±3 scale degrees |

The ranges are at amount 100; amount 0 changes nothing. Each press gives a new result. Results
are seeded, so the same press on the same pattern gives the same result again.

### Resampling

<p><img src="screens/resample.png" width="256" alt="RESAMPLE: 1 bar to a file, from the master"></p>

`FUNC+REC` opens RESAMPLE. UP/DOWN or A set the length (1, 2, 4 or 8 bars); LEFT/RIGHT or the
screen keys set where it goes: `TO FILE`, or `TO TRK n`. OK arms it (`ARMED`), and the take
starts on the next bar line (`REC 40%`), records the finished master including repeat and
effects, and is written to `/RECORD/STRATA_<n>.WAV` (`WRITING`, then `LAST STRATA_n`). With a
track as the destination, the take also becomes that track's sample and loads straight away, so
an 8-bar jam can become the loop on track 8 for SAMPLE or GRAIN. A take is at most 30 s
(`SHORTENED TO 30S`). CLR cancels.

**Sampling from the input.** B picks where the take comes from: `MASTER` (the default) or one of
the inputs, `LINE` (INPUT L and R), `LINE L` (INPUT L alone, on both sides), `MIC` (the built-in
microphone, which Enaga keeps silent unless headphones are in: the screen says
`MIC NEEDS HEADPHONES`), `DYN MIC` (INPUT R through the mic preamp) or `HI-Z` (INPUT L for a
guitar). C sets the input level, `LOW`, `NORM` or `HIGH` (on LINE, LOW is the -20 dB pad). An
input shows a meter under `FROM ...`; set the level so it does not reach `CLIP`. The take is the
input alone, before anything STRATA plays, and otherwise works exactly like a master take: it
starts on the next bar line with the transport running, lasts 1-8 bars, and lands in a file or
a track. You do not hear the input through STRATA; listen to the source directly. Turning B or C
makes the input quiet for about 10 ms, and Enaga puts the input back to LINE, NORM when STRATA
stops.

**Bouncing one track.** Past the inputs, B also offers `TRK 1` to `TRK 8`. The take is then that
track alone: its machine through its drive, filter and lo-fi, before its duck, fader, pan and
sends, so a quiet or panned track still comes out at full level, and the duck is not baked in.
It is mono, half the size of a master take. With `TO TRK n` it lands on that track like any
take, ready to slice, reverse or put on GRAIN; give that track SAMPLE or GRAIN to hear it. A
muted track bounces as silence, so unmute it first.

---

## 6. Saving, loading and the card

### Saving

<p><img src="screens/menu.png" width="256" alt="The FUNC menu on SAVE"></p>

- **Save:** `FUNC+HOME`, then `OK` (the FUNC menu opens on SAVE). Only the files that changed are written, and the
  audio keeps playing.
- **Autosave:** every time the transport stops (from STOP or from an outside clock), and when
  you leave the app through the Enaga menu, into `AUTOSAVE/`.
- **At start**, STRATA reopens the last project, or its autosave if that is newer. A first run
  gets the factory project, named `DEFAULT`.
- **UNSAVED** on the PROJECT screen means RAM differs from the card.

### Reload and new

In the FUNC menu (FUNC+HOME), each with OK twice within 2 s:

- RELOAD PROJECT (`OK AGAIN TO RELOAD`): read the project back from its last save, throwing away
  what is unsaved.
- NEW PROJECT (`OK AGAIN FOR NEW`): a new project with the factory content, named `NEWn`.
  Nothing is written until you save it, so the old project is untouched.

Both stop the transport.

### Projects: open, save as, rename and delete

<p><img src="screens/projects.png" width="256" alt="The PROJECTS page"></p>

PROJECT, then screen key RIGHT four times (or the FUNC menu's OPEN PROJECT), is the PROJECTS page: every project folder on the card,
with `NOW` and the open one's name at the top. UP/DOWN or A move the cursor, LEFT/RIGHT go a page
of eight at a time.

- **Open:** OK on a project, then OK again within 2 s (`AGAIN TO OPEN`). What is unsaved in the
  open project is thrown away and the transport stops, as with a reload. STRATA starts with that
  project from now on.
- **Save as:** `FUNC+DOWN` here, or the FUNC menu's SAVE AS, opens a name entry starting from the current name. A or LEFT/RIGHT
  move along it, B, VALUE or UP/DOWN change the character under the cursor (A-Z, 0-9, space and
  `- + . # !`, up to 16), OK saves, CLR cancels (`NAME KEPT`). The project is written whole into
  a new folder and carries on under the new name; the old folder stays as it was saved. A name
  another project already has is refused (`NAME TAKEN`): open that one instead.
- **Rename:** push B on a project opens the same name entry, titled RENAME and starting from
  that project's name. OK renames it (`RENAMING`), CLR leaves it (`NAME KEPT`); a name another
  project has is refused (`NAME TAKEN`). The open project can be renamed too: it carries on under
  the new name, and STRATA starts with it. Unsaved work stays unsaved; the rename moves what is
  on the card.
- **Delete:** push C on a project asks `DELETE? OK TWICE`; OK, then OK again within 2 s
  (`OK AGAIN TO DELETE`), deletes its folder and everything in it (`DELETING`). CLR, or any other
  key, keeps it. The open project cannot be deleted (`PROJECT IS OPEN`): open another one first.
  A delete cannot be undone.
- `FUNC+UP` reads the card again. CLR goes back to the slots.

Both are safe if the card is pulled or the power goes in the middle. The unit's file system has
no rename, so a rename copies the project into the new folder, its `PROJECT.BIN` last, and only
then deletes the old one: at any moment there is at least one whole copy, and at worst two. A
delete removes `PROJECT.BIN` first, so a folder it did not finish is no longer a project
(opening it says `NOT FOUND`, never a project with patterns missing); delete it again to finish.
A delete that finds a folder inside the project's folder (put there from a computer) removes
everything else and says `FOLDER NOT EMPTY`.

### The kit library

<p><img src="screens/kit.png" width="256" alt="KIT: the kit library"></p>

`FUNC+EDIT` twice opens KIT. Sixteen slots, two pages of eight; each shows `KIT`, `EMPTY`,
`UNREADABLE` or `...` (not read yet), and the kit under the cursor is listed as its eight
machines. UP/DOWN, A or the STEP keys move the cursor (they never load).

- **Recall:** OK, then OK again within 2 s (`OK AGAIN TO LOAD`). The transport keeps playing; only
  tracks whose sample changed reload theirs. A recall is **not undoable**.
- **Store:** `FUNC+DOWN` stores the current kit; over a stored kit it needs a second press
  (`AGAIN TO OVERWRITE`).
- `FUNC+UP` reads the card again. CLR goes back to MACHINE.

A recalled kit makes the project UNSAVED until you save it.

### Samples

Samples load into RAM in the background: the track stays silent until its sample is
ready, and everything else keeps playing. The PROJECT screen's `SMP` line shows each track (`O`
loaded, `S` streamed from the card, `.` loading, `X` failed, `-` none) and the memory used out of
the budget, `12/64M`.

STRATA reads WAV files: 8, 16 or 24-bit, or 32-bit float, mono or stereo, at any sample rate
(other rates are converted to 48 kHz on load).

- **At 48 kHz a file may be any length that fits the sample memory.** With the project's 64 MiB
  that is about 5.8 minutes of stereo, or 11.6 of mono, shared by all tracks; the hard limit is
  349 s. A long file is read in pieces (`LOADING LONG` and a percentage), about 7 s for a 50 MB
  stem, while everything keeps playing, and then plays like any other sample: pitch, slices,
  GRAIN.
- **At any other rate** a file is converted whole, so it may be up to **30 s at 96 kHz**; at
  lower rates the same buffer holds more (about 65 s at 44.1 kHz). Save long stems at 48 kHz.
- **A file too big for all that is streamed** from the card instead: one too big for the sample
  memory left, longer than 349 s, or at another rate and longer than the above. Its first two
  seconds load into memory and the rest is read while it plays; the SMP line shows `S` for the
  track. A streamed file plays from its start, forward, on one voice: pitch, the note, ATK, DEC,
  LVL, loop and LEN (where it stops, or loops back from) work, but START, slices and reverse do
  not, and on GRN only the first two seconds play. The card can only be read forward from where
  a stream was opened, and every jump costs a search through the file system that grows with the
  file, so a start anywhere but the beginning or playing backwards would need the card to jump
  all the time; docs/FEATURES.md has the numbers. Up to four tracks can stream at once. If the card cannot keep up, the gap plays
  as silence, the loop stays in time and the screen says `STREAM LATE`; a fast card (class 10 or
  better) avoids it.

### Picking a sample: the sample browser

<p><img src="screens/samples.png" width="256" alt="SAMPLES on track 8, in /SAMPLES"></p>

On a SMP track, press **OK** in EDIT (the page says `OK: PICK SAMPLE`) or in MACHINE; on a GRN
track, OK in MACHINE (OK in EDIT opens its SOUNDS). The
browser opens in the folder of the file the track plays, with the cursor on it, or in `/SAMPLES`
(in the SmplTrek's sample pool when the card has that and no `/SAMPLES`).

- UP/DOWN (or A) move the cursor, LEFT/RIGHT or the screen keys turn a page of eight.
- **OK on a file loads it into the track at once**, with the beat playing, and the cursor stays
  put: press DOWN and OK again to try the next one. The line under the title says `READING`,
  `CONVERTING` and so on while it loads, and the `NOW` line shows the file the track plays.
- **On a SMP track a picked file plays whole, from its start**: the pick sets STA 0, LEN 127,
  SLC 1 and DEC 127 (hold). PIT, ATK, PLY and LVL stay as they were. Turn SLC up afterwards to
  chop it. A project or kit you open keeps the values it was saved with, and a GRN track's knobs
  are left alone.
- OK on a folder (shown with a `/`) opens it. **CLR goes up a folder**; at the top are `SAMPLES/`
  and `RECORD/` (your resampled takes), and CLR from there goes back to where you came from.
- **The SmplTrek's own folders.** On a card the stock firmware has used, the top also shows
  `POOL/`, the stock sample pool (`/SmplTrek/Pool/Audio`, where the SmplTrek manual says to copy
  WAVs), and `SMPLTREK/`, the stock firmware's whole tree: `Project/<name>/Audio` holds each stock
  project's own samples, `Preset/` the factory ones. STRATA only reads there and never changes
  the stock folders; your kit remembers the file's full path and loads it from there next time.
- `FUNC+UP` reads the folder again, for a card swapped or a take just written.

Only WAV files and folders are listed; macOS's `._` files are hidden. A folder shows its first
256 entries; the count line says `+n` when it left some out, and also counts names too long to
store (a sample's whole path, folders included, may be 111 characters). Picking a file makes the
project UNSAVED, like any kit change. Names are shown in capitals, with characters the screen
cannot draw (such as `_`) shown as `-`.

A track also gets a sample by **resampling into it** (section 5) or by **recalling a kit** that
names one.

---

## 7. Performance tips

- **Build tension with locks, not new patterns.** One trigless step with a FLTR lock, or a bass
  RES lock on the accent, makes a pattern feel alive for longer than a copy does.
- **Use odd track lengths.** A 7-step loop or a 12-step perc against 16 never repeats exactly
  for several bars, and costs nothing.
- **Probability on the decorations.** 50-70% on a rim or a loop slice keeps a groove moving; keep
  the kick and the clap at 100%.
- **Keep side A empty.** Then the morph always starts from the sound you have, and B is the
  destination. Swap B's scene with SCENE B + STEP between sections.
- **Queue with END.** Changes land on the bar and nobody hears the seam. Use NOW for a hard cut.
- **Ride a factory macro over a build.** B ACID over 8 or 16 bars, then back to 0 for the drop:
  0 is always exactly where you started.
- **Mutate, then decide.** MUTATE is free to try: CLR takes it back, and FUNC+CLR undoes even a
  kept one later.
- **Pump the pads, not the hats.** Set the chord and pad tracks' duck to 60-80% with a long REL
  on the DUCK page and the hats to OFF: the pads breathe with the kick and the top end stays
  steady.
- **Kill the lows for the build.** FUNC+A takes the kick and bass out; push it again exactly on
  the one of the drop. FUNC+C on its own thins a busy top end for a breakdown.
- **Snare rolls into the drop.** FUNC+FILL, then hold the snare's pad and press harder over the
  last bar; REPEAT + VALUE to 1/32 for the final beat.
- **Tape stop into the drop.** Hold FUNC + push D on the last two beats of a build and let go
  exactly on the one. Or throw the chord into the delay, then FUNC + MUTATE to keep the echo as
  a pad while the beat drops out.
- **Loop, then rebuild underneath.** Push VALUE + STEP 16 to catch four bars, turn VALUE up to
  hold them, then mute, switch patterns or edit live and fade back when the new part is ready.
- **Throws on the off-beat parts.** THROW on the rim or the chord for a bar, then let it ring.
- **Resample a moment you like.** An 8-bar take of a jam into track 8, then sliced or on GRAIN,
  turns a happy accident into material.
- **Save before a set**, and remember that STOP autosaves.

---

## 8. Troubleshooting

### The audio load: LOAD and MAX

The top line's last figure, `23%`, is how much of each audio block the DSP used: the **load**.
It changes every block, so it flickers. Builds with the held **MAX** readout show
`MAX23.5` on the bottom line: the highest block since PLAY was last pressed, so press PLAY to
start a fresh reading.

STRATA is designed to stay under 60% in its worst case. On the unit the default beat reads
under 30%, and long, heavy play has peaked at 84% without a dropout. If the load climbs or the
sound crackles:

- **VA POLY, WAVE POLY and FM4** are the biggest cost: every chord note is a sustaining voice. Use shorter DEC, fewer
  notes per chord (MIN rather than MIN11), or fewer POLY tracks. All POLY tracks share 12 voices.
- **GRAIN** with high DEN is eight grains at once; lower DEN or use SAMPLE.
- **The CPU guard** steps in on its own when the load stays high: the bottom line shows
  `GUARD1` to `GUARD4`, POLY chords get thinner (two voices fewer a step, the quietest fading
  out first) and GRAIN clouds get sparser. It steps back down one level a second once the load
  falls, and a second STOP clears it. Seeing `GUARD` often means the pattern asks for more than
  the box has: use the tips above.
- **Freeze the dearest track.** FUNC+HOME, UP twice to FREEZE, OK: the selected track
  records its loop (1 bar; LEFT/RIGHT or B on the row for 2, 4 or 8) from the next bar line, then
  plays it from memory and stops computing. A six-voice POLY chord goes from the biggest cost
  in the project to almost nothing. See "Freeze" in section 5.
- **The insert filter and lo-fi** cost nothing when off. Set FLTR MOD to OFF and LOF to 0 on the
  tracks that do not need them.
- **Closed sends** cost nothing: keep SNA and SNB at 0 on tracks that do not use the delay or reverb.

### Sample messages

Shown on the PROJECT screen under the project name, and the track shows `X` on the SMP line.

| Message | Meaning | What to do |
| --- | --- | --- |
| `TOO LONG` | Longer than six hours | Trim it |
| `NO ROOM` | Does not fit in the sample memory left (`SMP ... nn/64M`), not even its first two seconds to stream | Use shorter or mono samples, or free a track |
| `STREAMS FULL` | Four tracks already stream | Free one of them, or use a shorter file that fits in memory |
| `STREAM FAILED` | The card would not open the file for streaming | Check the card; pick the sample again |
| `STREAM LATE` | The card fell behind a streamed file: a gap played as silence | Use a faster card, fewer streamed tracks, or 16-bit files |
| `NOT A WAV` / `BAD FORMAT` | Not a WAV STRATA reads | Export as 16 or 24-bit PCM WAV |
| `TOO MANY CH` | More than two channels | Export as mono or stereo |
| `EMPTY` | No audio in the file | |
| `NO SAMPLE` | the FUNC menu's RELOAD SAMPLE on a track whose kit names no file | |
| `NO CARD` | The card is missing or unusable | Reinsert it; samples reload by themselves |
| `NO SWAP` | The sample handover did not complete | A bug: please report it |

### External clock

<p><img src="screens/setup.png" width="256" alt="PROJECT's SETUP page"></p>

The clock source is a unit setting, not STRATA's: **FUNC+POWER** opens the Enaga menu with CLOCK
SRC (INTERNAL, MIDI, SYNC), CLOCK/SYNC OUT and MIDI CH. PROJECT's SETUP page (PROJECT, screen
key RIGHT three times) shows them.

Under MIDI or SYNC, line 2 shows `EXT MIDI` or `EXT SYNC`, and STRATA **follows**: PLAY, STOP,
FUNC+PLAY, FUNC+STOP and the tempo knob are refused with `EXT CLOCK` while the outside clock
runs. Start and stop from the master instead; STRATA follows, and still autosaves on its stop.
`CLOCK REFUSED` means the last transport request was refused. Reloading a project or starting a
new one needs the transport stopped, so stop the master first.

### Other messages

| Message | Meaning |
| --- | --- |
| `LOADING` on PLAY | A project is still loading; PLAY works when it finishes |
| `FULL n` on GRID | n locks refused because the pattern's 1,024-lock pool is full |
| `DROP n` on the bottom line | n edits were lost because they came faster than the audio side applies them |
| `GUARD n` on the bottom line | The CPU guard is shedding POLY voices and GRAIN grains to keep the load under the limit (see "The audio load") |
| `UNREADABLE` in KIT | That kit file is damaged |
| `PATTERN LOST` or another reason on PROJECT after a load | The card held a damaged file; STRATA refuses damaged files rather than crashing |

### When nothing makes sense

- **FUNC+STOP** resets the transport, the queue and the chain.
- **FUNC+CLR** undoes the last edit, up to 16.
- **FUNC+HOME, OK** saves; the menu's RELOAD PROJECT (OK twice) reloads the last save.
- **FUNC+POWER** leaves for the Enaga menu; STRATA autosaves on the way out.

---

<sub>STRATA is an independent project, not affiliated with or endorsed by Sonicware. SmplTrek is a
trademark of its owner. Provided as is, without warranty; use at your own risk (see LICENSE).</sub>
