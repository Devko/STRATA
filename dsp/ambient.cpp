// The ambient send modes. dsp/ambient.hpp has the design; ambientloops.ti.c the loops.
#include "ambient.hpp"

#include <cstddef>

#include <enaga/dsp.hpp>

namespace strata {

namespace ed = enaga::dsp;

namespace {

float clamp_to(float value, float low, float high) {
    const float lowered = value < low ? low : value;
    return lowered > high ? high : lowered;
}

// A one-pole low-pass's coefficient for a corner frequency.
float one_pole(float hz, float rate) {
    return 1.0f - ed::exp(-2.0f * ed::pi * hz * ed::reciprocal(rate));
}

// The phasor's rotation per frame for a rate in Hz.
void rotation(float hz, float rate, float &c, float &s) {
    const float turns = hz * ed::reciprocal(rate);
    c = ed::cos_turns(turns);
    s = ed::sin_turns(turns);
}

// Pull a phasor back onto the unit circle (one Newton step): rotating by a rounded sine and
// cosine drifts its length a little every frame.
void renormalise(float &s, float &c) {
    const float k = 1.5f - 0.5f * (s * s + c * c);
    s *= k;
    c *= k;
}

float *zeroed(std::size_t count) {
    float *p = new float[count];
    if (p == nullptr) return nullptr;
    for (std::size_t i = 0; i < count; i++) p[i] = 0.0f;
    return p;
}

constexpr int kLongDiffuse[2 * STRATA_LONG_DIFFUSERS] = {241, 383, 563, 829,    // left: 2016
                                                         263, 359, 601, 793};   // right: 2016
static_assert(LongDelay::kDiffuserFrames == STRATA_LONG_AHEAD, "the feed's lead is the all-passes'");
static_assert(kLongDiffuse[0] + kLongDiffuse[1] + kLongDiffuse[2] + kLongDiffuse[3] ==
                  LongDelay::kDiffuserFrames &&
                  kLongDiffuse[4] + kLongDiffuse[5] + kLongDiffuse[6] + kLongDiffuse[7] ==
                      LongDelay::kDiffuserFrames,
              "both sides' all-passes must add up to the compensation");

// The hall's lines at size 1 and 48 kHz (60 ms to 156 ms), mutually prime.
constexpr float kHallLines[STRATA_HALL_LINES] = {2903.0f, 3469.0f, 4093.0f, 4723.0f,
                                                 5413.0f, 6037.0f, 6763.0f, 7477.0f};
constexpr int kHallDiffuse[STRATA_HALL_DIFFUSERS] = {241, 347, 461, 587};
constexpr float kHallModFrames = 40.0f;   // the sweep's full depth at 48 kHz
constexpr float kShimmerGain = 2.0f;
constexpr float kHallMaxLength = float(STRATA_HALL_RING) - 2.0f * kHallModFrames - 4.0f;

// 1 / n without a division per block for the usual block size.
float per_frame(int n) {
    return n == 64 ? (1.0f / 64.0f) : ed::reciprocal(float(n > 0 ? n : 1));
}

}  // namespace

// --- the LONG delay -------------------------------------------------------------------------

bool LongDelay::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    // The feed is written STRATA_LONG_AHEAD frames ahead of the head (ambientloops.ti.c), so the
    // ring holds that much more than the longest read.
    const int size = int(kMaxSeconds * rate_) + STRATA_LONG_AHEAD + 64;
    const std::size_t diffuse = std::size_t(2 * STRATA_LONG_DIFFUSERS * STRATA_LONG_DIFFUSER_RING);
    state_ = strata_long_delay{};
    state_.left = zeroed(std::size_t(size));
    state_.right = zeroed(std::size_t(size));
    state_.diffuse = zeroed(diffuse);
    if (state_.left == nullptr || state_.right == nullptr || state_.diffuse == nullptr) return false;
    bytes_ = (2 * std::size_t(size) + diffuse) * sizeof(float);
    state_.size = size;
    state_.lfo_s = 0.0f;
    state_.lfo_c = 1.0f;
    state_.delay_whole = 16;
    for (int k = 0; k < 2 * STRATA_LONG_DIFFUSERS; k++) block_.diffuse_len[k] = kLongDiffuse[k];
    block_.input = 1.0f;
    set(0.5f, 0.5f, 0.5f, 80.0f, 0.0f, 0.5f, true, true);
    return true;
}

void LongDelay::set(float seconds, float feedback, float tone, float low_cut_hz, float wow,
                    float diffusion, bool ping_pong, bool snap) {
    const float max_wow = kMaxWowSeconds * rate_;
    const int longest = state_.size - STRATA_LONG_AHEAD - int(2.0f * max_wow) - 8;
    const int frames = int(clamp_to(seconds, 0.0f, kMaxSeconds) * rate_ + 0.5f) - kDiffuserFrames;
    block_.target = frames < 16 ? 16 : (frames > longest ? longest : frames);
    if (snap) {
        state_.delay_whole = block_.target;
        state_.delay_frac = 0.0f;
    }
    block_.glide = 0.5f;

    feedback_target_ = clamp_to(feedback, 0.0f, 0.95f);
    // The tone: 400 Hz dark to about 19 kHz bright, the classic delay's TON in feel.
    low_pass_ = one_pole(400.0f * ed::exp2(5.6f * clamp_to(tone, 0.0f, 1.0f)), rate_);
    high_pass_ = one_pole(clamp_to(low_cut_hz, 10.0f, 2000.0f), rate_);
    // The wow: squared, so the first half of the knob is a gentle drift; faster as it deepens.
    const float w = clamp_to(wow, 0.0f, 1.0f);
    wow_target_ = w * w * max_wow;
    rotation(0.12f + 0.6f * w, rate_, block_.rot_c, block_.rot_s);
    block_.diffusion = 0.7f * clamp_to(diffusion, 0.0f, 1.0f);
    block_.ping_pong = ping_pong ? 1 : 0;
}

