// Resolving arriving clock ticks into note events.
//
// core/timing decides WHICH steps land on a tick; this file decides whether a step that landed
// sounds, and with what. The order is fixed by scheduler.hpp -- trig state, condition,
// probability, ratchet -- and the whole thing is a pure function of (pattern, tick, perform
// state, chaining state): no RNG, no clock, no allocation.
//
// ## Decisions this file makes where the spec was silent
//
// - **Track order within a tick is ascending, 0 to 7.** `NEI` therefore reads the neighbour's
//   result from THIS tick when the neighbour also has a step landing on it, and its most recent
//   earlier result otherwise. One consistent rule; the alternative (a snapshot of the previous
//   tick) would make a NEI chain lag by a tick on every step that the two tracks share, which
//   is most of them.
// - **Track 0 has no left neighbour, so its neighbour result is false.** `NEI` on track 0 never
//   fires and `NOT NEI` on track 0 always fires. No wrap-around to track 7: that would couple
//   the two ends of the kit invisibly, and would also have to read track 7 from the previous
//   tick, breaking the rule above. False rather than true because silence is a discoverable
//   mistake and a step that always fires is not.
// - **Trigless steps run the whole pipeline.** A trigless step's condition and probability are
//   resolved exactly like a trig's, so locks can be made conditional ("sweep the filter on the
//   fill") -- that is the point of a trigless step. It then emits exactly ONE event with
//   `trigless = true`, ignoring its ratchet count: there is no note to ratchet.
// - **Both trig bits set is a corrupt step; the trig wins.** The UI cycles off -> on ->
//   trigless, so the state cannot be entered by hand. Playing the note is the louder, more
//   discoverable of the two readings.
// - **`1ST` is per track.** A track is in its first pass until it leaves the loop pass it was in
//   when `reset()` ran. With polymeter a 12-step track and a 16-step track do not change pass
//   together, and a global flag would have to pick one of them; per track is the only reading
//   that generalises. `reset(false)` means no pass is the first, so `1ST` never passes.
// - **A step that is off resolves nothing.** Its condition is not evaluated and does not enter
//   the PRE/NEI chain, because stage 1 of the order of resolution stops there. The playhead
//   still advances over it: the playhead follows time, not trigs.
// - **`max` is a hard limit, and truncation never changes the music.** Every track is resolved
//   in full whatever the buffer size -- conditions, the PRE/NEI chain and the playheads do not
//   depend on how much room the caller gave -- and only the writing of events is cut short.
//   A return value equal to `max` means events MAY have been dropped; a buffer of
//   kMaxEventsPerTick or more makes that impossible for any pattern that lands at most one
//   step per track per tick, which is every speed of 1x or slower.
//
// ## No division
//
// CLAUDE.md forbids division in the audio path and the C674x has no integer divide instruction,
// so the three places that need one use a reciprocal instead: `A:B` (a modulo by 2..8), the
// ratchet velocity ramp (a rounded divide by 1..8) and the probability draw (a 32-bit hash
// folded into 0..99 by a multiply-shift rather than a `% 100`). The exactness of each is swept
// in tests/test_scheduler.cpp against the built-in operators.
#include "scheduler.hpp"

#include "clamp.hpp"
#include "liverand.hpp"

