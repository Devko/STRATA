# Enaga API cheat sheet

**Every signature here was read out of the headers at the pinned commit `1b41111` and compiled.**
Nothing is from memory. If something you need is missing, read the header — do not guess a name,
because the plausible guesses are mostly wrong (`Oscillator::set`, `SvfCoefficients::set`,
`Envelope::gate`, `--version` on cl6x: all invented, all wrong).

Headers live in `third_party/enaga/include/enaga/`. `docs/api.md` in that submodule is the full
reference; this file is only what STRATA uses.

## Two conventions that run through the whole library

**1. Settings structs are built by a static `make(...)`, at control rate, once per change.**
There are no setters.

```cpp
const SvfCoefficients c  = SvfCoefficients::make(cutoff_hz, resonance, sample_rate);
const EnvelopeShape   s  = EnvelopeShape::make(attack_s, decay_s, sustain, release_s, step_rate);
const BiquadCoefficients b = BiquadCoefficients::make(type, frequency, q, sample_rate, gain_db);
```

**2. Oscillators take `dt` *and* `inv_dt`, because nothing in a hot loop may divide.**
The caller computes both at control rate:

```cpp
const float dt     = frequency / sample_rate;   // cycles per sample, in (0, 0.5)
const float inv_dt = sample_rate / frequency;   // its reciprocal
```

**Prefer the block functions to the per-sample ones** (`Svf::process` over `Svf::step`,
`Envelope::render` over `Envelope::step`). The headers say so, and they are what cl6x optimises.

## App (`<enaga/app.hpp>`)

Subclass `enaga::App`, override what you need, register with `ENAGA_APP(Type)` at file scope
outside any namespace. Defaults do nothing. Destructors never run — save in `stop()`.

```cpp
bool init();                                   // false shows "APP INIT FAILED"
void update(uint32_t ms);                      // main context, >= 750x a second
void draw(Canvas &canvas);                     // main context, every 50 ms (20 fps)
void stop();                                   // 3 s budget, including saves
void on_key(int key, bool down);
void on_encoder(int encoder, int delta);
void on_pad(int pad, unsigned pressure);       // every raw reading, 0-16383
void on_pad_press(int pad, int velocity);      // velocity 1-127
void on_pad_release(int pad);
void on_pad_pressure(int pad, int pressure);   // 0-127 while held, Enaga's own curve
void on_midi(const MidiMessage &message);      // MAIN context, no timestamp
void on_transport(int what, uint32_t position);// MAIN context
void on_midi_sysex(int port, const uint8_t *data, uint32_t bytes, bool start, bool end);
void on_card(bool usable);                     // false: open files are dead, close them
void audio(const float *in, float *out, uint32_t frames);   // INTERLEAVED stereo L R L R
```

`audio()` must write the whole `out` buffer: `2 * frames` floats.

Helpers:

```cpp
enaga::log(const char *text);
uint32_t enaga::ms();
uint32_t enaga::audio_load();        // PER MILLE of the block budget, so 1000 = the whole block
uint32_t enaga::audio_load_peak();
```

## LEDs (main context only; ignored from `audio()`)

Levels are 0-10.

```cpp
enaga::pad_led(int pad, unsigned red, unsigned green, unsigned blue);   // pad 0-14
enaga::step_led(int step, unsigned red, unsigned green);                // STEP 1-16
enaga::button_led(int key, unsigned red, unsigned green);               // a key with an LED
enaga::vol_led(int channel, unsigned level);                            // 0 = VOL L, 1 = VOL R
enaga::value_led(unsigned red, unsigned green, unsigned blue);          // the VALUE ring
enaga::mic_led(unsigned level);
```

## Screen (`enaga::Canvas`, 128 x 128, 16 greys 0-15)

```cpp
canvas.clear(uint8_t grey = 0);
canvas.pixel(int x, int y, uint8_t grey);
canvas.rect(int x, int y, int w, int h, uint8_t grey);
canvas.text(int x, int y, const char *s, uint8_t grey = 15);   // 5 x 7 font
canvas.data();                                                 // uint8_t[128*128], row-major
```

