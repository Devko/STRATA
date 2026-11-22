// The mixer: eight insert chains into a stereo bus, two send effects, a master chain, and the
// one interleaved stereo buffer audio() has to fill.
//
// docs/SPEC.md, "Mixer and buses":
//   Send A   a tempo-syncable stereo delay with ping-pong and filtered feedback, which can also
//            feed the reverb. Times arrive in SECONDS: the app owns the tempo (CLAUDE.md), so
//            it converts the division, not this file.
//   Send B   a reverb with a high-pass on its input.
//   Master   a one-knob DJ filter (left of centre low-pass, right high-pass, flat in the
//            middle), a glue compressor, a soft limiter and an output level.
//   Duck     one trigger-based duck envelope for the whole mixer (dsp/chain.hpp); which track
//            fires it is the app's business.
//
// How a block is mixed:
//
//     mixer.prepare(sends, master);              // control rate, once per block
//     for each track: mixer.chain(t).prepare(params, mix);
//     mixer.begin_block(frames);                 // clears the buses, renders the duck
//     for each sounding track:
//         float *mono = mixer.scratch();
//         enaga_clear(mono, frames);
//         machine.render_add(mono, frames);      // machines are MONO (dsp/track.hpp)
//         mixer.add_track(t, mono);              // insert chain, dry bus, both sends
//     mixer.finish_block(out, frames);           // sends, master, interleave into audio()'s out
//
// Planes, not interleaved samples, everywhere inside: every Enaga block function works on one
// plane, so the mixer keeps L and R apart and interleaves once, in finish_block. The whole cost
// of that choice is one pass over the output at the very end, and it buys all of
// enaga_ramp_stereo_add, Svf::process, StereoDelay and Reverb working on their natural layout.
//
// The two send buses are MONO (docs/PLAN.md M0.5 lists mono sends as the first cheapening):
// stereo sends would triple a track's bus traffic, the reverb sums its inputs to mono anyway,
// and the delay's ping-pong makes its own stereo out of a mono feed.
//
// Allocation: StereoDelay::init and Reverb::init take memory from the pool, so Mixer::init()
// belongs in App::init() and nowhere else. Nothing below it allocates.
#pragma once

#include <cstdint>

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "chain.hpp"
#include "project.hpp"
#include "track.hpp"

namespace strata {

// Send A and send B. Physical units; the app maps its FX page onto these at control rate.
struct SendSettings {
    // Send A: the delay.
    float delay_seconds = 0.375f;     // the app converts from the tempo; clamped to kMaxDelay
    float delay_feedback = 0.40f;     // 0 to 0.95
    float delay_tone = 0.5f;          // the feedback's low-pass: 0 dark, 1 bright
    bool delay_ping_pong = true;
    float delay_level = 1.0f;         // the delay's return into the mix
    float delay_to_reverb = 0.0f;     // the delay feeding the reverb
    // Send B: the reverb.
    float reverb_decay_s = 2.0f;      // 0.2 to 30
    float reverb_damping = 0.5f;      // 0 bright, 1 dark
    float reverb_high_pass_hz = 180.0f;   // the high-pass on the reverb's input
    float reverb_level = 1.0f;        // the reverb's return into the mix

    bool operator==(const SendSettings &) const = default;
};

// Which field `Cmd::set_sends` is carrying. Next to the struct on purpose: a field list and the
// indices that name it drift apart the moment they live in different files.
//
// **These numbers reach PROJECT.BIN**, so they are permanent in the same way core/scale's scale
// ids and core/project's MachineId are: new fields go on the END. An index this build does not
// have is refused and counted rather than writing somewhere unintended.
enum class SendField : uint8_t {
    delay_seconds = 0,
    delay_feedback,
    delay_tone,
    delay_ping_pong,
    delay_level,
    delay_to_reverb,
    reverb_decay_s,
    reverb_damping,
    reverb_high_pass_hz,
    reverb_level,
    count
};

// Write one field, clamped to the range its comment above states. False for an index this build
// does not have, so the caller can count the refusal.
bool send_field_set(SendSettings &settings, uint8_t field, float value);

// What it currently holds, for the screen. 0 for an unknown index.
float send_field_get(const SendSettings &settings, uint8_t field);

// Three upper-case characters, for the screen; "---" for an unknown index.
const char *send_field_name(uint8_t field);

// The master chain.
struct MasterSettings {
    float dj = 0.0f;            // -1 full low-pass ... 0 flat (and bypassed) ... +1 full high-pass
    float compressor = 0.3f;    // the glue compressor, 0 off (and bypassed)
    float limit = 0.80f;        // the soft limiter's threshold; the ceiling is always +-1
    float level = 1.0f;         // output level, 0 to 1

