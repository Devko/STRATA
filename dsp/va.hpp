// VA MONO: the bass voice, track 6 of the default techno kit.
//
// One monophonic virtual-analogue voice, built on the same primitives and in the same shape as
// Enaga's own `examples/synth` engine (two oscillators with hard sync, a sub, noise, a
// state-variable filter with a 24 dB cascade, ADSRs, MONO glide). That example is the measured
// reference the whole budget is built from -- 1,868 cycles per voice per block -- so following
// its structure is also how this file stays inside it.
//
// What the spec asks for (docs/SPEC.md, "Sound architecture"): "2 oscillators with sync, sub,
// noise, multimode filter, 2 envelopes, accent, slide, glide". All of it is here:
//
//   OSC 1   PolyBLEP saw, the backbone.
//   OSC 2   PolyBLEP pulse, HARD SYNCED to OSC 1 and tuned +19 semitones. Synced rather than
//           detuned because sync locks it to OSC 1's period: it adds a bright formant that
//           cannot drift out of tune with the fundamental, which is what a bass needs. Its
//           level is small normally and large on an accent, so the accent brings the bite.
//   SUB     Wave::triangle an octave down. Triangle, not sine: Enaga's triangle is naive
//           (piecewise linear, no PolyBLEP and no polynomial) and so is a third the cost of its
//           sine, while an octave below the fundamental the difference is inaudible under the
//           saw. This is the knob-free "weight" every techno bass needs.
//   NOISE   A trace, lifted by an accent. Mostly removed again by the low-pass; it is there as
//           attack grit, not as a voice of its own.
//   FILTER  A multimode SVF in the VOICE (see below), swept by its own envelope: the acid
//           squelch. 24 dB uses Svf::cascade, which is two poles for about the price of one.
//   ENVS    Two: the amplitude envelope at the sample rate, and the filter envelope at the
//           control rate. One DEC knob shapes both, 303-style, with the filter's sweep shorter
//           than the amplitude's so the "wow" lands at the front of the note.
//
// **The voice filter is on the SRC page, not the AMP page.** docs/SPEC.md gives the insert
// chain its own multimode filter on the FLTR page already; that one is the track's utility
// filter. This one is the voice's *source character* -- the envelope sweep is the thing a
// listener calls "the bass" -- so MOD, CUT and RES sit next to TUN on SRC, where they are
// three adjacent encoders you can ride together, and the AMP page keeps the envelope's decay,
// how much of it reaches the filter, the glide and the level.
//
// Monophonic: one voice, so a new trig steals it. A stolen voice keeps its oscillator phases
// and its filter state and its amplitude envelope goes on from the level it is at, exactly as
// the reference does, because resetting any of those under a sounding note is a step in the
// output -- a click.
//
// It reads the SRC page (0-3) and the AMP page (8-11); FLTR and FX belong to the insert chain.
#pragma once

#include <enaga/dsp.hpp>

#include "track.hpp"

namespace strata {

class VaMono final : public Machine {
public:
    // The voice filter's four settings. LP24 is a cascade of two poles (Svf::cascade); the
    // other three are one pole each.
    enum class Mode : uint8_t { lp24 = 0, lp12, bp12, hp12 };

    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // --- for the UI, and for tests that need to pin a mapping rather than guess at it -------

    // MOD's zones. Deliberately unequal: see va.cpp.
    static Mode mode_from(uint8_t value);
    Mode mode() const { return mode_; }
    // CUT in Hz, before the filter envelope and the accent move it.
    float base_cutoff_hz() const { return cutoff_hz_; }
    // The glide's current value and where it is heading, as note numbers (TUN included).
    float pitch() const { return pitch_; }
    float target_pitch() const { return target_; }
    // The amplitude envelope's level: what a slide must not reset.
    float amp_level() const { return amp_.level(); }
    float decay_seconds() const { return decay_s_; }
    float glide_seconds() const { return glide_s_; }

private:
    // Frames per control step. 16 is the reference's figure: modulation at 3 kHz, which is fast
    // enough that a filter sweep does not step audibly, and slow enough that the tangent and
    // the reciprocals behind a set of SVF coefficients cost 1/16 of what they would per sample.
    // A trig landing mid-step shortens one slice so a note still starts on its exact frame.
    static constexpr int kControl = 16;

    // A slide is always audible: below this it would be a jump, not a slide. The 303's own
    // slide time is fixed at about this, and GLD only ever makes a slide longer.
    static constexpr float kSlideFloorS = 0.055f;

    // The level below which the amplitude envelope counts as finished. An Envelope with
    // sustain 0 parks in the `sustain` stage rather than reaching `idle` on its decay path
    // (docs/ENAGA_API.md, "Gotchas"), so the level is what says a one-shot note is over.
    static constexpr float kSilent = 1e-4f;

    bool sounding() const {
        const enaga::dsp::Envelope::Stage s = amp_.stage();
        return s != enaga::dsp::Envelope::idle &&
               (s == enaga::dsp::Envelope::attack || amp_.level() >= kSilent);
    }

    void start();                       // begin the pending trig, on its exact frame
    void run(float *out, int n);        // one control step and its n samples

    // --- the voice ----------------------------------------------------------------------
    enaga::dsp::Oscillator osc1_, osc2_, sub_;
    enaga::dsp::Svf svf_a_, svf_b_;     // svf_a_ is LP24's first (non-resonant) stage
    enaga::dsp::Envelope amp_, fenv_;
    enaga::dsp::Random rng_{0x1F3B9A7Du};

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    float control_rate_ = 3000.0f;

    float pitch_ = 60.0f;               // glide's current value, in note numbers
    float target_ = 60.0f;              // where it is heading
    float sync_carry_ = 0.0f;           // enaga_oscillator_sync_add's sample in hand
    float gain_ = 0.0f;                 // the output gain the last slice ended on
    float velocity_ = 1.0f;             // this note's velocity, 0-1
    bool accent_ = false;               // this note's accent flag
    bool slide_ = false;                // this note arrived on a slid step
    bool first_note_ = true;            // nothing to glide from yet
    bool active_ = false;

    Trig next_{};                       // the pending trig and the frames until it starts
    int wait_ = 0;
    bool pending_ = false;

    // --- control-rate values, all made in prepare() -------------------------------------
    enaga::dsp::EnvelopeShape amp_shape_{}, amp_shape_accent_{};
    enaga::dsp::EnvelopeShape fenv_shape_{}, fenv_shape_accent_{};
    Mode mode_ = Mode::lp12;
    float tune_ = 0.0f;                 // TUN, semitones
    float cutoff_hz_ = 650.0f;          // CUT
    float k_res_ = 1.0f;                // RES as the SVF's k = 1 / Q
    float env_semis_ = 0.0f;            // ENV, semitones of cutoff at envelope 1
    float decay_s_ = 0.2f;              // DEC
    float glide_s_ = 0.0f;              // GLD
    float glide_coef_ = 1.0f;           // GLD as a per-control-step one-pole coefficient
    float slide_coef_ = 1.0f;           // the same for kSlideFloorS
    float level_osc1_ = 0.0f;           // the oscillator levels, resonance makeup included
    float level_osc2_ = 0.0f;
    float level_sub_ = 0.0f;
    float level_noise_ = 0.0f;
    float accent_osc2_ = 0.0f;          // what an accent adds to those two
    float accent_noise_ = 0.0f;
    float drive_ = 1.0f;                // the post-filter soft clip's drive
    float out_gain_ = 0.25f;            // LVL

    float voice_[kControl] = {};        // the slice being built
    float amp_env_[kControl] = {};
};

}  // namespace strata