The built-in font has **upper case, digits and a few symbols only**. There is no `printf` and no
libm: build strings by hand (see `app/app.cpp` for the digit helpers).

## Clock (`<enaga/clock.hpp>`) — Enaga owns this, STRATA does not

```cpp
namespace enaga::clock {
constexpr uint32_t ppqn = ENAGA_CLOCK_PPQN;        // 96
constexpr uint32_t sixteenth = ppqn / 4;           // 24 ticks

// Main context. ALL return false under an external clock source.
bool start();  bool stop();  bool continue_();
bool locate(uint32_t ticks);
bool set_tempo(float bpm);                         // 20-300

// Either context.
State state();                                     // .running .source .tempo .position .fraction
float tempo();  uint32_t position();  int source();  bool running();

// AUDIO context only: the ticks in this block, each with the frame it falls on.
uint32_t ticks(Tick *out, uint32_t max);           // Tick: .tick, .frame
}
```

Sources: `ENAGA_CLOCK_INTERNAL`, `_MIDI`, `_SYNC`. Transport events arriving at `on_transport`:
`ENAGA_TRANSPORT_START`, `_STOP`, `_CONTINUE`, `_LOCATE`.

**The source is the user's CLOCK SRC setting.** Under MIDI or SYNC, `start`/`stop`/`set_tempo`
do nothing and return `false`; follow `on_transport` instead and show `EXT` rather than a tempo
STRATA pretends to own.

## DSP (`<enaga/dsp.hpp>`, namespace `enaga::dsp`)

Needs `runtime/dsp/*.c` linked in; `host/Makefile` already does this, and
`tests/test_enaga_dsp_links.cpp` proves it.

### Oscillator

```cpp
enum class Wave : uint8_t { saw, pulse, triangle, sine };   // saw and pulse are PolyBLEP

void reset(float phase = 0.0f);
float phase() const;
void render_add(float *out, int n, Wave w, float dt, float inv_dt, float level, float width = 0.5f);
void advance(int n, float dt);                              // silent, for a sync master
void sync_add(float *out, int n, const Oscillator &master, float master_dt, float master_inv_dt,
              float &carry, Wave w, float dt, float inv_dt, float level, float width = 0.5f);
void table_add(float *out, int n, const float *table, int bits, float dt, float level);
void morph_add(float *out, int n, const float *a, const float *b, int bits, float morph,
               float dt, float level);
```

`render_add` **adds**, so clear the buffer first (`enaga_clear`) or accumulate deliberately.

### Filters

```cpp
SvfCoefficients::make(cutoff, resonance, sample_rate);     // resonance 0..1
SvfCoefficients::from_g(g, k);
class Svf {
    void reset();
    Out step(float x, const SvfCoefficients &c);           // Out { low, band, high }
    void process(const float *in, float *out, int n, const SvfCoefficients &c,
                 Output output = low_pass);                // low_pass, band_pass, high_pass
    static void cascade(Svf &a, const SvfCoefficients &ca, Svf &b, const SvfCoefficients &cb,
                        const float *in, float *out, int n, Output output = low_pass);
};
```

`cascade` is two poles in series for about the price of one — this is how to get 24 dB.

```cpp
BiquadCoefficients::make(Type, frequency, q, sample_rate, gain_db = 0.0f);
class Biquad { void process(const float *in, float *out, int n, const BiquadCoefficients &c); };
class OnePole { static float coefficient(float cutoff, float sample_rate); };
class Fir { void init(const float *taps, int length); };
```

### Envelope and LFO

```cpp
EnvelopeShape::make(attack_s, decay_s, sustain, release_s, step_rate);

class Envelope {
    void gate_on(bool retrigger_from_zero = false);
    void gate_off();
    void kill();
    Stage stage() const;    // idle, attack, decay, sustain, release
    bool active() const;
    float level() const;
    float step(const EnvelopeShape &s);
    void render(float *out, int n, const EnvelopeShape &s);
};

enum class LfoShape : uint8_t { sine, triangle, saw, square, sample_hold };
class Lfo {
    void reset(float phase = 0.0f);
    float phase() const;
    float step(LfoShape shape, float dt, Random &random);   // dt = rate / step_rate
    float value(LfoShape shape) const;
};
```

