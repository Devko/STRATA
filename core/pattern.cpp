// Pattern initialisation, trig-condition packing and the shared lock pool.
//
// The pool is bump-allocated. A step's locks are a contiguous run at `lock_first`, so a step
// can only grow in place when its run happens to sit at the top of the pool; otherwise the run
// is copied to the top and the old slots are abandoned. That costs pool space rather than a
// shift of everything above the run, which is the trade this file makes on purpose: an edit
// touches at most 9 locks, while a shift would touch up to 1,024 and would have to renumber
// every step's `lock_first` behind the sequencer's back. The abandoned slots come back at
// `pattern_compact_locks`, which docs/SPEC.md schedules for save time.
//
// Every entry point validates the indices it is given and the run it finds, so a corrupt
// pattern -- one built by a caller rather than loaded, since serialize.cpp refuses corrupt
// files -- can waste pool space but cannot make this file read outside `Pattern::locks`.
#include "pattern.hpp"

namespace strata {

namespace {

bool indices_ok(int track, int step) {
    return track >= 0 && track < kTracks && step >= 0 && step < kStepsPerTrack;
}

// A run is usable when it has locks, is inside the allocated part of the pool, and is no
// longer than a step is allowed to be. Anything else is treated as "no locks".
bool run_ok(const Pattern &pattern, const Step &step) {
    if (step.lock_count == 0) return false;
    if (step.lock_count > kLocksPerStep) return false;
    if (step.lock_first == kNoLock) return false;
    return uint32_t(step.lock_first) + step.lock_count <= pattern.locks_used;
}

// The index of (a, b) in the order 1:2 2:2 1:3 2:3 3:3 1:4 .. 8:8. `b` rows start at
// (b - 1) * b / 2 - 1, which is 0 for b = 2 and 27 for b = 8.
uint8_t ab_row(uint8_t b) { return uint8_t((b - 1) * b / 2 - 1); }

uint8_t clamp_u8(uint8_t value, uint8_t low, uint8_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

}  // namespace

// --- trig conditions ------------------------------------------------------

uint8_t condition_pack(Condition kind) {
    if (kind == Condition::A_B) return condition_pack_a_b(1, kConditionBMin);
    const uint8_t raw = uint8_t(kind);
    return raw < kConditionKinds ? raw : uint8_t(Condition::NONE);
}

uint8_t condition_pack_a_b(uint8_t a, uint8_t b) {
    b = clamp_u8(b, kConditionBMin, kConditionBMax);
    a = clamp_u8(a, 1, b);
    return uint8_t(kConditionAbBase + ab_row(b) + (a - 1));
}

Condition condition_kind(uint8_t packed) {
    if (packed < kConditionKinds) return Condition(packed);
    if (packed >= kConditionAbBase && packed < kConditionAbBase + kConditionAbPairs) {
        return Condition::A_B;
    }
    return Condition::NONE;
}

uint8_t condition_a(uint8_t packed) {
    if (condition_kind(packed) != Condition::A_B) return 0;
    if (packed == uint8_t(Condition::A_B)) return 1;
    const uint8_t index = uint8_t(packed - kConditionAbBase);
    for (uint8_t b = kConditionBMax; b >= kConditionBMin; b--) {
        if (index >= ab_row(b)) return uint8_t(index - ab_row(b) + 1);
    }
    return 0;
}

uint8_t condition_b(uint8_t packed) {
    if (condition_kind(packed) != Condition::A_B) return 0;
    if (packed == uint8_t(Condition::A_B)) return kConditionBMin;
    const uint8_t index = uint8_t(packed - kConditionAbBase);
    for (uint8_t b = kConditionBMax; b >= kConditionBMin; b--) {
        if (index >= ab_row(b)) return b;
    }
    return 0;
}

bool condition_valid(uint8_t packed) {
    if (packed < kConditionKinds) return true;
    return packed >= kConditionAbBase && packed < kConditionAbBase + kConditionAbPairs;
}

// --- initialising ---------------------------------------------------------

void step_clear(Step &step) { step = Step{}; }

void track_init(TrackPattern &track) {
    track.length = uint8_t(kStepsPerTrack);
    track.speed = uint8_t(Speed::x1);
    track.swing = kSwingInherit;
    for (int i = 0; i < kStepsPerTrack; i++) step_clear(track.steps[i]);
}

void pattern_init(Pattern &pattern) {
    for (int t = 0; t < kTracks; t++) track_init(pattern.tracks[t]);
    for (int i = 0; i < kLockPool; i++) pattern.locks[i] = Lock{};
    pattern.locks_used = 0;
    pattern.master_length = uint8_t(kStepsPerTrack);
    pattern.swing = kSwingMin;
    pattern.root = 0;
    pattern.scale = 0;
    pattern.seed = 1;
}

void step_sanitize(Step &step) {
    step.flags = uint8_t(step.flags & kStepFlagMask);
    step.note = uint8_t(step.note & 0x7Fu);
    step.velocity = clamp_u8(step.velocity, 1, 127);
    if (step.length == 0) step.length = 1;
    if (step.micro > kMicroRange) step.micro = int8_t(kMicroRange);
    if (step.micro < -kMicroRange) step.micro = int8_t(-kMicroRange);
    if (step.probability > 100) step.probability = 100;
    if (!condition_valid(step.condition)) step.condition = uint8_t(Condition::NONE);

    const uint8_t hits = clamp_u8(ratchet_count(step.ratchet), kRatchetMin, kRatchetMax);
    const RatchetRamp ramp = uint8_t(ratchet_ramp(step.ratchet)) > uint8_t(RatchetRamp::down)
                                 ? RatchetRamp::none
                                 : ratchet_ramp(step.ratchet);
    step.ratchet = ratchet_pack(hits, ramp);

    if (step.lock_count > kLocksPerStep) {
        step.lock_count = 0;
        step.lock_first = kNoLock;
    }
    if (step.lock_count == 0) step.lock_first = kNoLock;
}

// --- locks ----------------------------------------------------------------

LockView pattern_step_locks(const Pattern &pattern, int track, int step) {
    if (!indices_ok(track, step)) return LockView{};
    const Step &s = pattern.tracks[track].steps[step];
    if (!run_ok(pattern, s)) return LockView{};
    return LockView{&pattern.locks[s.lock_first], s.lock_count};
}

bool pattern_set_lock(Pattern &pattern, int track, int step, uint8_t param, uint8_t value) {
    if (!indices_ok(track, step)) return false;
    Step &s = pattern.tracks[track].steps[step];

    // A run that does not fit the pool is not a run. Drop it rather than grow it.
    if (s.lock_count != 0 && !run_ok(pattern, s)) {
        s.lock_count = 0;
        s.lock_first = kNoLock;
    }

    // Already locked? Then this is an edit, not an allocation.
    for (uint8_t i = 0; i < s.lock_count; i++) {
        if (pattern.locks[s.lock_first + i].param == param) {
            pattern.locks[s.lock_first + i].value = value;
            return true;
        }
    }

    if (s.lock_count >= kLocksPerStep) return false;

    // The run is already at the top of the pool: append in place.
    if (s.lock_count != 0 && uint32_t(s.lock_first) + s.lock_count == pattern.locks_used) {
        if (pattern.locks_used >= kLockPool) return false;
        pattern.locks[pattern.locks_used] = Lock{param, value};
        pattern.locks_used++;
        s.lock_count++;
        return true;
    }

    // Otherwise move the run to the top and append there. The old slots are abandoned until
    // the next compaction, which is why a pattern edited for a long time can exhaust the pool
    // while `pattern_locks_reachable` is still small.
    const uint32_t need = uint32_t(s.lock_count) + 1;
    if (uint32_t(pattern.locks_used) + need > uint32_t(kLockPool)) return false;

    const uint16_t dst = pattern.locks_used;
    for (uint8_t i = 0; i < s.lock_count; i++) {
        pattern.locks[dst + i] = pattern.locks[s.lock_first + i];
    }
    pattern.locks[dst + s.lock_count] = Lock{param, value};
    pattern.locks_used = uint16_t(dst + need);
    s.lock_first = dst;
    s.lock_count = uint8_t(s.lock_count + 1);
    return true;
}

bool pattern_get_lock(const Pattern &pattern, int track, int step, uint8_t param,
                      uint8_t &value) {
    const LockView view = pattern_step_locks(pattern, track, step);
    for (const Lock &lock : view) {
        if (lock.param != param) continue;
        value = lock.value;
        return true;
    }
    return false;
}

bool pattern_clear_lock(Pattern &pattern, int track, int step, uint8_t param) {
    if (!indices_ok(track, step)) return false;
    Step &s = pattern.tracks[track].steps[step];
    if (!run_ok(pattern, s)) return false;

    for (uint8_t i = 0; i < s.lock_count; i++) {
        if (pattern.locks[s.lock_first + i].param != param) continue;
        for (uint8_t j = uint8_t(i + 1); j < s.lock_count; j++) {
            pattern.locks[s.lock_first + j - 1] = pattern.locks[s.lock_first + j];
        }
        s.lock_count--;
        // If the run ended at the top of the pool, give the slot straight back.
        if (uint32_t(s.lock_first) + s.lock_count + 1 == pattern.locks_used) {
            pattern.locks_used--;
        }
        if (s.lock_count == 0) s.lock_first = kNoLock;
        return true;
    }
    return false;
}

void pattern_clear_step_locks(Pattern &pattern, int track, int step) {
    if (!indices_ok(track, step)) return;
    Step &s = pattern.tracks[track].steps[step];
    if (run_ok(pattern, s) && uint32_t(s.lock_first) + s.lock_count == pattern.locks_used) {
        pattern.locks_used = s.lock_first;
    }
    s.lock_count = 0;
    s.lock_first = kNoLock;
}

uint16_t pattern_locks_reachable(const Pattern &pattern) {
    uint32_t total = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            const uint8_t count = pattern_step_locks(pattern, t, i).count;
            // Overlapping runs in a hand-built pattern could otherwise total more than the
            // pool holds. Stop counting rather than report a number nothing can store.
            if (total + count > uint32_t(kLockPool)) return uint16_t(total);
            total += count;
        }
    }
    return uint16_t(total);
}

