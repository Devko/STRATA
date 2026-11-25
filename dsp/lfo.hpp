// The per-track LFO: docs/SPEC.md, "One LFO per track (sine, triangle, saw, square, sample and
// hold), free or tempo-synced, routable to any of the 16 parameters."
//
// ## Four bytes, like every other parameter
//
// The LFO's settings are parameters 16-19 of the track (dsp/track.hpp, `Page::lfo`): the kit
// stores them, a step can lock them, a scene stores and morphs them and a macro can drive them,
// all through the code that already does that for the 16 sound parameters. What each byte means:
//
//     16  WAV  the shape: SIN TRI RMP SAW SQR S+H, six bands of the 0-127 byte. RMP rises and
//              SAW falls, which is what stands in for a signed depth: an inverted sine or
//              triangle is the same wave half a cycle on, so only the ramp needs both.
//     17  RAT  the rate. 0-63 run FREE, 0.0625 Hz to 32 Hz, an octave every 7 counts; 64-127
//              are SYNCED to the clock, sixteen note lengths from 8 bars to a 1/32, four counts
//              each. One knob rather than a switch and a knob because a page has four encoders.
//     18  DEP  how far: 127 swings the destination about 64 counts either side of its value.
//     19  DST  which parameter: OFF, then the 16 sound parameters (1-16), seventeen bands.
//
// **Zeroed bytes are OFF** (DST 0, DEP 0), so a kit or a scene written before the LFO existed
// sounds exactly as it did, and a track with its LFO off costs one compare a block. That is what
// keeps every render test bit-exact.
//
// ## Block rate
//
// The LFO is stepped once per audio block, not per sample, and moves its destination's BYTE.
// That is the same path a knob turn or a lock takes (the machine re-prepares from the byte), so it
// adds no per-sample work at all; the cost is one `prepare` of the modulated track per block in
// which the byte actually changed, which docs/BUDGET.md measures. The price is resolution: the
// destination moves in 0-127 steps at about 750 updates a second, which is fine for a filter
// sweep or a pan and audible as steps on a slow, deep pitch sweep.
//
// ## TRIG: when a cycle starts
//
// Not a fifth byte -- the page has four encoders and the 32 bytes a track has are all spoken for
// -- but a kit setting beside the bytes (KitTrack::lfo_trig, KIT.BIN's optional section 9), put
// on a push of RAT's knob. No lock, scene or macro moves it, like SLC AUTO. Three values:
//
//     FREE      as it always was: the cycle runs on its own (or on the clock), notes or not
//     RETRIG    every note on the track restarts the cycle at phase 0 (S+H draws a new value)
//     ONE-SHOT  every note runs ONE cycle from phase 0 and then the LFO stops. This is the
//               second assignable envelope: the shapes become envelopes that start and end at
//               the knob's value -- SIN one wobble, TRI an attack and decay (up for half the
//               cycle, down for the other), RMP a rise that holds, SAW a decay from DEP down,
//               SQR a gate for half the cycle, S+H a random value per note, held. RAT is the
//               time: a free RAT is 1/Hz seconds (16 s down to 31 ms), a synced one the note
//               length. Before the first note, and after a cycle, it rests at 0 (RMP at its top
//               once it has risen; S+H on its value).
//
// A synced LFO in RETRIG or ONE-SHOT counts its cycle from the note at the tempo, not from the
// bar: that is what a retrigger means. A kit from before TRIG loads as FREE.
//
// ## Phase
//
// A free LFO runs on its own and never resets. A synced one takes its phase from the clock's
// tick count (`Engine` passes it), so it lines up with the bar wherever the song is, follows an
// external clock, and starts on its zero crossing when the transport starts at tick 0. While the
// transport is stopped the engine keeps counting at the tempo, so a synced LFO keeps moving.
#pragma once

#include "track.hpp"

#include <enaga/dsp.hpp>

#include <cstdint>

