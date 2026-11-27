// The arp's kit settings in the engine (core/arp.hpp: VAR, RHYTHM, ACCENT, RATCHET) and the
// factory arps' command (Cmd::arp_preset, core/arppresets.hpp).
//
// Heard through MIDI OUT on a MID track: every arp hit is a note-on there with its note and its
// velocity (127 for an accent), which is exactly what these settings change. core/arp.hpp's
// tables have their own tests in tests/test_arp.cpp; these pin what the engine does with them.
#include "check.hpp"
#include "apprig.hpp"
#include "fakehost.hpp"

#include "arp.hpp"
#include "arppresets.hpp"
#include "engine.hpp"

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::pump;
using strata::test::put_trig;

namespace {

constexpr uint8_t kMid = uint8_t(MachineId::midi);
constexpr int kChannel = 4;
constexpr uint8_t kNoteOn = 0x90 | (kChannel - 1);

Engine &eng() { return strata::test::engine(); }
FakeHost &host() { return FakeHost::the(); }

void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0,
          uint8_t c = 0) {
    strata::test::post(cmd, track, step, a, b, c);
}

// Track 0 on MID, sending on channel 4, the MIDI log empty and the clock stopped.
void start() {
    fresh();
    host().set_running(false);
    post(Cmd::set_machine, 0, 0, kMid);
    post(Cmd::set_midi_out, 0, 0, kChannel);
    pump(2);
    host().clear_midi();
}

// The ARP page's four bytes, at the bottom of each zone (as tests/test_engine.cpp's set_arp).
void set_arp(int mode, int rate, int octaves = 1) {
    auto smallest = [](int zone, int zones) { return uint8_t((zone * 128 + zones - 1) / zones); };
    post(Cmd::set_param, 0, 0, uint8_t(kArpFirst + 0), smallest(mode, int(ArpMode::count)));
    post(Cmd::set_param, 0, 0, uint8_t(kArpFirst + 1), smallest(rate, kArpRates));
    post(Cmd::set_param, 0, 0, uint8_t(kArpFirst + 2), smallest(octaves - 1, kArpMaxOctaves));
    post(Cmd::set_param, 0, 0, uint8_t(kArpFirst + 3), 0);
    pump();
}

void set_ext(int field, int value) {
    post(Cmd::set_arp_ext, 0, 0, uint8_t(field), uint8_t(value));
    pump();
}

// The note-ons sent so far, in order: notes and velocities.
int note_ons(uint8_t *notes, uint8_t *velocities, int max) {
    int n = 0;
    for (int i = 0; i < host().midi_count() && n < max; i++) {
        const FakeHost::MidiSent &m = host().midi_sent(i);
        if (m.status != kNoteOn || m.b == 0) continue;
        if (notes != nullptr) notes[n] = m.a;
        if (velocities != nullptr) velocities[n] = m.b;
        n++;
    }
    return n;
}

// Hold `count` KEYS notes (they start a held run), then run the clock from tick 1 to `last`,
// one block a tick.
void hold_and_run(const uint8_t *keys, int count, uint32_t last) {
    host().set_running(true);
    host().deliver_tick(0, 0);
    pump();
    for (int i = 0; i < count; i++) post(Cmd::trigger_note, 0, 0, keys[i], 100, 1);
    pump();
    for (uint32_t t = 1; t <= last; t++) {
        host().deliver_tick(t, 0);
        pump();
    }
}

void release(const uint8_t *keys, int count) {
    for (int i = 0; i < count; i++) post(Cmd::release_note, 0, 0, keys[i]);
    pump();
    host().set_running(false);
    pump(4);
}

constexpr uint8_t kTwo[2] = {60, 64};
// Lowest first: a held run hits its first key alone, before the others join its cycle.
constexpr uint8_t kThree[3] = {60, 64, 67};

}  // namespace

TEST(arpkit_all_four_zero_is_the_arp_as_it_was) {
    start();
    set_arp(1, 3);                                     // UP, 1/16: every 24 ticks
    hold_and_run(kTwo, 2, 96);                         // hits on 0 24 48 72 96
    uint8_t notes[16], vel[16];
    CHECK_EQ(note_ons(notes, vel, 16), 5);
    const uint8_t want[5] = {60, 64, 60, 64, 60};
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(int(notes[i]), int(want[i]));
        CHECK_EQ(int(vel[i]), 100);                    // no accent
    }
    release(kTwo, 2);
}

