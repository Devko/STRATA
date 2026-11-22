# STRATA concept review

Reviewed against the real Enaga platform on 2026-11-21. Evidence comes from clones of
`seawaffle/enaga` at `1b41111` (2026-11-20, 91 commits) and `seawaffle/smpltrek-re`.

**Verdict: the concept is sound and buildable, and it is easier than the spec thinks.**
The sound architecture, data model, panel mapping and milestone shape all survive contact
with the platform. Three things change materially: the pinned commit is wrong, an entire
milestone of platform work is already done, and the CPU budget is wrong in both directions.

---

## 1. Findings that change the work

### 1.1 The pinned commit excludes everything STRATA needs — critical

The spec pins Enaga at `269c015`. That commit is **8 commits behind `main`**, and those
8 commits are precisely the platform features the spec lists as missing:

| Commit | Lands |
| --- | --- |
| `5bcf656` | Pad pressure: `ENAGA_EVENT_PAD_PRESSURE`, `on_pad_pressure` |
| `31dc62d` | Input selection: `enaga::system::input(source, level)` |
| `3e54986` | **MIDI DIN**: UART0 driver, MIDI 1.0 parser, send, THRU, MIDI CH |
| `79238b9` | MIDI timing and loopback tests |
| `c6dae9b` | **Clock, transport and SYNC**: one 96-PPQN clock, MIDI clock or SYNC IN |
| `1b41111` | **USB MIDI**: TinyUSB 0.21.0 with a C6748 MUSB port |

`269c015` is only *"MIT licence; references point to the published smpltrek-re repository"*.
The spec was written against a pre-E4 snapshot and pinned the licence commit by mistake.

**Change: pin `1b41111` or later.** Pinning `269c015` would mean reimplementing all of the
above by hand.

### 1.2 M7 dissolves — no Enaga fork, no upstream PRs

The spec's "Enaga platform work needed" table, its "Proposed MIDI interface (to discuss
upstream)", the "How to contribute it" section and all of milestone M7 are obsolete:

| Spec says "not yet in Enaga" | Reality at `1b41111` |
| --- | --- |
| MIDI DIN in/out | Implemented. `<enaga/midi.hpp>`, `on_midi`, `on_midi_sysex`. Emulator-tested; **not yet run on a unit** |
| SYNC in/out | Implemented. SYNC IN rising edges, SYNC OUT 15 ms pulses. The original's polarity settings are not built |
| USB MIDI | Implemented (TinyUSB). USB IDs are test values `0xCAFE/0x4011` that "must not ship" |
| Input selection | Implemented in the API; routes not yet checked route-by-route on a unit |
| Pad pressure event | Implemented, with Enaga's own cube curve. Feel unchecked on a unit |
| USB audio / mass storage | Still absent — planned for Enaga E5 |

