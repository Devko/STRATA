// The sample memory budget: the free list behind core/samplepool.hpp.
//
// The region is a partition at all times: every byte of `budget_` is either inside exactly one
// live slot's range (`loading` or `ready`) or inside exactly one hole. `used()` is summed from
// the slots and `largest_free()` scanned from the holes, so the two sides of that partition are
// computed independently -- if they ever disagree, the pool is handing out memory twice, which
// on a unit sounds like a bug in the voice rather than like a bug here.
//
// Three things keep the partition true:
//
//   - `holes_` is kept sorted by offset with no two entries adjacent, which is what makes
//     "first fit" mean "the lowest address that fits". `coalesce()` is the only place that
//     invariant is established and it is idempotent, so every mutation can just call it.
//   - `add_hole` *merges* instead of inserting whenever the returned range touches a hole.
//     Inserting first and coalescing afterwards would need a tenth entry transiently (eight
//     live ranges already make up to nine holes), and kMaxHoles is nine.
//   - Nothing but `release` and `fail` ever gives bytes back, and both of them clear the slot
//     in the same breath, so a range cannot be live and free at once.
//
// Arithmetic: `init` and `set_budget` clamp the budget to 64 MiB, so every offset and length
// here is below 2^26 and `offset + bytes` cannot wrap a uint32_t. That clamp is the overflow
// guard -- a damaged project byte asking for 4 GiB would otherwise produce a hole whose end
// wraps to a low address, and the next first fit would hand out a range overlapping a loaded
// sample. There is no division: the one place a byte count becomes a frame count shifts.
#include "samplepool.hpp"