TEST(arpkit_a_rhythm_rests_its_slots_and_the_note_waits_for_the_next_hit) {
    start();
    set_arp(1, 3);
    set_ext(1, 1);                                     // RHYTHM X-: every other slot
    hold_and_run(kTwo, 2, 96);
    uint8_t notes[16];
    // Slots 0 2 4 play, 1 and 3 rest -- and a rest does not use the note up: 60 64 60.
    CHECK_EQ(note_ons(notes, nullptr, 16), 3);
    CHECK_EQ(int(notes[0]), 60);
    CHECK_EQ(int(notes[1]), 64);
    CHECK_EQ(int(notes[2]), 60);
    release(kTwo, 2);

    // XX-: two and a rest; over 2 notes the melody shifts against the rhythm.
    start();
    set_arp(1, 3);
    set_ext(1, 2);
    hold_and_run(kTwo, 2, 24 * 6);                     // slots 0-6: rests on 2 and 5
    CHECK_EQ(note_ons(notes, nullptr, 16), 5);
    release(kTwo, 2);
}

TEST(arpkit_accents_land_on_their_slots_at_full_velocity) {
    start();
    set_arp(1, 3);
    set_ext(2, 1);                                     // ACCENT X-
    hold_and_run(kTwo, 2, 96);
    uint8_t vel[16];
    CHECK_EQ(note_ons(nullptr, vel, 16), 5);
    for (int i = 0; i < 5; i++) CHECK_EQ(int(vel[i]), i % 2 == 0 ? 127 : 100);
    release(kTwo, 2);

    // CYC: the first note of each cycle, three notes long here.
    start();
    set_arp(1, 3);
    set_ext(2, kArpAccents - 1);
    hold_and_run(kThree, 3, 24 * 6);
    CHECK_EQ(note_ons(nullptr, vel, 16), 7);
    for (int i = 0; i < 7; i++) CHECK_EQ(int(vel[i]), i % 3 == 0 ? 127 : 100);
    release(kThree, 3);
}

TEST(arpkit_var_reshapes_a_held_run_and_follows_a_change_while_held) {
    start();
    set_arp(1, 3);
    set_ext(0, int(ArpVar::thumb));
    hold_and_run(kThree, 3, 24 * 3);                   // C E C G
    uint8_t notes[32];
    CHECK_EQ(note_ons(notes, nullptr, 32), 4);
    const uint8_t thumb[4] = {60, 64, 60, 67};
    for (int i = 0; i < 4; i++) CHECK_EQ(int(notes[i]), int(thumb[i]));

    // PINKY pushed while the keys are down: the run rebuilds at its next tick.
    host().clear_midi();
    set_ext(0, int(ArpVar::pinky));
    for (uint32_t t = 24 * 3 + 1; t <= 24 * 7; t++) {
        host().deliver_tick(t, 0);
        pump();
    }
    const int n = note_ons(notes, nullptr, 32);
    CHECK_EQ(n, 4);
    // The cycle is C G E G now: G every other note.
    int gs = 0;
    for (int i = 0; i < n; i++) gs += notes[i] == 67;
    CHECK_EQ(gs, 2);
    release(kThree, 3);
}

TEST(arpkit_a_ratchet_repeats_the_note_inside_its_slot_stopped_or_running) {
    // Stopped: a held run counts frames. 1/16 at 120 BPM is 6,000 frames; 2ALL puts a second hit
    // 3,000 frames into each.
    start();
    host().set_tempo(120.0f);
    set_arp(1, 3);
    set_ext(3, 5);                                     // RATCHET 2ALL
    post(Cmd::trigger_note, 0, 0, 60, 100, 1);
    pump();
    uint8_t notes[32], vel[32];
    CHECK_EQ(note_ons(notes, vel, 32), 1);
    const int half = int(3000.0f / float(strata::test::kRigBlock));
    for (int b = 0; b < half - 2; b++) pump();
    CHECK_EQ(note_ons(notes, vel, 32), 1);             // not yet
    pump(4);
    CHECK_EQ(note_ons(notes, vel, 32), 2);             // the repeat, on time
    CHECK_EQ(int(notes[1]), 60);                       // the same note
    CHECK_EQ(int(vel[1]), 100);                        // and never accented
    for (int b = 0; b < 2 * half; b++) pump();
    CHECK_EQ(note_ons(notes, vel, 32), 4);             // the next slot and its repeat
    post(Cmd::release_note, 0, 0, 60);
    pump(400);
    CHECK_EQ(note_ons(notes, vel, 32), 4);             // and nothing after the key is up

    // Running, at a tempo where a tick is exactly four blocks: a step at 1/16 with 2:4 ratchets
    // its fourth slot only. Four steps long: slots on 0 24 48 72, the repeat 12 ticks into 72.
    start();
    host().set_tempo(48000.0f * 60.0f / (96.0f * 4.0f * float(strata::test::kRigBlock)));
    set_arp(1, 3);
    set_ext(3, 1);                                     // RATCHET 2:4
    put_trig(0, 0);
    const int length_now = int(eng().project_for_load().live().tracks[0].steps[0].length);
    post(Cmd::step_field_delta, 0, 0, uint8_t(StepField::length),
         uint8_t(int8_t(12 - length_now)));                                // four steps
    pump();
    host().clear_midi();
    host().set_running(true);
    for (uint32_t t = 0; t < 96; t++) {
        host().deliver_tick(t, 0);
        pump(4);
    }
    CHECK_EQ(note_ons(notes, nullptr, 32), 5);
    host().set_running(false);
    pump(4);
}

