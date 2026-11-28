// KICK: the techno kick machine.
//
// Follows Mutable Instruments Plaits, `synthetic_bass_drum.h` -- the model its author describes
// as "naive bass drum model (modulated oscillator with FM + envelope). Inadvertently 909-ish."
// Plaits is MIT licensed, **Copyright 2016 Emilie Gillet**. STRATA reimplements the topology and
// the published constants on Enaga's primitives rather than porting stmlib, because our code has
// to be block-based with control-rate coefficients. docs/SOUND.md records the recipe, the reason
// the 909 architecture is the right default for techno, and what is left out here.
//
// The parts of the model implemented, with the reference's numbers:
//   - one phase accumulator, increment f0 * (1 + 3.5 * fm_amount * fm_envelope), capped at 0.5
//   - the phase pinned at 0.25 for 1.3 ms after a trigger, where the shaper reads 0, so the body
//     enters at a zero crossing and the click is what is heard first
//   - the body as lerp(clean sine, rational soft sine, dirtiness), where the fold is
//     2*t/(1+|t|) of a bare triangle t: dirtiness 0 is a pure sine and 1 is the fold, and the
//     bare triangle is only an intermediate, never the output. dirtiness is itself scaled by
//     max(1 - 8*f0, 0), which forces it towards clean where the distortion would alias
//   - amplitude envelope time constant 20 ms * 2^(5 * decay^2), so 20 ms to 640 ms, decay squared
//   - FM envelope time constant from a base of 8 ms
//   - click: a 1 ms gate, slewed 0.5 rising / 0.1 falling, a one-pole high-pass, then a low-pass
//     at 5 kHz with Q 2; attack noise: a one-pole low-pass at 0.05 minus one at 0.005
//   - output low-pass coefficient min(4 * f0 * 2^(tone * 108/12), 1)
//
// Left out, and why: the reference ties the transient level to `tone`, where docs/SPEC.md asks
// for a click control of its own, so CLK is split out and the output low-pass applies to the body
// only; the phase noise that Plaits also scales by dirtiness is not implemented, which keeps the
// hot loop free of a per-sample random.
//
// Two voices, so a ratchet or a flam does not cut itself off. When both are busy the quieter one
// is stolen; if it is still loud it fades over kFadeFrames first and the new hit starts at the
// end of that fade -- the spec's "retrigger with a short fade".
//
// The PNCH page (the ASR page's four slots, 24-27, which no other drum reads) adds what the
// techno benchmark (2026-11-28) found the model cannot reach: measured against 400+ techno kicks
// from sample libraries, its first 20 ms was 20-35 dB short from 250 Hz to 4 kHz, and nothing of
// it fell below f0. A layered techno kick is a low punch, a mid punch and a top; these are the
// last two and the fall:
//   KNK  knock: a second pitch envelope on top of FMA's, up to 15 x f0 more (four octaves above
//        the body), much faster -- the "mid punch" layer, 250 Hz-1 kHz in the first milliseconds
//   KND  the knock's time constant, 1-16 ms exponential
//   TCK  tick: noise high-passed at 3 kHz under a 1.5 ms envelope -- the top layer, 2-6 kHz
//   DRP  drop: the pitch keeps falling below TUN as the hit decays, up to an octave
// All four at 0 is the kick as it was, bit for bit: KNK 0 runs the old body loop, TCK 0 skips
// the tick (its noise has a generator of its own, so the click's noise stream never moves), and
// DRP 0 leaves f0 alone.
//
// It reads the SRC page (0-3), the AMP page (8-11) and PNCH (24-27); FLTR and FX belong to the
// insert chain.
#pragma once

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

// PNCH's knob curves, for the screen as well as for prepare().
float kick_knock_seconds(uint8_t value);     // KND: 1-16 ms
int kick_drop_cents(uint8_t value);          // DRP: 0 to -1200, where the fall ends

class Kick final : public Machine {
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

private:
    // The slice render_add works in, so every scratch buffer is a fixed member array: nothing is
    // allocated and nothing large goes on the stack. ENAGA_BLOCK_FRAMES is 64; a longer call is
    // cut into slices of this size.
    static constexpr int kBlock = 64;
    // The declick fade a stolen voice gets, and the level below which it is not worth one.
    static constexpr int kFadeFrames = 24;              // 0.5 ms at 48 kHz
    static constexpr float kFadeStep = 1.0f / float(kFadeFrames);
    static constexpr float kFadeFloor = 0.01f;