namespace strata {

namespace {

constexpr uint32_t kBytesPerMib = 1024u * 1024u;
constexpr uint32_t kBudgetCeiling = kSampleBudgetMaxMib * kBytesPerMib;

bool slot_ok(int slot) { return slot >= 0 && slot < kTracks; }

// The two states in which a slot owns bytes. `failed` deliberately owns none: `fail` hands the
// range back, because a track that will never play must not hold 8 MiB hostage until the user
// notices the FAIL on the screen.
bool live(SampleState state) {
    return state == SampleState::loading || state == SampleState::ready;
}

// What an out-of-range `slot()` reads. A caller with a bad index gets "no sample" rather than
// whatever lies past the array.
const SampleSlot kNoSlot{};

uint32_t clamp_budget(uint32_t bytes) { return bytes > kBudgetCeiling ? kBudgetCeiling : bytes; }

}  // namespace

const char *sample_state_name(SampleState state) {
    switch (state) {
        case SampleState::loading: return "LOAD";
        case SampleState::ready: return "OK";
        case SampleState::failed: return "FAIL";
        case SampleState::empty:
        case SampleState::count:
            break;
    }
    return "";
}

// --- budget ---------------------------------------------------------------

void SamplePool::init(uint32_t bytes) {
    budget_ = clamp_budget(bytes);
    reset();
}

bool SamplePool::set_budget(uint32_t bytes) {
    const uint32_t want = clamp_budget(bytes);
    if (want == budget_) return true;

    if (want < budget_) {
        // Shrinking is bounded by the top of the highest live range, not by `used()`. Nothing
        // here may move an allocation, so one 1 MiB sample sitting at offset 31 MiB pins the
        // budget at 32 MiB however little is used in total. Refusing is the only honest answer:
        // the alternative is a loaded sample that lives outside the region.
        uint32_t high_water = 0;
        for (const SampleSlot &s : slots_) {
            if (!live(s.state)) continue;
            const uint32_t end = s.offset + s.bytes;
            if (end > high_water) high_water = end;
        }
        if (want < high_water) return false;

        // Everything above `want` is free by that check, so the holes only need trimming to the
        // new end. coalesce() drops the entries this empties.
        budget_ = want;
        for (int i = 0; i < hole_count_; i++) {
            if (holes_[i].offset >= budget_) {
                holes_[i].bytes = 0;
                continue;
            }
            const uint32_t room = budget_ - holes_[i].offset;
            if (holes_[i].bytes > room) holes_[i].bytes = room;
        }
        coalesce();
        return true;
    }

    // Raising it: the new tail is free space, merged with whatever hole already ended at the
    // old budget so that raising 32 MiB to 64 MiB on an empty pool gives one 64 MiB hole and
    // not two that nothing can use together.
    add_hole(budget_, want - budget_);
    budget_ = want;
    return true;
}

void SamplePool::reset() {
    for (SampleSlot &s : slots_) s = SampleSlot{};
    for (Hole &h : holes_) h = Hole{};
    hole_count_ = 0;
    if (budget_ != 0) {
        holes_[0] = Hole{0, budget_};
        hole_count_ = 1;
    }
}

// --- slots ----------------------------------------------------------------

bool SamplePool::reserve(int slot, uint32_t bytes) {
    if (!slot_ok(slot)) return false;

    // The slot's own range goes back first. That is the whole point of a free list here: with a
    // bump pointer, auditioning a second sample on one track would raise the high-water mark
    // instead of reusing the first one's space, and 32 MiB would be gone after a handful of
    // auditions. The cost is that a *refused* reserve leaves a slot that had a sample `empty`
    // rather than holding its old range -- which is what the header promises, and why app/
    // checks `largest_free()` before it starts a load it cannot land.
    release(slot);

    // A zero-length range would be a `ready` slot no voice can read, and it would also be a
    // range that overlaps its neighbour under any definition of overlap that counts touching
    // ends. Refusing keeps every live range non-empty.
    if (bytes == 0) return false;

    // Nothing larger than the whole region can ever fit, and checking it here means the loop
    // below compares sizes only -- no `offset + bytes` on a length the caller invented.
    if (bytes > budget_) return false;

    for (int i = 0; i < hole_count_; i++) {
        if (holes_[i].bytes < bytes) continue;

        // `release` above has already reset the slot to its defaults, which is what keeps a
        // `loading` slot from describing the sample that was there before: a voice reading the
        // old frame count against the new bytes is the glitch docs/SPEC.md says a loading track
        // must not make. `finish` is the only thing that fills the description in.
        SampleSlot &s = slots_[slot];
        s.state = SampleState::loading;
        s.offset = holes_[i].offset;      // the bottom of the hole: first fit is by address
        s.bytes = bytes;

        holes_[i].offset += bytes;
        holes_[i].bytes -= bytes;
        coalesce();                       // drops the hole if the fit was exact
        return true;
    }
    return false;
}

bool SamplePool::finish(int slot, uint32_t frames, int channels, uint32_t rate) {
    if (!slot_ok(slot)) return false;
    SampleSlot &s = slots_[slot];
    if (s.state != SampleState::loading) return false;

    const uint8_t ch = channels >= 2 ? 2u : 1u;

    // How many int16 frames the range can actually hold: 2 bytes per channel. A shift rather
    // than a divide because a 32-bit division is a helper call on the C674x, but the clamp is
    // what matters -- a loader reporting more frames than it reserved would send voices reading
    // straight into the next track's sample, which is the exact failure this file exists to
    // prevent. Fewer frames than reserved is normal and expected (a truncated file).
    const uint32_t capacity = s.bytes >> (ch == 2 ? 2 : 1);
    s.frames = frames > capacity ? capacity : frames;
    s.channels = ch;
    s.rate = rate != 0 ? rate : 48000;    // a zero rate is a voice stepping nowhere
    s.state = SampleState::ready;
    return true;
}

void SamplePool::fail(int slot) {
    if (!slot_ok(slot)) return;
    release(slot);                        // the bytes come back; only the state is kept
    slots_[slot].state = SampleState::failed;
}

void SamplePool::release(int slot) {
    if (!slot_ok(slot)) return;
    SampleSlot &s = slots_[slot];
    if (live(s.state) && s.bytes != 0) add_hole(s.offset, s.bytes);
    s = SampleSlot{};
}

const SampleSlot &SamplePool::slot(int index) const {
    return slot_ok(index) ? slots_[index] : kNoSlot;
}

// --- reporting ------------------------------------------------------------

uint32_t SamplePool::used() const {
    // Summed from the slots on every call, not tracked in a counter. Eight additions at control
    // rate cost nothing, and a counter is one more thing that can drift away from the truth.
    uint32_t total = 0;
    for (const SampleSlot &s : slots_) {
        if (live(s.state)) total += s.bytes;
    }
    return total;
}

uint32_t SamplePool::largest_free() const {
    uint32_t best = 0;
    for (int i = 0; i < hole_count_; i++) {
        if (holes_[i].bytes > best) best = holes_[i].bytes;
    }
    return best;
}

int SamplePool::count_in(SampleState state) const {
    int n = 0;
    for (const SampleSlot &s : slots_) {
        if (s.state == state) n++;
    }
    return n;
}

// --- free list ------------------------------------------------------------

void SamplePool::add_hole(uint32_t offset, uint32_t bytes) {
    if (bytes == 0) return;

    // Merge in place where the range touches an existing hole, and only insert when it touches
    // none. The order matters for the array bound, not for the result: eight live ranges can
    // already make nine holes, so inserting unconditionally and coalescing afterwards would
    // need a tenth entry for an instant.
    for (int i = 0; i < hole_count_; i++) {
        if (holes_[i].offset == offset + bytes) {            // the range sits just below hole i
            holes_[i].offset = offset;
            holes_[i].bytes += bytes;
            coalesce();                                      // it may now reach the hole below
            return;
        }
        if (holes_[i].offset + holes_[i].bytes == offset) {   // and just above it
            holes_[i].bytes += bytes;
            coalesce();
            return;
        }
    }

    // Unreachable, and here is the proof rather than a hope. A range that touches no hole has a
    // live range or the region edge on both sides, so either three live ranges are contiguous
    // -- leaving five of the eight to make at most six runs, hence at most seven holes -- or a
    // run of two or more sits against an edge, which removes one of the holes the run count
    // allows, again seven. Eight is the peak and kMaxHoles is nine. Dropping the range if it
    // ever did happen loses capacity, which the caller sees through `largest_free()` and
    // survives; writing past `holes_` would corrupt a slot's offset and hand two tracks the
    // same memory.
    if (hole_count_ >= kMaxHoles) return;
    holes_[hole_count_] = Hole{offset, bytes};
    hole_count_++;
    coalesce();
}

void SamplePool::coalesce() {
    // Sorted by offset, because `reserve` reads first fit as "lowest address that fits": a pool
    // released in a strange order then hands out the same ranges as one filled from scratch,
    // which is what keeps fragmentation bounded over a long session of auditioning. Insertion
    // sort over at most nine entries, at control rate.
    for (int i = 1; i < hole_count_; i++) {
        const Hole h = holes_[i];
        int j = i - 1;
        for (; j >= 0 && holes_[j].offset > h.offset; j--) holes_[j + 1] = holes_[j];
        holes_[j + 1] = h;
    }

    int write = 0;
    for (int i = 0; i < hole_count_; i++) {
        if (holes_[i].bytes == 0) continue;     // set_budget's trim leaves these behind
        const bool adjacent = write > 0 &&
            holes_[write - 1].offset + holes_[write - 1].bytes == holes_[i].offset;
        if (adjacent) {
            holes_[write - 1].bytes += holes_[i].bytes;
            continue;
        }
        holes_[write] = holes_[i];
        write++;
    }
    for (int i = write; i < kMaxHoles; i++) holes_[i] = Hole{};
    hole_count_ = write;
}

}  // namespace strata