namespace strata {

namespace {

// --- division-free integer helpers ----------------------------------------

constexpr int kDivSmallMaxDivisor = 16;

// ceil(2^32 / d). Writing kReciprocal[d] = (2^32 + e) / d with 0 <= e < d, the multiply-shift
// below returns floor(n / d) for every n with n * e < 2^32. The worst e for d in 1..8 is 4
// (at d = 5), so those divisors are exact for n < 2^30; the worst for d in 1..16 is 14 (at
// d = 15), so the whole table is exact for n < 2^28.
//
// A:B uses d in 2..8 with n = a loop number, which is at most a 32-bit tick count divided by
// the shortest possible loop (one step at 2x, 12 ticks) -- the static_assert below pins that
// under the 2^30 bound. The velocity ramp uses d in 2..16 with n below 2^12.
constexpr uint64_t kReciprocal[kDivSmallMaxDivisor + 1] = {
    0,              // 0: never a divisor
    0x100000000ull, // 1
    2147483648ull,  // 2
    1431655766ull,  // 3
    1073741824ull,  // 4
    858993460ull,   // 5
    715827883ull,   // 6
    613566757ull,   // 7
    536870912ull,   // 8
    477218589ull,   // 9
    429496730ull,   // 10
    390451573ull,   // 11
    357913942ull,   // 12
    330382100ull,   // 13
    306783379ull,   // 14
    286331154ull,   // 15
    268435456ull,   // 16
};

static_assert(0xFFFFFFFFu / uint32_t(kTicksPerSixteenth / 2) < (1u << 30),
              "a loop number must stay inside the exact range of the d <= 8 reciprocals");

inline uint32_t div_small(uint32_t n, uint32_t d) {
    return uint32_t((uint64_t(n) * kReciprocal[d]) >> 32);
}

inline uint32_t mod_small(uint32_t n, uint32_t d) {
    return n - d * div_small(n, d);
}

// n / d rounded to nearest, halves up. Needs 2 * d <= kDivSmallMaxDivisor, so d <= 8.
inline uint32_t div_round_small(uint32_t n, uint32_t d) {
    return div_small(2u * n + d, 2u * d);
}

// --- hashing --------------------------------------------------------------

// A 32-bit avalanche (the "lowbias32" finalizer): every input bit reaches every output bit, so
// neighbouring steps and loops do not produce neighbouring draws. Multiplication wraps, which
// is defined for unsigned types.
inline uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// --- clamping -------------------------------------------------------------

inline uint8_t clamp_wide_u8(uint32_t value, uint8_t low, uint8_t high) {
    if (value < low) return low;
    return value > high ? high : uint8_t(value);
}

inline uint8_t ratchet_hits(uint8_t packed) {
    return clamp_wide_u8(ratchet_count(packed), kRatchetMin, kRatchetMax);
}

inline RatchetRamp ratchet_shape(uint8_t packed) {
    const RatchetRamp ramp = ratchet_ramp(packed);
    return uint8_t(ramp) > uint8_t(RatchetRamp::down) ? RatchetRamp::none : ramp;
}

// 1 / count for count in 0..kRatchetMax, so spreading a ratchet across a step is a multiply.
constexpr float kInvRatchetCount[kRatchetMax + 1] = {
    1.0f,                  // 0: never used; a count is clamped to 1 first
    1.0f,       1.0f / 2.0f, 1.0f / 3.0f, 1.0f / 4.0f,
    1.0f / 5.0f, 1.0f / 6.0f, 1.0f / 7.0f, 1.0f / 8.0f,
};

// Beyond this a float-to-uint32_t conversion is undefined, and no tempo STRATA supports gets
// anywhere near it: a step at 20 BPM and 1/8x speed is about 288,000 samples.
constexpr float kFrameCeiling = 1.0e9f;

}  // namespace

// --- the pieces -----------------------------------------------------------

uint32_t probability_roll(uint32_t seed, int track, int step, uint32_t loop) {
    // Mixed in sequence rather than xored together, so that (track, step) pairs cannot collide
    // the way track * A ^ step * B can.
    uint32_t h = mix32(seed ^ 0x9E3779B9u);
    h = mix32(h ^ (uint32_t(track) * 0x85EBCA77u + 0x165667B1u));
    h = mix32(h ^ (uint32_t(step) * 0xC2B2AE3Du + 0x27D4EB2Fu));
    h = mix32(h ^ (loop * 0x9E3779B1u + 0x85EBCA6Bu));

    // Fold the whole 32 bits into 0..99 with a multiply-shift instead of a modulo: no divide,
    // and it uses the high bits, which are the best mixed. h * 100 < 100 * 2^32, so the result
    // is always at most 99.
    return uint32_t((uint64_t(h) * 100ull) >> 32);
}

uint8_t lock_spread(uint8_t base, uint8_t width, uint32_t seed, int track, int step,
                    uint32_t loop, int param) {
    if (width == 0) return base;
    uint32_t h = mix32(seed ^ 0x2545F491u);
    h = mix32(h ^ (uint32_t(track) * 0x85EBCA77u + 0x165667B1u));
    h = mix32(h ^ (uint32_t(step) * 0xC2B2AE3Du + 0x27D4EB2Fu));
    h = mix32(h ^ (loop * 0x9E3779B1u + 0x85EBCA6Bu));
    h = mix32(h ^ (uint32_t(param) * 0x27D4EB2Fu + 0x9E3779B9u));
    // 0 .. 2 * width by multiply-shift (no modulo), then centred.
    const uint32_t w = width > 127u ? 127u : width;
    const int32_t offset = int32_t((uint64_t(h) * (2u * w + 1u)) >> 32) - int32_t(w);
    const int32_t value = int32_t(base) + offset;
    return uint8_t(value < 0 ? 0 : (value > 127 ? 127 : value));
}

uint8_t lock_resolve(const Pattern &pattern, int track, int step, uint32_t loop, int param,
                     uint8_t fallback) {
    uint8_t value = fallback;
    uint8_t width = 0;
    const LockView view = pattern_step_locks(pattern, track, step);
    for (const Lock &lock : view) {
        if (lock_param(lock.param) != uint8_t(param)) continue;
        if (lock_is_range(lock.param)) {
            width = lock.value;
        } else {
            value = lock.value;
        }
    }
    return lock_spread(value, width, pattern.seed, track, step, loop, param);
}

bool condition_passes(uint8_t packed, uint32_t loop, bool fill, bool previous, bool neighbour,
                      bool first_loop) {
    switch (condition_kind(packed)) {
        case Condition::NONE:
            return true;
        case Condition::A_B: {
            const uint8_t b = condition_b(packed);
            const uint8_t a = condition_a(packed);
            // Unreachable for a byte condition_kind() called A_B, but a zero divisor would be
            // a reciprocal-table read at index 0 rather than an obvious failure.
            if (b < kConditionBMin || b > kConditionBMax || a < 1 || a > b) return true;
            return mod_small(loop, b) + 1u == uint32_t(a);
        }
        case Condition::FILL:
            return fill;
        case Condition::NOT_FILL:
            return !fill;
        case Condition::PRE:
            return previous;
        case Condition::NOT_PRE:
            return !previous;
        case Condition::NEI:
            return neighbour;
        case Condition::NOT_NEI:
            return !neighbour;
        case Condition::FIRST:
            return first_loop;
        case Condition::NOT_FIRST:
            return !first_loop;
    }
    return true;    // an unknown kind reads as NONE
}

uint8_t ratchet_velocity(uint8_t velocity, uint8_t index, uint8_t count, RatchetRamp ramp) {
    const uint32_t hits = clamp_wide_u8(count, kRatchetMin, kRatchetMax);
    const uint32_t hit = index >= hits ? hits - 1u : uint32_t(index);
    const uint32_t full = clamp_wide_u8(velocity, 1, 127);

    uint32_t numerator;
    switch (ramp) {
        case RatchetRamp::up:
            numerator = full * (hit + 1u);      // the last hit is the step's own velocity
            break;
        case RatchetRamp::down:
            numerator = full * (hits - hit);    // the first hit is the step's own velocity
            break;
        case RatchetRamp::none:
        default:
            return uint8_t(full);
    }
    // numerator <= 127 * 8, so div_round_small stays far inside its exact range.
    return clamp_wide_u8(div_round_small(numerator, hits), 1, 127);
}

uint32_t ratchet_frame(uint32_t index, uint32_t count, float step_samples) {
    const uint32_t hits = count < kRatchetMin ? uint32_t(kRatchetMin)
                                              : (count > kRatchetMax ? uint32_t(kRatchetMax)
                                                                     : count);
    const uint32_t hit = index >= hits ? hits - 1u : index;

    // Catches zero, negatives and NaN in one test: an unconfigured tempo puts every hit on the
    // step's own frame rather than somewhere unpredictable.
    if (!(step_samples > 0.0f)) return 0;

    const float offset = float(hit) * step_samples * kInvRatchetCount[hits] + 0.5f;
    if (offset <= 0.0f) return 0;
    if (offset >= kFrameCeiling) return uint32_t(kFrameCeiling);
    return uint32_t(offset);
}

// --- parameter slide -----------------------------------------------------

namespace {

struct Glide {
    uint8_t step = 0;
    uint32_t loop = 0;
    uint32_t frames = 0;
};

// The next step after `firing` that this track plays with a trig or a trigless, walking the
// slots in play order (so REV, PING and the RND modes look ahead the way they will play), at
// most one whole pass -- which comes back to the step itself when it is the only one that plays.
// Its condition and probability are not drawn: a slide aims at what the step holds. The glide
// lasts from this step to that one's slot, swing, micro-timing and FEEL left out.
Glide glide_target(const TrackPattern &source, const TrackTiming &timing, float step_samples,
                   const Firing &firing) {
    Glide glide;
    const int length = clamp_int(int(timing.length), 1, kMaxSteps);
    int slot = int(firing.slot);
    uint32_t loop = firing.loop;
    for (int distance = 1; distance <= length; distance++) {
        slot++;
        if (slot >= length) {
            slot = 0;
            loop++;
        }
        const uint8_t step =
            timing.play == PlayMode::fwd ? uint8_t(slot) : play_step(slot, loop, timing);
        if ((source.steps[step].flags & (kStepTrig | kStepTrigless)) == 0) continue;
        const float frames = float(distance) * step_samples + 0.5f;
        glide.step = step;
        glide.loop = loop;
        glide.frames = !(frames > 0.0f) ? 0u
                       : frames >= kFrameCeiling ? uint32_t(kFrameCeiling)
                                                 : uint32_t(frames);
        return glide;
    }
    return glide;
}

}  // namespace

// --- the scheduler --------------------------------------------------------

void Scheduler::reset(bool first_loop) {
    // Chaining state only: configure()'s timing survives a reset, which is what lets the
    // transport stop and start without re-deriving every track.
    for (int t = 0; t < kTracks; t++) {
        tracks_[t].playhead = 0;
        tracks_[t].last_condition = true;   // PRE on a fresh pattern's first step reads true
        tracks_[t].first_pass_loop = 0;
        tracks_[t].seen_a_loop = false;
    }
    first_loop_ = first_loop;
}

void Scheduler::configure(const Pattern &pattern, float samples_per_tick) {
    pattern_seed_ = pattern.seed;

    const int pattern_swing = clamp_int(int(pattern.swing), kSwingMin, kSwingMax);
    const float tick_samples = samples_per_tick > 0.0f ? samples_per_tick : 0.0f;

    for (int t = 0; t < kTracks; t++) {
        const TrackPattern &source = pattern.tracks[t];
        TrackState &state = tracks_[t];

        state.timing.length = uint8_t(clamp_int(int(source.length), 1, kMaxSteps));
        state.timing.speed = source.speed < uint8_t(Speed::count) ? Speed(source.speed)
                                                                 : Speed::x1;
        state.timing.swing =
            uint8_t(source.swing == kSwingInherit
                        ? pattern_swing
                        : clamp_int(int(source.swing), kSwingMin, kSwingMax));

        // Hoisted: the inner path must not multiply out a step length, let alone divide one.
        state.step_samples = float(state.timing.step_ticks()) * tick_samples;

        if (state.playhead >= state.timing.length) state.playhead = 0;
    }
}

int Scheduler::tick(uint32_t tick_number, uint32_t frame, const Pattern &pattern,
                    const PerformState &perform, StepEvent *out, int max) {
    // The draw must follow the pattern it is handed, not one a stale configure() saw.
    pattern_seed_ = pattern.seed;

    const int limit = (out == nullptr || max <= 0) ? 0 : max;
    int written = 0;

    for (int t = 0; t < kTracks; t++) {
        TrackState &state = tracks_[t];
        const TrackPattern &source = pattern.tracks[t];

        // core/timing wants micro-timing as a plain array and Step is 12 bytes wide, so the
        // column has to be gathered. 64 bytes of stack, read fresh every tick: a step edit
        // must not need a configure() to be heard.
        int8_t micro[kStepsPerTrack];
        for (int i = 0; i < kStepsPerTrack; i++) micro[i] = source.steps[i].micro;

        // The play mode and its seed are read fresh too, for the same reason: an undo or a
        // mutation that changes them must be heard on the next step, configure() or not.
        // LIVE RND (core/liverand.hpp), PERFORM's FUNC + D, overrides it on an armed track.
        state.timing.play = live_play_mode(source.play, perform.rnd_unit, perform.rnd_armed, t);
        // Only the RND modes draw, so only they pay for the hash.
        state.timing.play_seed =
            state.timing.play >= PlayMode::rnd1 ? play_seed(pattern_seed_, t) : 0u;

        // FEEL, fresh as well: a pattern setting an undo or a LOCK jump can change.
        state.timing.feel = pattern.feel <= kFeelMax ? pattern.feel : uint8_t(0);
        state.timing.feel_seed = feel_mode(state.timing.feel) == FeelMode::drift
                                     ? feel_seed(pattern_seed_, t)
                                     : 0u;

        Firing firings[kMaxFiringsPerTick];
        const int found =
            steps_firing_at(tick_number, state.timing, micro, firings, kMaxFiringsPerTick);

        for (int f = 0; f < found; f++) {
            const Firing &firing = firings[f];
            if (firing.step >= state.timing.length) continue;    // cannot happen; cheap to deny

            // 1. trig state. The playhead follows time, so it moves over an off step too. It is
            // the step that plays, not the slot: under REV or RND the light jumps with the sound.
            state.playhead = firing.step;

            const Step &step = source.steps[firing.step];
            const bool has_trig = (step.flags & kStepTrig) != 0;
            const bool has_trigless = (step.flags & kStepTrigless) != 0;
            if (!has_trig && !has_trigless) continue;

            // 2. condition. Its result is what PRE and NEI chain off, so it is stored whether
            // or not the probability roll below then silences the step.
            if (!state.seen_a_loop) {
                state.first_pass_loop = firing.loop;
                state.seen_a_loop = true;
            }
            const bool is_first = first_loop_ && firing.loop == state.first_pass_loop;
            const bool neighbour = t == 0 ? false : tracks_[t - 1].last_condition;

            const bool passed = condition_passes(step.condition, firing.loop, perform.fill,
                                                 state.last_condition, neighbour, is_first);
            state.last_condition = passed;
            if (!passed) continue;

            // 3. probability. 0 and 100 are decided without a draw, so the two boundaries
            // cannot depend on the hash at all.
            if (step.probability == 0) continue;
            if (step.probability < 100 &&
                probability_roll(pattern_seed_, t, firing.step, firing.loop) >=
                    uint32_t(step.probability)) {
                continue;
            }

            const uint8_t note = uint8_t(step.note & 0x7Fu);
            const bool accent = (step.flags & kStepAccent) != 0;
            const bool slide = (step.flags & kStepSlide) != 0;

            // Parameter slide: where the slid locks glide to, read only when one slides.
            Glide glide;
            if (step.lock_count != 0 && pattern_step_slides(pattern, t, firing.step)) {
                glide = glide_target(source, state.timing, state.step_samples, firing);
            }

            if (!has_trig) {
                // Trigless: the locks apply, no note starts, and there is nothing to ratchet.
                if (written >= limit) continue;
                StepEvent &event = out[written++];
                event.track = uint8_t(t);
                event.step = firing.step;
                event.note = note;
                event.velocity = clamp_wide_u8(step.velocity, 1, 127);
                event.length_index = step.length;
                event.accent = accent;
                event.slide = slide;
                event.trigless = true;
                event.frame = frame;
                event.ratchet_index = 0;
                event.ratchet_count = 1;
                event.loop = firing.loop;
                event.glide_step = glide.step;
                event.glide_loop = glide.loop;
                event.glide_frames = glide.frames;
                continue;
            }

            // 4. ratchet.
            const uint8_t hits = ratchet_hits(step.ratchet);
            const RatchetRamp ramp = ratchet_shape(step.ratchet);
            for (uint8_t hit = 0; hit < hits; hit++) {
                if (written >= limit) break;
                StepEvent &event = out[written++];
                event.track = uint8_t(t);
                event.step = firing.step;
                event.note = note;
                event.velocity = ratchet_velocity(step.velocity, hit, hits, ramp);
                event.length_index = step.length;
                event.accent = accent;
                event.slide = slide;
                event.trigless = false;
                event.frame = frame + ratchet_frame(hit, hits, state.step_samples);
                event.ratchet_index = hit;
                event.ratchet_count = hits;
                event.loop = firing.loop;
                event.glide_step = glide.step;
                event.glide_loop = glide.loop;
                event.glide_frames = glide.frames;
            }
        }
    }

    return written;
}

bool stutter_event(const Pattern &pattern, int track, int step, uint32_t frame, StepEvent &out) {
    if (track < 0 || track >= kTracks || step < 0) return false;
    const TrackPattern &source = pattern.tracks[track];
    const int length = clamp_int(int(source.length), 1, kMaxSteps);
    // Subtraction, not a modulo: this chip has no integer divide, and step < 64 needs at most
    // 63 rounds even on a one-step track.
    int index = step;
    while (index >= length) index -= length;

    const Step &played = source.steps[index];
    const bool has_trig = (played.flags & kStepTrig) != 0;
    const bool has_trigless = (played.flags & kStepTrigless) != 0;
    if (!has_trig && !has_trigless) return false;

    out.track = uint8_t(track);
    out.step = uint8_t(index);
    out.note = uint8_t(played.note & 0x7Fu);
    out.velocity = clamp_wide_u8(played.velocity, 1, 127);
    out.length_index = played.length;
    out.accent = (played.flags & kStepAccent) != 0;
    out.slide = (played.flags & kStepSlide) != 0;
    out.trigless = !has_trig;
    out.frame = frame;
    out.ratchet_index = 0;
    out.ratchet_count = 1;
    // A stutter plays one moment over and over: its random ranges draw once, and nothing slides.
    out.loop = 0;
    out.glide_step = 0;
    out.glide_loop = 0;
    out.glide_frames = 0;
    return true;
}

uint8_t Scheduler::playhead(int track) const {
    if (track < 0 || track >= kTracks) return 0;
    return tracks_[track].playhead;
}

bool Scheduler::last_condition(int track) const {
    if (track < 0 || track >= kTracks) return true;
    return tracks_[track].last_condition;
}

}  // namespace strata