void LongDelay::process(const float *in, float *out_l, float *out_r, int n, float level) {
    if (n <= 0 || state_.left == nullptr) return;
    const float inv = per_frame(n);
    // Held: the feed fades out over the block, the loop goes to exactly 1 and every loss opens.
    const float input_to = held_ ? 0.0f : 1.0f;
    const float feedback_to = held_ ? 1.0f : feedback_target_;
    const float wow_to = held_ ? 0.0f : wow_target_;
    block_.input_step = (input_to - block_.input) * inv;
    block_.feedback_step = (feedback_to - block_.feedback) * inv;
    block_.wow_step = (wow_to - block_.wow) * inv;
    // The low cut's state is what it subtracts; a held loop must not keep subtracting a stale
    // one, so it starts from zero when the hold opens it.
    if (held_ && block_.high_pass != 0.0f) {
        state_.hp_l = 0.0f;
        state_.hp_r = 0.0f;
    }
    block_.low_pass = held_ ? 1.0f : low_pass_;
    block_.high_pass = held_ ? 0.0f : high_pass_;
    block_.level = level;
    strata_long_delay_process(&state_, &block_, in, out_l, out_r, n);
    // Land exactly on the targets: the ramps' sums are not.
    block_.input = input_to;
    block_.feedback = feedback_to;
    block_.wow = wow_to;
    renormalise(state_.lfo_s, state_.lfo_c);
}

// --- the HALL reverb ------------------------------------------------------------------------

bool HallReverb::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    state_ = strata_hall{};
    const std::size_t lines = std::size_t(STRATA_HALL_LINES) * STRATA_HALL_RING;
    const std::size_t diffuse = std::size_t(STRATA_HALL_DIFFUSERS) * STRATA_HALL_DIFFUSER_RING;
    state_.lines = zeroed(lines);
    state_.diffuse = zeroed(diffuse);
    state_.shimmer = zeroed(STRATA_SHIMMER_RING);
    if (state_.lines == nullptr || state_.diffuse == nullptr || state_.shimmer == nullptr) {
        return false;
    }
    bytes_ = (lines + diffuse + STRATA_SHIMMER_RING) * sizeof(float);
    state_.lfo_s = 0.0f;
    state_.lfo_c = 1.0f;
    for (int j = 0; j < STRATA_HALL_DIFFUSERS; j++) block_.diffuse_len[j] = kHallDiffuse[j];
    block_.diffusion = 0.65f;
    set(4.0f, 0.5f, 1.0f, 0.5f, 0.0f, true);
    return true;
}

void HallReverb::set(float decay_s, float damping, float size, float mod, float shimmer,
                     bool snap) {
    const float scale = clamp_to(size, kMinSize, kMaxSize) * rate_ * (1.0f / 48000.0f);
    const float decay = clamp_to(decay_s, 0.2f, 60.0f);
    const float per_second = -9.9658f * ed::reciprocal(decay * rate_);   // log2(0.001) / (T60 fs)
    for (int j = 0; j < STRATA_HALL_LINES; j++) {
        target_[j] = clamp_to(kHallLines[j] * scale, 64.0f, kHallMaxLength);
        if (snap) state_.length[j] = target_[j];
        block_.gain[j] = ed::exp2(per_second * target_[j]);
    }
    // Damping: 18 kHz bright down to about 280 Hz dark.
    block_.damping = one_pole(18000.0f * ed::exp2(-6.0f * clamp_to(damping, 0.0f, 1.0f)), rate_);
    const float m = clamp_to(mod, 0.0f, 1.0f);
    block_.mod = m * kHallModFrames * rate_ * (1.0f / 48000.0f);
    rotation(0.2f + 0.9f * m, rate_, block_.rot_c, block_.rot_s);
    // The shimmer's feed, scaled by how much the lines lose a pass: a long decay already
    // re-circulates nearly everything, and the same feed there would be a loop gain well over
    // one (a 30 s hall at a flat 0.55 swelled to +25 dB). This keeps the shifted loop's gain
    // about the same whatever the decay: a rising shimmer that still dies away.
    float loss = 0.0f;
    for (int j = 0; j < STRATA_HALL_LINES; j++) loss += 1.0f - block_.gain[j];
    block_.shimmer = kShimmerGain * loss * (1.0f / float(STRATA_HALL_LINES)) *
                     clamp_to(shimmer, 0.0f, 1.0f);
}

void HallReverb::process(const float *in, float *out_l, float *out_r, int n, float level) {
    if (n <= 0 || state_.lines == nullptr) return;
    // Each line glides toward its length at a quarter of a frame per frame: a SIZ turn bends
    // the tail's pitch a little, as a real hall's would not, but never clicks.
    const float most = 0.25f * float(n);
    const float inv = per_frame(n);
    for (int j = 0; j < STRATA_HALL_LINES; j++) {
        const float gap = clamp_to(target_[j] - state_.length[j], -most, most);
        block_.length_step[j] = gap * inv;
    }
    block_.level = level;
    strata_hall_process(&state_, &block_, in, out_l, out_r, n);
    for (int j = 0; j < STRATA_HALL_LINES; j++) {
        // Snap the last fraction of a frame so a still knob settles exactly.
        const float gap = target_[j] - state_.length[j];
        if (gap < 0.01f && gap > -0.01f) state_.length[j] = target_[j];
    }
    renormalise(state_.lfo_s, state_.lfo_c);
}

}  // namespace strata
