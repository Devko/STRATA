# STRATA user manual

This manual explains how STRATA works and how to play it. It goes with
[CHEATSHEET.md](CHEATSHEET.md), which lists every gesture, screen and abbreviation on one page:
read this once, then keep the cheatsheet next to the unit.

It describes the current build, written from the code in `app/`, `core/` and `dsp/`. STRATA is
early (see the README's status), so where the hardware has not confirmed something yet, this
manual says so.

**Key names.** Keys are named as printed on the SmplTrek: FUNC, REC, PROJECT, HOME, PLAY, STOP,
MIXER, EDIT, OK, CLR, the arrows (UP, DOWN, LEFT, RIGHT), the octave keys, the two selection keys
under the screen, encoders A-D and VALUE, STEP 1-16 and the pads. `FUNC+X` means hold FUNC and
press X. "Hold STEP" means hold a STEP key down while you do the rest. The cheatsheet's key
legend maps these to the F1-F8 names used in the code and in docs/SPEC.md.

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
factory macros B, C and D), KEYS. Bottom: MACHINE, the KIT browser, PROJECT.</sub>

---

## 1. Core concepts

### Tracks

STRATA has **eight tracks**. The eight lower pads are the tracks, left to right: tap one to select
it and hear it. Each track holds one **machine** (the sound source) followed by the same fixed
**insert chain**: drive, a multimode filter, lo-fi, the amplitude stage, the sidechain duck, pan
and level. From there each track goes to the mixer, the two send effects (delay and reverb) and
the master bus.

The selected track is the one the STEP keys, the encoders, KEYS and MUTATE work on. UP and DOWN
also walk the tracks in GRID, EDIT and KEYS.

### Machines

A machine is one of eight sound engines. Any track can host any machine (MACHINE mode,
`FUNC+EDIT`). The default kit uses all eight, one per track:

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

Every track has **16 parameters on four pages** of four, one parameter per encoder A-D:

- **SRC** and **AMP** belong to the machine and change meaning with it (section 3).
- **FLTR** and **FX** belong to the insert chain and are the same on every track:
  FLTR = DRV drive, CUT cutoff, RES resonance, MOD filter mode (OFF, LP12, LP24, BP12, BP24,
  HP12, HP24); FX = LOF lo-fi, PAN, SNA send A (delay), SNB send B (reverb).

Parameters run 0-127. Every change is smoothed over one audio block, so turning a knob or
playing a locked step never clicks.

### Patterns, banks and the project

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

The **chain** plays patterns in order: up to 64 entries, each a pattern and a repeat count
(X1-X16). It is a list, not a song timeline. You build it on PROJECT's CHAIN page and turn it on
and off with `FUNC+UP` in PROJECT.

### Scenes and morphing

A **scene** is a snapshot of parameter values for any tracks. A project has 16. Two of them sit
on the **A** and **B** sides, and in PERFORM the VALUE knob **morphs** the live sound from A to
B. A parameter that only one side stores fades between that side's value and the kit's value, so
a scene that touches only the bass leaves the drums alone. Scenes change the kit values only;
a step's locks still win over the morph.

### Macros

A **macro** is one knob that moves up to 4 parameters at once, each towards its own target value.
There are four, on encoders A-D in PERFORM. At 0 a macro does nothing, so its parameters still
follow their EDIT knobs and the morph; turned up, it sweeps them from wherever they are. A new
project has three: **B ACID**, **C DUB** and **D DRIVE**. Macro A is left for you, and pressure on
the selected track's pad pushes it up while you lean on it.

### MUTATE

**MUTATE** (upper pad 7) makes a seeded variation of the selected track: rotate it, add or
remove hits, or nudge velocity, timing, probability or notes within the scale. It is one undo
level: OK keeps it, CLR takes it back.

### Kits

A **kit** is the sound side of a project without the patterns: the eight machines, their 16
parameters each, the faders and the sample paths. Besides the project's own kit there is a
**kit library** of 16 slots on the card, shared by every project, so a sound you built in one
project can be recalled in another (KIT mode, `FUNC+EDIT` twice).

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

