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

// The master chain.
struct MasterSettings {
    float dj = 0.0f;            // -1 full low-pass ... 0 flat (and bypassed) ... +1 full high-pass
    float compressor = 0.3f;    // the glue compressor, 0 off (and bypassed)
    float limit = 0.80f;        // the soft limiter's threshold; the ceiling is always +-1
    float level = 1.0f;         // output level, 0 to 1

    bool operator==(const MasterSettings &) const = default;
};

class Mixer {
public:
    static constexpr int kTracks = 8;
    static constexpr int kMaxFrames = int(ENAGA_BLOCK_FRAMES);
    // 2 s covers a bar at 120 BPM and every useful division below it; 1 MiB of the 2 MiB
    // docs/SPEC.md budgets for delay and reverb buffers.
    static constexpr float kMaxDelaySeconds = 2.0f;

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
