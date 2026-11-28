// The mod slots' rules (core/modslot.hpp): sources, destinations, sanitizing, and the move.

#include "check.hpp"

#include "lfo.hpp"       // lfo_apply and the cutoff floors, which a slot follows
#include "modslot.hpp"
#include "project.hpp"
#include "track.hpp"     // dsp's page numbering, which core/modslot.cpp states again

using namespace strata;

namespace {

bool same(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

}  // namespace

TEST(mod_sources_are_named_and_a_push_walks_them_round) {
    CHECK(same(mod_source_name(kModSrcOff), "OFF"));
    CHECK(same(mod_source_name(kModSrcLfo), "LFO"));
    CHECK(same(mod_source_name(kModSrcEnv), "ENV"));
    CHECK(same(mod_source_name(kModSrcVel), "VEL"));
    CHECK(same(mod_source_name(kModSrcPrs), "PRS"));
    CHECK(same(mod_source_name(kModSrcRnd), "RND"));
    CHECK(same(mod_source_name(kModSrcCount), "OFF"));
    CHECK(same(mod_source_name(200), "OFF"));
    // PERMANENT: KIT.BIN stores these.
    CHECK_EQ(int(kModSrcLfo), 1);
    CHECK_EQ(int(kModSrcEnv), 2);
    CHECK_EQ(int(kModSrcVel), 3);
    CHECK_EQ(int(kModSrcPrs), 4);
    CHECK_EQ(int(kModSrcRnd), 5);
    uint8_t s = kModSrcOff;
    const uint8_t walk[] = {kModSrcLfo, kModSrcEnv, kModSrcVel, kModSrcPrs, kModSrcRnd, kModSrcOff, kModSrcLfo};
    for (uint8_t want : walk) {
        s = mod_source_next(s);
        CHECK_EQ(int(s), int(want));
    }
    CHECK_EQ(int(mod_source_next(99)), int(kModSrcOff));
}

TEST(mod_destinations_are_the_sound_asr_and_osc_never_the_lfo_or_arp) {
    // core/modslot.cpp's numbering against dsp's.
    for (int p = 0; p < kParams; p++) CHECK(mod_dest_allowed(p));
    for (int p = kParams; p < kAsrFirst; p++) CHECK(!mod_dest_allowed(p));
    for (int p = kAsrFirst; p < kOscEnd; p++) CHECK(mod_dest_allowed(p));
    // Nor the hole, nor SCN: a scan is a movement of its own (core/modslot.cpp).
    for (int p = kOscEnd; p < kTrackParams; p++) CHECK(!mod_dest_allowed(p));
    CHECK(!mod_dest_allowed(-1));
    CHECK(!mod_dest_allowed(kTrackParams));
    CHECK_EQ(kKitParams, kTrackParams);
}

TEST(mod_slot_sanitized_turns_damage_off) {
    KitModSlot slot = mod_slot_sanitized(kModSrcVel, 5, uint8_t(int8_t(-40)));
    CHECK(slot.on());
    CHECK_EQ(int(slot.source), int(kModSrcVel));
    CHECK_EQ(int(slot.dest), 5);
    CHECK_EQ(int(slot.depth), -40);
    CHECK_EQ(int(mod_slot_sanitized(kModSrcVel, 5, 0x80).depth), -127);
    CHECK_EQ(int(mod_slot_sanitized(kModSrcVel, 5, 127).depth), 127);
    CHECK_EQ(int(mod_slot_sanitized(kModSrcCount, 5, 10).source), int(kModSrcOff));
    CHECK_EQ(int(mod_slot_sanitized(kModSrcLfo, kLfoDepth, 10).source), int(kModSrcOff));
    CHECK_EQ(int(mod_slot_sanitized(kModSrcLfo, kArpFirst, 10).source), int(kModSrcOff));
    CHECK_EQ(int(mod_slot_sanitized(kModSrcLfo, 40, 10).source), int(kModSrcOff));
    // OFF is OFF all through: no destination or depth left behind to show.
    slot = mod_slot_sanitized(kModSrcOff, 5, 30);
    CHECK_EQ(int(slot.dest), 0);
    CHECK_EQ(int(slot.depth), 0);
    // A slot with no depth moves nothing, so it is not on.
    CHECK(!mod_slot_sanitized(kModSrcVel, 5, 0).on());
}

TEST(mod_depth_is_where_the_knob_was_left_less_where_it_started) {
    CHECK_EQ(int(mod_depth_from_turn(40, 80)), 40);
    CHECK_EQ(int(mod_depth_from_turn(80, 40)), -40);
    CHECK_EQ(int(mod_depth_from_turn(0, 127)), 127);
    CHECK_EQ(int(mod_depth_from_turn(127, 0)), -127);
    CHECK_EQ(int(mod_depth_from_turn(64, 64)), 0);
}

TEST(mod_apply_moves_by_value_times_depth_rounded_and_held) {
    CHECK_EQ(int(mod_apply(64, 0.0f, 100)), 64);
    CHECK_EQ(int(mod_apply(64, 1.0f, 40)), 104);
    CHECK_EQ(int(mod_apply(64, -1.0f, 40)), 24);
    CHECK_EQ(int(mod_apply(64, 0.5f, 41)), 85);           // 20.5 rounds away from zero
    CHECK_EQ(int(mod_apply(64, -0.5f, 41)), 43);
    CHECK_EQ(int(mod_apply(64, 1.0f, -40)), 24);           // a negative depth turns it down
    CHECK_EQ(int(mod_apply(100, 1.0f, 127)), 127);
    CHECK_EQ(int(mod_apply(20, -1.0f, 127)), 0);
    // A depth is in counts: at value 1 it is exactly where the learning knob was left.
    for (int start = 0; start < 128; start += 9) {
        for (int end = 0; end < 128; end += 7) {
            const int8_t depth = mod_depth_from_turn(uint8_t(start), uint8_t(end));
            CHECK_EQ(int(mod_apply(uint8_t(start), 1.0f, depth)), end);
        }
    }
}

TEST(mod_apply_stops_at_the_cutoff_floor_but_never_lifts_the_knob) {
    const uint8_t floor = kLfoInsertCutoffFloor;
    CHECK_EQ(int(mod_apply(80, -1.0f, 127, floor)), int(floor));
    CHECK_EQ(int(mod_apply(80, -1.0f, 20, floor)), 60);
    // A knob already under the floor stays where it is; the slot only takes it no lower.
    CHECK_EQ(int(mod_apply(10, -1.0f, 50, floor)), 10);
    CHECK_EQ(int(mod_apply(10, 1.0f, 50, floor)), 60);
    // The same rule as the LFO's, which the slot sits beside.
    CHECK_EQ(int(mod_apply(10, -1.0f, 50, floor)), int(lfo_apply(10, -1.0f, 127, floor, 50.0f)));
}

TEST(mod_pitch_and_velocity_values) {
    CHECK_EQ(mod_pitch_semis(1.0f, 64), 24.0f);            // 64 counts of TUN: two octaves
    CHECK_EQ(mod_pitch_semis(-1.0f, 8), -3.0f);
    CHECK_EQ(mod_pitch_semis(0.0f, 127), 0.0f);
    CHECK_EQ(mod_velocity_value(127), 1.0f);
    CHECK_EQ(mod_velocity_value(0), 0.0f);
    CHECK_EQ(mod_velocity_value(200), 1.0f);
}

// --- in the engine (app/engine.cpp, run_mod_slots) --------------------------------------------

#include "apprig.hpp"
#include "fakehost.hpp"
#include "va.hpp"

using strata::test::fresh;
using strata::test::post;
using strata::test::pump;

namespace {

constexpr int kTrack = 1;          // the snare in the rig's kit
constexpr int kDest = 2;           // a SRC parameter: the machine re-prepares from it

void slot(int s, uint8_t source, int dest, int depth, int track = kTrack) {
    Command command;
    command.cmd = Cmd::set_mod_slot;
    command.track = uint8_t(track);
    command.a = uint8_t(s);
    command.b = source;
    command.c = uint8_t(dest);
    command.d = uint8_t(int8_t(depth));
    strata::test::engine().commands().post(command);
    pump();
}

void off(int track = kTrack) {
    slot(0, kModSrcOff, 0, 0, track);
    slot(1, kModSrcOff, 0, 0, track);
    post(Cmd::set_pressure, uint8_t(track), 0, 0);
    pump();
}

void note(uint8_t velocity, int track = kTrack) {
    post(Cmd::trigger_note, uint8_t(track), 0, 60, velocity);
}

int heard(int param = kDest, int track = kTrack) {
    return strata::test::engine().sounding(track).v[param];
}

}  // namespace

TEST(mod_slot_off_leaves_the_track_alone_and_never_counts_as_the_lfo) {
    Engine &engine = fresh();
    pump(4);
    for (int t = 0; t < kTracks; t++) CHECK(!engine.lfo_on(t));
    CHECK_EQ(heard(), 64);
    // A slot on its own moves the byte but is not the LFO: the LFO page still says LFO OFF.
    slot(0, kModSrcPrs, kDest, 40);
    post(Cmd::set_pressure, kTrack, 0, 127);
    pump(2);
    CHECK_EQ(heard(), 104);
    CHECK(!engine.lfo_on(kTrack));
    // Off again: the kit's byte, and the value reads 0.
    off();
    pump(2);
    CHECK_EQ(heard(), 64);
    CHECK_EQ(engine.mod_value(kTrack, 0), 0.0f);
}

TEST(mod_slot_vel_follows_each_notes_velocity) {
    Engine &engine = fresh();
    slot(0, kModSrcVel, kDest, 40);
    note(127);
    pump();
    CHECK_EQ(heard(), 104);                           // the full depth at 127
    note(64);
    pump();
    CHECK_EQ(heard(), 64 + 20);                       // 64/127 of 40 = 20.2
    pump(20);
    CHECK_EQ(heard(), 84);                            // held until the next note
    CHECK_EQ(engine.mod_value(kTrack, 0), mod_velocity_value(64));
    // A negative depth: loud notes turn it down.
    slot(0, kModSrcVel, kDest, -60);
    note(127);
    pump();
    CHECK_EQ(heard(), 4);
    off();
}

TEST(mod_slot_rnd_draws_a_value_per_note_and_holds_it) {
    fresh();
    slot(1, kModSrcRnd, kDest, 40);
    int seen[4] = {};
    for (int i = 0; i < 4; i++) {
        note(100);
        pump();
        seen[i] = heard();
        CHECK(seen[i] >= 24 && seen[i] <= 104);
        pump(10);
        CHECK_EQ(heard(), seen[i]);                   // held between notes
    }
    CHECK(seen[0] != seen[1] || seen[1] != seen[2] || seen[2] != seen[3]);
    off();
}

TEST(mod_slot_prs_follows_the_pressure_and_both_slots_add) {
    fresh();
    slot(0, kModSrcPrs, kDest, 30);
    slot(1, kModSrcVel, kDest, -20);
    post(Cmd::set_pressure, kTrack, 0, 127);
    note(127);
    pump();
    CHECK_EQ(heard(), 64 + 30 - 20);
    post(Cmd::set_pressure, kTrack, 0, 0);
    pump();
    CHECK_EQ(heard(), 44);
    // Another track's slots and pressure are its own.
    CHECK_EQ(heard(kDest, 0), int(strata::test::kit_value(0, kDest)));
    off();
}

TEST(mod_slot_lfo_runs_the_lfo_with_its_own_depth_at_zero) {
    Engine &engine = fresh();
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), lfo_wave_byte(LfoWave::square));
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoRate), 63);
    slot(0, kModSrcLfo, kDest, 50);
    bool high = false;
    bool low = false;
    for (int i = 0; i < 60; i++) {
        pump();
        CHECK(!engine.lfo_on(kTrack));               // DEP 0: the LFO moves nothing itself
        const int v = heard();
        CHECK(v == 114 || v == 14);
        high = high || v == 114;
        low = low || v == 14;
        CHECK_EQ(engine.mod_value(kTrack, 0), engine.lfo_value(kTrack));
    }
    CHECK(high && low);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), 0);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoRate), 0);
    off();
}