The first project is called `DEFAULT`. A new project is named `NEW1`, `NEW2` and so on. There
is no file browser yet, so projects cannot be renamed or picked from a list on the unit; STRATA
reopens the last one.

---

## 2. Your first beat

This walks through the factory project, which is what a first start and every new project
contain. It takes about fifteen minutes.

**What is in it.** Bank A holds four patterns in C minor at 130 BPM:

| Slot | Name | What it is |
| --- | --- | --- |
| A01 | GROOVE | Four on the floor, backbeat clap, 16th hats with offbeat opens, a rolling bass, a dub chord stab, a rim and a few loop slices |
| A02 | GROOVE+ | The same beat developing: a kick pickup every fourth bar, a hat roll, a clap on FILL, a 12-step perc line and a 7-step loop against the 16 |
| A03 | BREAK | Kick and bass out; long dub chords, a perc ticking at 35% probability |
| A04 | PEAK | Everything up: ratcheted hats, an acid bass line, a rave stab, kick and clap rolls on FILL |

It also ships a chain (GROOVE x2, GROOVE+ x2, BREAK, PEAK x2, GROOVE+, about two minutes) that
starts **off**, eight mute states, and scenes 2-4: **DUB** (chord, rim, loop and clap pushed into
the sends), **DARK** (filters closed) and **TENSION** (the bass opens and screams). Scene 1 is
empty, so the morph starts from the kit as it is. Three macros are set up: **B ACID** (the bass
filter), **C DUB** (the chord and rim into the sends) and **D DRIVE** (the kick and hats). Track 8 plays a built-in placeholder of eight
short blips until you give it a sample (section 6).

### 1. Play it

Press **PLAY**. A01 starts; the top line reads `A01 130.0 > 20.0%` (pattern, tempo, `>` playing,
audio load). Turn **VALUE** to change the tempo; push and turn VALUE for swing. **STOP** stops
and lets tails ring; STOP again cuts them.

### 2. Edit steps

Press **HOME** for GRID: all eight tracks as rows, the selected one highlighted. Tap lower pad 2
to select the clap. Its steps show on the STEP LEDs; tap **STEP 8** to add a clap, tap it again to
remove it. The octave keys move between the four 16-step pages of a 64-step track; `L16` on screen
is the track's length.

Clear the whole track with **CLR**, and get it back with **FUNC+CLR** (undo). FUNC+OK is redo.
There are 16 levels.

### 3. Lock a parameter

Select the kick (lower pad 1) and press **EDIT**. The selection keys (or LEFT/RIGHT) flip the
pages SRC, FLTR, AMP, FX; go to **AMP**. Now hold **STEP 13** and turn **A** (DEC) down: the last
kick of the bar is shorter. That is a lock. GROOVE already has one on step 9; hold STEP 9 and push
**A** to remove it (`LOCK CLEARED`).

The encoders work the same way in GRID: they edit the current EDIT page of the selected track.

### 4. Change a note, open the step page

Select the bass (pad 6). Hold a STEP that has a note and turn **VALUE**: that changes the step's
note. Hold the STEP and press **OK** to open the **step page**: A-D edit NOTE, VEL, LEN, MICR;
the selection keys flip to PROB, COND, RTCH, RAMP and then ACNT, SLID, CHRD. LEFT/RIGHT walk to
the neighbouring steps. Turn SLID on for a 303-style slide into the next note. OK or CLR closes
it.

### 5. Play it in

Press **FUNC+HOME** for KEYS: STEP 1-16 play the scale from the root (C minor here), and the
octave keys change octave. Press **REC** (it lights red) while playing and what you play is
recorded onto the selected track, quantized by the SETUP page's QUANTIZE amount (100% by
default). Pads record too, with their velocity. REC again stops. Turning A-D while recording
writes locks onto the steps as they pass (motion recording).

### 6. Change pattern

Press **PROJECT**. The 16 slots of bank A are on screen and on the STEP keys. Press **STEP 2**:
`A02 END` shows, and GROOVE+ takes over at the end of the current pattern. Encoder **A** picks when
a change lands: END, after 16 or 32 steps, or NOW. Go on to A03 and A04. In PEAK, hold the
**FILL** pad (upper pad 1) for the last bar to hear the kick and clap rolls.

