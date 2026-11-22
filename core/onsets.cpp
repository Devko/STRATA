// core/onsets.hpp has the method and why.
#include "onsets.hpp"

namespace strata {

void OnsetFinder::reset(int channels) {
    *this = OnsetFinder{};
    channels_ = channels == 2 ? 2 : 1;
}

void OnsetFinder::feed(const int16_t *pcm, uint32_t first, uint32_t frames, int channels) {
    if (first == 0) reset(channels);
    if (broken_) return;
    if (pcm == nullptr || first != next_ || (channels == 2 ? 2 : 1) != channels_) {
        broken_ = true;
        return;
    }
    const uint32_t samples_per_frame = uint32_t(channels_);
    for (uint32_t f = 0; f < frames; f++) {
        const int16_t *frame = pcm + f * samples_per_frame;
        for (uint32_t c = 0; c < samples_per_frame; c++) {
            const int32_t v = frame[c];
            energy_ += uint32_t(v < 0 ? -v : v);
        }
        if (++in_hop_ == kHop) end_hop();
    }
    next_ += frames;
}

void OnsetFinder::end_hop() {
    // Twice the recent mean, without the divide: energy * kHistory against kRise * the sum.
    const uint32_t scaled = energy_ * uint32_t(kHistory);
    const uint32_t bar = kRise * history_sum_;
    const uint32_t floor = kFloorPerFrame * kHop * uint32_t(channels_);
    const bool hit = energy_ >= floor && scaled >= bar;
    if (hit) {
        const uint32_t strength = scaled - history_sum_;     // above the mean, times kHistory
        if (run_open_ && hop_ - run_start_hop_ < kMinGapHops) {
            if (strength > run_strength_) run_strength_ = strength;
        } else {
            close_run();
            run_open_ = true;
            run_start_hop_ = hop_;
            run_strength_ = strength;
        }
    }

    history_sum_ -= history_[history_at_];
    history_[history_at_] = energy_;
    history_sum_ += energy_;
    history_at_ = (history_at_ + 1) & (kHistory - 1);

    energy_ = 0;
    in_hop_ = 0;
    hop_++;
}

void OnsetFinder::close_run() {
    if (!run_open_) return;
    run_open_ = false;
    keep(run_start_hop_ * kHop, run_strength_);
}

void OnsetFinder::keep(uint32_t at, uint32_t strength) {
    if (at == 0) return;                       // a cut at the start is a slice of nothing
    if (kept_ < kPointsMax) {
        kept_at_[kept_] = at;
        kept_strength_[kept_] = strength;
        kept_++;
        return;
    }
    // Full: the weakest goes, if this one is stronger. On a tie the earlier one stays.
    int weakest = 0;
    for (int i = 1; i < kept_; i++) {
        if (kept_strength_[i] < kept_strength_[weakest]) weakest = i;
    }
    if (strength <= kept_strength_[weakest]) return;
    kept_at_[weakest] = at;
    kept_strength_[weakest] = strength;
}

void OnsetFinder::finish(uint32_t total, SlicePoints &out) {
    out = SlicePoints{};
    if (broken_ || total != next_) return;
    close_run();

    // By place, ascending. Fifteen entries: an insertion sort.
    for (int i = 1; i < kept_; i++) {
        const uint32_t at = kept_at_[i];
        const uint32_t strength = kept_strength_[i];
        int j = i - 1;
        while (j >= 0 && kept_at_[j] > at) {
            kept_at_[j + 1] = kept_at_[j];
            kept_strength_[j + 1] = kept_strength_[j];
            j--;
        }
        kept_at_[j + 1] = at;
        kept_strength_[j + 1] = strength;
    }
    // A point's rank is how many are stronger; on a tie the earlier one ranks first, so every
    // rank is used exactly once.
    for (int i = 0; i < kept_; i++) {
        int rank = 0;
        for (int j = 0; j < kept_; j++) {
            const bool stronger = kept_strength_[j] > kept_strength_[i] ||
                                  (kept_strength_[j] == kept_strength_[i] && j < i);
            rank += stronger ? 1 : 0;
        }
        out.at[i] = kept_at_[i];
        out.rank[i] = uint8_t(rank);
    }
    out.count = uint8_t(kept_);
}

}  // namespace strata
