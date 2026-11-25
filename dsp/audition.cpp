#include "audition.hpp"

namespace strata {

namespace {

constexpr uint32_t kFracMask = Audition::kStepOne - 1u;
constexpr float kInvFrac = 1.0f / 16777216.0f;          // 2^-24
constexpr float kInvFull = 1.0f / 32768.0f;
constexpr int kMaxBlock = 64;

// Catmull-Rom, as dsp/sample.cpp's hermite4: the cubic through x1 and x2 whose slopes are the
// central differences.
inline float hermite4(float x0, float x1, float x2, float x3, float t) {
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

inline float clamp1(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

}  // namespace

uint32_t Audition::step_for(uint32_t rate, uint32_t output_rate) {
    if (rate < kMinRate || rate > kMaxRate || output_rate == 0u) return 0u;
    return uint32_t((uint64_t(rate) << 24) / uint64_t(output_rate));
}

bool Audition::start(uint32_t total, int channels, uint32_t step) {
    on_ = false;
    fading_ = false;
    finished_ = false;
    if (total == 0u || (channels != 1 && channels != 2) || step == 0u || step > 4u * kStepOne) {
        return false;
    }
    channels_ = channels;
    total_ = total;
    pos_ = 0;
    frac_ = 0;
    istep_ = step >> 24;
    fstep_ = step & kFracMask;
    fifo_.reset(channels, 0);
    on_ = true;
    return true;
}

void Audition::stop() {
    if (on_) fading_ = true;
}

void Audition::render_add(float *out, int frames) {
    if (!on_ || frames <= 0) return;
    const int n = frames < kMaxBlock ? frames : kMaxBlock;
    const uint32_t last = total_ - 1u;

    // Every tap this block reads, from the first sample's x0 to the last one's x3, has to be in
    // the fifo already; otherwise hold. Exact, in 64 bits: no estimate decides a read.
    const uint64_t advance = uint64_t(frac_) + uint64_t(fstep_) * uint64_t(n - 1);
    const uint64_t far64 = uint64_t(pos_) + uint64_t(istep_) * uint64_t(n - 1) + (advance >> 24) + 2u;
    const uint32_t far = far64 > uint64_t(last) ? last : uint32_t(far64);
    const uint32_t near = pos_ > 0u ? (pos_ - 1u > last ? last : pos_ - 1u) : 0u;
    if (!(fifo_.has(near) && fifo_.has(far))) {
        if (fading_) {
            // Stopped while waiting for the card: it is already silent, so it simply ends.
            on_ = false;
            fading_ = false;
            finished_ = true;
        } else {
            late_++;
        }
        return;
    }

    // A stop fades to nothing over this block, a sample at a time.
    float gain = kGain * kInvFull;
    const float fall = fading_ ? gain * (1.0f / float(n)) : 0.0f;
    const bool stereo = channels_ == 2;
    for (int i = 0; i < n; i++) {
        if (pos_ >= total_) break;
        const uint32_t p = pos_;
        const uint32_t a = p > 0u ? p - 1u : 0u;
        const uint32_t b = p < last ? p + 1u : last;
        const uint32_t c = p + 2u < last ? p + 2u : last;
        // All four are between `near` and `far`, which the check above found in the fifo.
        const int16_t *f0 = fifo_.frame(a);
        const int16_t *f1 = fifo_.frame(p);
        const int16_t *f2 = fifo_.frame(b);
        const int16_t *f3 = fifo_.frame(c);
        const float t = float(frac_) * kInvFrac;
        if (stereo) {
            const float l = hermite4(float(f0[0]), float(f1[0]), float(f2[0]), float(f3[0]), t);
            const float r = hermite4(float(f0[1]), float(f1[1]), float(f2[1]), float(f3[1]), t);
            out[2 * i] = clamp1(out[2 * i] + gain * l);
            out[2 * i + 1] = clamp1(out[2 * i + 1] + gain * r);
        } else {
            const float m = hermite4(float(f0[0]), float(f1[0]), float(f2[0]), float(f3[0]), t);
            out[2 * i] = clamp1(out[2 * i] + gain * m);
            out[2 * i + 1] = clamp1(out[2 * i + 1] + gain * m);
        }
        gain -= fall;
        frac_ += fstep_;
        pos_ += istep_ + (frac_ >> 24);
        frac_ &= kFracMask;
    }
    if (pos_ > 1u) fifo_.drop_before(pos_ - 1u);

    if (fading_ || pos_ >= total_) {
        on_ = false;
        fading_ = false;
        finished_ = true;
    }
}

}  // namespace strata