namespace strata {

// The four LFO bytes, as indices into TrackParams.
constexpr int kLfoWave = kParams + 0;
constexpr int kLfoRate = kParams + 1;
constexpr int kLfoDepth = kParams + 2;
constexpr int kLfoDest = kParams + 3;

// Shapes, in the order the WAV byte walks them.
enum class LfoWave : uint8_t { sine = 0, triangle, ramp, saw, square, sample_hold, count };
constexpr int kLfoWaves = int(LfoWave::count);

// TRIG (KitTrack::lfo_trig): the numbers KIT.BIN stores, so PERMANENT. app/ asserts they match
// core/project.hpp's kLfoTrig* constants.
enum class LfoTrig : uint8_t { free = 0, retrig, one_shot, count };
// A damaged byte is FREE, as an absent one is.
constexpr LfoTrig lfo_trig(uint8_t byte) {
    return byte < uint8_t(LfoTrig::count) ? LfoTrig(byte) : LfoTrig::free;
}
const char *lfo_trig_name(LfoTrig trig);       // "FREE" "RETRIG" "ONE-SHOT"

// The RAT byte below this is free running; at or above it, synced.
constexpr uint8_t kLfoSyncFirst = 64;
constexpr int kLfoSyncDivisions = 16;

// The bands, as integer arithmetic (no divide): WAV in six, DST in seventeen.
constexpr LfoWave lfo_wave(uint8_t byte) {
    return LfoWave((int(byte > 127 ? 127 : byte) * kLfoWaves) >> 7);
}
// -1 for OFF, else the sound parameter 0-15.
constexpr int lfo_dest(uint8_t byte) {
    return ((int(byte > 127 ? 127 : byte) * (kParams + 1)) >> 7) - 1;
}
// The smallest byte in a band, which is what a test or the factory writes to pick it.
constexpr uint8_t lfo_wave_byte(LfoWave wave) {
    return uint8_t((int(wave) * 128 + kLfoWaves - 1) / kLfoWaves);
}
constexpr uint8_t lfo_dest_byte(int param) {
    return param < 0 ? uint8_t(0) : uint8_t(((param + 1) * 128 + kParams) / (kParams + 1));
}

// How many encoder clicks one band is worth, so one click on WAV or DST is one shape or one
// destination rather than a seventh of one. app/ scales a click by this.
constexpr int lfo_click(int index) {
    return index == kLfoWave ? 22 : (index == kLfoDest ? 8 : 1);
}

// One synced cycle in clock ticks (96 PPQN), longest first; 0 for a free-running byte.
uint32_t lfo_sync_ticks(uint8_t rate);

// A free-running byte's frequency, in Hz. Meaningless for a synced byte.
float lfo_free_hz(uint8_t rate);

// For the screen, at most five characters each: "SIN", "1/4T", "2B", "0.06", "OFF".
const char *lfo_wave_name(LfoWave wave);
const char *lfo_sync_name(uint8_t rate);
const char *lfo_param_name(int index);          // "WAV" "RAT" "DEP" "DST" for 16-19
// The rate in hundredths of a hertz, rounded, for the screen to print.
uint32_t lfo_free_centi_hz(uint8_t rate);

// How far DEP 127 swings the destination, in counts either side. 64 everywhere but a cutoff,
// where 64 counts was about five octaves each way (D9 leftover): there DEP 127 is about three
// octaves either side, on each curve --
//   FLTR CUT (9.8138 octaves over 127 counts): 39 counts = 3.01 octaves
//   the VAs' and WAVEs' CUT (8.4214 over 127): 45 counts = 2.98 octaves
constexpr float kLfoSpan = 64.0f;
constexpr float kLfoInsertCutoffSpan = 39.0f;
constexpr float kLfoVaCutoffSpan = 45.0f;

// The destination byte moved by the LFO: `base` plus `value` (-1..1) times DEP's share of `span`
// counts, rounded to nearest and clamped into 0-127. It never goes below `floor`, or below
// `base` where the knob itself sits under the floor: the floor limits the LFO, not the knob.
uint8_t lfo_apply(uint8_t base, float value, uint8_t depth, uint8_t floor = 0,
                  float span = kLfoSpan);

// The same offset unrounded, in semitones of TUN (24/64 a count): what a synth machine's voices
// add to their pitch when the LFO is on TUN, so a slow sweep glides instead of stepping.
float lfo_pitch_semis(float value, uint8_t depth);

// The cutoff floor: an LFO on a cutoff stops at about 100 Hz, so a deep sweep dips the sound
// instead of shutting it off (backlog D9). The bytes follow the two cutoff curves:
//   FLTR CUT (dsp/chain.cpp):        20 Hz * 2^(9.8138 * 30 / 127) =  99.7 Hz
//   the VAs' and WAVEs' CUT (va.cpp): 35 Hz * 2^(8.4214 * 23 / 127) = 100.7 Hz
constexpr uint8_t kLfoInsertCutoffFloor = 30;
constexpr uint8_t kLfoVaCutoffFloor = 23;
// The floor for an LFO on `dest`: FLTR's cutoff on a track with an insert chain, SRC's third
// byte on a VA or WAVE machine, and 0 (no floor) for everything else. The macros and the scene
// morph stop at the same floor (app/engine.cpp, apply_performance): only the knob, a lock and the
// kit itself close a filter all the way.
uint8_t lfo_cutoff_floor(int dest, bool va_filter, bool insert_chain);
// And the span for an LFO on `dest`, on the same terms: a cutoff's, or kLfoSpan.
float lfo_span(int dest, bool va_filter, bool insert_chain);
// Which parameter a track's cutoff is, on those terms: SRC's third byte on a synth machine,
// FLTR's second on a machine with the insert filter, and -1 for MID, which has neither.
int lfo_cutoff_param(bool va_filter, bool insert_chain);

// The four bytes resolved, at control rate, only when they change.
struct LfoSettings {
    LfoWave wave = LfoWave::sine;
    uint32_t sync_ticks = 0;        // 0: free
    float inv_sync_ticks = 0.0f;    // 1 / sync_ticks, so the block never divides
    float hz = 0.0f;
    uint8_t depth = 0;
    int dest = -1;                  // -1: off