    bool operator==(const MasterSettings &) const = default;
};

// Which field `Cmd::set_master` is carrying. Permanent, and on disk, for the same reason as
// SendField above.
enum class MasterField : uint8_t { dj = 0, compressor, limit, level, count };

bool master_field_set(MasterSettings &settings, uint8_t field, float value);
float master_field_get(const MasterSettings &settings, uint8_t field);
const char *master_field_name(uint8_t field);

// --- the encoder domain, and the file's ----------------------------------------------------
//
// Every field above is a BYTE under the encoder and in PROJECT.BIN, and a float only once it
// reaches the mixer. core/project.hpp holds the bytes; this is the only place that knows what
// one means. The asserts are what stop a field being added to one side alone.
static_assert(kSendFields == int(SendField::count),
              "core/project.hpp's send byte count and SendField have diverged");
static_assert(kMasterFields == int(MasterField::count),
              "core/project.hpp's master byte count and MasterField have diverged");

// The delay is TEMPO-SYNCED (docs/SPEC.md), so its byte is a division of the beat and not a
// time: the same project at 128 and at 140 BPM must give a delay that still lands on the grid.
// Seconds arrive in `SendSettings` because dsp/ does not own the tempo (CLAUDE.md).
constexpr int kDelayDivisions = 10;

// Which division a byte names, and the byte an encoder sitting on that division produces.
int delay_division_index(uint8_t byte);
uint8_t delay_division_byte(int index);

// Four characters at most, for the screen: "1/16", "1/8D", "1/1". "----" out of range.
const char *delay_division_name(uint8_t byte);

// The division as a time. `beat_seconds` is 60 / BPM, which only `app/` knows.
float delay_division_seconds(uint8_t byte, float beat_seconds);

// Move a field's byte by `delta` of ITS OWN UNITS, clamped to 0-127. For almost every field a
// unit is one count of the byte; for the delay's time it is one DIVISION, and for the ping-pong
// switch only the sign is read.
//
// A delta rather than a value because the UI must not compute the next byte itself. The byte
// lives in the project, on the audio side, and the audio side applies a command on its next
// block -- so a hand turning the encoder sixteen times in one pass would read the same stale
// byte sixteen times and the last post would win, moving the knob by one click instead of
// sixteen. app/commands.hpp states the rule for every encoder gesture; these are no different.
uint8_t send_byte_step(uint8_t field, uint8_t byte, int delta);
uint8_t master_byte_step(uint8_t field, uint8_t byte, int delta);

// How many of those units one encoder CLICK is worth. Two for a knob, so its whole range is 64
// clicks -- the same feel as a macro -- and one for the two fields whose unit is already a
// click's worth. The UI scales by this; the engine does not, so that any byte can be reached in
// one command and a test can ask for an exact value.
int send_field_click(uint8_t field);
constexpr int kMasterFieldClick = 2;

// One field's physical value from its byte. `beat_seconds` is read for
// `SendField::delay_seconds` and ignored for every other field.
float send_value_from_byte(uint8_t field, uint8_t byte, float beat_seconds);
float master_value_from_byte(uint8_t field, uint8_t byte);

// And the whole struct from the bytes a project holds -- `kSendFields` / `kMasterFields` of
// them. This is how a loaded file, and a fresh project, reach the mixer.
void sends_from_bytes(SendSettings &settings, const uint8_t *bytes, float beat_seconds);
void master_from_bytes(MasterSettings &settings, const uint8_t *bytes);

class Mixer {
public:
    static constexpr int kTracks = 8;
    static constexpr int kMaxFrames = int(ENAGA_BLOCK_FRAMES);
    // 2 s covers a bar at 120 BPM and every useful division below it; 1 MiB of the 2 MiB
    // docs/SPEC.md budgets for delay and reverb buffers.
    static constexpr float kMaxDelaySeconds = 2.0f;
    // The longest a send keeps running unfed: feedback 0.95 on a 2 s delay needs 272 s to fall
    // 60 dB. Bounded only so a damaged setting cannot make the count overflow.
    static constexpr float kMaxTailSeconds = 300.0f;

    Mixer() = default;
    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;

