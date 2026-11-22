# STRATA: techno performance groovebox for the SmplTrek

Nov 21, 2026 · @Roland

> **Revised 2026-11-21** against the real Enaga platform at commit `1b41111`. The platform
> turned out to be further along than this document assumed: MIDI DIN, SYNC, USB MIDI, input
> selection, pad pressure and a 96-PPQN clock with transport all exist. Corrections are folded
> in below and marked **[rev]** where they change a decision. `docs/REVIEW.md` holds the
> evidence; `docs/PLAN.md` holds the build order and supersedes the Milestones and
> Instructions sections at the end of this document.

## Summary

STRATA (working title) is an Enaga app that turns the SmplTrek into an 8-track techno groovebox for live jamming: hybrid synth and sample engines, a step sequencer with parameter locks and trig conditions, and performance tools that change the music without stopping it. Target: under 60% of the audio budget with all 8 tracks playing, and no audible timing jitter.

The stock firmware is sample-only. STRATA's edge is synthesized drums and bass next to samples, per-step sound changes, and a performance layer (mutes, scenes, morphing, repeat, mutation) designed for long, evolving techno sets.

**Design principles**

- **Performance first.** Every edit works while the transport runs and lands on the beat. No modal screen ever stops playback.
- **Two keys at most.** The most used actions need one key, or F1 plus one key.
- **Timing is sacred.** Steps are scheduled inside the audio callback against Enaga's clock and land on an exact sample. UI or card activity never moves a note. **[rev]**
- **Controlled accidents.** Randomness is seeded, bounded and undoable: probability, mutation and generative fills all have an undo.
- **Small, deep, finished.** Fewer engines done well beat many half-done ones. Every milestone ends with a playable instrument.
- **Always restorable.** STRATA never writes the SPI flash. All state lives on the SD card, so one official update restores the unit.

**Non-goals for v1**

- A linear song timeline. Pattern chains only.
- Multi-take audio tracks like the stock GLOBAL tracks.
- USB audio and USB mass storage (no stack yet; planned for Enaga E5). **[rev]** USB MIDI does exist, and it arrives through the same `on_midi` path as the DIN jacks, so it costs nothing extra once MIDI is wired.
- A full sample editor. Start, end, loop and slices only.
- Testing on the Lofi-12 XT and CyDrums. Same board, so keep key labels device-aware, but develop and test on the SmplTrek.

## Platform facts and constraints

STRATA is one Enaga app on a TI C6748 DSP at 372 MHz with about 124 MiB of heap. The hardware and the Enaga rules below are fixed; design around them.

| Resource | Fact |
| --- | --- |
| CPU | 1 C674x VLIW core, 372 MHz, hardware float. 496,000 cycles per 64-frame block; Enaga itself uses about 3% |
| Audio | 48 kHz stereo in and out, float ±1.0, 64-frame blocks (1.33 ms), **interleaved L R L R**. Input is selectable through `enaga::system::input(source, level)` **[rev]** |
| Memory | About 124 MiB app heap in DDR2 (16-bit, 150 MHz, about 0.6 GB/s). L1 32 + 32 KiB and L2 256 KiB, all used as cache |
| Storage | SD card, FAT or exFAT. Reads about 7.6 MB/s, writes about 2 MB/s. App may write only in `/ENAGA/DATA/<package>`, `/SAMPLES`, `/RECORD` |
| Screen | 128 × 128 pixels, 16 greys, about 20 frames/s. Built-in 5 × 7 font: A-Z upper case, 0-9, space and `! # + - . / : = ? % >` |
| Pads | 15 pressure pads: raw 14-bit readings (about 262 per second each), press with velocity 1-127 within 3.8 ms, release |
| Keys | F1-F8, STEP 1-16, OK, CLR, 4 cursor keys, 2 octave keys, 2 selection keys, 5 encoder pushes. Read every 6 ms, debounced, no auto-repeat |
| Encoders | A-D plus VALUE, signed detents |
| LEDs | Pads RGB; STEP 1-16, F1-F8, OK, CLR red and green; VALUE ring RGB. Levels 0-10, main context only |
| Clock | One unit-wide clock at 96 PPQN (`ENAGA_CLOCK_PPQN`), 20-300 BPM in 0.01 steps. Its source is the user's CLOCK SRC setting: INTERNAL, MIDI clock or SYNC IN **[rev]** |
| Already in Enaga | MIDI DIN in/out, USB MIDI, SYNC in/out, input selection, pad pressure events, the clock and transport — all of them **[rev]**, none yet run on a unit |
| Still missing | USB audio, USB mass storage, the original's SYNC polarity settings **[rev]** |
| No UI toolkit | Enaga has `Canvas` only (`clear`, `pixel`, `rect`, `text`, `data()`). Every screen, widget and LED shadow is STRATA's own **[rev]** |

**Hard rules (Enaga 0.1, app interface version 0)**

