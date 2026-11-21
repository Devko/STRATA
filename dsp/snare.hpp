// SNARE: a tuned pair under band-limited noise, with a clap mode.
//
// Follows Mutable Instruments Plaits, `synthetic_snare_drum.h` -- the 909 model, whose author
// notes it "uses a few magic numbers taken from the 909 schematics". Plaits is MIT licensed,
// **Copyright 2016 Emilie Gillet**. STRATA reimplements the topology and the published constants
// on Enaga's primitives rather than porting stmlib. docs/SOUND.md records the recipe.
//
// The parts of the model implemented, with the reference's numbers:
//   - two oscillators at f and f * 1.47 (the ratio is off the 909 schematic), both through the
//     same rational soft-sine shaper
//   - drum = (-0.1 + 0.60 * osc0 + 0.25 * osc1) * drum_env * sqrt(1 - snappy), then a one-pole
//     low-pass at 3 * f0
//   - noise -> low-pass at min(35 * f0, 0.5) with Q = 0.5 + 2 * snappy -> one-pole high-pass at
//     min(10 * f0, 0.5), scaled by sqrt(snappy)
//   - the noise envelope's hold stage, 0.04 + 0.03 * decay seconds, before it decays at all --
//     the thing that makes a 909 snare sound right and the easiest to miss
//   - drum envelope time constant 15 ms * 2^((decay * 72 + fm_amount * 12 - snappy * 7) / 12),
//     the reference's "very long tail"
//   - clap mode retunes the pair to the 808 shell ratio 1.00 : 2.00
//
// Left out, and why: the 909's oscillator coupling (a phase reset with a reflected wrap, active
// only at low f0) is not implemented, so BDY only carries the reference's fm_amount term into
// the drum envelope; the 808 clap's full five-mode shell resonator with its g0/g1 gains is not
// implemented either -- the clap's bursts go through the 909 noise chain, and only the 1.00 :
// 2.00 ratio is taken from it. The noise tail's own time constant follows the drum envelope's
// law without the fm_amount term, which is derived rather than published.
//
// Two voices, the quieter one stolen with a short declick fade. SRC (0-3) and AMP (8-11) only;
// FLTR and FX belong to the insert chain.
#pragma once

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

class Snare final : public Machine {
public:
    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // f0 as TUN last set it, in Hz, before the trig note transposes it. For the UI, and for
    // tests that pin the tuning.
    float base_frequency() const { return base_hz_; }

    static constexpr int kVoices = 2;
    // The 909 schematic's interval between the two tuned oscillators, and the 808 shell ratio
    // the clap mode borrows instead. Both are reference constants: tests pin them.
    static constexpr float kDrumRatio = 1.47f;
    static constexpr float kClapRatio = 2.00f;

private:
    static constexpr int kBlock = 64;
    static constexpr int kFadeFrames = 24;
    static constexpr float kFadeStep = 1.0f / float(kFadeFrames);
    static constexpr float kFadeFloor = 0.01f;
    // The drum part comes out of its one-pole at 3*f0 well below full scale, and the noise
    // through a Q 0.5-2.5 band is quieter still: one trim, so the default sounds like a snare
    // and LVL 127 sits where KICK's and HAT's do.
    static constexpr float kMakeup = 1.5f;

    struct Voice {
        enaga::dsp::Envelope drum, noise;
        enaga::dsp::Svf noise_band;
        enaga::dsp::OnePole drum_lp, noise_hp;
        float phase_a = 0.0f, phase_b = 0.0f;
        float ratio = kDrumRatio;   // latched at the hit, so turning CLP cannot jump the pitch
        float note_ratio = 1.0f;
        float trigger_level = 1.0f;
        float gain = 1.0f;
        int hold = 0;          // frames the noise envelope is still held at full level
        int bursts = 0;        // clap bursts still to fire after this one
        int burst_wait = 0;    // frames until the next clap burst
        int fade = 0;
        int wait = 0;
        bool pending = false;
        Trig next{};

        // Sustain is 0 for everything but the noise envelope's hold, so a finished envelope sits
        // in the `sustain` stage rather than `idle`: neither Envelope::active() nor the stage
        // alone can say a one-shot voice has stopped, its level can. The attack is excluded
        // because it starts from zero.
        static bool done(const enaga::dsp::Envelope &e) {
            const enaga::dsp::Envelope::Stage s = e.stage();
            return s == enaga::dsp::Envelope::idle ||
                   (s != enaga::dsp::Envelope::attack && e.level() < 1e-4f);
        }
        bool sounding() const {
            return hold > 0 || bursts > 0 || !done(noise) || !done(drum);
        }
        bool busy() const { return pending || sounding(); }
        float level() const { return noise.level() + drum.level(); }
    };

    void start(Voice &voice);
    void silence(Voice &voice);
    void render_voice(Voice &voice, float *out, int n);
    void run(Voice &voice, float *out, int n);

    Voice voices_[kVoices];
    enaga::dsp::Random rng_{0x1BD11BDAu};
    enaga::dsp::EnvelopeShape drum_shape_{};
    enaga::dsp::EnvelopeShape noise_hold_shape_{};   // sustain 1: the hold stage
    enaga::dsp::EnvelopeShape noise_tail_shape_{};   // sustain 0: what the hold decays with
    enaga::dsp::EnvelopeShape burst_shape_{};        // a clap burst before the last

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    float base_hz_ = 195.0f;       // TUN
    float drum_gain_ = 0.7f;       // sqrt(1 - snappy)
    float noise_gain_ = 0.7f;      // sqrt(snappy)
    float noise_k_ = 1.0f;         // 1 / (0.5 + 2 * snappy)
    float tone_ratio_ = 1.0f;      // TON, as a ratio on the reference's 35 * f0
    int hold_frames_ = 2640;       // 0.04 + 0.03 * decay seconds
    int clap_bursts_ = 0;          // CLP: 0 for a snare, else 3 or 4
    int burst_frames_ = 480;       // SPR
    float out_gain_ = 0.25f;       // LVL
    bool active_ = false;

    float mix_[kBlock] = {};
    float drum_[kBlock] = {};
    float noise_[kBlock] = {};
    float env_[kBlock] = {};
    float aux_[kBlock] = {};
};

}  // namespace strata