uint16_t pattern_locks_free(const Pattern &pattern) {
    if (pattern.locks_used >= kLockPool) return 0;
    return uint16_t(kLockPool - pattern.locks_used);
}

uint16_t pattern_compact_locks(Pattern &pattern) {
    // 2 KiB of stack, well inside the 16 KiB frame limit, and this runs at save time in the
    // main context -- never in audio(). Copying through scratch rather than in place keeps the
    // move trivially correct: runs are bump-allocated and so appear in the pool in no
    // particular order, and an in-place pack walking steps in order would overwrite a run it
    // had not moved yet.
    Lock packed[kLockPool];
    uint16_t write = 0;

    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kStepsPerTrack; i++) {
            Step &s = pattern.tracks[t].steps[i];
            const LockView view = pattern_step_locks(pattern, t, i);
            if (view.count == 0 || uint32_t(write) + view.count > uint32_t(kLockPool)) {
                s.lock_count = 0;
                s.lock_first = kNoLock;
                continue;
            }
            const uint16_t first = write;
            for (uint8_t k = 0; k < view.count; k++) packed[write++] = view.locks[k];
            s.lock_first = first;
        }
    }

    for (uint16_t i = 0; i < write; i++) pattern.locks[i] = packed[i];
    for (int i = write; i < kLockPool; i++) pattern.locks[i] = Lock{};
    pattern.locks_used = write;
    return write;
}

}  // namespace strata
