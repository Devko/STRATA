#include "lockstore.hpp"

namespace strata {

bool live_state_same(const LiveState &a, const LiveState &b) {
    if (__builtin_memcmp(&a.pattern, &b.pattern, sizeof a.pattern) != 0) return false;
    if (__builtin_memcmp(a.params, b.params, sizeof a.params) != 0) return false;
    for (int t = 0; t < kTracks; t++) {
        if (a.level[t] != b.level[t]) return false;
    }
    if (__builtin_memcmp(a.sends, b.sends, sizeof a.sends) != 0) return false;
    if (__builtin_memcmp(a.master, b.master, sizeof a.master) != 0) return false;
    if (__builtin_memcmp(a.macros, b.macros, sizeof a.macros) != 0) return false;
    return a.mutes == b.mutes && a.morph == b.morph;
}

void LockStore::attach(LiveState *memory) {
    memory_ = memory;
    clear();
}

void LockStore::clear() {
    for (bool &has : has_) has = false;
    back_for_ = -1;
}

void LockStore::pattern_changed() { back_for_ = -1; }

bool LockStore::has(int index) const {
    return memory_ != nullptr && index >= 0 && index < kProjectPatterns && has_[index];
}

void LockStore::stored(int index) {
    if (memory_ == nullptr || index < 0 || index >= kProjectPatterns) return;
    has_[index] = true;
    back_for_ = -1;
}

LockTap LockStore::tap(int index) {
    if (!has(index)) return LockTap::empty;
    if (back_for_ == index) {
        back_for_ = -1;
        return LockTap::swapped;
    }
    back_for_ = index;
    return LockTap::jumped;
}

}  // namespace strata
