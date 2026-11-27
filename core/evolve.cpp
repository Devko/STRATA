// EVOLVE. core/evolve.hpp has the design.
#include "evolve.hpp"

#include "clamp.hpp"

namespace strata {
namespace {

bool steps_identical(const Step &a, const Step &b) {
    return a.flags == b.flags && a.note == b.note && a.velocity == b.velocity &&
           a.length == b.length && a.micro == b.micro && a.probability == b.probability &&
           a.condition == b.condition && a.ratchet == b.ratchet &&
           a.lock_first == b.lock_first && a.lock_count == b.lock_count && a.chord == b.chord;
}

// The step's extra notes by id, kNoExtra where it has none with that id.
void read_extras(const Pattern &pattern, int track, int step, uint8_t *out) {
    for (int i = 0; i < kStepExtraNotes; i++) {
        uint8_t value = 0;
        out[i] = pattern_get_lock(pattern, track, step, uint8_t(kLockNoteFirst + i), value)
                     ? uint8_t(value & 0x7Fu)
                     : kNoExtra;
    }
}

// Writes `extras` back into the locks the step has. An edit of a lock the step already has
// never allocates; an id the step does not have (it cannot, after a restore of the step that
// held it) is left alone rather than allocated.
void write_extras(Pattern &pattern, int track, int step, const uint8_t *extras) {
    for (int i = 0; i < kStepExtraNotes; i++) {
        if (extras[i] == kNoExtra) continue;
        const uint8_t id = uint8_t(kLockNoteFirst + i);
        uint8_t value = 0;
        if (!pattern_get_lock(pattern, track, step, id, value)) continue;
        if (value != extras[i]) pattern_set_lock(pattern, track, step, id, extras[i]);
    }
}

bool extras_identical(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < kStepExtraNotes; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Every step the user changed since the last turn goes into HOME as it is now.
void merge_edits(Evolve &evolve, const Pattern &pattern, int track) {
    const TrackPattern &t = pattern.tracks[track];
    for (int i = 0; i < kStepsPerTrack; i++) {
        uint8_t now[kStepExtraNotes];
        read_extras(pattern, track, i, now);
        if (steps_identical(t.steps[i], evolve.last.steps[i]) &&
            extras_identical(now, evolve.last_extra[i])) {
            continue;
        }
        evolve.home.steps[i] = t.steps[i];
        for (int k = 0; k < kStepExtraNotes; k++) evolve.home_extra[i][k] = now[k];
    }
}

// HOME's steps back on the track. Reports whether anything changed.
bool restore(Evolve &evolve, Pattern &pattern, int track) {
    TrackPattern &t = pattern.tracks[track];
    bool changed = false;
    for (int i = 0; i < kStepsPerTrack; i++) {
        if (!steps_identical(t.steps[i], evolve.home.steps[i])) {
            t.steps[i] = evolve.home.steps[i];
            changed = true;
        }
        uint8_t now[kStepExtraNotes];
        read_extras(pattern, track, i, now);
        if (!extras_identical(now, evolve.home_extra[i])) {
            write_extras(pattern, track, i, evolve.home_extra[i]);
            changed = true;
        }
    }
    return changed;
}

void record_last(Evolve &evolve, const Pattern &pattern, int track) {
    evolve.last = pattern.tracks[track];
    for (int i = 0; i < kStepsPerTrack; i++) read_extras(pattern, track, i, evolve.last_extra[i]);
}

}  // namespace

const char *evolve_mode_name(EvolveMode mode) {
    switch (mode) {
        case EvolveMode::orbit: return "ORBIT";
        case EvolveMode::drift: return "DRIFT";
        default:                return "?";
    }
}

const char *evolve_period_name(int index) {
    switch (index) {
        case 1:  return "1 LOOP";
        case 2:  return "2 LOOPS";
        case 3:  return "4 LOOPS";
        case 4:  return "8 LOOPS";
        default: return "OFF";
    }
}

void evolve_set(Evolve &evolve, const Pattern &pattern, int track, int period) {
    if (track < 0 || track >= kTracks) return;
    const int index = clamp_int(period, 0, kEvolvePeriods - 1);
    if (index == 0) {
        evolve_keep(evolve);
        return;
    }
    if (!evolve.on()) {
        evolve.home = pattern.tracks[track];
        for (int i = 0; i < kStepsPerTrack; i++) {
            read_extras(pattern, track, i, evolve.home_extra[i]);
        }
        record_last(evolve, pattern, track);
        evolve.passes = 0;
    }
    evolve.period = uint8_t(index);
}

bool evolve_pass(Evolve &evolve, Pattern &pattern, int track, EvolveMode mode, Mutation kind,
                 int amount, uint32_t variant) {
    if (!evolve.on() || track < 0 || track >= kTracks) return false;
    evolve.passes++;
    if (evolve.passes < kEvolvePeriod[evolve.period]) return false;
    evolve.passes = 0;

    merge_edits(evolve, pattern, track);
    bool changed = false;
    if (mode == EvolveMode::orbit) changed = restore(evolve, pattern, track);

    // The whole track, as the MUTATE pad does with no range held.
    if (mutate_apply(pattern, track, 0, kStepsPerTrack, kind, variant, amount)) changed = true;

    record_last(evolve, pattern, track);
    return changed;
}

void evolve_keep(Evolve &evolve) {
    evolve.period = 0;
    evolve.passes = 0;
}

bool evolve_home(Evolve &evolve, Pattern &pattern, int track) {
    if (!evolve.on() || track < 0 || track >= kTracks) return false;
    merge_edits(evolve, pattern, track);
    const bool changed = restore(evolve, pattern, track);
    evolve_keep(evolve);
    return changed;
}

}  // namespace strata
