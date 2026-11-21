// HAT: six square oscillators and clocked noise through a band-pass -- the 808 metallic recipe.
//
// Follows Mutable Instruments Plaits, `hi_hat.h` with its `SquareNoise` source: "808 style
// 'metallic noise' with 6 square oscillators". Plaits is MIT licensed, **Copyright 2016 Emilie
// Gillet**. STRATA reimplements the topology and the published constants on Enaga's primitives
// rather than porting stmlib. docs/SOUND.md records the recipe.
//
// The parts of the model implemented, with the reference's numbers:
//   - six squares at ratios 1.0, 1.304, 1.466, 1.787, 1.932, 2.536 of a nominal f0 of 414 Hz,
//     each a 32-bit phase accumulator whose square is its top bit, which costs no oscillator
//     maths at all; each frequency clamped to 0.499 normalised
//   - the mix 0.33 * (number of squares currently high) - 1.0. 0.33, not 1/3: the asymmetry is
//     in the source
//   - a band-pass at 150 Hz * 2^(tone * 72/12), so 150 Hz to 9.6 kHz, Q = 3 + 3 * tone
//   - then the VCA, asymmetric (x4 positive, x0.1 negative, through a soft clip), then a
//     high-pass at the same cutoff with Q 0.5
//   - the envelope's trigger level (1.5 + 0.5 * (1 - decay)) * (0.3 + 0.7 * accent), which feeds
//     that non-linear VCA, so an accent brightens as well as lifts
//   - a time constant of 7 ms at decay 0 to 0.9 s at decay 1
//   - the clocked sample-and-hold noise at f0 * (16 + 16 * (1 - noisiness)), crossfaded in by
//     noisiness. The reference's author is explicit that this is not part of the 808 circuit, so
//     it is an extra -- it is what docs/SPEC.md means by "plus noise"
//   - the two-stage envelope's faster tail, used as the choke
//
// Left out, and why: Enaga's cubic soft clip stands in for Plaits' rational one, which is the
// same job; the reference engages the faster envelope stage once the level passes below 0.5,
// where a choke has to bite at once, so choke() switches to it immediately; the KR-55 and FM
// alternatives to SquareNoise are not implemented.
//
// Choking is a mechanism, not a wiring diagram. choke() switches a voice to the fast tail, and
// set_choke_target() makes this hat choke another one whenever it is triggered. The app decides
// that the closed hat on track 3 chokes the open one on track 4; CHK is only the group number it
// can do that by.
#pragma once

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

class Hat final : public Machine {
public:
    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // --- the choke mechanism ---

    // Cut this hat short by switching its voices to the fast envelope tail that the reference's
    // two-stage envelope uses, rather than stepping them to zero. CHO sets how fast. Cheap and
    // safe to call when nothing is sounding.
    void choke();

    // Make this hat choke `target` every time it is triggered. The app wires it (track 3 to
    // track 4 in the default kit); nullptr, and the default, is no choking. A hat never chokes
    // itself.
    void set_choke_target(Hat *target) { choke_target_ = target == this ? nullptr : target; }
    Hat *choke_target() const { return choke_target_; }

    // CHK as a group number, 0 (none) to 4, for an app that would rather wire by group than by
    // pointer. The machine itself does nothing with it.
    int choke_group() const { return choke_group_; }

    static constexpr int kVoices = 2;
    static constexpr int kOscillators = 6;
    // The reference's ratios and its nominal f0. Tests pin these so a later tidy-up cannot
    // quietly change the sound.
    static constexpr float kRatio[kOscillators] = {1.0f,   1.304f, 1.466f,
                                                   1.787f, 1.932f, 2.536f};
    static constexpr float kNominalHz = 414.0f;

    // The ratio actually in use for oscillator `index` once DTN has scaled it. Knob 64 is
    // exactly kRatio.
    float detune_ratio(int index) const;

    // f0 as TUN last set it, in Hz, before the trig note transposes it. Knob 64 is kNominalHz.
    float base_frequency() const { return base_hz_; }

private:
    static constexpr int kBlock = 64;
    static constexpr int kFadeFrames = 24;
    static constexpr float kFadeStep = 1.0f / float(kFadeFrames);
    static constexpr float kFadeFloor = 0.01f;
    // The VCA leaves the signal inside +-1 and mostly below it, so one trim puts the default
    // near full scale.
    static constexpr float kMakeup = 1.6f;

    struct Voice {
        enaga::dsp::Envelope amp;
        enaga::dsp::Svf band, high;
        uint32_t phase[kOscillators] = {};
        uint32_t sample_hold_phase = 0;
        float sample_hold = 0.0f;
        float note_ratio = 1.0f;
        float trigger_level = 1.0f;
        float gain = 1.0f;
        int fade = 0;
        int wait = 0;
        bool pending = false;
        bool choked = false;
        Trig next{};

        // Sustain is 0, so a finished envelope sits in the `sustain` stage rather than `idle`:
        // neither Envelope::active() nor the stage alone can say a one-shot voice has stopped,
        // its level can. The attack is excluded because it starts from zero.
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
    Hat *choke_target_ = nullptr;
    enaga::dsp::Random rng_{0x7A31C05Bu};
    enaga::dsp::EnvelopeShape amp_shape_{};
    enaga::dsp::EnvelopeShape choke_shape_{};
    enaga::dsp::SvfCoefficients band_{};
    enaga::dsp::SvfCoefficients high_{};

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    float base_hz_ = kNominalHz;              // TUN
    float ratio_[kOscillators] = {};          // DTN, applied to kRatio
    float noisiness_ = 0.0f;                  // NOI
    float sample_hold_ratio_ = 32.0f;         // 16 + 16 * (1 - NOI)
    float decay_ = 0.5f;                      // DEC, for the trigger level
    float out_gain_ = 0.25f;                  // LVL
    int choke_group_ = 0;                     // CHK
    bool active_ = false;

    float mix_[kBlock] = {};
    float env_[kBlock] = {};
};

}  // namespace strata
