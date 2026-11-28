# Sound direction

Techno drum synthesis is extremely well-trodden ground. The topologies, the frequency ratios and
the envelope shapes that make a kick sound like a kick are published, and several open-source
implementations are proven on embedded hardware. **STRATA follows those rather than inventing its
own**, so the question in review is "does this match the reference" and not "does this sound
plausible to whoever wrote it".

## The reference

[Mutable Instruments Plaits](https://github.com/pichenettes/eurorack/tree/master/plaits/dsp/drums)
— five drum models by Émilie Gillet, **MIT licensed, Copyright 2016 Emilie Gillet**. This is the
canonical embedded implementation: it runs on a Cortex-M4 under the same kind of budget we have,
and its models are 808 and 909 circuit analyses rather than guesses.
[DaisySP](https://github.com/electro-smith/DaisySP) carries MIT ports of them if a second reading
of the same designs is useful.

The five files are `analog_bass_drum.h` (808), `synthetic_bass_drum.h` (909-ish),
`analog_snare_drum.h` (808), `synthetic_snare_drum.h` (909), `hi_hat.h` (808, with a KR-55/FM
alternative).

### How we use it, and the licence

We implement the **topology and the published constants** using Enaga's own DSP primitives
(`Svf`, `Oscillator`, `Random`, `soft_clip`, `semitones`), because Plaits' code depends on
`stmlib` helpers we do not have and because our code has to be block-based with control-rate
coefficients and `*.ti.c` hot loops. Frequency ratios and filter topologies are facts about how a
sound is made; expressing them in our own code is ordinary engineering practice.

Even so: **this repository is public, and every machine that follows a Plaits model names it in
its file header, with the copyright line.** If any code is ported rather than reimplemented, the
MIT notice travels with it — the licence requires it in "all copies or substantial portions". Not
legal advice; if STRATA is ever released as a binary, check this properly.

## The correction this makes to the spec

`docs/SPEC.md` describes KICK as "sine or triangle body, pitch envelope, click, drive, decay".
That is the **909** architecture, and for techno it is the right default — but it is worth knowing
that it is a choice, because the 808 is a genuinely different machine:

| | 808 kick | 909 kick |
| --- | --- | --- |
| Architecture | **A resonator**, not an oscillator. A band-pass filter is struck by a pulse and rings | An oscillator with an FM pitch envelope through a VCA |
| Where the decay comes from | The resonator's Q. There is **no amplitude envelope at all** | An explicit exponential amplitude envelope |
| Transient | A 1 ms trigger pulse plus a 6 ms FM pulse, and self-FM "punch" | A separate click plus a filtered noise burst |
| Character | Long, pure, sub-heavy tail | More attack and body; takes distortion well |

Techno kicks are overwhelmingly 909-descended, and the 909's separate attack and body circuits are
what let a kick cut through a mix. So **KICK follows `synthetic_bass_drum` (909-ish) as its primary
model**, with a decay range long enough to reach 808 territory. If an 808-style resonator model is
wanted later it is a second machine, not a parameter.

## Per-instrument recipes

Constants below are from the Plaits sources. Where a number is derived rather than literal it says
so.

### KICK — follow `synthetic_bass_drum` (909-ish)

Author's own comment: *"Naive bass drum model (modulated oscillator with FM + envelope).
Inadvertently 909-ish."*

- One phase accumulator. Increment `f0 * (1 + 3.5 * fm_amount * fm_envelope)`, capped at 0.5.
- **The phase is held at 0.25 for 1.3 ms after the trigger.** This is the click trick: the
  oscillator starts at its peak instead of at zero.
- Waveshaping, and **read the direction carefully** — an earlier wording of this line sent the
  first implementation the wrong way. The source computes two signals and crossfades between
  them:

  ```
  tri   = (phase < 0.5 ? phase : 1 - phase) * 4 - 1     // a bare triangle
  sine  = 2 * tri / (1 + |tri|)                          // rational soft sine: the DIRTY one
  clean = Sine(phase + 0.75)                             // a real sine: the CLEAN one
  out   = sine + (1 - dirtiness) * (clean - sine)        // = lerp(clean, sine, dirtiness)
  ```

  So **dirtiness 0 gives the clean sine and dirtiness 1 gives the rational soft sine.** The bare
  triangle is only an intermediate and is never the output — ending the sweep on it would be
  considerably harsher than the reference. The knob does get brighter as it opens, but between
  those two endpoints.
- Dirtiness is then scaled by `max(1 - 8*f0, 0)`, so it only applies low down. That is a
  sensible reason to trust the direction above: at high pitches the distortion would alias, so
  the model forces its way back to the clean sine.
- FM (pitch) envelope: exponential, base ≈ 8 ms, scaled by `fm_envelope_decay`.
- Amplitude envelope: exponential, 1 ms hold, time constant ≈ **20 ms × 2^(5·decay²)**, so roughly
  **20 ms to 640 ms**. `decay` is squared before use.
- Transient: a click (1 ms gate, slope 0.5 rise / 0.1 fall, one-pole high-pass, then a
  **low-pass at 5 kHz, Q 2**) plus attack noise (one-pole LP 0.05 minus one-pole HP 0.005).
- Output low-pass: `min(4*f0 * 2^(tone*108/12), 1)`.

Sound-design cross-check from the production literature: a kick is three parts — the transient
click, the descent from click into thud, and the decay. A **pitch decay of roughly 20–70 ms** is
what gives the trademark click. The 909 plays very well with distortion, which is why `drive`
sits right after it in our insert chain.
([Ableton](https://www.ableton.com/en/blog/drop-it-kick-electronic-music/),
[Perfect Circuit](https://perfectcircuit.com/signal/kick-drum-synthesis))

### KICK's PNCH page — what the benchmark added (2026-11-28)

Measured against 400+ techno kick one-shots from sample libraries (octave bands over the whole
hit tiled in a loop and over its first 20 ms), the 909 model above is 11-25 dB short in the
attack from 250 Hz to 4 kHz, and has nothing below f0. Commercial techno kicks are built in layers: a low punch (this
model's body), a mid punch (a fast pitch sweep through 250 Hz-1 kHz), a top (a click at 2-6 kHz)
and often a falling sub tail. The model's own click is a 1 ms gate, whose spectrum has nulls at
1, 2 and 3 kHz, and DRT moves the waveform but barely the spectrum. So the PNCH page adds the
missing layers rather than changing the reference's knobs:

- **KNK / KND**: a second pitch envelope, `f0 * (1 + 3.5*FMA*fm_env + 15*KNK*knock_env)`, with a
  1-16 ms time constant. The knock starts up to four octaves over the body and is gone before
  FMD's sweep is.
- **TCK**: white noise band-passed at 3 kHz (Q 0.7) under a 1.5 ms envelope, added after TON so
  the body's low-pass never dulls it. Its own noise generator, so the click's never moves.
- **DRP**: f0 falls towards `2^-DRP` of itself on the amplitude's time constant (control rate).

All three at 0 run the reference exactly. The techno presets land within a few dB of the
references in the loop and 5-13 dB under them at 1-4 kHz in the attack. Two lessons from tuning
them: the body's length (DEC) sets the loop's balance more than anything on the page, and heavy
drive or a loud tick overshoots the loop's mids and top by 10-20 dB while barely helping the
attack. Measure the attack with a window that does not fade in over its first milliseconds
(a Hann window there hides the tick).

**TAIL** (slots 28-31). RMB and RLN are a rumble after fm1-HYP1's kick (GPL-3; the idea, not the
code): white noise through two one-pole low-passes near 90 Hz, plus a sine at the kick's own note,
under a swell that is a slow envelope (RLN, 0.1-1.2 s) less a fast one (35 ms), so it rises after
the hit; added to the machine's output, before the chain's drive, so body and rumble distort
together. HLD is a 909's amplitude hold (ReplicantFM1's 909 model holds the body before its
decay): the level held for up to 400 ms, then DEC's time constant. Measured against RY Divinity's
layered kicks, their 30-150 Hz energy 100-450 ms after the hit sits 2-6 dB above the hit's first
50 ms; HEAVY sits 0-3 dB above, FAT 0-1.5. An earlier version of the rumble ran the kick's tail
through all-passes and combs; the swell is cheaper, needs no delay memory and growls with the
drive.

**CHK** (slot 28): sample kicks are cut at the next beat. With CHK above 0, a trigger fades
every other sounding voice out over 60 ms (CHK 1) down to 1 ms (127), starting at the new hit's
frame. Two blind tests with Roland (2026-11-28) also taught a voicing rule: the kicks that passed
as samples share one recipe (TECHNO's); variants that changed drive, dirt, sweep and length at
once were heard as synthetic.

### SNARE — follow `synthetic_snare_drum` (909), with 808 mode ratios available

Author's comment: *"Uses a few magic numbers taken from the 909 schematics"* and
*"It's a snare, it's a drum, it's a snare drum."*

- Two oscillators at `f` and **`f * 1.47`** — the ratio is straight off the 909 schematic. Both
  through the same distorted-sine shaper.
- Drum part: `(-0.1 + 0.60*osc0 + 0.25*osc1) * drum_amp * sqrt(1 - snappy)`, then a one-pole
  low-pass at `3*f0`.
- Noise part: white noise → band-/low-pass at `min(35*f0, 0.5)` with `Q = 0.5 + 2*snappy` →
  one-pole high-pass at `min(10*f0, 0.5)`, scaled by `sqrt(snappy)`.
- **The noise envelope has a hold stage of 40–70 ms before it decays** (`0.04 + 0.03*decay`
  seconds). This is what makes a 909 snare sound right and is easy to miss.
- Drum envelope: *"a very long tail"* — exponential, 15 ms base, exponent
  `-(decay*72 + fm_amount*12 - snappy*7)` semitones.
- Oscillator coupling: *"The 909 circuit has a funny kind of oscillator coupling"* — a phase reset
  with a reflected wrap, active only at low `f0`.

For the clap mode the spec asks for, the 808 shell ratios are the useful reference:
`f0 × {1.00, 2.00, 3.18, 4.16, 5.62}` as five band-pass modes, of which the 808 itself uses only
the first two. Those two have gains `g0 = 1.5 + (1-t)²*4.5` and `g1 = 2t + 0.15`.

### HAT — follow `hi_hat.h` with its `SquareNoise` source (the 808)

Author's comments: *"808 style 'metallic noise' with 6 square oscillators"* and *"808 HH, with a
few extra parameters to push things to the CY territory"*.

This is exactly the recipe `docs/SPEC.md` describes, now with the real numbers:

- **Six square oscillators at ratios `1.0, 1.304, 1.466, 1.787, 1.932, 2.536`**, with the source
  comment *"Nominal f0: 414 Hz"* — so about 414, 540, 607, 740, 800 and 1050 Hz. Each is a 32-bit
  phase accumulator whose square is simply the top bit, so this costs no oscillator math at all.
  Each frequency clamps to 0.499 normalised.
- Mix: `0.33 * (number of squares currently high) - 1.0`. Note **0.33, not 1/3** — the asymmetry is
  deliberate and in the source.
- Then a band-pass: cutoff `150 Hz * 2^(tone*72/12)`, so **150 Hz to 9.6 kHz** across the tone
  knob, clamped at 16 kHz; `Q = 3 + 3*tone`.
- Then the VCA, then a **high-pass at the same cutoff with Q 0.5**.
- Envelope: exponential, trigger level `(1.5 + 0.5*(1-decay)) * (0.3 + 0.7*accent)`; time constant
  ≈ **7 ms at decay 0 to 0.9 s at decay 1**. A two-stage variant switches to a faster tail once the
  envelope drops below 0.5 — that faster stage is how a closed hat chokes.
- Optional clocked sample-and-hold noise crossfaded in, at `f0 * (16 + 16*(1 - noisiness))`. The
  author is explicit that this is *"not at all part of the 808 circuit"* — it is there for variety,
  so treat it as an extra, not as part of the model.
- The VCA is asymmetric (`×4` positive, `×0.1` negative, rational soft clip), which is part of the
  character rather than an accident.

## A porting trap worth repeating

**Plaits' envelope decay constants are per-sample and are not scaled by the sample rate.** For
example the hi-hat uses `1 - 0.003 * 2^(-decay*84/12)` per sample, and the 808 snare's noise
envelope uses `1 - 0.0017 * ...`. Those numbers assume Plaits' own sample rate. Copying them
unchanged at 48 kHz gives the wrong decay times.

So: derive the intended **time constant in seconds** from the constant, then recompute the
coefficient for 48 kHz. The derived times are given per instrument above; use those as the target
and make the shape with `EnvelopeShape::make(...)`, which already takes a step rate.

## What good looks like

The parameters have to feel right across their whole range, not just be mathematically tidy. For
each machine:

- At the default parameter set it should sound like the machine it is named after, with no tweaking.
- `decay` should sweep from a tight tick to a long tail without a dead zone at either end.
- `tone` should stay useful at both extremes — not thin at one end and muddy at the other.
- Accent should be audible as more than just volume: in the references it changes the trigger level
  into a non-linear stage, so it brightens as well as lifts.
- A kick at default settings, through the insert chain's drive, should hold up. That pairing is why
  drive comes first in the chain.

Roland will listen and judge. These notes exist so that what he hears is a deliberate position
with a known reference, not an accident.
