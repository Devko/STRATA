// core/overview.hpp has the why; this is the arithmetic.
#include "overview.hpp"

namespace strata {

uint32_t OverviewBuilder::boundary(uint32_t column) const {
    // ceil(column * total / K) in 64 bits: total is up to 2^30 frames. Once per column.
    const uint64_t k = uint64_t(kWaveColumns);
    return uint32_t((uint64_t(column) * uint64_t(total_) + k - 1u) / k);
}

void OverviewBuilder::begin(WaveOverview &out, uint32_t total) {
    out = WaveOverview{};
    out_ = &out;
    total_ = total;
    next_ = 0;
    column_ = 0;
    column_end_ = boundary(1);
    lo_ = 32767;
    hi_ = -32768;
    open_ = false;
    broken_ = total == 0;
}

void OverviewBuilder::close_column() {
    if (column_ >= uint32_t(kWaveColumns)) return;
    if (open_) {
        out_->lo[column_] = int8_t(lo_ >> 8);
        out_->hi[column_] = int8_t(hi_ >> 8);
    } else {
        // A sample shorter than kWaveColumns frames leaves columns with no frame of their own:
        // they repeat the one before, so a short sample draws as a staircase, not a comb.
        out_->lo[column_] = column_ > 0 ? out_->lo[column_ - 1] : int8_t(0);
        out_->hi[column_] = column_ > 0 ? out_->hi[column_ - 1] : int8_t(0);
    }
    column_++;
    column_end_ = boundary(column_ + 1);
    lo_ = 32767;
    hi_ = -32768;
    open_ = false;
}

void OverviewBuilder::feed(const int16_t *pcm, uint32_t first, uint32_t frames, int channels) {
    if (broken_ || out_ == nullptr) return;
    if (first != next_ || (channels != 1 && channels != 2) || (frames > 0 && pcm == nullptr) ||
        frames > total_ - next_) {
        broken_ = true;
        return;
    }
    uint32_t i = 0;
    while (i < frames && column_ < uint32_t(kWaveColumns)) {
        const uint32_t at = next_ + i;
        const uint32_t room = column_end_ - at;
        const uint32_t n = room < frames - i ? room : frames - i;
        // The run up to the column's end, with no boundary test inside it.
        int32_t lo = lo_;
        int32_t hi = hi_;
        if (channels == 2) {
            const int16_t *p = pcm + 2u * i;
            for (uint32_t j = 0; j < n; j++) {
                const int32_t v = (int32_t(p[2u * j]) + int32_t(p[2u * j + 1u])) >> 1;
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        } else {
            const int16_t *p = pcm + i;
            for (uint32_t j = 0; j < n; j++) {
                const int32_t v = p[j];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        }
        lo_ = lo;
        hi_ = hi;
        open_ = open_ || n > 0;
        i += n;
        if (at + n == column_end_) close_column();
    }
    next_ += frames;
}

void OverviewBuilder::finish(uint32_t frames) {
    if (broken_ || out_ == nullptr || frames == 0 || frames != next_) return;
    if (open_) close_column();
    if (frames < total_) {
        // Shorter than begin() was told: column_ columns hold it, stretched over all of them so
        // they line up with a machine that maps `frames`. Back to front, which makes it safe in
        // place: column j reads columns at or before j, and only columns after j are written.
        const uint32_t n = column_;
        if (n == 0) return;
        for (int j = kWaveColumns - 1; j >= 0; j--) {
            const uint32_t a = uint32_t(j) * n / uint32_t(kWaveColumns);
            uint32_t b = uint32_t(j + 1) * n / uint32_t(kWaveColumns);
            if (b <= a) b = a + 1;
            int8_t lo = out_->lo[a];
            int8_t hi = out_->hi[a];
            for (uint32_t s = a + 1; s < b; s++) {
                lo = out_->lo[s] < lo ? out_->lo[s] : lo;
                hi = out_->hi[s] > hi ? out_->hi[s] : hi;
            }
            out_->lo[j] = lo;
            out_->hi[j] = hi;
        }
    } else {
        // Columns past the last frame (a sample shorter than kWaveColumns frames ends inside an
        // early column) repeat it.
        while (column_ < uint32_t(kWaveColumns)) close_column();
    }
    out_->filled = uint8_t(kWaveColumns);
    out_->ready = true;
    broken_ = true;                    // done: a stray feed changes nothing
}

void OverviewBuilder::finish_head() {
    if (broken_ || out_ == nullptr) return;
    if (open_) close_column();
    out_->filled = uint8_t(column_);
    out_->ready = column_ > 0;
    broken_ = true;
}

}  // namespace strata