To let STRATA play the arrangement itself, press **FUNC+UP** in PROJECT (`CHAIN ON`).
**FUNC+STOP** stops and resets everything, and puts a running chain back to its first entry.

### 7. Perform

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

### 8. Save

Press **FUNC+PROJECT** twice within two seconds (`FUNC+PROJECT TO SAVE`, then `SAVING`). The
PROJECT screen stops saying `UNSAVED`. STRATA also autosaves whenever the transport stops.

---

## 3. The machines

Each machine uses the SRC and AMP pages; FLTR and FX are the insert chain (section 1). On every
machine, AMP's first knob is the decay and its last is LVL, the output level (square law, so
the quiet end has room). Where a machine follows the trig note, note 60 (C4) plays the knob's own
pitch and other notes transpose from there.

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

### GRN: GRAIN

Granular playback of the track's sample: up to 8 overlapping grains in one voice. The most
expensive machine (section 7).

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **POS** position in the sample | **SIZ** grain size, 5-390 ms | **DEN** overlap: stutter (low) to smooth cloud (high) | **SPR** spray: chorus, cloud, wash |
| AMP | **PIT** ±24 semitones | **SHP** grain shape: smooth to plucked | **DEC** 5 ms-16 s; 127 = hold | **LVL** |

GRAIN has no tempo of its own. To make a cloud move in time, **lock POS on steps**, or motion
record a sweep of POS. The trig note transposes, so a cloud plays chromatically.

### MNO: VA MONO

The bass: a saw, a hard-synced pulse, a sub an octave down and a little noise, through its own
resonant filter with an envelope. One voice, with accent, slide and glide.

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** ±24 semitones | **MOD** voice filter: 0-63 LP24, 64-95 LP12, 96-111 BP12, 112-127 HP12 | **CUT** cutoff, 35 Hz-12 kHz | **RES** resonance |
| AMP | **DEC** decay of both envelopes, 15 ms-2.4 s | **ENV** filter envelope amount, up to +66 semitones | **GLD** glide, 0-0.5 s | **LVL** |

This filter is the voice's own, on SRC; the FLTR page's filter is a second, utility filter after
it. ENV 0 is a static dub bass; ENV and RES up is the acid squelch. **Accent** (ACNT on the step
page) brings the pulse and noise forward and opens the filter further; **slide** (SLID) glides
into the next note.

### PLY: VA POLY

The same voice as VA MONO, up to six notes at once, for chords. Each step can play a chord
(CHRD on the step page: MIN, MAJ, SUS2, SUS4, DIM, MIN7, DOM7, MAJ7, MIN9, MIN11, 5TH, OCT, 5TH OCT,
each with inversions `/n`).

| Page | A | B | C | D |
| --- | --- | --- | --- | --- |
| SRC | **TUN** | **MOD** as MNO | **CUT** | **RES** |
| AMP | **DEC** also the chord's length: short stab or long pad | **ENV** | **DET** voice detune, up to a quarter semitone | **LVL** |

Every note is a sustaining voice and costs CPU while it rings. All VA POLY tracks together share
a budget of **12 voices**; past it, the quietest note is faded out quickly to make room. A longer
DEC means more notes overlap.

### The key and the scale