TEST(arpkit_settings_are_the_kits_and_a_value_past_a_table_is_off) {
    start();
    set_ext(0, int(ArpVar::converge));
    set_ext(1, 7);
    set_ext(2, 99);
    CHECK_EQ(int(eng().project().kit.tracks[0].arp.var), int(ArpVar::converge));
    CHECK_EQ(int(eng().project().kit.tracks[0].arp.rhythm), 7);
    CHECK_EQ(int(eng().project().kit.tracks[0].arp.accent), 0);
    CHECK_EQ(int(eng().project().kit.tracks[1].arp.rhythm), 0);   // one track only
}

TEST(arpkit_a_preset_writes_the_arp_whole_and_as_was_puts_it_back) {
    start();
    set_arp(2, 1);                                     // DN 1/8: the track's own arp
    set_ext(1, 3);
    const uint8_t own_mod = eng().params(0).v[kArpFirst];
    const uint8_t own_cut = eng().params(0).v[5];
    post(Cmd::arp_preset, 0, 0, 0, kSoundPresetKeep);
    for (int i = 0; i < arp_preset_count(); i++) {
        post(Cmd::arp_preset, 0, 0, 0, uint8_t(i));
        pump();
        CHECK(arp_preset_matches(i, &eng().params(0).v[kArpFirst], eng().project().kit.tracks[0].arp));
        CHECK_EQ(int(eng().params(0).v[5]), int(own_cut));          // the sound untouched
    }
    post(Cmd::arp_preset, 0, 0, 0, kSoundPresetBack);
    pump();
    CHECK_EQ(int(eng().params(0).v[kArpFirst]), int(own_mod));
    CHECK_EQ(int(eng().project().kit.tracks[0].arp.rhythm), 3);

    // AS WAS for another track than the one remembered, and a preset that does not exist, are
    // refused and change nothing.
    const uint32_t refusals = eng().status().refused.load();
    post(Cmd::arp_preset, 1, 0, 0, kSoundPresetBack);
    post(Cmd::arp_preset, 0, 0, 0, 200);
    pump();
    CHECK_EQ(eng().status().refused.load(), refusals + 2);
    CHECK_EQ(int(eng().params(0).v[kArpFirst]), int(own_mod));

    // A preset plays: TRANCE on a held note is an UP run at 1/16.
    host().clear_midi();
    post(Cmd::arp_preset, 0, 0, 0, 0);
    pump();
    const uint8_t one[1] = {48};
    hold_and_run(one, 1, 96);
    uint8_t notes[16];
    CHECK_EQ(note_ons(notes, nullptr, 16), 5);
    CHECK_EQ(int(notes[1]), 60);                       // two octaves up from C3
    release(one, 1);
}

TEST(arpkit_a_recalled_kit_brings_its_arp_settings) {
    // The kit library's recall (Cmd::kit_recall from the inbox) brings the ARP bytes as
    // parameters; the settings that shape them come with them, or a recalled arp would play
    // with the last kit's rhythm.
    start();
    set_ext(1, 4);
    Kit &inbox = eng().kit_for_recall();
    inbox = eng().project().kit;
    inbox.tracks[0].arp = {uint8_t(ArpVar::pinky), 9, 2, 0};
    post(Cmd::kit_recall, 0, 0, 1);
    pump();
    const KitArp &now = eng().project().kit.tracks[0].arp;
    CHECK_EQ(int(now.var), int(ArpVar::pinky));
    CHECK_EQ(int(now.rhythm), 9);
    CHECK_EQ(int(now.accent), 2);
    fresh();
}

TEST(arpkit_rnd_draws_new_notes_on_every_pass) {
    // RND was seeded by the track and step, so every pass played one fixed pattern (Roland,
    // 2026-11-27: "random is not really random"). Each run now draws anew: one step, 16 hits of
    // RND over a three-note chord over two octaves, played on two passes, differ.
    start();
    set_arp(5, 3, 2);                                  // RND, 1/16, two octaves
    const uint8_t chord[3] = {60, 64, 67};
    uint8_t first[32], second[32];
    for (int pass = 0; pass < 2; pass++) {
        host().clear_midi();
        hold_and_run(chord, 3, 24 * 16);
        uint8_t *out = pass == 0 ? first : second;
        CHECK_EQ(note_ons(out, nullptr, 32), 17);
        release(chord, 3);
    }
    int differ = 0;
    for (int i = 3; i < 17; i++) differ += first[i] != second[i];   // after the keys joined
    CHECK(differ >= 4);
}