TEST(mod_slot_env_runs_the_lfo_shape_once_per_note_whatever_trig_says) {
    fresh();
    // SAW at 32 Hz: a 31 ms decay from the top. The LFO itself is off and FREE.
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), lfo_wave_byte(LfoWave::saw));
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoRate), 63);
    slot(0, kModSrcEnv, kDest, 63);
    pump(10);
    CHECK_EQ(heard(), 64);                            // no note yet: at rest
    note(100);
    pump();
    CHECK_EQ(heard(), 127);
    int last = 127;
    for (int i = 0; i < 40; i++) {
        pump();
        CHECK(heard() <= last);
        last = heard();
    }
    CHECK_EQ(heard(), 64);
    note(100);
    pump();
    CHECK_EQ(heard(), 127);                           // and again on the next note
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoWave), 0);
    post(Cmd::set_param, kTrack, 0, uint8_t(kLfoRate), 0);
    off();
}

TEST(mod_slot_on_a_cutoff_stops_at_the_floor_and_on_a_synths_tun_glides) {
    Engine &engine = fresh();
    const int cut = page_first(Page::fltr) + 1;       // the snare's insert cutoff
    post(Cmd::set_param, kTrack, 0, uint8_t(cut), 64);
    slot(0, kModSrcPrs, cut, -127);
    post(Cmd::set_pressure, kTrack, 0, 127);
    pump(2);
    CHECK_EQ(heard(cut), int(kLfoInsertCutoffFloor));
    off();

    constexpr int kVa = 5;                            // VA MONO in the rig's kit
    CHECK_EQ(int(engine.machine_id(kVa)), int(MachineId::va_mono));
    slot(0, kModSrcPrs, 0, 32, kVa);
    post(Cmd::set_pressure, kVa, 0, 64);
    pump(2);
    CHECK_EQ(heard(0, kVa), 64);                      // the byte stays on the knob
    const float bend = mod_pitch_semis(mod_velocity_value(64), 32);
    CHECK_EQ(engine.lfo_pitch_mod(kVa), bend);
    const auto *va = static_cast<const VaMono *>(engine.machine(kVa));
    CHECK_EQ(va->settings().pitch_mod, bend);
    off(kVa);
    pump(2);
    CHECK_EQ(va->settings().pitch_mod, 0.0f);
}

TEST(mod_slot_command_refuses_a_bad_slot_and_sanitizes_the_rest) {
    Engine &engine = fresh();
    slot(2, kModSrcVel, kDest, 40);                   // there is no third slot
    for (int s = 0; s < kModSlots; s++) {
        CHECK(!engine.project().kit.tracks[kTrack].mod[s].on());
    }
    slot(0, kModSrcVel, kLfoDepth, 40);               // the LFO's own byte: OFF
    CHECK(!engine.project().kit.tracks[kTrack].mod[0].on());
    slot(0, 9, kDest, 40);                            // no such source: OFF
    CHECK(!engine.project().kit.tracks[kTrack].mod[0].on());
    slot(1, kModSrcEnv, kAsrFirst, -128);             // ASR is a destination; -128 is -127
    const KitModSlot &s1 = engine.project().kit.tracks[kTrack].mod[1];
    CHECK(s1.on());
    CHECK_EQ(int(s1.dest), kAsrFirst);
    CHECK_EQ(int(s1.depth), -127);
    off();
}