Each pattern has a key (root) and a scale, set in PROJECT with encoders **B** (key) and **C**
(scale). The melodic machines **fold** their notes into the scale: VA MONO, VA POLY, GRAIN, and
SAMPLE when it is not sliced (SLC 1). Drums and sliced samples are not folded, since there the
note picks a pitch or a slice rather than a melody. Scales: CHROMATIC, MINOR, MAJOR, DORIAN,
PHRYGIAN, LYDIAN, MIXOLYDIAN, LOCRIAN, HARM MINOR, MEL MINOR, MINOR PENT, MAJOR PENT, BLUES,
WHOLE TONE, DIMINISHED. Notes are named with sharps (D#, not Eb).

### Changing a track's machine

`FUNC+EDIT` opens MACHINE. UP/DOWN or encoder A pick the machine, and you hear the change at
once; LEFT/RIGHT or the selection keys move to the next track. `FUNC+EDIT` again opens the kit
library (section 6).

---

## 4. The sequencer in depth

### Steps

Tap a STEP for a trig; tap again to remove it. **FUNC + tap** makes a **trigless** step: it plays
no note but its locks still apply, which is how you change a sound mid-note or set up the next
hit. The STEP LEDs show: green = trig, dim green = trig with a condition or probability, amber =
trig with locks, red = trigless, bright red = the playhead.

The step page's fields:

| Field | Range | What it does |
| --- | --- | --- |
| NOTE | 0-127 | The note (also: hold STEP + turn VALUE) |
| VEL | 1-127 | Velocity |
| LEN | 1/16 step ... 64 STP | Note length |
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

### Ranges, copy and paste

Hold two STEP keys to select the range between them (`SEL a-b`). Then CLR clears it, FUNC+UP
copies it, FUNC+DOWN pastes it, encoder A transposes it and encoder B nudges its micro-timing.
With nothing held, CLR clears the whole track and FUNC+UP / FUNC+DOWN copy and paste the whole
track. Hold a STEP and press UP / DOWN to copy / paste one step. Everything goes through undo.

### Length, speed and swing

- `FUNC + octave keys`: the selected track's length, 1-64 steps.
- `FUNC + selection keys`: its speed, 2x, 1x, 3/4, 1/2, 1/4, 1/8.
- `VALUE push + turn`: the pattern's swing, 50-75%, on the even 16ths.
- PROJECT, octave RIGHT twice: the **PATTERN** page, where A sets the master length (LEN) and B
  the swing (SWG).

A 12-step perc against a 16-step kick lands somewhere new each bar for three bars; GROOVE+ does
exactly this.

### Euclidean fill

In GRID, `FUNC + A / B / C` set hits, steps and rotation (`EUC 5/16 R2`), applied live to the
selected track. The steps number is a cycle, not the track's length: 3 in 8 on a 16-step track
plays the 8-step figure twice. Edit the result by hand afterwards.

### Pattern changes and the chain

In PROJECT, the selection keys (or LEFT/RIGHT) pick the **shown** bank, which is not necessarily
the playing one; STEP 1-16 or the cursor plus OK queue a slot from it. CLR cancels a queued change
(`QUEUE OFF`). The slots show the playing pattern bright, patterns with trigs mid grey, empty ones
dark, and `05>` marks a queued slot.

The **CHAIN** page (PROJECT, octave RIGHT) lists the entries, `>02 A05 X2 PLAY`. UP/DOWN move the
cursor, a STEP key puts that slot of the shown bank into the entry, LEFT/RIGHT step the entry's
pattern, A or VALUE set its repeats, OK adds a copy after the cursor (up to 64), FUNC+LEFT removes
the entry, FUNC+UP turns the chain on or off. Chain edits are not undoable.

### Recording

REC toggles live and motion recording. With the transport running, pad hits, KEYS and MIDI notes
are recorded with their timing, corrected for the pad's latency and quantized by QUANTIZE (SETUP
page, encoder A, 0-100%; 0 keeps your timing as micro-timing). A hit that cannot be stored where it
was played says `OFF GRID`. One recording pass is one undo level.

MIDI in: channels 1-8 play tracks 1-8; other channels play the selected track.

---

## 5. Performing

The seven upper pads are the same in every mode:

| Pad | Plain | With FUNC |
| --- | --- | --- |
| 1 FILL | FILL conditions on while held | |
| 2 REPEAT | Beat repeat of the master while held; VALUE picks the length | Pitch drops as it repeats |
| 3 MUTE | MUTE MODE on/off: lower pads mute at once | |
| 4 SCENE A | Hold + STEP 1-16 puts that scene on side A | |
| 5 SCENE B | The same for side B | |
| 6 THROW | Selected track's delay send to full while held | Reverb instead |
| 7 MUTATE | Mutate the selected track; OK keeps, CLR reverts | |

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