    // App::init() ONLY: this is where the delay and reverb take their buffers from the pool.
    void init(float sample_rate);

    // Control rate, once per block. Every coefficient, reciprocal and tail length lives here.
    void prepare(const SendSettings &sends, const MasterSettings &master);

    // The sidechain duck. The app fires it from its trigger track; this file does not care which.
    void set_duck(float attack_s, float hold_s, float release_s) { duck_.set(attack_s, hold_s, release_s); }
    void trigger_duck(uint32_t frame = 0) { duck_.trigger(frame); }
    const Duck &duck() const { return duck_; }

    InsertChain &chain(int track) { return chains_[track]; }
    const InsertChain &chain(int track) const { return chains_[track]; }

    // A mono scratch block for a machine to render into, big enough for kMaxFrames. The caller
    // clears it, the machine adds into it, add_track() consumes it in place.
    float *scratch() { return scratch_; }

    // Clears the dry bus and both sends and renders this block's duck curve.
    void begin_block(int frames);
    // One track's mono block through its insert chain and into the buses. Consumes `mono`.
    void add_track(int track, float *mono);
    // Both sends, the master chain, and the interleaved L R L R result for audio().
    void finish_block(float *out, int frames);

    // Introspection for the UI and the tests.
    float compressor_gain() const { return compressor_gain_; }
    float peak() const { return peak_; }
    bool delay_running() const { return delay_tail_ > 0; }
    bool reverb_running() const { return reverb_tail_ > 0; }

private:
    // Settings to coefficients, only when the settings have actually changed.
    void resolve_sends(const SendSettings &sends);
    void resolve_master(const MasterSettings &master_settings);

    // The master chain, in place on mix_l_ / mix_r_: DJ filter, peak, glue compressor and
    // output level as one ramp, then the soft limiter.
    void master(int frames);

    float rate_ = 48000.0f;
    int frames_ = 0;

    InsertChain chains_[kTracks];
    Duck duck_;
    bool duck_active_ = false;

    enaga::dsp::StereoDelay delay_;
    enaga::dsp::Reverb reverb_;
    enaga::dsp::Biquad reverb_high_pass_;
    enaga::dsp::BiquadCoefficients reverb_high_pass_coefficients_{};

    // The master's DJ filter: a cascade per channel, so 24 dB for about the price of 12.
    enaga::dsp::Svf dj_l_a_{}, dj_l_b_{}, dj_r_a_{}, dj_r_b_{};
    enaga::dsp::SvfCoefficients dj_coefficients_{};
    bool dj_on_ = false;
    enaga::dsp::Svf::Output dj_output_ = enaga::dsp::Svf::low_pass;

    // Settings resolved at control rate, and the inputs they were resolved from: nothing below
    // is recomputed while a knob is still. Reverb::set alone is eight exp2 calls.
    SendSettings last_sends_{};
    MasterSettings last_master_{};
    bool resolved_ = false;
    float delay_level_ = 1.0f, delay_to_reverb_ = 0.0f, reverb_level_ = 1.0f;
    bool compressor_on_ = false;
    float compressor_slope_ = 0.0f, compressor_inv_threshold_ = 2.0f;
    float compressor_attack_ = 0.5f, compressor_release_ = 0.05f;
    float limit_threshold_ = 0.8f, limit_inv_headroom_ = 5.0f, limit_headroom_ = 0.2f;
    float out_level_ = 1.0f;

    // Running state.
    float compressor_gain_ = 1.0f;   // smoothed, one update per block
    float master_gain_ = 1.0f;       // where the master's gain ramp is now
    float peak_ = 0.0f;              // this block's peak, after the DJ filter
    int delay_tail_ = 0, reverb_tail_ = 0;     // blocks of tail left to render once unfed
    int delay_tail_blocks_ = 1, reverb_tail_blocks_ = 1;

    // 1 / frames, cached: the master's gain ramp needs it and nothing may divide in a loop.
    int ramp_frames_ = 0;
    float inv_frames_ = 1.0f;

    float mix_l_[kMaxFrames] = {};
    float mix_r_[kMaxFrames] = {};
    float send_a_[kMaxFrames] = {};
    float send_a_right_[kMaxFrames] = {};
    float send_b_[kMaxFrames] = {};
    float wet_l_[kMaxFrames] = {};
    float wet_r_[kMaxFrames] = {};
    float duck_curve_[kMaxFrames] = {};
    float scratch_[kMaxFrames] = {};
};

}  // namespace strata
