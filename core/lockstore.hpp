// LOCK: one stored moment per pattern, a jump back to it, and a second tap that swaps back.
//
// Backlog D3, D4, D13, D14 (Roland, 2026-11-25). Hold FUNC + MUTATE stores a lock for the
// playing pattern; a tap jumps to it at once; a second tap swaps back to the state from just
// before the jump, so a lock is also an A/B compare.
//
// ## What a lock holds
//
// `LiveState`: the playing pattern whole (steps, p-locks, conditions, lengths, swing), every
// track's sound bytes and fader, the send and master bytes, the mutes, the morph position and
// the four macro positions, REL ALL and LEN ALL (core/moveall.hpp). Not the tempo (STRATA may
// not own it under external sync), not the samples, the machines or the chain, and not the
// performance mixer: a jump rides the picked effect to rest instead (app/), and a loop keeps
// playing.
//
// ## Why the swap-back exists (R1)
//
// Undo (core/undo.hpp) covers the pattern only; the sound is not undoable. So "a jump is one
// undo level" would bring the steps back and leave the sound where the jump put it. The second
// tap restores the state captured at the jump, sound and all.
//
// ## Kit bytes are shared (R2)
//
// The sound is one kit for every pattern, so a lock taken on A01 and jumped to puts A01's
// sound back for every pattern. Accepted and documented (docs/MANUAL.md): the steps are per
// pattern, the sound comes from whichever lock you jumped to.
//
// ## Where this runs
//
// Audio context (app/engine.cpp), like undo: the live pattern is the audio side's, and the copies
// happen at a block boundary. RAM only, never saved; a project load or NEW clears every lock.
// The store is about 1.06 MiB, so it is allocated once in Engine::init and never a local.
//
// This file decides; the engine copies. `tap` says whether to jump or swap back and keeps the
// bookkeeping, and the engine moves state between the live project and `slot()` / `back()`.
#pragma once

#include "pattern.hpp"
#include "perform.hpp"     // kMacros
#include "project.hpp"     // kProjectPatterns, kKitParams, kSendFields, kMasterFields

#include <cstdint>

namespace strata {

// One moment of the live set, as bytes. Copied with memcpy and compared with memcmp (see
// live_state_same), so padding inside `pattern` travels with it and two copies of the same
// state compare equal.
struct LiveState {
    Pattern pattern;
    uint8_t params[kTracks][kKitParams];
    float level[kTracks];
    uint8_t sends[kSendFields];
    uint8_t master[kMasterFields];
    uint8_t mutes;                 // bit t = track t muted
    uint8_t macros[kMacros];
    uint16_t morph;
    int8_t rel_all;                // REL ALL and LEN ALL (core/moveall.hpp), 0 at rest
    int8_t len_all;
};

// Byte for byte, except the faders, which are compared as floats (a fader is never NaN: the
// engine clamps it). For the "changed since the lock" mark.
bool live_state_same(const LiveState &a, const LiveState &b);

// What a tap did.
enum class LockTap : uint8_t {
    empty,       // this pattern has no lock: nothing happened
    jumped,      // capture the live state into back(), then make slot(index) live
    swapped,     // make back() live: the state from before the jump
};

// What the engine reports after a store, beside LockTap's three (app/commands.hpp, lock_last).
constexpr uint8_t kLockStored = 3;
// app/commands.hpp's Status::lock_state bits.
constexpr uint8_t kLockHas = 1;          // the playing pattern has a lock
constexpr uint8_t kLockChanged = 2;      // and the live set differs from it
constexpr uint8_t kLockSwapReady = 4;    // the next tap swaps back

class LockStore {
public:
    // `memory` holds kProjectPatterns + 1 states (the locks, then the swap-back), from the
    // caller's init(). Null leaves the store unusable: has() is false and tap() is `empty`.
    void attach(LiveState *memory);
    static constexpr int kStates = kProjectPatterns + 1;

    // Forget every lock and the swap-back: a project load or NEW.
    void clear();

    // The playing pattern changed: the swap-back belonged to the moment of a jump on the
    // pattern just left, and swapping back onto another pattern would overwrite its steps.
    void pattern_changed();

    bool has(int index) const;
    // A swap-back is waiting for this pattern: the next tap swaps back.
    bool jumped(int index) const { return back_for_ == index && index >= 0; }

    // Where a store copies the live state to; then `stored(index)`.
    LiveState &slot(int index) { return memory_[index]; }
    const LiveState &slot(int index) const { return memory_[index]; }
    LiveState &back() { return memory_[kProjectPatterns]; }

    // A lock was just written into slot(index). A store also ends a pending swap-back: the new
    // lock is the state you are in, so there is nothing to swap back to.
    void stored(int index);

    // Decide a tap, and keep the bookkeeping: `jumped` sets the swap-back to this pattern,
    // `swapped` clears it. The caller does the copies named on LockTap.
    LockTap tap(int index);

private:
    LiveState *memory_ = nullptr;
    bool has_[kProjectPatterns] = {};
    int back_for_ = -1;
};

}  // namespace strata