- **OK** captures the whole current sound into the scene on side A.
- Hold a scene pad + **OK** captures only the selected track into that side's scene.
- Hold a scene pad + **CLR** erases that side's scene.

A useful habit: leave side A on an empty scene, build a sound, capture it into a scene on side B,
then undo your knob moves and morph back and forth. The morph position itself is not saved.

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

1. Select a track (lower pad) and a macro (LEFT/RIGHT or the octave keys).
2. Press a **STEP** key: STEP 1-4 are that track's SRC parameters, 5-8 FLTR, 9-12 AMP, 13-16 FX.
   The screen confirms, for example `A T6 CUT`. Repeat for up to four parameters, on any tracks.
3. Turn the macro's encoder (A-D).

The target is whichever end of 0-127 is further from the parameter's value when you assign it,
so the knob always has somewhere to go: a filter already open gets a macro that closes it. If the
macro is already turned up when you assign, the parameter moves straight to where the macro puts
it. **CLR** clears the selected macro. Macro setups are saved with the project (PERFORM.BIN).

**Pad pressure** pushes **macro A** up while you lean on the selected track's pad, never below
where its knob is, and lets go back to the knob's setting. This is why A ships empty: assign it
to the selected track's sound and the pads become expressive.

### Beat repeat, throws and auto-fill

REPEAT loops the last 1/4, 1/8, 1/16 or 1/32 (or triplets) of the finished master while held;
release and the music is where it would have been. In PERFORM, **FUNC+UP / FUNC+DOWN** set
**auto-fill**: OFF, or FILL on the last bar of every 4, 8 or 16. PERFORM's lower lines show what
is active: `MUT NOTES T1 50`, `FILL AF16 1/16- DLY`, `BAR n`.

### Mutation

In PERFORM, UP/DOWN pick the mutation and the selection keys the amount (0-100, in tens):

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

`FUNC+REC` opens RESAMPLE. UP/DOWN or A set the length (1, 2, 4 or 8 bars); LEFT/RIGHT or the
selection keys set where it goes: `TO FILE`, or `TO TRK n`. OK arms it (`ARMED`), and the take
starts on the next bar line (`REC 40%`), records the finished master including repeat and
effects, and is written to `/RECORD/STRATA_<n>.WAV` (`WRITING`, then `LAST STRATA_n`). With a
track as the destination, the take also becomes that track's sample and loads straight away, so
an 8-bar jam can become the loop on track 8 for SAMPLE or GRAIN. A take is at most 30 s
(`SHORTENED TO 30S`). CLR cancels.

---

## 6. Saving, loading and the card

### Saving

- **Save:** `FUNC+PROJECT` twice within 2 s. Only the files that changed are written, and the
  audio keeps playing.
- **Autosave:** every time the transport stops (from STOP or from an outside clock), and when
  you leave the app through the Enaga menu, into `AUTOSAVE/`.
- **At start**, STRATA reopens the last project, or its autosave if that is newer. A first run
  gets the factory project, named `DEFAULT`.
- **UNSAVED** on the PROJECT screen means RAM differs from the card.

### Reload and new

On PROJECT's slots or SETUP page:

- `FUNC+LEFT` twice (`AGAIN TO RELOAD`): read the project back from its last save, throwing away
  what is unsaved.
- `FUNC+RIGHT` twice (`AGAIN FOR NEW`): a new project with the factory content, named `NEWn`.
  Nothing is written until you save it, so the old project is untouched.

Both stop the transport.

### The kit library

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

Samples are loaded whole into RAM in the background: the track stays silent until its sample is
ready, and everything else keeps playing. The PROJECT screen's `SMP` line shows each track (`O`
loaded, `.` loading, `X` failed, `-` none) and the memory used out of the budget, `12/32M`.

STRATA reads WAV files: 8, 16 or 24-bit, or 32-bit float, mono or stereo, at any sample rate
(other rates are converted to 48 kHz on load). A file may be up to **30 s at 96 kHz**; at lower
rates the same buffer holds more (about 65 s at 44.1 kHz).

### Picking a sample: the sample browser

