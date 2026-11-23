#include "fm.hpp"

#include "va.hpp"   // the ASR page's knob curves, so ATK and REL feel the same on every machine

namespace strata {
namespace {

using enaga::dsp::EnvelopeShape;
using enaga::dsp::clamp;
using enaga::dsp::exp2;
using enaga::dsp::reciprocal;
using enaga::dsp::semitones;

constexpr float kRatios[kFmRatios] = {0.5f, 1.0f, 2.0f, 3.0f, 3.5f, 4.0f, 7.0f, 14.0f};
constexpr float kCarriers[5] = {1.0f, 0.5f, 2.0f, 3.0f, 4.0f};

constexpr float kIndexMaxTurns = 2.0f;      // IDX 127: about 12.6 radians, a very bright FM
constexpr float kFeedbackMaxTurns = 0.22f;  // FBK 127: op 4 most of the way to noise
constexpr float kDetuneMaxCents = 25.0f;
constexpr float kDecayLowS = 0.015f;
constexpr float kDecayOctaves = 9.0589f;    // log2(8 / 0.015): DEC, 15 ms to 8 s
constexpr float kIndexDecayShare = 0.35f;   // the index envelope falls in about a third the time
constexpr float kLevelMax = 0.5f;           // LVL 127; a carrier peaks at 1

// Yamaha's eight 4-operator algorithms as link masks (m12 m13 m14 m23 m24 m34) and output
// weights. Operator 1 is always a carrier.
struct Algorithm {
    float m12, m13, m14, m23, m24, m34;
    float c1, c2, c3, c4;
};
constexpr Algorithm kAlgorithms[kFmAlgorithms] = {
    // 1: 4 > 3 > 2 > 1, one carrier: the brightest, most complex
    {1, 0, 0, 1, 0, 1, 1.0f, 0, 0, 0},
    // 2: (3 + 4) > 2 > 1
    {1, 0, 0, 1, 1, 0, 1.0f, 0, 0, 0},
    // 3: 3 > 2 > 1, and 4 > 1
    {1, 0, 1, 1, 0, 0, 1.0f, 0, 0, 0},
    // 4: 4 > 3 > 1, and 2 > 1
    {1, 1, 0, 0, 0, 1, 1.0f, 0, 0, 0},
    // 5: two pairs, 2 > 1 and 4 > 3: the e-piano and the thick detuned pair
    {1, 0, 0, 0, 0, 1, 0.5f, 0, 0.5f, 0},
    // 6: 4 > 1, 2 and 3: one modulator under three carriers
    {0, 0, 1, 0, 1, 1, 0.3333f, 0.3333f, 0.3333f, 0},
    // 7: 4 > 3, and 1 and 2 plain: one FM pair over two sines
    {0, 0, 0, 0, 0, 1, 0.3333f, 0.3333f, 0.3333f, 0},
    // 8: four sines, additive: an organ
    {0, 0, 0, 0, 0, 0, 0.25f, 0.25f, 0.25f, 0.25f},
};

int zone(uint8_t value, int zones) {
    const int z = int(value) * zones / 128;
    return z < zones - 1 ? z : zones - 1;
}

// The OSC page arrives with the VA overhaul (dsp/track.hpp, kOscFirst). Until a build has it,
// the four bytes read as their defaults: no detune, no fine offset, no keytrack, carrier 1.
template <int N>
uint8_t osc_byte(const uint8_t (&v)[N], int k) {
    constexpr int first = kAsrFirst + kAsrParams;
    if constexpr (N >= first + 4) {
        return v[first + k];
    } else {
        return k == 1 ? 64 : 0;
    }
}

}  // namespace

int fm_algorithm(uint8_t value) { return zone(value, kFmAlgorithms); }
float fm_ratio(uint8_t value) { return kRatios[zone(value, kFmRatios)]; }
float fm_fine(uint8_t value) { return float(int(value) - 64) * (1.0f / 128.0f); }
float fm_carrier_ratio(uint8_t value) { return kCarriers[zone(value, 5)]; }
float fm_detune_cents(uint8_t value) {
    const float u = float(value) * (1.0f / 127.0f);
    return kDetuneMaxCents * u * u;
}

FmSettings FmSettings::make(const TrackParams &params, float rate, float inv_rate) {
    FmSettings s;
    s.rate = rate;
    s.inv_rate = inv_rate;
    s.algorithm = fm_algorithm(params.get(0));

    const float mod = fm_ratio(params.get(1)) + fm_fine(osc_byte(params.v, 1));
    const float car = fm_carrier_ratio(osc_byte(params.v, 3));
    const float spread = semitones(fm_detune_cents(osc_byte(params.v, 0)) * 0.01f);
    s.ratio[0] = car;
    s.ratio[1] = mod > 0.05f ? mod : 0.05f;
    s.ratio[2] = car * spread;
    s.ratio[3] = s.ratio[1] * spread;

    const float idx = params.unit(2);
    s.index = kIndexMaxTurns * idx * idx;
    s.feedback = kFeedbackMaxTurns * params.unit(3);
    s.index_env = params.unit(9);
    s.velocity_index = params.unit(10);
    s.keytrack = float(osc_byte(params.v, 2)) * (1.0f / 127.0f);
    const float lvl = params.unit(11);
    s.out_gain = kLevelMax * lvl * lvl;

    s.decay_s = kDecayLowS * exp2(kDecayOctaves * params.unit(8));
    const float attack_s = va_attack_seconds(params.get(Page::asr, 0));
    s.sustain = params.unit(Page::asr, 1);
    const float release_s = va_release_seconds(params.get(Page::asr, 2));
    const float index_sustain = params.unit(Page::asr, 3);
    s.amp_shape = EnvelopeShape::make(attack_s, s.decay_s, s.sustain, release_s, rate);
    s.index_shape = EnvelopeShape::make(attack_s, s.decay_s * kIndexDecayShare, index_sustain,
                                        release_s * kIndexDecayShare,
                                        rate * (1.0f / float(FmVoice::kControl)));
    return s;
}

void FmVoice::reset() {
    amp_.kill();
    index_env_.kill();
    state_ = strata_fm_state{};
    for (float &i : index_) i = 0.0f;
    fb_ = 0.0f;
    fresh_ = true;
    choke_left_ = 0;
    choke_gain_ = 1.0f;
}

void FmVoice::kill() {
    amp_.kill();
    index_env_.kill();
    choke_left_ = 0;
    choke_gain_ = 1.0f;
}

void FmVoice::gate_on() {
    // A voice taken while silent starts its operators from zero, so every note of a patch has
    // the same attack; one retriggered while sounding keeps its phases, since a jump there clicks.
    if (!sounding()) state_ = strata_fm_state{};
    amp_.gate_on();
    index_env_.gate_on(true);
    fresh_ = true;
    choke_left_ = 0;
    choke_gain_ = 1.0f;
}

void FmVoice::choke() {
    if (!sounding() || choke_left_ > 0) return;
    choke_left_ = kChokeFrames;
    choke_step_ = choke_gain_ * (1.0f / float(kChokeFrames));
}

void FmVoice::run(const FmSettings &s, float *out, int n, float *env) {
    if (n <= 0) return;
    // --- control rate: the operators' increments and the indices this slice ramps to ---
    const float hz = 440.0f * semitones(note_ - 69.0f);
    const float base = hz * s.inv_rate;
    strata_fm_block b{};
    for (int op = 0; op < 4; op++) {
        float inc = base * s.ratio[op];
        inc = inc < 0.49f ? inc : 0.49f;
        b.inc[op] = uint32_t(inc * 4294967296.0f);
    }
    const Algorithm &a = kAlgorithms[s.algorithm];
    b.m12 = a.m12;
    b.m13 = a.m13;
    b.m14 = a.m14;
    b.m23 = a.m23;
    b.m24 = a.m24;
    b.m34 = a.m34;
    b.c[0] = a.c1;
    b.c[1] = a.c2;
    b.c[2] = a.c3;
    b.c[3] = a.c4;

    const float ienv = index_env_.step(s.index_shape);
    float index = s.index * ((1.0f - s.index_env) + s.index_env * ienv);
    index *= (1.0f - s.velocity_index) + s.velocity_index * velocity_;
    // BRT: the index falls with the key above middle C, so high notes do not turn to glass.
    index *= exp2(-s.keytrack * clamp((note_ - 60.0f) * (1.0f / 12.0f), 0.0f, 6.0f));
    float target[3];
    for (int m = 0; m < 3; m++) {
        // An index whose sidebands would fold past Nyquist is capped: Carson's rule, the band
        // of an FM pair is about 2 (I + 1) fm, with I in radians.
        const float fm = base * s.ratio[m + 1];
        const float cap = fm > 1e-6f ? (0.45f * reciprocal(fm) - 1.0f) * 0.159155f : kIndexMaxTurns;
        target[m] = clamp(index, 0.0f, cap > 0.0f ? cap : 0.0f);
    }
    if (fresh_) {
        for (int m = 0; m < 3; m++) index_[m] = target[m];
        fb_ = s.feedback;
        fresh_ = false;
    }
    const float inv_n = reciprocal(float(n));
    b.i2 = index_[0];
    b.i3 = index_[1];
    b.i4 = index_[2];
    b.fb = fb_;
    b.di2 = (target[0] - index_[0]) * inv_n;
    b.di3 = (target[1] - index_[1]) * inv_n;
    b.di4 = (target[2] - index_[2]) * inv_n;
    b.dfb = (s.feedback - fb_) * inv_n;
    for (int m = 0; m < 3; m++) index_[m] = target[m];
    fb_ = s.feedback;

    // --- the amplitude envelope at the sample rate, with the level and any choke in it ---
    amp_.render(env, n, s.amp_shape);
    const float level = s.out_gain;
    if (choke_left_ <= 0) {
        for (int i = 0; i < n; i++) env[i] *= level;
    } else {
        // A choke (the shared voice budget taking this voice): a straight fade to silence over
        // kChokeFrames, then the voice is free. Rare, so it can afford its branches.
        for (int i = 0; i < n; i++) {
            env[i] *= level * choke_gain_;
            choke_gain_ = choke_gain_ > choke_step_ ? choke_gain_ - choke_step_ : 0.0f;
            if (--choke_left_ == 0) {
                for (int j = i + 1; j < n; j++) env[j] = 0.0f;
                kill();
                break;
            }
        }
    }
    strata_fm_add(out, n, env, &state_, &b);
}

}  // namespace strata
