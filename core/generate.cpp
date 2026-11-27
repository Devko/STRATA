// GEN. core/generate.hpp has the design, the weights and the ranges.
#include "generate.hpp"

#include "clamp.hpp"
#include "scale.hpp"
#include "stepnotes.hpp"
#include "timing.hpp"

namespace strata {
namespace {

// The finalizer core/mutate.cpp and core/scheduler.cpp use, repeated for the reason they give:
// a draw from an identity, and no module linking another to decide a velocity.
inline uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// What a draw is for. Part of its identity, so the hit and the velocity of one step are not the
// same number read twice.
enum Draw : uint32_t { d_hit = 1, d_velocity, d_move, d_size, d_gate, d_direction, d_root, d_slide };

// A number in 0..n-1 for this identity: multiply-shift from the high bits, no divide.
int roll(uint32_t seed, int track, int step, Draw what, uint32_t variant, int n) {
    uint32_t h = mix32(seed ^ 0x5BD1E995u);
    h = mix32(h ^ (uint32_t(track) * 0x85EBCA77u + 0x165667B1u));
    h = mix32(h ^ (uint32_t(step) * 0xC2B2AE3Du + 0x27D4EB2Fu));
    h = mix32(h ^ (uint32_t(what) * 0x9E3779B1u + 0x85EBCA6Bu));
    h = mix32(h ^ (variant * 0xCC9E2D51u + 0x1B873593u));
    return int((uint64_t(h) * uint64_t(n > 0 ? n : 1)) >> 32);
}

// Where a step sits in a 16-step bar: 0 the one, 1 another beat, 2 an off-beat 8th, 3 a 16th.
int position_class(int index) {
    const int in_bar = index & 15;
    if (in_bar == 0) return 0;
    if ((in_bar & 3) == 0) return 1;
    if ((in_bar & 1) == 0) return 2;
    return 3;
}

constexpr int kWeight[4] = {100, 80, 50, 25};

// Velocity ranges per position class, drum and melodic: {low, high}.
constexpr int kDrumVelocity[4][2] = {{110, 127}, {95, 115}, {75, 100}, {40, 70}};
constexpr int kMelodicVelocity[4][2] = {{105, 120}, {95, 115}, {85, 110}, {80, 100}};

// Gates for a melodic hit: mostly one step (index 8), now and then half a step (6) or two (10).
// Indices into core/timing.hpp's table.
constexpr uint8_t kGates[5] = {8, 8, 8, 6, 10};

uint8_t draw_velocity(uint32_t seed, int track, int i, bool melodic, uint32_t variant) {
    const int cls = position_class(i);
    const int low = melodic ? kMelodicVelocity[cls][0] : kDrumVelocity[cls][0];
    const int high = melodic ? kMelodicVelocity[cls][1] : kDrumVelocity[cls][1];
    return uint8_t(low + roll(seed, track, i, d_velocity, variant, high - low + 1));
}

// `degree` reflected into 0..top, so a walk that would leave the range turns back.
int reflect(int degree, int top) {
    if (top <= 0) return 0;
    // At most a few degrees past either edge (a move is five at most and top is at least one
    // scale), so two reflections are always enough and no modulo is needed.
    for (int k = 0; k < 2; k++) {
        degree = degree < 0 ? -degree : degree;
        degree = degree > top ? 2 * top - degree : degree;
    }
    return clamp_int(degree, 0, top);
}

}  // namespace

const char *gen_what_name(GenWhat what) {
    switch (what) {
        case GenWhat::both:   return "BOTH";
        case GenWhat::rhythm: return "RHYTHM";
        case GenWhat::notes:  return "NOTES";
        default:              return "?";
    }
}

GenSettings GenSettings::make(int density, int range, int leap, int what) {
    GenSettings s;
    s.density = uint8_t(clamp_int(density, 0, 100));
    s.range = uint8_t(clamp_int(range, kGenRangeMin, kGenRangeMax));
    s.leap = uint8_t(clamp_int(leap, 0, 100));
    s.what = what >= 0 && what < int(GenWhat::count) ? GenWhat(what) : GenWhat::both;
    return s;
}

int gen_hit_chance(int index, int density) {
    const int d = clamp_int(density, 0, 100);
    const int chance = (kWeight[position_class(index)] * d + 30) / 60;
    return chance > 100 ? 100 : chance;
}

bool generate_track(Pattern &pattern, int track, bool melodic, const GenSettings &settings,
                    uint32_t variant) {
    if (track < 0 || track >= kTracks) return false;
    if (!melodic && settings.what == GenWhat::notes) return false;

    TrackPattern &t = pattern.tracks[track];
    const int length = clamp_int(int(t.length), 1, kStepsPerTrack);
    const uint32_t seed = pattern.seed;

    // The part's own sound, taken before anything is written: a step that gains a hit sounds
    // like the part (core/mutate.cpp's donor), and the first trig's octave is where the melody
    // walks.
    const Step *donor = nullptr;
    for (int i = 0; i < length && donor == nullptr; i++) {
        if ((t.steps[i].flags & kStepTrig) != 0) donor = &t.steps[i];
    }
    const Step donated = donor != nullptr ? *donor : Step{};
    const bool inherit = donor != nullptr;

    bool changed = false;

    // --- the rhythm ---
    if (settings.what != GenWhat::notes) {
        for (int i = 0; i < length; i++) {
            Step &step = t.steps[i];
            const bool forced = !melodic && i == 0 && settings.density >= kGenDownbeatDensity;
            const bool want =
                forced || roll(seed, track, i, d_hit, variant, 100) < gen_hit_chance(i, settings.density);
            const bool have = (step.flags & kStepTrig) != 0;
            if (want == have) continue;
            if (want) {
                if (inherit && (step.flags & (kStepTrig | kStepTrigless)) == 0) {
                    step.note = donated.note;
                    step.length = donated.length;
                }
                step.flags = uint8_t(step.flags | kStepTrig);
            } else {
                step.flags = uint8_t(step.flags & ~uint8_t(kStepTrig));
            }
            changed = true;
        }
    }

    // --- velocities, on every hit the part now has ---
    for (int i = 0; i < length; i++) {
        Step &step = t.steps[i];
        if ((step.flags & kStepTrig) == 0) continue;
        const uint8_t v = draw_velocity(seed, track, i, melodic, variant);
        if (step.velocity != v) {
            step.velocity = v;
            changed = true;
        }
    }

    // --- the notes ---
    if (melodic && settings.what != GenWhat::rhythm) {
        const int scale = int(pattern.scale);
        const uint8_t root = uint8_t(pattern.root % 12);
        const int per_octave = scale_size(scale);
        const int top = per_octave * int(settings.range);

        // The root at or below the track's own note: the bottom of the walk.
        const int from = inherit ? int(donated.note) : int(kGenDefaultBase);
        int base = from - ((from - int(root) + 120) % 12);
        if (base < 0) base += 12;
        while (base + 12 * int(settings.range) > 127) base -= 12;

        int degree = 0;
        bool first = true;
        for (int i = 0; i < length; i++) {
            Step &step = t.steps[i];
            if ((step.flags & kStepTrig) == 0) continue;

            if (first) {
                degree = 0;                     // a part starts on its root
                first = false;
            } else if ((i & 15) == 0 && roll(seed, track, i, d_root, variant, 100) < 60) {
                // The one of a bar: the nearest root, which on a two-octave walk may be the
                // middle one.
                const int below = degree - degree % per_octave;
                degree = degree - below > per_octave / 2 && below + per_octave <= top
                             ? below + per_octave
                             : below;
            } else if (roll(seed, track, i, d_move, variant, 100) < int(settings.leap)) {
                const int by = 2 + roll(seed, track, i, d_size, variant, 4);    // 2..5
                degree = reflect(degree + (roll(seed, track, i, d_direction, variant, 2) ? by : -by), top);
            } else {
                degree = reflect(degree + roll(seed, track, i, d_size, variant, 3) - 1, top);
            }

            const uint8_t note = scale_step(uint8_t(base), root, scale, degree);
            if (step.note != note) {
                step_shift_extra_notes(pattern, track, i, int(note) - int(step.note));
                step.note = note;
                changed = true;
            }

            const uint8_t gate = kGates[roll(seed, track, i, d_gate, variant, 5)];
            if (step.length != gate) {
                step.length = gate;
                changed = true;
            }

            // A slide now and then, at a quarter of LEAP's rate: a part of slides is a smear.
            const bool slide_on =
                roll(seed, track, i, d_slide, variant, 400) < int(settings.leap);
            const uint8_t flags = slide_on ? uint8_t(step.flags | kStepSlide)
                                           : uint8_t(step.flags & ~uint8_t(kStepSlide));
            if (step.flags != flags) {
                step.flags = flags;
                changed = true;
            }
        }
    }

    return changed;
}

}  // namespace strata