On a SMP or GRN track, press **OK** in EDIT (the page says `OK: PICK SAMPLE`) or in MACHINE. The
browser opens in the folder of the file the track plays, with the cursor on it, or in `/SAMPLES`.

- UP/DOWN (or A) move the cursor, LEFT/RIGHT or the selection keys turn a page of eight.
- **OK on a file loads it into the track at once**, with the beat playing, and the cursor stays
  put: press DOWN and OK again to try the next one. The line under the title says `READING`,
  `CONVERTING` and so on while it loads, and the `NOW` line shows the file the track plays.
- OK on a folder (shown with a `/`) opens it. **CLR goes up a folder**; at the top are `SAMPLES/`
  and `RECORD/` (your resampled takes), and CLR from there goes back to where you came from.
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
- **Throws on the off-beat parts.** THROW on the rim or the chord for a bar, then let it ring.
- **Resample a moment you like.** An 8-bar take of a jam into track 8, then sliced or on GRAIN,
  turns a happy accident into material.
- **Save before a set**, and remember that STOP autosaves.

---

## 8. Troubleshooting

### The audio load: LOAD and MAX

The top line's last figure, `23.4%`, is how much of each audio block the DSP used: the **load**.
It changes every block, so it flickers. Builds with the held **MAX** readout show
`MAX23.5` on the bottom line: the highest block since PLAY was last pressed, so press PLAY to
start a fresh reading.

STRATA is designed to stay under 60% in its worst case. On the first unit boot the default beat
read about 0-26%; the factory project's load on the unit is still to be measured. If the load climbs or the sound crackles:

- **VA POLY** is the biggest cost: every chord note is a sustaining voice. Use shorter DEC, fewer
  notes per chord (MIN rather than MIN11), or fewer POLY tracks. All POLY tracks share 12 voices.
- **GRAIN** with high DEN is eight grains at once; lower DEN or use SAMPLE.
- **The insert filter and lo-fi** cost nothing when off. Set FLTR MOD to OFF and LOF to 0 on the
  tracks that do not need them.
- **Closed sends** cost nothing: keep SNA and SNB at 0 on tracks that do not use the delay or reverb.

### Sample messages

Shown on the PROJECT screen under the project name, and the track shows `X` on the SMP line.

| Message | Meaning | What to do |
| --- | --- | --- |
| `TOO LONG` | Longer than 30 s at its own rate (up to 96 kHz) | Trim it, or save it at 48 kHz or lower |
| `NO ROOM` | Does not fit in the sample memory left (`SMP ... nn/32M`) | Use shorter or mono samples, or free a track |
| `NOT A WAV` / `BAD FORMAT` | Not a WAV STRATA reads | Export as 16 or 24-bit PCM WAV |
| `TOO MANY CH` | More than two channels | Export as mono or stereo |
| `EMPTY` | No audio in the file | |
| `NO SAMPLE` | `FUNC+DOWN` on a track whose kit names no file | |
| `NO CARD` | The card is missing or unusable | Reinsert it; samples reload by themselves |
| `NO SWAP` | The sample handover did not complete | A bug: please report it |

### External clock

The clock source is a unit setting, not STRATA's: **FUNC+POWER** opens the Enaga menu with CLOCK
SRC (INTERNAL, MIDI, SYNC), CLOCK/SYNC OUT and MIDI CH. PROJECT's SETUP page (PROJECT, octave
RIGHT three times) shows them.

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
| `UNREADABLE` in KIT | That kit file is damaged |
| `PATTERN LOST` or another reason on PROJECT after a load | The card held a damaged file; STRATA refuses damaged files rather than crashing |

### When nothing makes sense

- **FUNC+STOP** resets the transport, the queue and the chain.
- **FUNC+CLR** undoes the last edit, up to 16.
- `FUNC+LEFT` twice in PROJECT reloads the last save.
- **FUNC+POWER** leaves for the Enaga menu; STRATA autosaves on the way out.

---

<sub>STRATA is an independent project, not affiliated with or endorsed by Sonicware. SmplTrek is a
trademark of its owner. Provided as is, without warranty; use at your own risk (see LICENSE).</sub>
