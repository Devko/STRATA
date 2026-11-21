// Resampling's capture buffer. dsp/capture.hpp says what it records, why it is int16, and why
// bar alignment is the caller's job.
#include "capture.hpp"

#include <cstddef>   // std::size_t, rather than relying on <cstdint> to drag it in

namespace strata {
namespace {

// Float to int16, the way dsp/sample.cpp's writer does it: scale by 32767 and clamp, so a master
// that has been through the limiter at exactly +/-1.0 lands on full scale rather than wrapping.
//
// Branch-free on purpose. This runs twice per frame of a take, which at 48 kHz is 96,000 times a
// second, and the C674x pays for a mispredicted branch (CLAUDE.md: hot loops use `?:`).
int16_t to_s16(float x) {
    const float scaled = x * 32767.0f;
    const float low = scaled < -32768.0f ? -32768.0f : scaled;
    const float high = low > 32767.0f ? 32767.0f : low;
    return int16_t(high);
}

}  // namespace

bool Capture::init(float sample_rate) {
    const float rate = sample_rate > 1.0f ? sample_rate : 48000.0f;
    capacity_ = uint32_t(rate * kCaptureSeconds);
    if (capacity_ < 2u) capacity_ = 2u;

    // Interleaved stereo int16: 5.76 MiB at 48 kHz. Allocated once, here, as CLAUDE.md requires.
    buffer_ = new int16_t[2u * std::size_t(capacity_)];
    if (buffer_ == nullptr) return false;
    for (uint32_t i = 0; i < 2u * capacity_; i++) buffer_[i] = 0;

    wanted_ = 0;
    written_ = 0;
    state_ = State::idle;
    clamped_ = false;
    return true;
}

bool Capture::arm(uint32_t frames) {
    // A take in flight is not interrupted. A second press on the resample key during a take
    // would otherwise throw away what had been recorded and start again from the next bar, which
    // is never what someone pressing it wants and is unrecoverable.
    if (state_ == State::recording) return false;

    // Nor is a finished take overwritten before the card has had it. `release()` is the main
    // context saying it is done, and until then this buffer is the take.
    if (state_ == State::done) return false;

    if (frames == 0u || buffer_ == nullptr) return false;

    clamped_ = frames > capacity_;
    wanted_ = clamped_ ? capacity_ : frames;
    written_ = 0;
    state_ = State::armed;
    return true;
}

void Capture::begin() {
    if (state_ != State::armed) return;
    // `arm` has already zeroed this and `armed` is only reachable through `arm`, so the line is
    // redundant today -- a mutation that removes it survives, and that is honest rather than a
    // missing test. It stays because "a take starts at frame 0" belongs to the function that
    // starts the take: the day something else arms a capture, the invariant is still here.
    written_ = 0;
    state_ = State::recording;
}

void Capture::cancel() {
    // Not `done`: a finished take belongs to the main context until it releases it, and a stop
    // or a pattern change must not pull a buffer out from under a save in progress.
    if (state_ == State::armed || state_ == State::recording) {
        state_ = State::idle;
        wanted_ = 0;
        written_ = 0;
    }
}

void Capture::process(const float *master, int frames) {
    if (state_ != State::recording || master == nullptr || frames <= 0) return;

    uint32_t room = wanted_ - written_;
    uint32_t take = uint32_t(frames);
    if (take > room) take = room;      // the take ends on its exact frame, mid-block if need be

    int16_t *out = buffer_ + 2u * std::size_t(written_);
    for (uint32_t i = 0; i < take; i++) {
        out[2u * i] = to_s16(master[2u * i]);
        out[2u * i + 1u] = to_s16(master[2u * i + 1u]);
    }
    written_ += take;

    if (written_ >= wanted_) state_ = State::done;
}

void Capture::release() {
    if (state_ != State::done) return;
    state_ = State::idle;
    wanted_ = 0;
    written_ = 0;
}

}  // namespace strata
