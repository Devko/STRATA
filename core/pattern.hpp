// The pattern data model: one pattern of 8 tracks x 64 steps, with per-step parameter locks
// drawn from a pool shared by the whole pattern.
//
// The layout is the sketch in docs/SPEC.md, "Data model and storage", field for field. The
// field *order* is load-bearing, not cosmetic: eight single bytes come first so that
// `uint16_t lock_first` lands on offset 8 and stays naturally aligned. A misaligned 16- or
// 32-bit access on the C674x does not fault, it silently reads the wrong address, so a
// reordering that looks harmless here would surface as corrupted steps on the unit only.
// The static_asserts at the end of this header are the tripwire.
//
// Everything a lock index means is defined here as well, because two readers have to agree on
// it: the sequencer walking a step's locks in audio context, and serialize.cpp writing them to
// a file. The pool is bump-allocated -- growing a step's run that is not at the top of the
// pool copies the run to the top and abandons the old slots -- so the pool holds garbage until
// `pattern_compact_locks` runs. docs/SPEC.md says the pool is "compacted on save"; the writer
// in serialize.cpp emits the compacted form without touching the pattern in RAM, so a save
// from the main context never writes pattern memory the audio side owns.
//
// core/ is portable C++20 with no Enaga headers, no allocation and no exceptions. Every
// function here takes storage from its caller, is bounds-checked against the indices it is
// given, and reports failure by return value.
#pragma once

#include <cstddef>
#include <cstdint>

// For `Speed`, `kSwingMin`, `kSwingMax`, `kMicroRange` and `kMaxSteps`. TrackPattern stores a
// speed and a swing amount, core/timing.hpp decides what they mean on the tick grid, and the
// numbers are serialized -- so there has to be one definition of them and not two. It lives
// there rather than here because that is where it is reasoned about; this header only has to
// agree with it, which the static_assert below checks.
#include "timing.hpp"