    struct Voice {
        enaga::dsp::Envelope amp, transient;
        enaga::dsp::Svf click_band;                  // the reference's 5 kHz Q 2 on the click
        enaga::dsp::OnePole noise_lp, noise_hp;      // attack noise, 0.05 minus 0.005
        enaga::dsp::OnePole tone_lp;                 // the output low-pass
        enaga::dsp::Svf tick_hp;                     // TCK's 3 kHz high-pass
        float phase = 0.0f;
        float fm_env = 0.0f;
        float knock_env = 0.0f;    // KNK's pitch envelope
        float tick_env = 0.0f;     // TCK's level envelope
        float drop_env = 1.0f;     // DRP: 1 at the hit, falling towards 0 (the full drop)
        float slew = 0.0f;         // the click gate's slew limiter
        float click_hp = 0.0f;     // the click's one-pole high-pass state
        float note_ratio = 1.0f;   // the trig note's frequency ratio against note 60
        float trigger_level = 1.0f;   // 0.3 + 0.7 * accent, as in the reference
        float gain = 1.0f;         // the declick fade's current gain
        int hold = 0;              // frames the phase is still pinned at 0.25
        int gate = 0;              // frames the click gate is still high
        int fade = 0;              // frames of fade left, 0 when not fading
        int wait = 0;              // frames until `next` starts
        bool pending = false;
        Trig next{};

        // An envelope with sustain 0 ends up in the `sustain` stage, not `idle`, so neither
        // Envelope::active() nor the stage alone can say a one-shot voice has finished: its
        // level is what tells us. The attack is excluded because it starts from zero.
        static bool done(const enaga::dsp::Envelope &e) {
            const enaga::dsp::Envelope::Stage s = e.stage();
            return s == enaga::dsp::Envelope::idle ||
                   (s != enaga::dsp::Envelope::attack && e.level() < 1e-4f);
        }
        bool sounding() const { return !done(amp); }
        bool busy() const { return pending || sounding(); }
    };

    void start(Voice &voice);
    void silence(Voice &voice);
    void render_voice(Voice &voice, float *out, int n);
    void run(Voice &voice, float *out, int n);

    Voice voices_[kVoices];
    enaga::dsp::Random rng_{0x5F2A41C7u};
    enaga::dsp::Random tick_rng_{0x2C9E5B13u};    // TCK's own, so the click's noise never moves
    enaga::dsp::EnvelopeShape amp_shape_{};
    enaga::dsp::EnvelopeShape transient_shape_{};
    enaga::dsp::SvfCoefficients click_coefficients_{};
    enaga::dsp::SvfCoefficients tick_coefficients_{};

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    int hold_frames_ = 62;         // 1.3 ms
    int gate_frames_ = 48;         // 1 ms
    float base_hz_ = 55.0f;        // TUN
    float fm_amount_ = 0.0f;       // FMA
    float fm_decay_ = 0.9f;        // FMD, as a per-sample multiplier
    float dirtiness_ = 0.0f;       // DRT, before the f0 scaling
    float click_level_ = 0.0f;     // CLK
    float tone_ratio_ = 1.0f;      // TON, as 2^(tone * 108/12)
    float out_gain_ = 0.25f;       // LVL
    float knock_amount_ = 0.0f;    // KNK: extra multiples of f0 at the hit, 0-15
    float knock_decay_ = 0.98f;    // KND, as a per-sample multiplier
    float tick_gain_ = 0.0f;       // TCK
    float tick_decay_ = 0.986f;    // the tick's 1.5 ms, as a per-sample multiplier
    float drop_target_ = 1.0f;     // DRP: f0's ratio at the end of the fall, 1 (none) to 0.5
    float drop_rate_ = 0.0f;       // -1 / (the fall's time constant in frames)
    bool active_ = false;

    float body_[kBlock] = {};
    float env_[kBlock] = {};
    float transient_env_[kBlock] = {};
    float click_[kBlock] = {};
    float noise_[kBlock] = {};
    float aux_[kBlock] = {};
};

}  // namespace strata
