// TUN and PIT as notes (core/tuning.hpp).
//
// The byte-to-pitch maps in core/tuning.cpp are copies of the machines' own, so the first tests
// hold them against the machines: a range changed in dsp/ and not in core/ names the wrong note,
// and only a test would notice. The rest pin what the screen shows and what a click in NOTE view
// does.
#include "check.hpp"

#include <enaga/abi.h>

#include <cmath>
#include <cstring>

#include "hat.hpp"
#include "kick.hpp"
#include "perc.hpp"
#include "project.hpp"
#include "snare.hpp"
#include "tuning.hpp"
#include "va.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr float kC4 = 261.62556f;

uint8_t id(MachineId m) { return uint8_t(m); }

template <typename Drum>
void check_drum_against_machine(MachineId machine) {
    Drum drum;
    drum.init(kRate);
    TrackParams p{};
    for (int v = 0; v <= 127; v++) {
        p.set(0, uint8_t(v));
        drum.prepare(p);
        const float cents = 1200.0f * std::log2(drum.base_frequency() / kC4);
        CHECK_NEAR(tune_cents(id(machine), uint8_t(v)), cents, 0.5);
    }
}

bool text_is(MachineId machine, uint8_t value, const char *want) {
    char text[kTuneTextMax];
    tune_text(id(machine), value, text, kTuneTextMax);
    if (std::strcmp(text, want) == 0) return true;
    std::printf("    byte %d: \"%s\", wanted \"%s\"\n", int(value), text, want);
    return false;
}

}  // namespace

TEST(tuning_the_drum_maps_are_the_machines_own) {
    check_drum_against_machine<Kick>(MachineId::kick);
    check_drum_against_machine<Snare>(MachineId::snare);
    check_drum_against_machine<Hat>(MachineId::hat);
    check_drum_against_machine<Perc>(MachineId::perc);
}

TEST(tuning_the_va_map_is_the_machines_own) {
    TrackParams p{};
    for (int v = 0; v <= 127; v++) {
        p.set(0, uint8_t(v));
        const VaSettings s = VaSettings::make(p, kRate, 1.0f / kRate);
        CHECK_NEAR(tune_cents(id(MachineId::va_mono), uint8_t(v)), s.tune * 100.0f, 0.01);
        CHECK_NEAR(tune_cents(id(MachineId::va_poly), uint8_t(v)), s.tune * 100.0f, 0.01);
    }
}

TEST(tuning_sample_and_grain_are_24_63_semitones_a_count) {
    CHECK_NEAR(tune_cents(id(MachineId::sample), 64), 0.0f, 0.01);
    CHECK_NEAR(tune_cents(id(MachineId::sample), 127), 2400.0f, 0.1);
    CHECK_NEAR(tune_cents(id(MachineId::grain), 1), -2400.0f, 0.1);
}

TEST(tuning_which_parameter_is_the_pitch) {
    CHECK_EQ(tune_param(id(MachineId::kick)), 0);
    CHECK_EQ(tune_param(id(MachineId::va_mono)), 0);
    CHECK_EQ(tune_param(id(MachineId::sample)), 0);
    CHECK_EQ(tune_param(id(MachineId::grain)), 8);
    CHECK_EQ(tune_param(id(MachineId::midi)), -1);
    CHECK_EQ(tune_param(200), -1);
}

TEST(tuning_names_the_note_a_c4_step_plays) {
    CHECK(text_is(MachineId::va_mono, 64, "C-4"));
    CHECK(text_is(MachineId::va_mono, 96, "C-5"));      // exactly +12
    CHECK(text_is(MachineId::va_mono, 32, "C-3"));
    CHECK(text_is(MachineId::va_mono, 72, "D#4"));      // exactly +3
    CHECK(text_is(MachineId::va_mono, 83, "G-4+"));     // +7.125: 12.5 cents sharp
    CHECK(text_is(MachineId::va_mono, 82, "G-4-"));     // +6.75: 25 cents flat
    CHECK(text_is(MachineId::va_mono, 0, "C-2"));
    CHECK(text_is(MachineId::sample, 64, "C-4"));
    CHECK(text_is(MachineId::kick, 64, "A-1"));         // 55 Hz exactly
    CHECK(text_is(MachineId::hat, 64, "G#4-"));         // 414 Hz: G#4 is 415.3
    CHECK(text_is(MachineId::perc, 64, "D-4+"));        // 300 Hz: 37 cents sharp of D-4
    // A machine with no pitch names C-4 rather than nothing; the UI never asks it.
    CHECK(text_is(MachineId::midi, 0, "C-4"));
}