namespace strata {

// --- sizes ----------------------------------------------------------------

constexpr int kTracks = 8;
constexpr int kStepsPerTrack = 64;   // 4 pages of 16
constexpr int kLockPool = 1024;      // locks shared by the whole pattern
constexpr int kLocksPerStep = 8;

// Step::lock_first when a step has no locks.
constexpr uint16_t kNoLock = 0xFFFFu;

// --- step flags -----------------------------------------------------------

// A trig plays a note; a trigless step applies its locks without one (docs/SPEC.md, "Step
// data"). Both bits clear means the step is off. Slide is MONO tracks only.
enum StepFlag : uint8_t {
    kStepTrig = 1u << 0,
    kStepTrigless = 1u << 1,
    kStepAccent = 1u << 2,
    kStepSlide = 1u << 3,
};
constexpr uint8_t kStepFlagMask = kStepTrig | kStepTrigless | kStepAccent | kStepSlide;

// --- trig conditions ------------------------------------------------------

// The kinds of trig condition from docs/SPEC.md, "Trig conditions".
//
// These numbers go on disk inside Step::condition. **Never renumber them**: a pattern saved by
// an older build would come back with every condition shifted, which is silent and musical
// rather than loud and obvious. Add new kinds after NOT_FIRST instead.
enum class Condition : uint8_t {
    NONE = 0,
    A_B = 1,         // fires on loop A of every B, B = 2-8; see the packing note below
    FILL = 2,        // follows the fill key
    NOT_FILL = 3,
    PRE = 4,         // follows the last condition result on this track
    NOT_PRE = 5,
    NEI = 6,         // follows the track to the left
    NOT_NEI = 7,
    FIRST = 8,       // first loop after a pattern change
    NOT_FIRST = 9,
};
constexpr int kConditionKinds = 10;

// `A:B` needs two more numbers than the other kinds, and the spec's 12-byte Step has no byte
// to spare for them, so Step::condition is a packed byte rather than a bare enum value:
//
//     0 .. 9     the Condition value itself. 1 (A_B) on its own reads back as 1:2.
//     16 .. 50   an A:B pair: kConditionAbBase + the index of (A, B) in the order
//                1:2 2:2 1:3 2:3 3:3 1:4 .. 8:8 -- every A in 1..B for each B in 2..8.
//     anything else   invalid; it reads back as NONE.
//
// Use the helpers below rather than the numbers. `condition_pack(Condition::A_B)` produces
// 1:2, so a writer never emits the bare 1; readers accept it because a hand-made file might.
constexpr uint8_t kConditionAbBase = 16;
constexpr uint8_t kConditionAbPairs = 35;
constexpr uint8_t kConditionBMin = 2;
constexpr uint8_t kConditionBMax = 8;

// The byte for a condition that carries no A:B. A_B yields 1:2.
uint8_t condition_pack(Condition kind);

// The byte for `a:b`, clamped into b = 2..8 and a = 1..b.
uint8_t condition_pack_a_b(uint8_t a, uint8_t b);

// NONE for any byte that is not a condition this build knows.
Condition condition_kind(uint8_t packed);

// A and B of an A:B condition; both 0 for every other kind.
uint8_t condition_a(uint8_t packed);
uint8_t condition_b(uint8_t packed);

bool condition_valid(uint8_t packed);

// --- ratchets -------------------------------------------------------------

// Step::ratchet packs the hit count in the low nibble and the velocity ramp in the high one,
// as docs/SPEC.md specifies. Count 0 is not a thing: one hit is the unratcheted step.
enum class RatchetRamp : uint8_t { none = 0, up = 1, down = 2 };

constexpr uint8_t kRatchetMin = 1;
constexpr uint8_t kRatchetMax = 8;

constexpr uint8_t ratchet_pack(uint8_t count, RatchetRamp ramp) {
    return uint8_t((count & 0x0Fu) | uint8_t(uint8_t(ramp) << 4));
}
constexpr uint8_t ratchet_count(uint8_t packed) { return uint8_t(packed & 0x0Fu); }
constexpr RatchetRamp ratchet_ramp(uint8_t packed) { return RatchetRamp(packed >> 4); }

// --- swing ----------------------------------------------------------------

// Swing is a percentage and core/timing.hpp bounds it at 50 (straight) to 75. A track's own
// value may override the pattern's (docs/SPEC.md, "STRATA's own timing work"); 0 is outside
// that range, which makes it the natural "follow the pattern" sentinel.
constexpr uint8_t kSwingInherit = 0;

// A track's swing moved by `delta` encoder clicks along the one line the PATTERN page shows:
// PAT (follow the pattern), then 50 to 75. One click below 50 is PAT, and nothing is below PAT,
// so a knob turned all the way down always lands on "follow the pattern" rather than on 50 --
// which would look the same and stop following. A damaged byte between 1 and 49 counts as PAT.
constexpr uint8_t track_swing_step(uint8_t current, int delta) {
    const int at = current < kSwingMin ? kSwingMin - 1 : (current > kSwingMax ? kSwingMax : current);
    const int next = at + delta;
    if (next < kSwingMin) return kSwingInherit;
    return uint8_t(next > kSwingMax ? kSwingMax : next);
}

// --- the structures -------------------------------------------------------

// Exactly 12 bytes. Do not reorder: see the header comment.
struct Step {
    uint8_t flags = 0;                  // StepFlag bits
    uint8_t note = 60;                  // 0-127
    uint8_t velocity = 100;             // 1-127
    // Index into core/timing.hpp's note-length table. 8 is "1 step", NOT 1.
    //
    // 1 is the table's shortest entry -- a SIXTEENTH OF A STEP, about 7 ms at 130 BPM -- and it
    // was this field's default for as long as nothing read it. `Trig::gate_frames` reads it now,
    // so the default decides how long an untouched note sounds, and the shortest entry in the
    // table is a blip rather than a note: measured, it cut 2.4 points off the emulator's load
    // on the default beat because the stab was released almost as soon as it started.
    //
    // There is no "no gate" value to default to instead: docs/SPEC.md reserves index 0 as
    // invalid on purpose, so that a zeroed or corrupt byte is refusable rather than meaning the
    // shortest note, and `step_sanitize` clamps to 1. One step is what a sequencer means by an
    // ungated note, and it matches `StepEvent::length_index`, which has defaulted to 8 since M3.
    uint8_t length = 8;                 // index into the note-length table: 8 = one step
    int8_t micro = 0;                   // -11..+11 ticks of the 24 in a 16th
    uint8_t probability = 100;          // 0..100 %
    uint8_t condition = 0;              // packed; see condition_pack
    uint8_t ratchet = 1;                // count 1-8 low nibble, RatchetRamp high nibble
    uint16_t lock_first = kNoLock;      // index into Pattern::locks
    uint8_t lock_count = 0;             // 0..8
    uint8_t chord = 0;                  // POLY: type and inversion
};

// One parameter override. `param` is a track parameter index (dsp/track.hpp has 16 of them,
// all 0-127, which is why a lock fits in two bytes). Deliberately not range-checked here:
// core/ does not depend on dsp/, and an unknown param id should survive a round trip rather
// than be thrown away.
struct Lock {
    uint8_t param = 0;
    uint8_t value = 0;
};

struct TrackPattern {
    uint8_t length = uint8_t(kStepsPerTrack);   // 1-64 steps (polymeter)
    uint8_t speed = uint8_t(Speed::x1);
    uint8_t swing = kSwingInherit;
    Step steps[kStepsPerTrack];
};

struct Pattern {
    TrackPattern tracks[kTracks];
    Lock locks[kLockPool];                      // shared pool, compacted on save
    uint16_t locks_used = 0;                    // bump pointer, not the reachable count
    uint8_t master_length = uint8_t(kStepsPerTrack);
    uint8_t swing = uint8_t(kSwingMin);
    uint8_t root = 0;                           // 0-11
    uint8_t scale = 0;
    uint32_t seed = 1;                          // probability and mutation; 0 is a bad seed
};

// --- layout guarantees ----------------------------------------------------

static_assert(sizeof(Step) == 12, "Step must stay 12 bytes: the memory budget in docs/SPEC.md "
                                  "and every file it has ever written depend on it");
static_assert(sizeof(Lock) == 2, "a Lock is a param/value pair and nothing else");
static_assert(alignof(Step) == 2, "Step is 8 bytes, a uint16_t and 2 bytes: alignment 2");
static_assert(offsetof(Step, lock_first) == 8,
              "lock_first must sit on an even offset or the C674x reads the wrong address");
static_assert(offsetof(Step, lock_first) % sizeof(uint16_t) == 0, "lock_first is misaligned");
static_assert(sizeof(TrackPattern) == 4 + 12 * kStepsPerTrack, "TrackPattern grew padding");

// The data model and the tick mapping must agree about how long a track can be and how far a
// step may be nudged, or a step would be stored that the scheduler can never fire.
static_assert(kStepsPerTrack == kMaxSteps, "core/timing.hpp disagrees about the step count");
static_assert(kMicroRange == 11, "micro-timing is +/-11 of the 24 ticks in a 16th");

// --- initialising ---------------------------------------------------------

void step_clear(Step &step);
void track_init(TrackPattern &track);

// An empty pattern: no trigs, no locks, every track 64 steps at 1x.
void pattern_init(Pattern &pattern);

// Forces every field of `step` into the range the sequencer is allowed to assume. Used on load
// against corruption that survived the CRC -- a hand-edited file, say -- so that a bad byte
// costs one odd-sounding step rather than an out-of-range table read.
void step_sanitize(Step &step);

// --- locks ----------------------------------------------------------------

// A step's locks, in the pool. `locks` is null and `count` 0 when the step has none, and also
// when its run does not fit inside the pool -- callers never see a run they must not read.
struct LockView {
    const Lock *locks = nullptr;
    uint8_t count = 0;