    bool on() const { return dest >= 0 && depth != 0; }

    static LfoSettings make(const uint8_t bytes[kLfoParams]);
};

// One track's oscillator. Wraps Enaga's `Lfo` (sine, triangle, saw, square, S&H) and adds the
// falling ramp.
class TrackLfo {
public:
    // A distinct seed per track keeps eight S&H tracks from moving together.
    void init(uint32_t seed);

    // Free running: advance by `cycles` (Hz x seconds) and return the value, -1..1.
    float advance(LfoWave wave, float cycles);

    // Synced: move to `phase` (0..1) and return the value. A move backwards counts as a wrap,
    // so S&H draws a new value each time a synced cycle starts.
    float seek(LfoWave wave, float phase);

    float phase() const { return lfo_.phase(); }

    // RETRIG and ONE-SHOT: a note on the track. Back to phase 0, S+H drawing a new value as a
    // wrap would, and a one-shot armed for one cycle.
    void restart();

    // ONE-SHOT: advance the one cycle by `cycles` and return its envelope value (see TRIG
    // above). Once the cycle is through it stays at its rest value until restart().
    float one_shot(LfoWave wave, float cycles);
    bool one_shot_done() const { return done_; }
    // ONE-SHOT back to waiting for a first note, at rest (a TRIG change, a project load). The
    // free cycle is untouched, so a FREE LFO is not moved by it.
    void rest() {
        shot_ = 1.0f;
        done_ = true;
        fired_ = false;
        shot_held_ = 0.0f;
    }

    // The one-shot shapes at `u` (0..1 through the cycle) with S+H holding `held`; at u = 1 (and
    // past it) the rest value. Exposed for the tests.
    static float one_shot_value(LfoWave wave, float u, float held);

private:
    float shaped(LfoWave wave, float raw) const;
    static enaga::dsp::LfoShape enaga_shape(LfoWave wave);

    enaga::dsp::Lfo lfo_;
    enaga::dsp::Random random_;
    // ONE-SHOT's own cycle: where it is, whether it has run out, whether any note has started
    // one (before the first, every shape rests at 0), and S+H's value for this note.
    float shot_ = 1.0f;
    bool done_ = true;
    bool fired_ = false;
    float shot_held_ = 0.0f;
};

}  // namespace strata