The risk the spec ranked highest ("MIDI pull requests not accepted upstream → maintaining a
fork") disappears. What remains is **verification on a unit**, which is STRATA's job either
way. M7 becomes a thin settings-and-verification task that folds into M1.

### 1.3 Do not build a clock — Enaga already has exactly the one specified

`include/enaga/clock.hpp` provides, verbatim, what `core/clock` was going to be:

- `ENAGA_CLOCK_PPQN == 96` — the spec's PPQN, now a platform constant.
- `clock::ticks(Tick *out, uint32_t max)`, **audio context**: the ticks falling in the block
  being computed and `.frame`, the frame each falls on — *"so steps land on the right sample"*.
  That is the spec's "every event carries a sample offset inside the 64-frame block".
- `start/stop/continue_/locate/set_tempo`; state as `running/source/tempo/position/fraction`.
- Tempo **20–300** BPM, set as BPM×100, so 0.01 BPM resolution (the spec asked for 40–300 in 0.1).
- Source is the user's CLOCK SRC setting: `ENAGA_CLOCK_INTERNAL`, `_MIDI`, `_SYNC`.
- With CLOCK OUT on, Enaga already sends MIDI clock and transport; with SYNC OUT, pulses.

**Change:** `core/clock` becomes `core/scheduler` — swing, per-track length and speed,
micro-timing, probability, conditions and ratchets on top of Enaga's ticks. The spec's host
test "no drift over one simulated hour at 40, 130 and 300 BPM" is now Enaga's
responsibility, not STRATA's, and the 32.32 fixed-point tick length leaves STRATA entirely.

### 1.4 STRATA does not own the tempo — design this in at M1

A consequence the spec misses. The clock is **unit-wide** and the user's CLOCK SRC setting
governs it: *"with an outside source they do nothing and return false, and `on_transport`
tells the app what the outside clock did."*

So:

- `F5 PLAY` / `F6 STOP` must handle `clock::start()` returning `false` and follow
  `on_transport` instead. The spec's "PLAY starts, STOP stops" is not unconditional.
- `VALUE = tempo` must read `clock::tempo()` back and show `EXT` when
  `source() != ENAGA_CLOCK_INTERNAL`, because `set_tempo` will be refused.
- The spec defers sync to M7. It cannot be deferred: external sync changes the transport and
  tempo behaviour that M1 builds. **Handle it in M1.**

### 1.5 The CPU budget is wrong in both directions

The spec says only Enaga's 3% is measured and the rest is extrapolated from "sample voice
about 0.5%, example synth 2–3% per voice". Real unit measurements exist
(`docs/design.md`, first unit session, caches on, 496,000 cycles per block):

| Workload | Cycles/voice | Share of block | Unit cap |
| --- | --- | --- | --- |
| SYNTH (full VA: 2 osc, sub, noise, filter, 3 ADSR, 2 LFO, 8-slot matrix) | 1,868 | **0.377%** | 228 voices (85.9%) |
| SAMPLE (reads DDR) | 2,592 | **0.523%** | 163 voices (85.3%) |
| GRAIN (reads DDR) | 2,690 | **0.542%** | 160 voices (87.0%) |

Two corrections follow.

**Voices are 5–8× cheaper than the spec assumed.** STRATA's full 8-track worst case is
17 computed voices plus 8 grains:

```
T1-T5 drums/perc  10 computed x 1,868 = 18,680
T6 BASS            1 computed x 1,868 =  1,868
T7 CHORD           6 computed x 1,868 = 11,208
T8 LOOP            8 grains   x 2,690 = 21,520
                        total = 53,276 cycles = 10.7% of the block
```

That figure is conservative: Enaga's 1,868-cycle voice is a *full* VA with a mod matrix,
where STRATA's drum machines are far simpler.

**The chord-voice lever is wrong.** The spec: *"The chord track is the biggest lever: 4
voices instead of 6 saves about 5%."* Two VA voices are 3,736 cycles = **0.75%**, not 5%.
Cutting chord voices buys almost nothing.

**The effects are the real cost, and the spec does not budget for them.** Measured on the
emulator: *"reverb and delay alone 16% of the audio budget"*. That is a **fixed tax** paid on
every block however many tracks sound, and it is larger than all 18 voices put together.
Add 8 insert chains (drive, Svf, lo-fi, env, duck, pan, level) and the master chain, and the
picture inverts:

| Component | Estimate (cl6x) | Basis |
| --- | --- | --- |
| 18 voices, worst case | 11% | measured per-voice figures |
| Send A delay + Send B reverb | 12–16% | measured together at 16% (emulator) |
| 8 per-track insert chains | 8–16% | extrapolated; **unmeasured, the main unknown** |
| Master (DJ filter, glue comp, limiter) | 2–4% | extrapolated |
| Sequencer, command ring, beat-repeat write | ~2% | extrapolated |
| Enaga itself | 3% | measured |
| **Total** | **~38–52%** | inside the 60% target |

The headline lands near the spec's 42%, but for different reasons, and the guidance that
follows from it reverses: **cut effects or insert stages before cutting voices.**

### 1.6 TI cl6x is load-bearing, not a nice-to-have

Measured: Enaga's synth worst case is **30% of the budget with cl6x and 65% with GCC alone**
— about 2.2×. Applying that to the table above puts STRATA near or past 100% on a GCC-only
build.

The spec files the cl6x licence as a release-speed risk ("Release blocked or slower GCC
build"). It is bigger than that: **the 8-tracks-under-60% target is not reachable without
cl6x.** Keeping every `*.ti.c` GCC-clean remains right as a portability rule, but a GCC-only
build will not hit the headline target. Check TI's redistribution terms early — the
architecture already depends on the answer.

### 1.7 MIDI arrives in the main context, unstamped

The spec proposes: *"The UART interrupt stamps every received byte with the audio sample
counter... The app reads incoming MIDI inside `audio()` with those timestamps."* Enaga does
not work that way. `on_midi` and `on_transport` are both documented **main context**.

This does **not** hurt clock sync: Enaga consumes MIDI clock internally and `clock::ticks()`
still reports sample-accurate frames, so sync jitter stays inside Enaga. It does affect
**MIDI note input for live recording** — notes arrive unstamped with the same latency problem
as pads. Fix: stamp MIDI notes with the audio sample counter exactly as the spec already
plans for pad hits. One mechanism covers both.

### 1.8 The build host is Linux-only; this machine is Windows

Enaga's README: *"Requirements: Linux, Python 3.10 or newer, host `gcc`/`g++`"*. Windows is
not supported, and prebuilt toolchains are only a 1.0 plan.

Checked on this machine:

| | Git Bash (Windows) | WSL Ubuntu 24.04 |
| --- | --- | --- |
| g++ / gcc | **missing** | 13.3.0 |
| make | missing | 4.3 |
| cmake | missing | **missing** |
| python3 | 3.12.10 | 3.12.3 |
| git | 2.43 | 2.43 |

**WSL Ubuntu is the build host** (16 cores, 15 GiB RAM, `/mnt/d/DEV` visible). Two notes:
`cmake` is absent, so the host test runner should be a plain Makefile or shell script rather
than depend on cmake; and building on `/mnt/d` crosses the 9p filesystem boundary, which is
slow — keep `build/` inside the WSL filesystem.

The tic6x cross-compiler is **not prebuilt**: `tools/build_tic6x_binutils.sh` (a few minutes)
then `tools/build_tic6x_gcc.sh` (10–30 minutes), installing into `tools/tic6x`.

### 1.9 The flat-folder build constraint is real — confirmed in code

`tools/build_app.py` line 115:

```python
sources = sorted(app_dir.glob("*.cpp")) + sorted(app_dir.glob("*.c"))
```

`glob`, not `rglob`: **no subdirectories**. The only include path is `-I{INCLUDE}`
(Enaga's own). The spec's staging approach is therefore a requirement, not a preference:
`tools/build.sh` must copy `core/`, `dsp/` and `app/` flat into `build/stage/`, filenames
must be unique across all three, and includes must be bare (`#include "pattern.hpp"`).
Object files are named from the stem, so `core/state.cpp` and `app/state.cpp` would collide.

### 1.10 No UI toolkit exists

Enaga commit `40a25df` plans *"a small first UI toolkit"*, but `include/enaga/` has no UI
header. STRATA writes every screen, the LED shadow and all widgets itself on `Canvas`
(`clear`, `pixel`, `rect`, `text`, `data()`). The spec's plan to draw a custom bitmap font
into `canvas.data()` is the right call, and it is load-bearing for every screen rather than a
nicety.

---

## 2. Validated without change

Everything below was checked against the headers and needs no revision.

| Spec claim | Status |
| --- | --- |
| 372 MHz C6748, ~124 MiB app heap, 496,000 cycles/block | Correct |
| 48 kHz, 64-frame blocks, float ±1.0 | Correct — and **interleaved stereo L R L R** |
| `ENAGA_BLOCK_FRAMES` 64, `ENAGA_SAMPLE_RATE` 48000 | Correct |
| Pads: lower row = even ids 0–14, upper = odd 1–13 | Correct (`docs/hardware.md`) |
| Key codes F1–F8, STEP1+n, OK, CLR, cursors, encoders | Correct (`abi.h`) |
| Pad raw ~1,500 light to ~3,400 hard, velocity 1–127 | Correct |
| `StereoDelay`, `Reverb` in `<enaga/dsp.hpp>` | Both exist, plus `Svf`, `Biquad`, `Oscillator`, `Envelope`, `Lfo`, `Random`, `Chorus`, `DelayLine`, `OnePole`, `Fir`, `Memory` |
| No libm; math in `<enaga/dsp.hpp>` | Correct |
| `files::save` crash-safe, `files::load` background, `files::Stream` | All exist |
| `<enaga/settings.hpp>` for SETTINGS.TXT | Exists (**32-key limit**) |
| `enaga::system::device()` for device-aware labels | Exists |
| Writable paths `/ENAGA/DATA/<pkg>`, `/SAMPLES`, `/RECORD` | Correct |
| Watchdog 1 s main / 100 ms audio / 3 s stop | Correct |
| `audio()` must not allocate, block, touch files, draw or set LEDs | Correct |
| Misaligned 32-bit read silently uses the address below | Correct — parse byte by byte |
| C++20, `-fno-exceptions -fno-rtti`, `-Wall -Wextra -Werror` | Correct |
| Stack frames ≤ 16 KiB, no VLAs or `alloca` | Correct (rejected at build time) |
| Screen 128×128, 16 greys, 5×7 font, A–Z and digits only | Correct; `draw()` every 50 ms = 20 fps |
| `Step` = 12 bytes → 128 patterns ≈ 1.03 MiB | Arithmetic checks out (`uint16_t` at offset 8 is aligned) |
| Beat-repeat ring 2 bars @ 60 BPM stereo float = 2.93 MiB | Correct |
| Memory total ~76 MiB of 124 MiB | Comfortable |
| Official recovery image on a second card | `SmplTrek.bin` v3.2.174 is already at `D:\DEV\smpltrek\` |

**Three constraints the spec does not mention:**

1. **Destructors never run.** Static constructors do. All state must be saved in `stop()`,
   never in a destructor.
2. **At most 8 open files and 8 streams per app.** The spec uses streams for recording and
   for loops over 30 s; the ring must be a power of two ≥ `ENAGA_STREAM_RING_MIN` and
   belongs to Enaga until `close`.
3. **`on_card(bool)`**: on removal, open files are dead and must be closed. The spec's card
   tests need this callback wired, not just retry logic.

---

## 2a. Deviations from the spec's key table, and why

docs/SPEC.md's key table assigns every key, plain and with F1. Three things it does not give a
gesture to had to be reachable anyway, so each took a key that does nothing in the mode it is
used in. All three are small and all three are here rather than only in a comment.

| What | Where it went | Why it could not go where the spec says |
| --- | --- | --- |
| **The send and master settings** | MIXER mode's pages 3-6: DLY, REV, ROUTE, MST, on the selection keys next to the two pages of faders | The spec gives the sends no gesture at all and the master only F1 + VALUE for the DJ filter. Fourteen settings reach PROJECT.BIN; a setting in a file that no gesture can change is a setting nobody can use. MIXER is the mode already about the buses |
| **The record quantize amount** | PROJECT mode's SETUP page, encoder A | It was on F1 + VALUE, which the spec gives to the master DJ filter. That had nowhere to go either -- it is one knob with no page -- so the spec's own assignment won and quantize moved |
| **The SETUP page itself** | The octave keys, in PROJECT mode only | Every F-key is taken, plain and with F1. In PROJECT mode the octave keys were setting GRID's step page, which is not on screen there, so the gesture was free |

**EDIT has no encoder-slot strip at the bottom**, against the spec's "Bottom area: the 4 encoder
slots" on every screen. EDIT already shows the same four parameters in large type, one row per
encoder with its letter, and the strip only repeated them; the space it took now names the
machine gesture (F1 + EDIT). Every other screen that edits parameters keeps the strip, drawn as
name over value because "CUT 064" on one line is 41 pixels in a 32-pixel slot.

**CLOCK SRC, CLOCK OUT, SYNC OUT and MIDI CH are NOT editable, and that is not a deviation.**
`<enaga/clock.hpp>` and `<enaga/midi.hpp>` expose `source()` and `channel()` and nothing that
writes either: they are the user's settings, in the Enaga menu. docs/SPEC.md's "STRATA only
exposes the settings" can therefore only mean *shows* them, which the SETUP page does, with a
line saying where they live. That is the whole of M7's app-side work besides the tempo refusal
the spec asks for in its key table, which `app/` now honours.

---

## 3. Answers to the spec's open questions

**How much sample RAM per project?** 64 MiB at the measured 7,600 KiB/s is about **8.4 s** of
loading. Recommend a 32 MiB default cap (~4.2 s), raisable per project, with background
`files::load` and drums playable first — the spec's own mitigation, just with a lower default.

**Is 96 PPQN enough?** No longer a choice: `ENAGA_CLOCK_PPQN` is 96. But the app owns the
`.frame` offset within the block, so finer-than-tick placement is available in **samples** if
live-recorded hits need it. 96 PPQN for storage, sample offsets for feel.

**Lower pads as tracks, 7-pad performance row?** Keep it. Enaga's own synth example already
treats the two pad rows as separate musical surfaces, so the split is idiomatic.

**Should scenes morph locks?** Base kit values only in v1. A lock is a per-step override and a
morph is continuous, so crossfading the two has no single sensible meaning. Revisit after M5.

**Final name.** Yours to make.

---

## 4. Risk register, revised

| Risk | Spec's rank | Revised |
| --- | --- | --- |
| cl6x licence / availability | "release blocked or slower build" | **Downgraded, 2026-11-22.** The AVAILABILITY half stands: the 60% target needs cl6x's 2.2x. The LICENCE half rested on a premise nobody checked and that does not survive reading the build -- no TI code, no TI headers and no TI run-time library reach `APP.ENA`, only instruction sequences. docs/BUDGET.md has the table and the one file still worth reading |
| 8 insert chains + master cost | not identified | **High.** The largest unmeasured term. Measure at M1, not M2 |
| Enaga's app interface changes before 1.0 | high | Unchanged. The ABI is "provisional until 1.0"; keep Enaga calls behind an adapter in `app/` |
| MIDI/SYNC unverified on a unit | — | **New.** Implemented but never run on hardware. STRATA may be the first to exercise it; budget debugging time and report upstream |
| MIDI PRs rejected upstream | high | **Gone.** No platform work needed |
| CPU figures are estimates | high | **Reduced.** Real per-voice numbers exist; voices have large headroom |
| Pad feel and crosstalk | medium | Unchanged — Enaga lists crosstalk as an open item |
| Project load time | medium | Unchanged; mitigate with the 32 MiB default |
| Only one unit has run Enaga | medium | Unchanged |
| Windows host | not identified | **New, low.** WSL covers it; no cmake, keep `build/` off `/mnt/d` |