`step_rate` is whatever rate you step it at: the sample rate for an amplitude envelope, the
control rate (once per block) for modulation.

### Effects

```cpp
class StereoDelay {
    void init(float max_seconds, float sample_rate);
    void set(float seconds, float feedback, float tone, bool ping_pong);   // feedback 0-0.95
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix);
};
class Reverb {
    void init(float sample_rate);
    void set(float decay_s, float damping);
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix);
};
class Chorus { void init(float sample_rate); void set(float sweep_hz, float depth); void process(...); };
class DelayLine { void allocate(uint32_t max_samples); };
```

`init`/`allocate` take memory from a pool, so **call them in `App::init()`, never in `audio()`**.
Delay plus reverb measured **16% of the audio budget together** — budget for them.

### Math (no libm) and buffers

```cpp
exp2, exp, log2, semitones, sin_turns, cos_turns, tan_pi, reciprocal,
soft_clip, clamp, lerp, floor_int, pi
class Random { ... bipolar() ... };

enaga_clear, enaga_add, enaga_mix, enaga_scale, enaga_multiply, enaga_mix_product,
enaga_soft_clip_buffer, enaga_ramp_stereo_add
```

## Gotchas found in practice

These cost real time to discover while writing the drum machines. They are not in Enaga's docs.

**`Envelope` with `sustain = 0` never reaches `idle` on its decay path.** It parks in the
`sustain` stage, so `active()` stays true forever and `stage()` never returns `idle` *on that
path*, even though `level()` does fall to zero. A one-shot percussive voice therefore cannot use
`active()` to decide it has finished.

(`idle` is reachable normally — after `kill()`, and at the end of a `gate_off`-driven release. It
is only the sustain-0 decay that never gets there. That matters, because the hold trick below
relies on a release reaching `idle` the usual way.)

Test the level instead:

```cpp
const bool finished = envelope.stage() != Envelope::attack && envelope.level() < 1e-4f;
```

**There is no hold stage.** The 909 snare needs one (40–70 ms before the noise decays). The way
to get it without a second envelope: build the shape with `sustain = 1.0`, then on the hold's last
frame hand `render()` a shape whose sustain is 0. The sustain stage follows the new sustain value
smoothly, so the envelope starts falling with no `gate_off` and no extra state.

**`EnvelopeShape::make` takes a time-to-1%, not a time constant.** The header says "to within 1%
of their target". Reference designs almost always publish a time constant (tau). The two differ by
`ln(100) = 4.60517`, so a published tau must be multiplied by that before being passed in. Getting
this wrong makes every envelope about 4.6× too short.

**`SvfCoefficients::make` takes resonance 0–1, not Q.** To set an exact Q from a reference, go
through `from_g` instead:

```cpp
const SvfCoefficients c = SvfCoefficients::from_g(tan_pi(cutoff_hz / sample_rate), 1.0f / q);
```

**`Oscillator` cannot do everything.** It cannot hold or pin its phase (the 909 kick needs the
phase pinned at 0.25 for 1.3 ms to make its click), it cannot take a custom waveshaper, and it has
no naive square — `Wave::pulse` is PolyBLEP-corrected. For the 808 hi-hat's six squares that
correction is both unwanted and expensive: six PolyBLEP pulses per voice would have been the single
costliest thing in the app. Six plain 32-bit phase accumulators read at their top bit are what the
reference does and are far cheaper. Writing a bare float accumulator is the right call whenever a
machine needs a shape or a phase behaviour the class does not offer.

**No libm, so some basics need building.** A square root at control rate is
`exp2(0.5f * log2(x))`; an absolute value is a select, not `fabsf`. Enaga's `soft_clip` is a cubic
and stands in acceptably for the rational soft clips that reference designs tend to use.

**An instant attack works but is not documented.** An `EnvelopeShape` whose attack time is under
one step gets coefficient 1.0, so `EnvelopeShape::make(0.0f, ...)` is a genuine zero-length
attack. That is what a sliced one-shot wants — its transient *is* the sound, and any attack ramp
softens it.

