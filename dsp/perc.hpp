// PERC: 2-operator FM percussion -- rims, toms and blips.
//
// docs/SPEC.md's PERC row is "2-op FM: ratio, index, index envelope, pitch envelope", and that is
// exactly what this is: one modulator phase-modulating one carrier, with an envelope on the
// modulation index and a short downward sweep on both operators' pitch.
//
// There is no Plaits model to follow here -- its five drum files are two analogue models, two
// 909-ish models and a square-noise hat, none of them FM -- so unlike KICK, SNARE and HAT this
// machine has no reference topology to be judged against. What it does follow is the oldest and
// best documented fact about FM percussion, from Chowning's 1973 paper onward: a two-operator
// pair with a FALLING index is percussive, and the same pair with a static index is an organ.
// Everything below is arranged around that.
//
// How the four FM controls make the three sounds track 5 is for:
//   - ratio near an integer puts every sideband on a harmonic of the carrier, so the result is
//     periodic and reads as a pitched, hollow drum: toms.
//   - ratio at an irrational value puts the sidebands between the harmonics, so there is no
//     common period and the result reads as struck metal: rims and cowbells.
//   - a high index with a very short index decay is a click with a tail: blips.
//
// RAT therefore has to reach both kinds, and its lower half is QUANTISED to a table of 16
// useful ratios rather than sweeping continuously (see the RAT comment in perc.cpp for why).
//
// Two voices, so a flam or a ratchet does not cut itself off. When both are busy the quieter one
// is stolen, and if it is still loud it is faded over kFadeFrames first and the new hit starts at
// the end of that fade -- the same approach as dsp/kick.cpp, for the same reason.
//
// It reads the SRC page (0-3) and the AMP page (8-11); FLTR and FX belong to the insert chain.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

class Perc final : public Machine {
public:
    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // The carrier's frequency as TUN last set it, in Hz, before the trig note transposes it. For
    // the UI, and for tests that need to know where the carrier is.
    float base_frequency() const { return base_hz_; }
    // The modulator : carrier ratio as RAT last set it. For the UI's value readout, and for
    // tests that pin the quantisation.
    float ratio() const { return ratio_; }
    // The peak index in TURNS of carrier phase, as IDX last set it, before velocity scales it and
    // before the anti-alias ceiling caps it. One turn is 2*pi radians of the usual FM index.
    float peak_index() const { return index_; }

    // The RAT mapping on its own, so the UI can show the ratio a knob value will give without
    // disturbing the machine. Pure, and cheap enough to call while drawing.
    static float ratio_for(uint8_t knob);

    static constexpr int kVoices = 2;
    // RAT's quantised region: knob 0-63 selects one of 16 ratios, 64-127 sweeps continuously.
    static constexpr int kRatioSlots = 16;
    static constexpr int kQuantisedTop = 63;

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
        enaga::dsp::Envelope amp;
        float carrier_phase = 0.0f;
        float mod_phase = 0.0f;
        float index = 0.0f;        // the index envelope: falls towards 0 every sample
        float sweep = 0.0f;        // the pitch envelope: falls from 1 towards 0 every sample
        float ratio = 1.0f;        // latched at the hit, so a lock cannot bend a ringing tail
        float note_ratio = 1.0f;   // the trig note's frequency ratio against note 60
        float trigger_level = 1.0f;   // 0.3 + 0.7 * accent, as the other machines use it
        float gain = 1.0f;         // the declick fade's current gain
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
    enaga::dsp::EnvelopeShape amp_shape_{};

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    float base_hz_ = 300.0f;       // TUN
    float ratio_ = 4.0f;           // RAT
    float index_ = 0.5f;           // IDX, in turns of carrier phase
    float index_decay_ = 0.99f;    // IDD, as a per-sample multiplier
    float sweep_amount_ = 0.0f;    // SWP, as a multiple of f0 added at the attack
    float sweep_decay_ = 0.98f;    // SWT, as a per-sample multiplier
    float out_gain_ = 0.25f;       // LVL
    bool active_ = false;

    float body_[kBlock] = {};
    float env_[kBlock] = {};
};

}  // namespace strata
