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
constexpr float kLevelMax = 0.25f;          // LVL 127: a chord sits with VA POLY's (tests/test_fm_machine.cpp)

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

const float *fm_sine_table() {
    static float table[STRATA_FM_SINE_SIZE + 1];
    static bool filled = false;
    if (!filled) {
        for (int i = 0; i < STRATA_FM_SINE_SIZE; i++) {
            table[i] = enaga::dsp::sin_turns(float(i) * (1.0f / float(STRATA_FM_SINE_SIZE)));
        }
        table[STRATA_FM_SINE_SIZE] = table[0];
        filled = true;
    }
    return table;
}

namespace {

// How far up a carrier's sidebands may reach by Carson's estimate, as a fraction of the rate.
// Above 0.5 they fold back down as inharmonic tones -- FM's aliasing, which was loud: the stack
// (ALG 1) at IDX 90 on middle C had a folded tone 12 dB under the loudest harmonic, and 9 dB
// on C6, with the old per-pair cap. Carson's rule is pessimistic through a chain (the far
// sidebands it counts are weak), so the limit is set by measurement (tools/sound_probe.py),
// not at 0.5: at 1.0 the strongest fold under 5 kHz is 45-56 dB down, about the VA oscillators'
// own floor, and middle C keeps 20x of its 23x brightness; at 0.625 it was -93 dB but only 9x
// bright, and at 1.5 -28 dB.
constexpr float kBandLimit = 1.0f;

// The highest frequency each carrier reaches at index `turns` (fm_index_cap). Carson's rule
// for a sine is (I + 1) fm; the `min(I, 1)` makes the +1 fade in from I = 0, where a link with
// no index adds nothing.
float fm_band(const Algorithm &a, const float f[4], float b4, float turns) {
    const float rad = 6.2831853f * turns;
    const float q = rad + (rad < 1.0f ? rad : 1.0f);
    const float b3 = f[2] + a.m34 * q * b4;
    const float b2 = f[1] + q * (a.m23 * b3 + a.m24 * b4);
    const float b1 = f[0] + q * (a.m12 * b2 + a.m13 * b3 + a.m14 * b4);
    float top = b1;
    top = a.c2 > 0.0f && b2 > top ? b2 : top;
    top = a.c3 > 0.0f && b3 > top ? b3 : top;
    top = a.c4 > 0.0f && b4 > top ? b4 : top;
    return top;
}

}  // namespace

float fm_index_cap(int algorithm, const float f[4], float feedback, float limit) {
    const Algorithm &a = kAlgorithms[algorithm < 0 ? 0 : algorithm % kFmAlgorithms];
    // Op 4's own band with its feedback, which is self-modulation of index FBK.
    const float fb_rad = 6.2831853f * feedback;
    const float b4 = f[3] * (1.0f + fb_rad + (fb_rad < 1.0f ? fb_rad : 1.0f));
    // A modulator past Nyquist cannot be played at all (its increment is clamped, so it would
    // sound at the wrong frequency): no index. RAT 14 on a high note is the case.
    const bool mod2 = a.m12 > 0.0f, mod3 = a.m13 + a.m23 > 0.0f, mod4 = a.m14 + a.m24 + a.m34 > 0.0f;
    if ((mod2 && f[1] >= 0.45f) || (mod3 && f[2] >= 0.45f) || (mod4 && f[3] >= 0.45f)) return 0.0f;
    if (fm_band(a, f, b4, kIndexMaxTurns) <= limit) return kIndexMaxTurns;
    if (fm_band(a, f, b4, 0.0f) >= limit) return 0.0f;
    float lo = 0.0f, hi = kIndexMaxTurns;
    for (int i = 0; i < 16; i++) {
        const float mid = 0.5f * (lo + hi);
        if (fm_band(a, f, b4, mid) <= limit) lo = mid;
        else hi = mid;
    }
    return lo;
}

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
    (void)fm_sine_table();
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
    // The note with VariSpeed on it; at rest the sum is the note to the bit.
    const float pitch = note_ + s.pitch_mod;
    const float hz = 440.0f * semitones(pitch - 69.0f);
    const float base = hz * s.inv_rate;
    strata_fm_block b{};
    b.sine = fm_sine_table();
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
    // An index whose sidebands would fold past Nyquist is capped, for the whole chain at once
    // (fm_index_cap): every link carries the same index, so one cap holds the algorithm's
    // balance and only lowers its brightness. Recomputed when the note or a knob moves.
    if (pitch != cap_note_ || s.algorithm != cap_algorithm_ || s.feedback != cap_feedback_ ||
        s.inv_rate != cap_inv_rate_ || s.ratio[0] != cap_ratio_[0] ||
        s.ratio[1] != cap_ratio_[1] || s.ratio[2] != cap_ratio_[2] || s.ratio[3] != cap_ratio_[3]) {
        cap_note_ = pitch;
        cap_algorithm_ = s.algorithm;
        cap_feedback_ = s.feedback;
        cap_inv_rate_ = s.inv_rate;
        float f[4];
        for (int op = 0; op < 4; op++) {
            cap_ratio_[op] = s.ratio[op];
            f[op] = base * s.ratio[op];
        }
        cap_ = fm_index_cap(s.algorithm, f, s.feedback, kBandLimit);
    }
    float target[3];
    for (int m = 0; m < 3; m++) target[m] = clamp(index, 0.0f, cap_);
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
    // FBK at 0 across the slice: the loop with no sample-to-sample chain (fmloops.h).
    if (b.fb == 0.0f && b.dfb == 0.0f) {
        strata_fm_add_free(out, n, env, &state_, &b);
    } else {
        strata_fm_add(out, n, env, &state_, &b);
    }
}

}  // namespace strata
