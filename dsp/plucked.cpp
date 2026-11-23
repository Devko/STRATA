#include "plucked.hpp"

#include <new>

namespace strata {
namespace {

using enaga::dsp::clamp;
using enaga::dsp::cos_turns;
using enaga::dsp::exp2;
using enaga::dsp::log2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;
using enaga::dsp::sin_turns;

constexpr float kDecayLowS = 0.1f;
constexpr float kDecayOctaves = 7.6439f;     // log2(20 / 0.1): DEC, 0.1 s to 20 s
constexpr float kLevelMax = 0.6f;            // LVL 127
constexpr float kHighestHz = 4000.0f;        // a period of 12 samples: the loop's tuning holds
constexpr float kLog2Thousand = 9.96578f;    // a T60 is a fall of 1000 times

// A tuned bar's modes (marimba-like: the first overtone two octaves up) and a free bar's
// (Rossing, "The Science of Sound": 1, 2.756, 5.404, 8.933).
constexpr float kTuned[STRATA_BAR_MODES] = {1.0f, 4.0f, 10.0f, 20.0f};
constexpr float kFree[STRATA_BAR_MODES] = {1.0f, 2.756f, 5.404f, 8.933f};

float sqrt_approx(float x) { return x > 0.0f ? exp2(0.5f * log2(x)) : 0.0f; }

}  // namespace

PluckModel pluck_model(uint8_t value) { return value < 64 ? PluckModel::string : PluckModel::bar; }

PluckExciter pluck_exciter(uint8_t value) {
    return value < 43 ? PluckExciter::pluck : value < 86 ? PluckExciter::noise : PluckExciter::mallet;
}

float pluck_decay_seconds(uint8_t value) {
    return kDecayLowS * exp2(kDecayOctaves * float(value) * (1.0f / 127.0f));
}

PluckSettings PluckSettings::make(const TrackParams &params, float rate, float inv_rate) {
    PluckSettings s;
    s.rate = rate;
    s.inv_rate = inv_rate;
    s.model = pluck_model(params.get(0));
    s.exciter = pluck_exciter(params.get(1));
    s.brightness = params.unit(2);
    s.position = 0.05f + 0.45f * params.unit(3);
    s.decay_s = pluck_decay_seconds(params.get(8));
    s.inharmonicity = params.unit(9);
    s.velocity_bright = params.unit(10);
    const float lvl = params.unit(11);
    s.out_gain = kLevelMax * lvl * lvl;
    return s;
}

bool PluckVoice::init() {
    if (string_.line == nullptr) string_.line = new (std::nothrow) float[STRATA_PLUCK_LINE];
    if (string_.line == nullptr) return false;
    reset();
    return true;
}

void PluckVoice::reset() {
    if (string_.line != nullptr)
        for (int i = 0; i < STRATA_PLUCK_LINE; i++) string_.line[i] = 0.0f;
    string_.write = 0;
    string_.lp = 0.0f;
    bar_ = strata_bar_state{};
    burst_left_ = 0;
    burst_pos_ = 0;
    peak_ = 0.0f;
    quiet_slices_ = 0;
    sounding_ = false;
    muted_ = false;
    coef_note_ = -1.0f;
    choke_left_ = 0;
    choke_gain_ = 1.0f;
}

void PluckVoice::kill() {
    // Silence now. The line keeps what it holds -- clearing 2,048 samples on a kill is a cost in
    // audio() for nothing -- and the next note adds into it at below kSilent.
    sounding_ = false;
    burst_left_ = 0;
    bar_ = strata_bar_state{};
    string_.lp = 0.0f;
    choke_left_ = 0;
    choke_gain_ = 1.0f;
}

void PluckVoice::choke() {
    if (!sounding_ || choke_left_ > 0) return;
    choke_left_ = kChokeFrames;
    choke_step_ = choke_gain_ * (1.0f / float(kChokeFrames));
}

float PluckVoice::frequency(const PluckSettings &s) const {
    (void)s;
    return clamp(440.0f * semitones(note_ - 69.0f), kLowestHz, kHighestHz);
}

void PluckVoice::gate_on(const PluckSettings &s, float *scratch) {
    model_ = s.model;
    muted_ = false;
    choke_left_ = 0;
    choke_gain_ = 1.0f;
    quiet_slices_ = 0;
    sounding_ = true;
    // Velocity brightens as well as lifts, by VEL: a harder pluck has more highs in it.
    const float bright = clamp(
        s.brightness * ((1.0f - s.velocity_bright) + s.velocity_bright * velocity_), 0.0f, 1.0f);
    const float amp = velocity_;

    if (model_ == PluckModel::bar) {
        // A burst into the four resonators, its sum about 1 so a mode rings at about its gain.
        for (int i = 0; i < kBurst; i++) burst_[i] = 0.0f;
        if (s.exciter == PluckExciter::mallet) {
            const int w = 2 + int((1.0f - bright) * 40.0f);   // a soft mallet is a wide pulse
            const float g = amp * 1.5708f * reciprocal(float(w));
            for (int i = 0; i < w; i++) burst_[i] = g * sin_turns(0.5f * float(i) * reciprocal(float(w)));
            burst_left_ = w;
        } else if (s.exciter == PluckExciter::noise) {
            strata_pluck_noise(burst_, kBurst, &rng_, 0.2f + 0.8f * bright);
            const float g = amp * 0.25f;
            for (int i = 0; i < kBurst; i++) burst_[i] *= g * (1.0f - float(i) * (1.0f / float(kBurst)));
            burst_left_ = kBurst;
        } else {
            burst_[0] = amp;   // a click
            burst_left_ = 1;
        }
        burst_pos_ = 0;
        coef_note_ = -1.0f;   // a new note, new coefficients
        return;
    }

    // The string: one period of excitation added into the line, pick-position comb applied.
    const float hz = frequency(s);
    const float period = s.rate * reciprocal(hz);
    int n = int(period) + 2;
    n = n < STRATA_PLUCK_LINE - 2 ? n : STRATA_PLUCK_LINE - 2;
    float gain = amp;
    if (s.exciter == PluckExciter::mallet) {
        const int w = 2 + int((0.5f - 0.45f * bright) * float(n));
        for (int i = 0; i < n; i++)
            scratch[i] = i < w ? sin_turns(0.5f * float(i) * reciprocal(float(w))) : 0.0f;
    } else {
        // Filtered noise: a one-pole's output is about sqrt(c / (2 - c)) of its input's level,
        // so the gain puts every brightness at about the same loudness.
        const float c = s.exciter == PluckExciter::noise ? 0.4f + 0.6f * bright : 0.03f + 0.97f * bright * bright;
        strata_pluck_noise(scratch, n, &rng_, c);
        gain *= 0.6f * sqrt_approx((2.0f - c) * reciprocal(c));
    }
    int comb = int(s.position * float(n) + 0.5f);
    comb = comb < 1 ? 1 : comb;
    strata_pluck_inject(string_.line, string_.write, scratch, n, comb, gain);
}

void PluckVoice::run(const PluckSettings &s, float *out, int n, float *work, float *env) {
    if (n <= 0 || !sounding_) return;

    // The level, with any choke in it.
    if (choke_left_ <= 0) {
        for (int i = 0; i < n; i++) env[i] = s.out_gain;
    } else {
        for (int i = 0; i < n; i++) {
            env[i] = s.out_gain * choke_gain_;
            choke_gain_ = choke_gain_ > choke_step_ ? choke_gain_ - choke_step_ : 0.0f;
            if (--choke_left_ == 0) {
                for (int j = i + 1; j < n; j++) env[j] = 0.0f;
                break;
            }
        }
        if (choke_left_ == 0) sounding_ = false;   // this slice is the fade's last
    }

    const float t60 = muted_ ? (s.decay_s < kMuteS ? s.decay_s : kMuteS) : s.decay_s;
    const float hz = frequency(s);
    float peak;
    if (model_ == PluckModel::bar) {
        if (coef_note_ != note_ || coef_decay_ != t60 || coef_inh_ != s.inharmonicity ||
            coef_bright_ != s.brightness || coef_pos_ != s.position) {
            coef_note_ = note_;
            coef_decay_ = t60;
            coef_inh_ = s.inharmonicity;
            coef_bright_ = s.brightness;
            coef_pos_ = s.position;
            const float roll = 0.3f + 0.7f * s.brightness;
            float tilt = 1.0f;
            for (int m = 0; m < STRATA_BAR_MODES; m++) {
                // Geometric blend between the tuned and the free bar's ratios.
                const float ratio = exp2((1.0f - s.inharmonicity) * log2(kTuned[m]) + s.inharmonicity * log2(kFree[m]));
                const float w = hz * ratio * s.inv_rate;   // turns a sample
                const float t = t60 * reciprocal(1.0f + 1.2f * float(m));
                const float r = exp2(-kLog2Thousand * reciprocal(t * s.rate));
                const float shape = sin_turns(0.5f * float(m + 1) * s.position);
                const float amp = (shape < 0.0f ? -shape : shape) * tilt;
                const bool audible = w < 0.45f;
                coefs_.a1[m] = audible ? 2.0f * r * cos_turns(w) : 0.0f;
                coefs_.a2[m] = audible ? -r * r : 0.0f;
                coefs_.g[m] = audible ? amp * sin_turns(w) : 0.0f;   // a unit impulse rings at amp
                tilt *= roll;
            }
        }
        // The burst, then silence, as the resonators' input.
        for (int i = 0; i < n; i++) {
            const bool on = burst_left_ > 0;
            work[i] = on ? burst_[burst_pos_] : 0.0f;
            burst_pos_ += on ? 1 : 0;
            burst_left_ -= on ? 1 : 0;
        }
        peak = strata_bar_run(out, n, work, env, &bar_, &coefs_);
    } else {
        const float coef = 0.15f + 0.85f * s.brightness;
        const float period = s.rate * reciprocal(hz);
        // The one-pole delays the loop by (1 - c) / c samples at low frequencies; the line is
        // shortened by as much so the note stays in tune.
        delay_ = clamp(period - (1.0f - coef) * reciprocal(coef), 2.0f, float(STRATA_PLUCK_LINE - 2));
        const float gain = clamp(exp2(-kLog2Thousand * period * reciprocal(t60 * s.rate)), 0.0f, 0.99999f);
        peak = strata_pluck_run(out, n, env, &string_, delay_, coef, gain);
    }

    peak_ = peak;
    if (peak < kSilent && burst_left_ == 0) {
        if (++quiet_slices_ > 8) sounding_ = false;
    } else {
        quiet_slices_ = 0;
    }
}

}  // namespace strata