TEST(tuning_text_fits_and_truncates_rather_than_overruns) {
    char text[4];
    tune_text(id(MachineId::va_mono), 83, text, 4);      // "G-4+" does not fit in 4
    CHECK(std::strcmp(text, "G-4") == 0);
    for (int m = 0; m < kMachines; m++) {
        for (int v = 0; v <= 127; v++) {
            char full[kTuneTextMax];
            tune_text(uint8_t(m), uint8_t(v), full, kTuneTextMax);
            CHECK(std::strlen(full) <= 4);              // the width the EDIT rows give it
        }
    }
}

TEST(tuning_a_click_is_one_semitone) {
    // From TUN 64 (C-4), twelve clicks up land on C-5 exactly, one note at a time.
    uint8_t v = 64;
    for (int n = 1; n <= 12; n++) {
        v = tune_step(id(MachineId::va_mono), v, +1);
        CHECK_EQ(int(tune_note(id(MachineId::va_mono), v)), 60 + n);
    }
    CHECK_EQ(int(v), 96);
    // Every pitched machine walks its whole range one note a click, both ways, and every byte
    // it lands on is as close to its note as any byte gets.
    for (int m = 0; m < kMachines; m++) {
        if (tune_param(uint8_t(m)) < 0) continue;
        uint8_t at = 0;
        int note = tune_note(uint8_t(m), at);
        for (int guard = 0; guard < 200 && at < 127; guard++) {
            const uint8_t next = tune_step(uint8_t(m), at, +1);
            CHECK(next > at);
            const int next_note = tune_note(uint8_t(m), next);
            if (next < 127) CHECK_EQ(next_note, note + 1);
            const float error = std::fabs(tune_cents(uint8_t(m), next) - float((next_note - 60) * 100));
            for (int other = 0; other <= 127 && next < 127; other++) {
                CHECK(error <= std::fabs(tune_cents(uint8_t(m), uint8_t(other)) -
                                         float((next_note - 60) * 100)) + 0.01f);
            }
            at = next;
            note = next_note;
        }
        CHECK_EQ(int(at), 127);
        for (int guard = 0; guard < 200 && at > 0; guard++) {
            const uint8_t next = tune_step(uint8_t(m), at, -1);
            CHECK(next < at);
            at = next;
        }
        CHECK_EQ(int(at), 0);
    }
}

TEST(tuning_steps_clamp_and_zero_is_no_move) {
    CHECK_EQ(int(tune_step(id(MachineId::va_mono), 127, +5)), 127);
    CHECK_EQ(int(tune_step(id(MachineId::va_mono), 0, -5)), 0);
    CHECK_EQ(int(tune_step(id(MachineId::va_mono), 70, 0)), 70);
    CHECK_EQ(int(tune_step(id(MachineId::midi), 70, +1)), 70);
    // Several clicks in one event: an octave from C-4 is C-5.
    CHECK_EQ(int(tune_step(id(MachineId::va_mono), 64, +12)), 96);
    CHECK_EQ(int(tune_step(id(MachineId::va_mono), 64, -24)), 0);
}

// From every byte, not only the ones a walk lands on: a click always moves. And a text with a
// mark keeps it when the buffer has exactly the room for it.
TEST(tuning_a_click_from_any_byte_moves_and_a_mark_that_fits_stays) {
    for (int m = 0; m < kMachines; m++) {
        if (tune_param(uint8_t(m)) < 0) continue;
        for (int v = 0; v <= 127; v++) {
            if (v < 127) CHECK(tune_step(uint8_t(m), uint8_t(v), +1) > v);
            if (v > 0) CHECK(tune_step(uint8_t(m), uint8_t(v), -1) < v);
        }
    }
    int marked = 0;
    for (int m = 0; m < kMachines; m++) {
        for (int v = 0; v <= 127; v++) {
            char full[kTuneTextMax];
            tune_text(uint8_t(m), uint8_t(v), full, kTuneTextMax);
            const int n = int(std::strlen(full));
            const char last = full[n - 1];
            if (last != '+' && last != '-') continue;
            char exact[kTuneTextMax];
            tune_text(uint8_t(m), uint8_t(v), exact, n + 1);
            CHECK(std::strcmp(exact, full) == 0);
            marked++;
        }
    }
    CHECK(marked > 0);
    char one[2] = {'x', 'x'};
    tune_text(id(MachineId::va_mono), 64, one, 1);
    CHECK_EQ(one[0], '\0');
}