- `audio()` never allocates, blocks, touches files, draws or sets LEDs. Share state through single-writer atomics or single-producer single-consumer rings.
- Allocate everything the audio side needs in `init()`. No `std::vector`, `std::string` or iostreams. No libm: use the math in `<enaga/dsp.hpp>`.
- **Destructors never run** (static constructors do). Save all state in `stop()`, never in a destructor. **[rev]**
- At most 8 open files and 8 streams per app. A stream's ring is a power of two, at least `ENAGA_STREAM_RING_MIN`, and belongs to Enaga until `close`. **[rev]**
- `on_card(false)` means the card went: open files are already dead and must be closed. **[rev]**
- `on_midi` and `on_transport` are **main context** and carry no timestamp. Clock sync is unaffected (Enaga consumes MIDI clock itself), but MIDI notes need the same latency stamping as pad hits. **[rev]**
- C++20 with `-fno-exceptions -fno-rtti`, built with `-Wall -Wextra -Werror`.
- Stack frames up to 16 KiB, no VLAs or `alloca`. Put large buffers in statics or on the heap.
- A misaligned 32-bit access silently uses the aligned address below, and null reads return garbage. Parse file formats byte by byte.
- Watchdog: main-context callbacks return within 1 s, `audio()` within 100 ms, `stop()` within 3 s including saves.
- Hot loops go in `*.ti.c` files: plain C, branch-free (`cond ? a : b`), no division. They must also build with GCC. **[rev]** cl6x is worth about **2.2×** on DSP-heavy code (Enaga's synth: 30% of budget with it, 65% without), so the 60% target depends on it — see the budget section.
- `build_app.py` compiles **one flat folder** with Enaga's include path only. Filenames must be unique across `core/`, `dsp/` and `app/`, and includes are bare (`#include "pattern.hpp"`). **[rev]**
- The build host is Linux. On Windows that means WSL; there is no cmake there, and `build/` belongs inside the WSL filesystem rather than on `/mnt/d`. **[rev]**
- F1 + POWER always opens the Enaga menu and POWER never reaches the app. F1 alone is the app's shift key.
- The app interface is provisional: Enaga is pinned at release **`v0.2.0`** (`3da7f89`; `1b41111` until 2026-11-22) **[rev]** and breaking changes are expected before 1.0.

## Sound architecture

Eight tracks, each one machine plus a fixed insert chain, feed a mixer with two send effects and a master bus. Any track can host any machine; the template below is the default techno kit.

| Track | Default machine | Character |
| --- | --- | --- |
| 1 | KICK | Sine or triangle body, pitch envelope, click, drive. Tuned to the key |
| 2 | SNARE | Tone plus noise; clap mode fires 3-4 noise bursts |
| 3 | HAT | Six-square metallic oscillator or sample; chokes track 4 |
| 4 | HAT (open) | Same machine, long decay, choked by track 3 |
| 5 | PERC | 2-operator FM percussion: rims, toms, blips |
| 6 | BASS | Mono VA with accent and slide |
| 7 | CHORD | Poly VA, 6 voices, chord per step |
| 8 | LOOP | Sample, slices or granular loop locked to tempo |

**Machines (v1)**

| Machine | Voices per track | Engine |
| --- | --- | --- |
| KICK | 2 (retrigger with short fade) | Oscillator, pitch envelope (amount, time), click transient, drive, decay |
| SNARE | 2 | Tuned tone pair, filtered noise, snap; clap burst count and spread |
| HAT | 2 | Six detuned squares through band-pass plus noise, or a sample; decay, tone, choke group |
| PERC | 2 | 2-op FM: ratio, index, index envelope, pitch envelope |
| SAMPLE | 4 | One-shot or slice playback from RAM: pitch, start, length, loop, reverse, 4-point interpolation |
| GRAIN | 1 (up to 8 grains) | Granular playback, tempo-locked stretch: position, size, density, spray, pitch |
| VA MONO | 1 | 2 oscillators with sync, sub, noise, multimode filter, 2 envelopes, accent, slide, glide |
| VA POLY | 6 | Same voice as VA MONO; chord mode, spread, unison 2 |

**Per-track insert chain (fixed order)**

Machine, then drive (asymmetric soft clip), multimode filter (LP, HP, BP, 12 or 24 dB), lo-fi (bit depth and rate reduction, off by default), amplitude envelope, sidechain duck, pan, level.

**Mixer and buses**

- **Send A, delay:** tempo-synced stereo delay with ping-pong and filtered feedback (`enaga::StereoDelay`).
- **Send B, reverb:** `enaga::Reverb` with pre-delay, size, damping and a high-pass on the input. Delay can feed the reverb.
- **Sidechain duck:** a duck envelope (attack, hold, release) fired by a chosen trigger track (default track 1). Trigger-based, not an envelope follower: tighter and cheaper.
- **Master:** one-knob DJ filter (left = low-pass, right = high-pass), **[rev]** a three-band kill EQ (a DJ isolator: LOW / MID / HI split at 250 Hz and 2.5 kHz with 24 dB Linkwitz-Riley crossovers, each band from a kill to +6 dB with a latching kill switch, bypassed while flat), glue compressor, soft limiter, output level.

**Parameters**

- Each machine exposes 16 parameters on 4 pages of 4 (SRC, FLTR, AMP, FX), mapped to encoders A-D.
- Stored as 0-127 in patterns and kits so parameter locks stay small. Each machine maps them to physical units.
- The audio side ramps every parameter change across one block, so edits and locks never click.
- One LFO per track (sine, triangle, saw, square, sample and hold), free or tempo-synced, routable to any of the 16 parameters.
- Voices come from fixed pools allocated in `init()`; nothing is allocated while playing.

## Sequencer

The sequencer runs inside `audio()` against **Enaga's** 96 PPQN clock. A pattern holds 8 tracks of up to 64 steps each, with per-step parameter locks, trig conditions and micro-timing.

**Clock and transport [rev]**

STRATA does not implement a clock and does not own the tempo. Enaga's `<enaga/clock.hpp>` is already what this section originally specified, so STRATA schedules steps on top of it.

- `clock::ticks(out, max)` in `audio()` returns the ticks falling in the block being computed and `.frame`, the frame each falls on, so a step starts on that exact sample. The 32.32 fixed-point tick length and the drift guarantee are Enaga's, not STRATA's.
- Tempo 20-300 BPM, set as BPM × 100, so 0.01 BPM resolution.
- **The clock's source is the user's CLOCK SRC setting**: INTERNAL, MIDI clock at MIDI IN, or SYNC IN. Under an outside source `start`, `stop` and `set_tempo` do nothing and return `false`, and `on_transport` reports what the outside clock did.
- So PLAY and STOP must handle a refusal and follow `on_transport` instead, and the tempo display shows `EXT` whenever `clock::source()` is not internal. This is not deferrable: it shapes the transport from the first milestone.
- With CLOCK OUT on, Enaga already sends MIDI clock and transport; with SYNC OUT on, pulses at SYNC OUT. STRATA only exposes the settings.
- **STRATA's own timing work**: swing 50-75% per pattern on even 16ths with an optional per-track override, per-track length and speed, and micro-timing.
- STOP stops with release tails, STOP twice cuts all voices.

**Pattern structure**

- 8 banks × 16 patterns = 128 patterns per project.
- Per-track length 1-64 steps (polymeter) and per-track speed: 2×, 1×, 3/4×, 1/2×, 1/4×, 1/8×. A master length resets all tracks.
- 16 steps per page, 4 pages for 64 steps.

**Step data**

| Field | Range | Notes |
| --- | --- | --- |
| Trig | on, off, trigless | Trigless = locks without a new note |
| Note | 0-127 | POLY tracks add chord type and inversion |
| Velocity | 1-127 |  |
| Length | 1/16 step to 64 steps | **[rev]** The table this indexes was never published; it is now `kLengthSixteenthsOfStep` in `core/timing.hpp`: 20 entries from 1/16 step to 64 steps, held in sixteenths of a step so every entry is exact and independent of the track's speed. **Index 0 is reserved as invalid**, so a zeroed or corrupt byte is refusable rather than meaning the shortest note. A sixteenth of a 24-tick step is 1.5 ticks, which 96 PPQN cannot express — gate length is therefore converted to *samples* at control rate, not to ticks |
| Micro-timing | ±11 ticks | 24 ticks per 16th at 96 PPQN. Finer placement is available in samples through the tick's `.frame` offset if live-recorded hits need it **[rev]** |
| Probability | 0-100% |  |
| Condition | see below | **[rev]** `A:B` has nowhere to live: this table gives `condition` one byte, but `A:B` with B = 2-8 is 35 combinations on top of 10 condition kinds, and 4 bits of kind plus 6 bits of pair do not fit in 8. The byte is therefore **partitioned**, not enumerated: 0-9 are the condition kinds, 16-50 are the 35 `(A,B)` pairs, everything else is invalid and reads back as `NONE`. The alternative was growing `Step` past 12 bytes |
| Ratchet | 1-8 hits | With a velocity ramp up or down |
| Accent, slide | flags | Slide on MONO tracks only |
| Parameter locks | up to 8 per step | From a shared pool of 1,024 per pattern |

**Trig conditions**

- `A:B` fires on loop A of every B (B = 2-8).
- `FILL` and `NOT FILL` follow the fill key.
- `PRE` and `NOT PRE` follow the last condition result on this track; `NEI` and `NOT NEI` follow the track to the left.
- `1ST` and `NOT 1ST` fire on the first loop after a pattern change.

**Recording**

- Step entry: tap STEP to toggle a trig; hold STEP and turn an encoder to lock that parameter.
- Live recording: REC + PLAY records pad hits with velocity. Each hit is stamped with the audio clock's sample position minus the measured pad latency (about 4-8 ms), then quantized by a 0-100% amount. **[rev]** Incoming MIDI notes go through the same stamping, because `on_midi` is main context and carries no timestamp of its own.
- Motion recording: encoder moves during live recording write locks on the steps they pass.

**Pattern changes and chains**

- The next pattern is queued and switches at the end of the current one, or after 16 or 32 steps, or at once keeping the position.
- Chains: up to 64 entries per project, each a pattern plus a repeat count. No song timeline in v1.
- Copy, paste and clear for steps, tracks and patterns; 16 levels of undo for edits.

## Performance and creativity

Eight tools change the music live without touching the stored pattern unless you commit: mutes, fills, scenes with morphing, beat repeat, throws, mutation, macros and resampling.

| Tool | What it does | Commit? |
| --- | --- | --- |
| Mutes | Lower pads toggle track mutes, instantly or at the next bar. 8 stored mute states per project, recalled from the upper row | Saved with the pattern |
| Fill | Hold to activate `FILL` conditions; optional auto-fill every 4, 8 or 16 bars | No |
| Scenes and morph | A scene stores parameter values for any tracks. Assign scene A and scene B, then VALUE crossfades the live sound from A to B. 16 scenes per project | Scene edits save; the morph position does not |
| Beat repeat | Hold to loop the last 1/4, 1/8, 1/16 or 1/32 (or triplets) of the master output from a 2-bar ring buffer, with optional pitch-down | No |
| Throws | Hold to push delay or reverb send to full on the selected tracks; release returns it | No |
| Mutation | Variations of the selected track: rotate, Euclidean density up or down, bounded randomizing of velocity, micro-timing, probability and notes in scale. Seeded and undoable | OK keeps it, CLR reverts |
| Macros | 4 macros on encoders A-D in PERFORM mode. Each drives up to 4 parameters on any tracks, each towards its own end. **[rev]** At 0 a macro lets go, and turning it up sweeps each parameter from its live value (kit, then morph) to that end, so a resting macro never holds an EDIT knob or a scene (Roland, 2026-11-22) | Macro setup saves |
| Resample | Record the master or one track for 1, 2, 4 or 8 bars to `/RECORD` as WAV, then load it into a SAMPLE or GRAIN track in one action. **[rev]** Or record the audio input (2026-11-22): encoder B in RESAMPLE picks MASTER or an input route (LINE, LINE L, MIC, DYN MIC, HI-Z) through `enaga::system::input`, C its level; the take is `audio()`'s `in`, bar-aligned like a master take, with a meter and no monitoring. Recording one track alone is still not built | Writes a file |

**Musical defaults that help creativity**

- Global key and scale. Melodic tracks fold notes into the scale, and KICK tunes to the root.
- Euclidean generator per track (hits, steps, rotation) as a starting point that you then edit by hand.
- Pad pressure modulates the selected track's macro 1 while a pad is held. **[rev]** Use Enaga's `on_pad_pressure` event (0-127, already shaped by its own cube curve); the app-side smoothing and change-filtering this document planned over raw `on_pad` readings is no longer needed.
- Every random operation takes its seed from the pattern, so a mutation can be recalled exactly.

## Panel mapping and UI

The 8 lower pads are the 8 tracks, the 7 upper pads are a fixed performance row, and the function block picks the mode. Labels below are the SmplTrek's; the Lofi-12 XT and CyDrums print others at the same places, so names on screen come from `enaga::system::device()`.

**Pads**

The panel controller numbers pads left to right, alternating rows: lower row = even ids 0-14, upper row = odd ids 1-13.

| Pad | Id | Role |
| --- | --- | --- |
| Lower 1-8 | 0, 2, 4, 6, 8, 10, 12, 14 | Tap: select track and play it with velocity. In MUTE mode: toggle mute. F1 + pad: toggle mute in any mode |
| Upper 1 | 1 | FILL (hold) |
| Upper 2 | 3 | REPEAT (hold; VALUE picks the length while held) |
| Upper 3 | 5 | MUTE mode (toggle) |
| Upper 4 | 7 | Scene A select (hold + STEP picks the scene) |
| Upper 5 | 9 | Scene B select (hold + STEP picks the scene) |
| Upper 6 | 11 | THROW (hold; delay, or reverb with F1) |
| Upper 7 | 13 | MUTATE selected track (OK keeps, CLR reverts) |

**Function block and other keys**

| Key | Plain | With F1 |
| --- | --- | --- |
| F1 (FUNC) | Shift | F1 + POWER is the Enaga menu |
| F2 (REC) | Live record on or off | Resample setup |
| F3 (PROJECT) | Pattern mode: STEP 1-16 queues a pattern in the bank. **[rev]** The bank is on the selection keys in this mode | F1 + F3 twice = save project. **[rev]** No longer bank select: the selection keys took it |
| F4 (HOME) | GRID mode: STEP keys edit the selected track | KEYS mode: STEP keys play notes in scale |
| F5 (PLAY) | Play | Restart from step 1 |
| F6 (STOP) | Stop; twice cuts all voices | Stop and reset all tracks |
| F7 (MIXER) | Mixer: A-D levels of 4 tracks, selection keys switch 1-4 and 5-8 | PERFORM mode: A-D macros, VALUE morph |
| F8 (EDIT) | Sound pages of the selected track; selection keys switch SRC, FLTR, AMP, FX | Kit and machine select. **[rev]** The first press is MACHINE; F1 + F8 again opens the KIT browser (16 kits on the card), and again goes back |
| Octave left/right | Step page 1-4 in GRID; octave in KEYS | Pattern length |
| UP, DOWN | **[rev]** The rows on screen: the track in GRID, EDIT and KEYS; the machine in MACHINE; the kit slot in KIT; the length in RESAMPLE; the mutation in PERFORM; the slot cursor in PROJECT. With a STEP held: copy, paste | Copy, paste. In PROJECT: chain on/off, reload the sample. In KIT: read the card again, save the kit into the slot. In PERFORM: auto-fill |
| LEFT, RIGHT | **[rev]** Sideways: the page wherever the selection keys flip pages; the step on the step page; octave in KEYS; macro in PERFORM; track in MACHINE; the page of eight kit slots in KIT; destination in RESAMPLE; the slot cursor in PROJECT | Free |
| OK, CLR | Confirm, cancel. **[rev]** OK picks the slot under the cursor in PROJECT, and recalls the kit under it in KIT (twice, since a recall is not undoable), and in EDIT or MACHINE on a SAMPLE or GRAIN track opens the sample browser, where it loads a file or opens a folder; CLR closes the step page and SETUP, and goes up a folder in the browser | F1 + CLR = undo, F1 + OK = redo |
| VALUE | Tempo; push + turn = swing. **[rev]** Shows `EXT` and refuses edits when CLOCK SRC is not INTERNAL | Master DJ filter |
| Push A, B, C | **[rev]** Remove a lock with a STEP held. On the MIXER's EQ page: the kill EQ's LOW, MID, HI kill switch | **[rev]** The master kill EQ's LOW, MID, HI kill switch, in every mode |

**[rev] Navigation.** The arrows follow Enaga's own menu and SETTINGS page, so one habit works in
STRATA and in Enaga: UP and DOWN move through the rows on screen, LEFT and RIGHT move sideways,
OK picks, CLR goes back, and F1 + an arrow is the mode's second meaning. New list-like settings
(chain entries, swing) belong on the arrows, not on new key chords.

**Step editing**

- Tap STEP: toggle trig. Hold STEP + turn A-D: lock that parameter on the current page. Hold STEP + push A-D: clear that parameter's lock on the step (one undo level). Hold STEP + VALUE: note.
- Hold STEP + OK: open the step page (note, velocity, length, micro-timing / probability, condition, ratchet, accent and slide) on A-D, selection keys switch its two pages.
- Hold two STEP keys: set the selection range for copy, clear and mutation.

**LED language**

| LED | State | Look |
| --- | --- | --- |
| STEP | Trig | Green 6 |
| STEP | Trig with locks | Green 6 + red 3 (amber) |
| STEP | Trig with condition or probability | Green 2 |
| STEP | Playhead | Red 10 |
| Lower pads | Track idle / triggered / muted | Track colour at 2 / 10 for 60 ms / off |
| Upper pads | Function available / active | White 2 / function colour 10 |
| F-keys | Current mode | Green 8; REC red 8 while recording |
| VALUE ring | Beat pulse | White 10 on the beat, fading over 100 ms |

LEDs update from the main context only. The UI keeps a shadow of every LED and sends a command only when a value changes.

**Screen (128 × 128, 21 characters × 14 lines)**

- Top line on every screen: pattern (A01), tempo, play state, audio load in %.
- Bottom area: the 4 encoder slots, each a 3-letter name, a value and a small bar.
- GRID: an 8 × 16 mini-grid of all tracks (8 px per step), the selected track highlighted.
- EDIT: the 4 parameters of the current page in large type. STEP page: the step fields.
- MIXER: 8 level meters with mute and solo state. PERFORM: 4 macro bars and the A/B morph position.
- Drawing does no allocation and no file access; screens draw only from UI state.

## Data model and storage

A project is one folder on the card. It loads fully into RAM when opened (patterns about 1.1 MiB, samples up to 64 MiB) and saves bank by bank with Enaga's crash-safe `files::save`.

**Card layout**

```
/ENAGA/APPS/STRATA/APP.ENA                  the app (8.3 folder name = package name)
/ENAGA/DATA/STRATA/SETTINGS.TXT             last project, preferences (<enaga/settings.hpp>)
/ENAGA/DATA/STRATA/PROJECTS/<NAME>/PROJECT.BIN   tempo, scenes, macros, chains, mute states
                                  /KIT.BIN       per track: machine, 16 parameters, LFO, mixer, sample path
                                  /BANK_A.BIN … BANK_H.BIN   16 patterns each
/ENAGA/DATA/STRATA/AUTOSAVE/                the working copy, same layout
/ENAGA/DATA/STRATA/KITS/KIT_01.BIN … KIT_16.BIN   [rev] the kit library, shared by every project; KIT.BIN's format
/SAMPLES/…                                  user samples, WAV 16/24/32-bit, mono or stereo
/RECORD/STRATA_<n>.WAV                      resampled audio
```

**File format rules**

- Little-endian binary: magic `STRA`, format version, section table, CRC-32 over the payload.
- Written and read byte by byte through a small serializer. Never cast file bytes to structs (alignment rule).
- An unknown version or a bad CRC is refused with a message on screen; it never crashes the app.

**Core structures (sketch)**

```cpp
struct Step {                 // 12 bytes
    uint8_t  flags;           // trig, trigless, accent, slide
    uint8_t  note, velocity;
    uint8_t  length;          // index into a table: 1/16 step .. 64 steps
    int8_t   micro;           // -11..+11 ticks
    uint8_t  probability;     // 0..100
    uint8_t  condition;       // enum: NONE, A_B, FILL, NOT_FILL, PRE, ...
    uint8_t  ratchet;         // count 1-8 (low nibble), ramp (high nibble)
    uint16_t lock_first;      // index into Pattern::locks, 0xFFFF = none
    uint8_t  lock_count;      // 0..8
    uint8_t  chord;           // POLY: type and inversion
};
struct Lock { uint8_t param, value; };
struct TrackPattern { uint8_t length, speed, swing; Step steps[64]; };
struct Pattern {
    TrackPattern tracks[8];
    Lock     locks[1024];     // shared pool, compacted on save
    uint16_t locks_used;
    uint8_t  master_length, swing, root, scale;
    uint32_t seed;            // for probability and mutation
};
```

About 6 KiB of steps plus 2 KiB of locks per pattern, so 128 patterns fit in about 1.1 MiB.
**[rev]** Confirmed by the implementation: `sizeof(Step) == 12` and `sizeof(Pattern) == 8236`, so
128 patterns are 1.006 MiB and `lock_first` at offset 8 is naturally aligned.

**[rev] Three gaps found while implementing this, all now decided:**

- **Mute states have nowhere to live.** The performance table says mute states are "saved with the
  pattern", but `Pattern` has no field for them. The serializer uses a section table and skips
  unknown sections, so this is cheap to add later as a new section; making it *required* needs a
  format version bump.
- **The lock pool's only reclaim is compaction.** Growing a step's lock run that is not at the top
  of the pool relocates it and abandons the old slots, so a long editing session can exhaust all
  1,024 slots while far fewer locks are actually reachable. "Compacted on save" is not frequent
  enough: the UI must compact when the pool reports no free slots and retry, before telling the
  user it is full.
- **Saving must not mutate the pattern.** The spec says the pool is "compacted on save". Writing
  renumbers lock runs into compacted order *as it writes*, from a `const Pattern &`, so a save
  running in the main context never writes pattern memory the audio side owns. Compaction in RAM
  is a separate, explicit operation.

**[rev]** Loading is **value-tolerant but structure-strict**: out-of-range field values are clamped
rather than refusing the file, while structural damage (bad magic, unknown version, bad CRC, a
malformed section table or a non-compacted lock pool) is refused. One bad byte that survives the
CRC then costs one odd-sounding step instead of a lost pattern.

**[rev] `Speed::x1` is 0, so zeroed memory means normal speed.** "Clear pattern" is a user-facing
feature and the obvious implementation is a `memset`; with 2x at zero that would silently double
every track's tempo. The same reasoning makes a per-track swing of 0 mean "follow the pattern".
The enum numbering is therefore not the order the UI shows the speeds in.

**Samples**

- Loaded whole into RAM as int16 at project open. **[rev] 32 MiB by default** (about 2.9 minutes of stereo at 48 kHz), raisable per project to the 64 MiB the memory budget allows: at the measured 7,600 KiB/s, 64 MiB costs 8.4 s to load against 4.2 s for 32 MiB. Voices convert to float as they read.
- Loaded in the background with `files::load`, whose `done` callback runs in the main context. A track whose sample is still loading stays silent rather than glitching.
- Files not at 48 kHz are resampled once at load, in main-loop slices.
- Streams (`files::Stream`) are used only for recording and for loops longer than 30 s.
- **[rev]** Samples are picked on the box with the **sample browser** (2026-11-22): OK in EDIT or
  MACHINE on a SAMPLE or GRAIN track lists `/SAMPLES` (and `/RECORD`) through `files::list`, and
  OK on a WAV puts its path in the track's kit and loads it through the ordinary swap, transport
  running. `core/browse.hpp` holds what is listed (WAVs and folders, no `._` files, 256 entries
  of 64 bytes, paths that fit the kit's 112 bytes).

**Saving**

- Save writes only dirty banks: about 130 KiB each, so the UI stalls for tens of milliseconds, never the audio.
- Autosave to `AUTOSAVE/` whenever the transport stops, and in `stop()` within its 3 s budget.
- On start, STRATA reopens the last project and offers the autosave if it is newer.

## Software architecture

Two contexts, one rule: the audio side owns scheduling and pattern memory, the main context owns everything slow, and they meet only through two lock-free rings and read-only pattern memory. **[rev]** Time itself belongs to Enaga's clock, which both contexts read.

&#91;embedded content: STRATA software architecture · 2 contexts, 3 shared structures\]

The UI never writes pattern memory directly. It posts every edit as a command, which the audio side applies at the start of the next block.

**Rules**

- Commands are fixed-size 16-byte structs in a power-of-two single-producer single-consumer ring. The audio side applies at most 64 per block, so a burst cannot break the deadline.
- Every block, the audio side publishes playhead, per-track trigger flags, peak meters, voice counts and load. The UI reads only the newest values.
- A save is consistent because the main context posts no commands while its blocking save runs.
- Live recording: the main context stamps each pad hit with the audio sample counter (an atomic the audio side updates) and posts a record command.
- Samples: the loader fills a new buffer, then posts a swap command. The old buffer is freed only after the status ring confirms the swap through a generation counter.
- DSP works in blocks: per-voice inner loops in `*.ti.c`, modulation once per block at control rate, no per-sample branches.
- `core/` builds on the host with no Enaga headers; `app/` is the only folder that includes `<enaga/app.hpp>`.
- **[rev] `core/scheduler`, not `core/clock`.** Given a tick from `clock::ticks()`, it decides which steps fire on which tracks, applying swing, per-track speed and length, and micro-timing. Swing and micro-timing both shift a step off its nominal tick, and a negative micro value means firing *before* it, so the mapping is inverted: for each arriving tick, scan the few steps whose nominal tick lies in `[t - 11 - swing, t + 11]`. The shift is bounded by less than half a 16th, so that is at most about 3 steps per track per tick — bounded and branch-free-able.

## CPU and memory budget

**[rev] This section was rebuilt on Enaga's real unit measurements.** The original estimate (about 42%, worst case 54%) happens to land in the right place, but for the wrong reasons, and the conclusion it drew was backwards.

Measured on a unit with caches on, against the 496,000-cycle block:

| Workload | Cycles/voice | Share of block | Unit cap |
| --- | --- | --- | --- |
| SYNTH (full VA: 2 osc, sub, noise, filter, 3 ADSR, 2 LFO, 8-slot matrix) | 1,868 | 0.377% | 228 voices (85.9%) |
| SAMPLE (reads DDR) | 2,592 | 0.523% | 163 voices (85.3%) |
| GRAIN (reads DDR) | 2,690 | 0.542% | 160 voices (87.0%) |

**Voices are 5-8× cheaper than this document first assumed.** STRATA's full 8-track worst case is 17 computed voices plus 8 grains:

```
T1-T5 drums/perc  10 computed x 1,868 = 18,680
T6 BASS            1 computed x 1,868 =  1,868
T7 CHORD           6 computed x 1,868 = 11,208
T8 LOOP            8 grains   x 2,690 = 21,520
                        total = 53,276 cycles = 10.7% of the block
```

That is conservative: Enaga's 1,868-cycle voice is a full VA with a mod matrix, where the drum machines are far simpler.

**The effects are the real cost.** Delay and reverb together measured **16% of the budget on their own** — a fixed tax paid on every block however many tracks sound, and larger than all 18 voices put together:

| Component | Estimate (cl6x) | Basis |
| --- | --- | --- |
| 18 voices, worst case | 11% | measured per-voice figures |
| Send A delay + Send B reverb | 12-16% | measured together at 16% |
| 8 per-track insert chains | 8-16% | extrapolated; the main unknown |
| Master (DJ filter, glue comp, limiter) | 2-4% | extrapolated |
| **[rev]** Master kill EQ, only while a band is off flat | ~1.4% (GCC-only ~2.2%) | host proxy, docs/BUDGET.md; 0 while bypassed |
| Sequencer, command ring, beat-repeat write | ~2% | extrapolated |
| Enaga itself | 3% | measured |
| **Total** | **~38-52%** | inside the 60% target |

Two conclusions replace the original ones:

- **Cut effects or insert stages before cutting voices.** The chord track is *not* the lever this document claimed: 4 voices instead of 6 saves 3,736 cycles, about **0.75%**, not 5%.
- **cl6x is a requirement, not an optimization.** Enaga's synth is 30% of budget with cl6x and 65% with GCC alone. A GCC-only STRATA does not fit under 60%. Record every load figure both ways so the GCC-only cost stays visible instead of surfacing at release.

The 8 insert chains are the largest unmeasured term in the design, so they are measured **before** any engine work rather than at the M1 and M2 exits — see M0.5 in `docs/PLAN.md`. Keep all real figures in `docs/BUDGET.md`.

**Memory (about 124 MiB available)**

| Use | Budget |
| --- | --- |
| Samples, int16 | 64 MiB |
| Beat repeat ring: 2 bars at 60 BPM, stereo float | 3 MiB |
| Delay and reverb buffers | 2 MiB |
| Patterns, kit, scenes, 16 undo levels | 2 MiB |
| Recording ring and stream buffers | 1 MiB |
| App image, voice pools, UI | 4 MiB |
| **Total** | **76 MiB, leaving about 48 MiB spare** |

## Platform status

**[rev] This section replaces "Enaga platform work needed".** It asked for MIDI DIN, SYNC, input selection and pad pressure as upstream contributions, calling MIDI "the one platform gap that blocks STRATA". All of it already exists: Enaga landed the lot in the eight commits between `269c015`, which this document originally pinned, and `1b41111`. **STRATA needs no Enaga fork and no upstream feature work.**

| Item | Status at `1b41111` | What STRATA still does |
| --- | --- | --- |
| MIDI DIN in/out | Implemented: UART0 driver, full MIDI 1.0 parser, send, THRU, MIDI CH. `<enaga/midi.hpp>`, `on_midi`, `on_midi_sysex` | Expose settings; stamp notes for live recording |
| Clock, transport, SYNC | Implemented: one 96-PPQN clock driven by the app, MIDI clock or SYNC IN. `<enaga/clock.hpp>`, `on_transport` | Schedule steps on `clock::ticks()`; expose CLOCK SRC, CLOCK OUT, SYNC OUT |
| USB MIDI | Implemented: TinyUSB 0.21.0 with a C6748 MUSB port | Nothing — it shares the `on_midi` path |
| Input selection | Implemented: `enaga::system::input(source, level)` on the original REC SRC routes | Pick a source for resampling |
| Pad pressure | Implemented: `on_pad_pressure`, 0-127, with Enaga's own cube curve | Use the event; no app-side filtering of raw readings |
| USB audio, USB mass storage | **Still missing**, planned for Enaga E5 | Out of scope for v1 |
| SYNC polarity settings | **Not built** — Enaga uses rising edges in, active-high out | Note the limitation; the original was selectable |

**The catch: none of it has run on a unit.** Enaga's MIDI, SYNC, USB MIDI, input routes and pad-pressure feel are all emulator-tested only. STRATA will probably be the first thing to exercise them on hardware, so expect to find Enaga bugs rather than STRATA bugs during sync verification, and budget time for it. Report findings upstream with the failing case; do not patch `third_party/enaga` in this repo.

Two loose ends worth knowing: Enaga's USB IDs are TinyUSB test values (`0xCAFE/0x4011`) that its own design log says must not ship in a release, and the input routes have not been checked route by route.

## Milestones

**[rev] `docs/PLAN.md` holds the authoritative build order.** The list below is kept as the shape of the project; the plan revises it in three ways: a budget spike (M0.5) goes in before any engine work, because the 8 insert chains are the largest unmeasured cost in the design; external sync moves from M7 into M1, because it changes transport and tempo behaviour that M1 builds; and M7 shrinks from implementing MIDI upstream to verifying Enaga's existing MIDI on a unit.

1. **M0, setup.** Toolchain, emulator, Enaga pinned as a submodule, repo skeleton, host test runner.
   - Exit: an empty STRATA app shows its name and audio load in the emulator panel and on the unit; host tests run with one command.
2. **M0.5, budget spike [rev].** A throwaway load harness: 8 insert chains, both sends, the master chain, no voices.
   - Exit: measured figures in `docs/BUDGET.md` with and without cl6x, and a go/no-go on 8 full insert chains.
3. **M1, scheduler and drums [rev].** `core/scheduler` on Enaga's ticks, `core/pattern` and its serializer, KICK, SNARE and HAT machines, GRID mode, 4 tracks, play, stop, tempo, swing, **and external-sync handling**.
   - Exit: a 4-track beat at 130 BPM on the unit under 15% load; onset jitter in a line-out recording under 0.1 ms; the transport follows external MIDI clock.
4. **M2, full kit and mixer.** 8 tracks, PERC, SAMPLE and VA MONO machines, insert chain, sends, master bus, sidechain duck, EDIT and MIXER modes, kits.
   - Exit: the techno template plays all 8 tracks under 40% worst-case load, measured on the unit.
5. **M3, sequencer depth.** Parameter locks, trig conditions, probability, ratchets, micro-timing, polymeter, live and motion recording, undo.
   - Exit: every step field from the Sequencer section works and survives a save and reload.
6. **M4, projects and storage.** Projects, banks, queued pattern changes, chains, sample loading, autosave.
   - Exit: a power cycle restores the last state; pulling the card during a save never corrupts the project.
7. **M5, performance layer.** Mutes and mute states, fill, scenes and morph, beat repeat, throws, macros, mutation.
   - Exit: a 30-minute jam using only the performance row, F7 PERFORM and pattern changes, with no edit screens.
8. **M6, full concept.** VA POLY with chords, GRAIN, KEYS mode, resampling, pad pressure, Euclidean generator.
   - Exit: all machines and tools from this spec, worst-case load under 60%.
9. **M7, sync on hardware [rev].** STRATA's sync settings, then verification of Enaga's MIDI and SYNC on a unit — not upstream feature work.
   - Exit: one hour locked to the Force's MIDI clock with no drift; STRATA as clock master drives external gear.

## Testing and verification

Every feature is proven three times: by host tests on your PC, by a scripted run in the emulator, and by a checklist item on the unit. Only the unit's numbers count for CPU load and timing.

| Level | What runs | What it proves |
| --- | --- | --- |
| Host | `core/` and `dsp/` built with host g++; a test runner plus an offline renderer that writes N bars to WAV | Sequencer logic, conditions, locks, serializer round-trips, mutation determinism, DSP sanity. **[rev]** The scheduler is driven from a synthetic tick stream, so the tests are fast and deterministic and need no clock |
| Emulator | Enaga's `tools/emulate.py` with scripted key and pad presses, and `tools/panel.py` for playing by hand | The app builds, boots, reacts to input and renders frames and audio on the real firmware |
| Unit | The installed app with a debug overlay (audio load and peak, voice counts, underruns) | Real CPU load, timing, pad feel, card behaviour |

**Rules**

- Tests are deterministic: fixed seeds, fixed tempo, fixed input scripts.
- **[rev]** Clock drift is Enaga's guarantee, not STRATA's: the "no drift over a simulated hour at 40, 130 and 300 BPM" test belongs upstream. STRATA tests swing, speed, length, micro-timing, conditions and seeded mutation.
- **[rev]** Every load figure is recorded twice, with cl6x and with `ENAGA_TI_CGT=none`, so the GCC-only cost never has to be discovered at release.
- Golden renders compare onset times and levels within a tolerance, never bit-exact output. GCC for the DSP computes float division with a reciprocal estimate, so device and host results differ in the last bits.
- The emulator has no cache or memory-latency model: on the unit pure computation ran about 25% faster and sample reading slower. Use emulator load figures only to compare versions.
- Timing check on the unit: record line out into a computer at 48 kHz and measure the spacing of 16th-note clicks over 5 minutes.
- Soak test before each milestone exit: one hour playing with pattern changes and the performance tools, then check the peak load and underrun count.
- Card tests: pull the card during a save, during sample loading and during resampling; the app must recover without a restart.

**Unit safety checklist**

- SYSTEM INFO shows BOOT 1.0.17 (all recovery evidence is for that bootloader).
- An official `SmplTrek.bin` sits on a second SD card. HOME (F4) held at power-on reaches BOOT MODE for recovery.
- Risky builds run on the adapter without batteries, because a hung image leaves the power key dead.
- STRATA and its Enaga build never write the SPI flash and never ship BOOT or MCU sections.

## Open questions and risks

**[rev]** The biggest risk is no longer MIDI, which exists. It is the compiler: cl6x is worth about 2.2× and the headline target depends on shipping code built with it.

| Risk | Impact | Mitigation |
| --- | --- | --- |
| **TI cl6x licence for distributing built code [rev]** | **The 60% target is unreachable on GCC alone, so this blocks the headline claim, not just a release date** | Read TI's redistribution terms in M0, before the architecture leans further on it; keep every `*.ti.c` GCC-clean; record both figures always |
| **8 insert chains and the master cost [rev]** | The largest unmeasured term; a late discovery would force an architecture change | Measure in M0.5, before any engine work. If the fixed tax exceeds ~30%: mono sends, a shared drum filter, lo-fi out of the fixed chain, or a cheaper reverb |
| Enaga's app interface changes before 1.0 | Rework in the glue layer | Pin an Enaga release (`v0.2.0`) that matches the unit's firmware; keep all Enaga calls in `app/` behind a thin adapter |
| **Enaga's MIDI and SYNC have never run on a unit [rev]** | Sync may fail in ways that are not STRATA's fault | Expect to debug Enaga during M7; verify early with a cable loop; report upstream with the failing case |
| Pad feel (velocity from two reports, crosstalk between some pads) | Finger drumming feels off | Tune the velocity curve per pad; test early in M1. Enaga lists crosstalk as an open item |
| Built-in font has upper case, digits and 11 symbols only | Cramped, plain screens | Draw a custom bitmap font and icons into `canvas.data()` — and note there is no UI toolkit at all, so every widget is STRATA's |
| Project load time: 64 MiB of samples at about 7.6 MB/s is about 8.4 s | Slow project switching | Background `files::load`, drums playable first, **32 MiB default cap [rev]** raisable per project |
| Only one unit has ever run Enaga | Unknown hardware variance | Follow the unit safety checklist; report findings upstream |
| Windows development host **[rev]** | Enaga's tools are Linux-only | Build in WSL; no cmake, so the host runner is a plain Makefile; keep `build/` off `/mnt/d` |
| ~~MIDI pull requests not accepted upstream~~ | — | **Gone [rev]**: no platform work needed |
| ~~CPU figures are estimates~~ | — | **Reduced [rev]**: real per-voice figures exist and voices have large headroom |

**Open questions** — **[rev]** four of the five are now answered.

- ~~How much sample RAM per project?~~ **32 MiB by default**, raisable per project. 64 MiB at the measured 7,600 KiB/s is 8.4 s of loading; 32 MiB is about 4.2 s, with drums playable first.
- ~~Is 96 PPQN enough?~~ **Settled by the platform**: `ENAGA_CLOCK_PPQN` is 96 and not a choice. But the app owns the `.frame` offset within the block, so finer placement is available in samples if live-recorded hits need it. 96 PPQN for storage, sample offsets for feel.
- ~~Lower pads as tracks?~~ **Keep the 7-pad performance row.** Enaga's own synth example already treats the two pad rows as separate musical surfaces, so the split is idiomatic rather than a compromise.
- ~~Should scenes morph locks?~~ **Base kit values only in v1.** A lock is a per-step override and a morph is continuous, so crossfading the two has no single sensible meaning. Revisit after M5.
- **Final name: STRATA is still a working title.** The only one left open.

## Instructions for Claude Code

**[rev] `CLAUDE.md` in the repository root is the rule set**, corrected for the pin, the clock, cl6x and the WSL build host; the first prompts below were for M0 and are history. Use `CLAUDE.md`, not an older copy. The repo layout below stands, with the pin and `core/` corrected.

**Repo layout**

```
strata/
  CLAUDE.md               rules for Claude Code
  docs/SPEC.md            this document
  docs/REVIEW.md          the platform review this revision came from
  docs/PLAN.md            authoritative build order
  docs/BUDGET.md          measured CPU load per component, updated at every milestone
  docs/UNIT_CHECKLIST.md  what to check at the next session on the unit
  third_party/enaga/      git submodule, pinned at v0.2.0 (3da7f89)
  core/                   portable C++20: scheduler, pattern, steps, conditions, locks, mutation, serializer
  dsp/                    machines and effects; may use <enaga/dsp.h>, which also builds on the host
  app/                    Enaga glue: App subclass, input, LEDs, screens, files
  host/                   host test runner and offline renderer
  tests/                  host tests and emulator scenarios
  tools/build.sh          stages sources into one flat folder, then runs Enaga's build_app.py
  tools/panel.sh          builds and opens the emulator panel
```

Enaga's `tools/build_app.py` compiles only the `*.cpp` and `*.c` files of one flat folder, with only Enaga's include path — confirmed in its source, which uses `glob("*.cpp")`, not `rglob`. So `tools/build.sh` copies every source and header from `core/`, `dsp/` and `app/` into `build/stage/`. File names must be unique across the repo, and includes use bare file names (`#include "pattern.hpp"`). **[rev]** The staging script must fail loudly on a duplicate basename, because object files are named from the stem and a collision would otherwise silently drop a source file. **[rev]** The old alternative — adding an include-path option in an Enaga fork — is dropped: STRATA keeps no fork.

**CLAUDE.md and the first prompts [rev]**

`CLAUDE.md` in the repository root is the one copy to keep correct; the first prompts were for M0 and are history. The versions that stood here are superseded: they pinned `269c015`, told Claude Code to implement `core/clock` with a 32.32 fixed-point tick length and drift tests, and said platform work happens in an Enaga fork. All three are wrong — Enaga owns the clock, the pin is a release tag (now `v0.2.0`), and there is no fork.

## Sources

- [Enaga README](https://github.com/seawaffle/enaga): app model, build, install, emulator
- [Enaga API reference](https://github.com/seawaffle/enaga/blob/main/docs/api.md): every app call used in this spec
- [Enaga design log](https://github.com/seawaffle/enaga/blob/main/docs/design.md): unit load measurements, open items, pad pressure plan
- [Enaga hardware notes](https://github.com/seawaffle/enaga/blob/main/docs/hardware.md): keys, pads, LEDs, audio, SD card
- [smpltrek-re hardware access](https://github.com/seawaffle/smpltrek-re/blob/main/docs/04-hardware-access.md): MIDI UART0, SYNC, clocks, DDR2
- [smpltrek-re safety](https://github.com/seawaffle/smpltrek-re/blob/main/SAFETY.md): recovery and what can brick a unit
- **[rev]** `third_party/enaga/include/enaga/clock.hpp`: the clock STRATA schedules against
- **[rev]** [smpltrek-re](https://github.com/seawaffle/smpltrek-re): also the emulator. Clone it separately and set `ENAGA_EMULATOR` to the directory holding `cpu.py` — Enaga's tools take it by environment variable, not as a submodule
