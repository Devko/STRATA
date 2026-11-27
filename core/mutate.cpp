// Mutation. core/mutate.hpp has the design and the bounds.
#include "mutate.hpp"

#include "clamp.hpp"
#include "scale.hpp"
#include "stepnotes.hpp"

namespace strata {
namespace {

// The same finalizer core/scheduler.cpp uses, and for the same reason: a deterministic draw from
// an identity rather than a running RNG. It is repeated here rather than shared because
// core/scheduler.cpp keeps it private and a mutation must not have to link the scheduler to
// decide a velocity -- the same independence core/queue.cpp keeps from core/project.cpp.
inline uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// Is a step on at all? A trigless step applies its locks without a note, and docs/SPEC.md makes
// that the point of it, so a velocity or micro mutation has to reach it: its locks are what the
// step is for, and its timing still decides when they land.
bool step_is_on(const Step &step) {
    return (step.flags & (kStepTrig | kStepTrigless)) != 0;
}

bool steps_identical(const Step &a, const Step &b) {
    return a.flags == b.flags && a.note == b.note && a.velocity == b.velocity &&
           a.length == b.length && a.micro == b.micro && a.probability == b.probability &&
           a.condition == b.condition && a.ratchet == b.ratchet &&
           a.lock_first == b.lock_first && a.lock_count == b.lock_count && a.chord == b.chord;
}

// `amount` of a span, rounded to nearest rather than truncated. Truncating would make the note
// mutation dead below amount 34, because 3 degrees times 33 over 100 is 0 -- a knob that does
// nothing for its first third is a knob that looks broken.
int span_for(int amount, int full) { return (amount * full + 50) / 100; }

}  // namespace

const char *mutation_name(Mutation kind) {
    switch (kind) {
        case Mutation::rotate_left:  return "ROT LEFT";
        case Mutation::rotate_right: return "ROT RIGHT";
        case Mutation::density_up:   return "DENSITY +";
        case Mutation::density_down: return "DENSITY -";
        case Mutation::velocity:     return "VELOCITY";
        case Mutation::micro:        return "MICRO";
        case Mutation::probability:  return "CHANCE";
        case Mutation::notes:        return "NOTES";
        case Mutation::mix:          return "MIX";
        default:                     return "?";
    }
}

int mutate_draw(uint32_t seed, int track, int step, Mutation kind, uint32_t variant, int span) {
    if (span <= 0) return 0;

    // Mixed in sequence rather than xored together, so that (track, step) pairs cannot collide
    // the way track * A ^ step * B can -- core/scheduler.cpp's probability_roll says the same.
    // The kind is part of the identity: a velocity mutation and a micro mutation of the same
    // step on the same press must not move by the same amount, which they would if the draw
    // depended only on where the step is.
    uint32_t h = mix32(seed ^ 0x2545F491u);
    h = mix32(h ^ (uint32_t(track) * 0x85EBCA77u + 0x165667B1u));
    h = mix32(h ^ (uint32_t(step) * 0xC2B2AE3Du + 0x27D4EB2Fu));
    h = mix32(h ^ (uint32_t(kind) * 0x9E3779B1u + 0x85EBCA6Bu));
    h = mix32(h ^ (variant * 0xCC9E2D51u + 0x1B873593u));

    // Into [-span, span]: 2 * span + 1 values, folded from the whole 32 bits with a
    // multiply-shift rather than a modulo. No divide, and it uses the high bits, which are the
    // best mixed. The product cannot overflow: span is at most 48 here and h is 32-bit.
    const uint32_t range = uint32_t(2 * span + 1);
    const uint32_t roll = uint32_t((uint64_t(h) * uint64_t(range)) >> 32);
    return int(roll) - span;
}

// --- the generator --------------------------------------------------------

int euclid_trigs(int hits, int steps, int length) {
    const int cycle = clamp_int(steps, 1, kStepsPerTrack);
    const int k = clamp_int(hits, 0, cycle);
    const int span = clamp_int(length, 0, kStepsPerTrack);
    int n = 0;
    for (int i = 0; i < span; i++) {
        if (euclidean_hit(i % cycle, k, cycle)) n++;
    }
    return n;
}

bool euclid_generate(Pattern &pattern, int track, int hits, int steps, int rotation) {
    if (track < 0 || track >= kTracks) return false;

    TrackPattern &t = pattern.tracks[track];
    const int length = clamp_int(int(t.length), 1, kStepsPerTrack);
    const int cycle = clamp_int(steps, 1, kStepsPerTrack);
    const int k = clamp_int(hits, 0, cycle);

    // A donor for the steps that gain a trig: the first one that already has it. Same reasoning
    // as the density mutation -- on a melodic track the alternative is a middle C nobody asked
    // for. Taken before anything is written, so it cannot be a step this call just created.
    const Step *donor = nullptr;
    for (int i = 0; i < length && donor == nullptr; i++) {
        if ((t.steps[i].flags & kStepTrig) != 0) donor = &t.steps[i];
    }
    const Step donated = donor != nullptr ? *donor : Step{};
    const bool inherit = donor != nullptr;

    // The rotation, reduced into the cycle ONCE. Positive moves the figure later, so step 0
    // plays the figure's position -rotation. Inside the loop the position is carried with a
    // compare-and-subtract: two modulos per step would be two software divides per step on a
    // chip with no integer divide.
    int at = (-rotation) % cycle;
    at = at < 0 ? at + cycle : at;

    bool changed = false;
    for (int i = 0; i < length; i++) {
        Step &step = t.steps[i];
        const bool want = euclidean_hit(at, k, cycle);
        const bool have = (step.flags & kStepTrig) != 0;

        if (want != have) {
            if (want) {
                if (inherit && (step.flags & (kStepTrig | kStepTrigless)) == 0) {
                    step.note = donated.note;
                    step.velocity = donated.velocity;
                    step.length = donated.length;
                }
                step.flags = uint8_t(step.flags | kStepTrig);
            } else {
                // The trig bit and nothing else: a step that loses its trig keeps its note, its
                // locks and its timing, so generating a sparser figure and then a denser one
                // gets the part back rather than a row of defaults.
                step.flags = uint8_t(step.flags & ~uint8_t(kStepTrig));
            }
            changed = true;
        }

        at++;
        at = at >= cycle ? 0 : at;
    }
    return changed;
}

Mutation mutate_mix_kind(uint32_t seed, int track, uint32_t variant) {
    uint32_t h = mix32(seed ^ 0x68E31DA4u);
    h = mix32(h ^ (uint32_t(track) * 0x85EBCA77u + 0x165667B1u));
    h = mix32(h ^ (variant * 0xCC9E2D51u + 0x1B873593u));
    return Mutation(uint32_t((uint64_t(h) * uint64_t(Mutation::mix)) >> 32));
}

bool mutate_apply(Pattern &pattern, int track, int first, int count, Mutation kind,
                  uint32_t variant, int amount) {
    if (track < 0 || track >= kTracks) return false;

    if (kind == Mutation::mix) {
        // A refused kind changes nothing (every case below checks before it writes), so trying
        // the next is safe; at most the eight kinds once each.
        const int start = int(mutate_mix_kind(pattern.seed, track, variant));
        for (int i = 0; i < int(Mutation::mix); i++) {
            const int at = start + i < int(Mutation::mix) ? start + i : start + i - int(Mutation::mix);
            if (mutate_apply(pattern, track, first, count, Mutation(at), variant, amount)) return true;
        }
        return false;
    }

    TrackPattern &t = pattern.tracks[track];

    // Clamped to the TRACK's length, not to kStepsPerTrack. A 16-step track's steps 16..63 are
    // not part of the pattern, and rotating them in would bring in steps the sequencer never
    // plays and the user has never seen.
    const int length = clamp_int(int(t.length), 1, kStepsPerTrack);
    const int from = clamp_int(first, 0, length);
    const int to = clamp_int(first + count, from, length);
    const int span_steps = to - from;
    if (span_steps <= 0) return false;

    amount = clamp_int(amount, 0, 100);

    switch (kind) {
        case Mutation::rotate_left:
        case Mutation::rotate_right: {
            if (span_steps < 2) return false;
            // A rotation of identical steps is not a mutation, and spending an undo level on it
            // would be a press that looks like it did nothing because it did nothing.
            bool varied = false;
            for (int i = from + 1; i < to && !varied; i++) {
                if (!steps_identical(t.steps[i], t.steps[from])) varied = true;
            }
            if (!varied) return false;

            // Whole Step values move, so each step's `lock_first` travels with it and the lock
            // pool is untouched -- which is why this cannot fail for want of pool space.
            if (kind == Mutation::rotate_left) {
                const Step held = t.steps[from];
                for (int i = from; i < to - 1; i++) t.steps[i] = t.steps[i + 1];
                t.steps[to - 1] = held;
            } else {
                const Step held = t.steps[to - 1];
                for (int i = to - 1; i > from; i--) t.steps[i] = t.steps[i - 1];
                t.steps[from] = held;
            }
            return true;
        }

        case Mutation::density_up:
        case Mutation::density_down: {
            int hits = 0;
            for (int i = from; i < to; i++) {
                if ((t.steps[i].flags & kStepTrig) != 0) hits++;
            }
            const int target = clamp_int(hits + (kind == Mutation::density_up ? 1 : -1), 0,
                                         span_steps);
            if (target == hits) return false;      // already empty, or already full

            // A donor for the steps that gain a trig: the first step in the range that already
            // has one. A new hit then sounds like the part it joins rather than like a bare
            // default, which on a melodic track is the difference between a note and a middle C
            // nobody asked for. No donor (the range was empty) leaves each step's own values.
            const Step *donor = nullptr;
            for (int i = from; i < to && donor == nullptr; i++) {
                if ((t.steps[i].flags & kStepTrig) != 0) donor = &t.steps[i];
            }

            // The hits are REDISTRIBUTED evenly, not nudged: `density_up` is "k + 1 hits spread
            // as evenly as possible", not "add one hit and keep the rest where they were". That
            // is what Euclidean density means, it is the same result every time for the same
            // inputs, and the undo makes it safe to try. The gentler reading -- keep the
            // existing hits and add one -- drifts somewhere arbitrary after a few presses.
            for (int i = from; i < to; i++) {
                Step &step = t.steps[i];
                const bool want = euclidean_hit(i - from, target, span_steps);
                const bool have = (step.flags & kStepTrig) != 0;
                if (want == have) continue;
                if (want) {
                    if (donor != nullptr && !step_is_on(step)) {
                        step.note = donor->note;
                        step.velocity = donor->velocity;
                        step.length = donor->length;
                    }
                    step.flags = uint8_t(step.flags | kStepTrig);
                } else {
                    // Only the trig bit. Everything else stays, so an undo, or the next
                    // density up, gets the step's sound back rather than a default.
                    step.flags = uint8_t(step.flags & ~uint8_t(kStepTrig));
                }
            }
            return true;
        }

        case Mutation::velocity:
        case Mutation::micro:
        case Mutation::probability:
        case Mutation::notes: {
            const int full = kind == Mutation::velocity      ? kMutateVelocitySpan
                             : kind == Mutation::micro       ? kMutateMicroSpan
                             : kind == Mutation::probability ? kMutateProbabilitySpan
                                                             : kMutateNoteDegrees;
            const int span = span_for(amount, full);
            if (span <= 0) return false;           // amount 0: the knob is off, not broken

            bool changed = false;
            for (int i = from; i < to; i++) {
                Step &step = t.steps[i];
                if (!step_is_on(step)) continue;   // a step that is off has nothing to vary
                const int draw = mutate_draw(pattern.seed, track, i, kind, variant, span);
                if (draw == 0) continue;

                switch (kind) {
                    case Mutation::velocity: {
                        // 1..127, never 0: a velocity of 0 is a note that does not sound, which
                        // is a step the user would have turned off instead.
                        const uint8_t was = step.velocity;
                        step.velocity = uint8_t(clamp_int(int(step.velocity) + draw, 1, 127));
                        if (step.velocity != was) changed = true;
                        break;
                    }
                    case Mutation::micro: {
                        const int8_t was = step.micro;
                        step.micro = int8_t(
                            clamp_int(int(step.micro) + draw, -kMicroRange, kMicroRange));
                        if (step.micro != was) changed = true;
                        break;
                    }
                    case Mutation::probability: {
                        const uint8_t was = step.probability;
                        step.probability =
                            uint8_t(clamp_int(int(step.probability) + draw, 0, 100));
                        if (step.probability != was) changed = true;
                        break;
                    }
                    default: {
                        // By DEGREES of the pattern's own key, so a line stays in the scale
                        // instead of being transposed out of it. Pattern::root is a pitch class
                        // and scale ids this build does not know play chromatically
                        // (core/scale.hpp).
                        const uint8_t was = step.note;
                        step.note = scale_step(step.note, pattern.root, int(pattern.scale), draw);
                        if (step.note != was) changed = true;
                        // A chord played into the step moves with its note, by the same
                        // semitones; playback folds each note into the key (core/stepnotes.hpp).
                        step_shift_extra_notes(pattern, track, i, int(step.note) - int(was));
                        break;
                    }
                }
            }
            return changed;
        }

        default:
            return false;
    }
}

}  // namespace strata