    const Lock *begin() const { return locks; }
    const Lock *end() const { return locks + count; }
};

LockView pattern_step_locks(const Pattern &pattern, int track, int step);

// Sets `param` on a step: updates the existing lock for that parameter if there is one, and
// otherwise adds a lock. False, with the pattern unchanged, when the step already holds
// kLocksPerStep locks or the pool has no room. Never allocates and never overflows.
bool pattern_set_lock(Pattern &pattern, int track, int step, uint8_t param, uint8_t value);

// Reads one parameter's lock. False if the step has no lock for it; `value` is untouched then.
bool pattern_get_lock(const Pattern &pattern, int track, int step, uint8_t param,
                      uint8_t &value);

// Removes one parameter's lock. False if the step did not have one.
bool pattern_clear_lock(Pattern &pattern, int track, int step, uint8_t param);

// Removes all of a step's locks.
void pattern_clear_step_locks(Pattern &pattern, int track, int step);

// How many locks are reachable from a step -- what a save writes, as against `locks_used`,
// which also counts the slots abandoned by relocation.
uint16_t pattern_locks_reachable(const Pattern &pattern);

// Slots left before the pool is full. Zero means the next `pattern_set_lock` that has to
// allocate will fail; compacting first may free slots.
uint16_t pattern_locks_free(const Pattern &pattern);

// Packs every step's locks back down to a contiguous run at the bottom of the pool, in track
// then step order, dropping the slots relocation abandoned. Every step keeps exactly the locks
// it had, in order. Returns the new `locks_used`.
uint16_t pattern_compact_locks(Pattern &pattern);

}  // namespace strata