**There is no interpolated read anywhere in the library.** No resampler, no fractional delay tap,
no arbitrary-phase table read. `Oscillator::table_add` takes a `dt` over a power-of-two table and
is no use for arbitrary PCM. Anything that plays a sample at a non-unity rate writes its own tap
gather and interpolator — and must do its own bounds work, because an out-of-range read on this
chip does not fault, it silently returns garbage.

**There is no int16-to-float buffer conversion either**, so a sampler's convert sits in its hot
loop. Fold the `1/32768` (and any channel halving) into the output gain so it costs one multiply
per block rather than one per sample.

**`enaga_reciprocal` is an estimate, good to about 2e-7.** Fine for audio, not fine for deciding
how many samples it is safe to read: anything that computes a loop bound from it needs an exact
integer check afterwards. "Never read outside the buffer" is not a 2e-7 kind of rule.

## Files (`<enaga/files.hpp>`) — main context only

```cpp
const char *files::data_folder();                       // "/ENAGA/DATA/STRATA"
int32_t files::read_all(path, void *buffer, uint32_t max);
int32_t files::save(path, const void *data, uint32_t bytes);     // blocking, survives a pulled card
int32_t files::load(path, void *buffer, uint32_t max, done, user);   // background; done() in main
int32_t files::info(path, Info &out);
int32_t files::remove(path);  int32_t files::make_folder(path);
class files::File { ... };     class files::Stream { ... };
```

Writable paths only: `files::data_folder()`, `/SAMPLES`, `/RECORD`. At most **8 open files and
8 streams**. A `Stream` ring must be a power of two, at least `ENAGA_STREAM_RING_MIN`, and
belongs to Enaga until `close`.

## Settings (`<enaga/settings.hpp>`)

`enaga::Settings` — `load()`, `save()`, `get(key, fallback)`, `text(key, fallback)`,
`set(key, value)`, `changed()`. KEY=VALUE text in the data folder. **32 keys maximum.**

## System

```cpp
int enaga::system::device();        // ENAGA_DEVICE_SMPLTREK, _LOFI12XT, _CYDRUMS, _UNKNOWN
void enaga::system::open_menu();
void enaga::system::set_busy(bool);
void enaga::system::speaker(bool);
bool enaga::system::headphones();
int  enaga::system::input(int source, int level = ENAGA_INPUT_NORMAL);
```

## Constants (`<enaga/abi.h>`)

```cpp
ENAGA_SAMPLE_RATE    48000
ENAGA_BLOCK_FRAMES   64          // 1.33 ms; 496,000 CPU cycles
ENAGA_CLOCK_PPQN     96
```

Keys: `ENAGA_KEY_F1`=19, `F2`=26, `F3`=20, `F4`=27, `F5`=22, `F6`=29, `F7`=21, `F8`=28,
`STEP1`=32 (`STEP n = ENAGA_KEY_STEP1 + n - 1`), `OK`=23, `CLR`=30, `UP`=17, `RIGHT`=18,
`LEFT`=24, `DOWN`=25, `OCTAVE_LEFT`=16, `OCTAVE_RIGHT`=31, `SELECT_LEFT`=8, `SELECT_RIGHT`=9,
`PUSH_B`=0, `PUSH_A`=1, `PUSH_C`=2, `PUSH_D`=3, `PUSH_VALUE`=4.

**Two different encoder numberings, and mixing them up is silent.**

- `on_encoder(int encoder, ...)` uses the **event** id, which *is* alphabetical:
  `0-3 = A-D, 4 = VALUE` (`ENAGA_EVENT_ENCODER` in `abi.h`).
- The **push-button** key codes are not: `ENAGA_KEY_PUSH_B = 0`, `PUSH_A = 1`, `PUSH_C = 2`,
  `PUSH_D = 3`, `PUSH_VALUE = 4`.

So encoder 1 is B when it turns and A when it is pressed. Reading the push codes as the turn
order swaps A and B, which no compiler will catch.

Pads: the lower row is **even** ids 0, 2, 4, 6, 8, 10, 12, 14; the upper row is **odd** ids
1, 3, 5, 7, 9, 11, 13.
