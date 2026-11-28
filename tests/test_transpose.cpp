// TRANSPOSE (core/transpose.hpp): its arithmetic, then the engine moving exactly the tracks that
// fold, every way a note reaches a machine (a step, a chord, a pad, a held key and its release,
// an arp), and a LOCK holding it. The gesture is in tests/test_ui.cpp.
#include "check.hpp"

#include "apprig.hpp"
#include "arp.hpp"
#include "fakehost.hpp"
#include "lockstore.hpp"
#include "project.hpp"
#include "scale.hpp"
#include "track.hpp"
#include "transpose.hpp"
#include "vapoly.hpp"

#include <cstring>

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::pump;
using strata::test::put_trig;

namespace {

Engine &eng() { return strata::test::engine(); }
FakeHost &host() { return FakeHost::the(); }

void post(Cmd cmd, uint8_t track = 0, uint8_t step = 0, uint8_t a = 0, uint8_t b = 0,
          uint8_t c = 0) {
    strata::test::post(cmd, track, step, a, b, c);
}

Step &raw_step(int track, int step) {
    return eng().project_for_load().live().tracks[track].steps[step];
}

// Semitones unless `in_key`: most engine tests here are about WHICH notes move, which is easiest
// to read in semitones; the in-key ones say so.
void set_transpose(int amount, bool in_key = false) {
    post(Cmd::set_transpose, 0, 0, uint8_t(int8_t(amount)), in_key ? 0 : 1);
}

// A fresh engine in C minor, the transpose at rest and MIDI OUT's log empty.
void start() {
    fresh();
    post(Cmd::set_root, 0, 0, 0);
    post(Cmd::set_scale, 0, 0, uint8_t(Scale::minor));
    set_transpose(0);
    pump(2);
    host().clear_midi();
}

// One step at tick `tick`, frame 0, on a running clock.
void play_tick(uint32_t tick) {
    host().set_running(true);
    host().deliver_tick(tick, 0);
    pump();
}

void stop_and_settle() {
    host().set_running(false);
    post(Cmd::stop, 0, 0, 1);
    pump(2);
    CHECK(strata::test::settle());
}

int ons(int channel, int note = -1) { return host().midi_messages(0x90, channel, note); }
int offs(int channel, int note = -1) { return host().midi_messages(0x80, channel, note); }

const VaPoly &poly_on(int track) { return *static_cast<const VaPoly *>(eng().machine(track)); }

}  // namespace

// --- the arithmetic ---------------------------------------------------------------------------

TEST(transpose_turns_a_semitone_a_click_and_stops_at_an_octave) {
    CHECK_EQ(int(transpose_turn(0, 1)), 1);
    CHECK_EQ(int(transpose_turn(0, -1)), -1);
    CHECK_EQ(int(transpose_turn(11, 1)), 12);
    CHECK_EQ(int(transpose_turn(12, 1)), 12);
    CHECK_EQ(int(transpose_turn(-12, -1)), -12);
    CHECK_EQ(int(transpose_turn(3, 0)), 3);
    // A damaged byte comes back inside the range.
    CHECK_EQ(int(transpose_turn(int8_t(100), 0)), 12);
    CHECK_EQ(int(transpose_turn(int8_t(-100), 0)), -12);
}

TEST(transpose_moves_exactly_the_tracks_that_fold) {
    for (int m = 0; m < kMachines; m++) {
        for (int slices = 1; slices <= 16; slices++) {
            CHECK_EQ(transpose_moves(uint8_t(m), slices), machine_folds_note(uint8_t(m), slices));
        }
    }
    CHECK(transpose_moves(uint8_t(MachineId::va_mono), 1));
    CHECK(transpose_moves(uint8_t(MachineId::fm), 1));
    CHECK(transpose_moves(uint8_t(MachineId::sample), 1));
    CHECK(!transpose_moves(uint8_t(MachineId::sample), 2));
    CHECK(!transpose_moves(uint8_t(MachineId::kick), 1));
    CHECK(!transpose_moves(uint8_t(MachineId::midi), 1));
}

TEST(transpose_note_sits_on_the_edges_and_the_root_goes_round) {
    CHECK_EQ(int(transpose_note(60, 3)), 63);
    CHECK_EQ(int(transpose_note(60, -12)), 48);
    CHECK_EQ(int(transpose_note(125, 12)), 127);
    CHECK_EQ(int(transpose_note(5, -12)), 0);
    CHECK_EQ(int(transpose_root(9, 3)), 0);       // A + 3 = C
    CHECK_EQ(int(transpose_root(0, -1)), 11);     // C - 1 = B
    CHECK_EQ(int(transpose_root(11, 12)), 11);
    CHECK_EQ(int(transpose_root(0, -12)), 0);
    CHECK_EQ(int(transpose_root(14, 0)), 2);      // a damaged root, as its pitch class
}

// Folding into the key and then moving is the same as moving and folding into the moved key:
// the claim the engine's "after the fold" rests on, checked for every note, root, scale and
// shift away from the MIDI range's edges.
TEST(transpose_after_the_fold_is_the_fold_in_the_moved_key) {
    for (int scale = 0; scale < kScaleCount; scale++) {
        for (uint8_t root = 0; root < 12; root++) {
            for (int semis = -kTransposeRange; semis <= kTransposeRange; semis++) {
                for (int note = 24; note < 104; note++) {
                    const int after = transpose_note(scale_fold(uint8_t(note), root, scale), semis);
                    const int moved = scale_fold(uint8_t(note + semis), transpose_root(root, semis),
                                                 scale);
                    CHECK_EQ(after, moved);
                }
            }
        }
    }
}

TEST(transpose_text_names_the_key_in_sharps_or_says_in_key) {
    char text[kTransposeTextMax];
    transpose_text(3, true, 9, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE +3 C") == 0);
    transpose_text(-1, true, 9, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE -1 G#") == 0);
    transpose_text(0, true, 9, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE 0") == 0);
    transpose_text(-12, true, 1, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE -12 C#") == 0);
    transpose_text(2, false, 9, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE +2 IN KEY") == 0);
    transpose_text(0, false, 9, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE 0") == 0);
    transpose_text(-12, false, 9, text, sizeof text);
    CHECK(std::strcmp(text, "TRANSPOSE -12 IN KEY") == 0);
    CHECK(std::strlen(text) < size_t(kTransposeTextMax));
    CHECK(std::strlen(text) <= 21u);              // one screen line
    transpose_text(5, true, 0, text, 4);          // a short buffer is cut, never overrun
    CHECK(std::strcmp(text, "TRA") == 0);

    char brief[kTransposeShortMax];
    transpose_short(3, brief, sizeof brief);
    CHECK(std::strcmp(brief, "TR+3") == 0);
    transpose_short(-12, brief, sizeof brief);
    CHECK(std::strcmp(brief, "TR-12") == 0);
    transpose_short(0, brief, sizeof brief);
    CHECK(brief[0] == 0);
}

// In scale steps a note in the key walks the scale; one outside keeps its distance to the note it
// folds to; a step under CHROMATIC is a semitone.
TEST(transpose_in_key_walks_the_scale) {
    const int minor = int(Scale::minor);
    // A minor (root 9): A3 = 57. Two steps down is F3, two up C4, seven up A4.
    CHECK_EQ(int(transpose_in_key(57, -2, 9, minor)), 53);
    CHECK_EQ(int(transpose_in_key(57, 2, 9, minor)), 60);
    CHECK_EQ(int(transpose_in_key(57, 7, 9, minor)), 69);
    CHECK_EQ(int(transpose_in_key(57, 0, 9, minor)), 57);
    // A# (58) is not in A minor: it folds down to A, which moves, and keeps its semitone.
    CHECK_EQ(int(transpose_in_key(58, 2, 9, minor)), 61);
    CHECK_EQ(int(transpose_in_key(60, 3, 0, int(Scale::chromatic))), 63);
    // Every in-key note stays in the key, for every scale, root and amount.
    for (int scale = 0; scale < kScaleCount; scale++) {
        for (uint8_t root = 0; root < 12; root++) {
            for (int steps = -kTransposeRange; steps <= kTransposeRange; steps++) {
                for (int note = 24; note < 104; note++) {
                    if (!scale_contains(uint8_t(note), root, scale)) continue;
                    const uint8_t moved = transpose_in_key(uint8_t(note), steps, root, scale);
                    CHECK(scale_contains(moved, root, scale));
                    CHECK_EQ(int(transpose_apply(uint8_t(note), steps, false, root, scale)),
                             int(moved));
                }
            }
        }
    }
    CHECK_EQ(int(transpose_apply(60, 3, true, 9, minor)), 63);
}

// --- the engine -------------------------------------------------------------------------------

// The default, in the key: C minor's tonic triad two steps up is the triad on E-flat, the key's
// III, every note still C minor's. And a line: E folds to D#, and D# two steps up is G.
TEST(transpose_engine_in_key_keeps_every_note_in_the_scale) {
    start();
    constexpr uint8_t kPolyTrack = 6, kMonoTrack = 5;
    post(Cmd::set_machine, kPolyTrack, 0, uint8_t(MachineId::va_poly));
    post(Cmd::set_machine, kMonoTrack, 0, uint8_t(MachineId::va_mono));
    put_trig(kPolyTrack, 0);
    raw_step(kPolyTrack, 0).note = 60;
    raw_step(kPolyTrack, 0).chord = uint8_t(ChordType::major);
    put_trig(kMonoTrack, 0);
    raw_step(kMonoTrack, 0).note = 64;                     // E, folds to D# (63)
    set_transpose(2, true);
    play_tick(0);
    CHECK(!eng().transpose_semitones());
    CHECK_EQ(poly_on(kPolyTrack).busy_voices(), 3);
    CHECK_EQ(poly_on(kPolyTrack).voice_note(0), 63);       // C D# G -> D# G A#
    CHECK_EQ(poly_on(kPolyTrack).voice_note(1), 67);
    CHECK_EQ(poly_on(kPolyTrack).voice_note(2), 70);
    CHECK_EQ(int(eng().sent_note(kMonoTrack)), 67);        // D# two steps up is G
    stop_and_settle();
}

// The same step on a VA MONO, a KICK and a sliced SAMPLE: the line moves, the kick and the
// slices do not.
TEST(transpose_engine_moves_a_melodic_step_and_leaves_drums_and_slices_alone) {
    start();
    constexpr uint8_t kMonoTrack = 5, kSampleTrack = 7;
    constexpr uint8_t kSlc = 3;                    // SRC slot 3 on SAMPLE (dsp/sample.cpp)
    post(Cmd::set_machine, kMonoTrack, 0, uint8_t(MachineId::va_mono));
    post(Cmd::set_machine, 0, 0, uint8_t(MachineId::kick));
    post(Cmd::set_machine, kSampleTrack, 0, uint8_t(MachineId::sample));
    post(Cmd::set_param, kSampleTrack, 0, kSlc, 127);    // sixteen slices
    const uint8_t tracks[] = {0, kMonoTrack, kSampleTrack};
    for (uint8_t t : tracks) {
        put_trig(t, 0);
        raw_step(t, 0).note = 64;                  // E: not in C minor
    }
    set_transpose(3);
    pump();
    CHECK_EQ(int(eng().transpose()), 3);
    play_tick(0);
    CHECK_EQ(int(eng().sent_note(kMonoTrack)), 63 + 3);   // folded to D#, then up three
    CHECK_EQ(int(eng().sent_note(0)), 64);                // the kick keeps its tuning
    CHECK_EQ(int(eng().sent_note(kSampleTrack)), 64);     // slice 4 stays slice 4
    // The pattern is not written.
    CHECK_EQ(int(raw_step(kMonoTrack, 0).note), 64);

    // And down, past where it was.
    set_transpose(-5);
    play_tick(1536);
    CHECK_EQ(int(eng().sent_note(kMonoTrack)), 63 - 5);
    stop_and_settle();
}

TEST(transpose_engine_moves_every_note_of_a_chord) {
    start();
    constexpr uint8_t kPolyTrack = 6;
    post(Cmd::set_machine, kPolyTrack, 0, uint8_t(MachineId::va_poly));
    put_trig(kPolyTrack, 0);
    raw_step(kPolyTrack, 0).note = 60;
    raw_step(kPolyTrack, 0).chord = uint8_t(ChordType::major);
    set_transpose(2);
    play_tick(0);
    // C minor's tonic triad (core/chord.hpp folds the stack), each note up a whole tone: D minor.
    CHECK_EQ(poly_on(kPolyTrack).busy_voices(), 3);
    CHECK_EQ(poly_on(kPolyTrack).voice_note(0), 62);
    CHECK_EQ(poly_on(kPolyTrack).voice_note(1), 65);
    CHECK_EQ(poly_on(kPolyTrack).voice_note(2), 69);
    stop_and_settle();
}

// A held key takes the transpose of its press, and its release lets go of that pitch even after
// the transpose has moved: otherwise the note would hang. MIDI OUT shows both halves.
TEST(transpose_engine_a_held_key_is_let_go_at_the_pitch_it_sounded) {
    start();
    constexpr uint8_t kMonoTrack = 5;
    post(Cmd::set_machine, kMonoTrack, 0, uint8_t(MachineId::va_mono));
    post(Cmd::set_midi_out, kMonoTrack, 0, 3);
    pump(2);
    host().clear_midi();
    set_transpose(4);
    post(Cmd::trigger_note, kMonoTrack, 0, 60, 100, 1);     // a KEYS key down
    pump();
    CHECK_EQ(ons(3, 64), 1);
    set_transpose(-2);
    post(Cmd::release_note, kMonoTrack, 0, 60);
    pump();
    CHECK_EQ(offs(3, 64), 1);
    CHECK_EQ(offs(3, 58), 0);

    // The next press takes the new transpose.
    post(Cmd::trigger_note, kMonoTrack, 0, 60, 100, 1);
    pump();
    CHECK_EQ(ons(3, 58), 1);
    post(Cmd::release_note, kMonoTrack, 0, 60);
    pump();
    CHECK_EQ(offs(3, 58), 1);

    // A pad on a drum is not moved.
    post(Cmd::set_midi_out, 0, 0, 4);
    post(Cmd::set_machine, 0, 0, uint8_t(MachineId::kick));
    pump(2);
    post(Cmd::trigger_note, 0, 0, 40, 100, 0);
    pump();
    CHECK_EQ(ons(4, 40), 1);
    stop_and_settle();
}

// An arp's hits move with the transpose of the moment: a run already going follows a change.
TEST(transpose_engine_moves_an_arps_hits) {
    start();
    constexpr uint8_t kMonoTrack = 5;
    post(Cmd::set_machine, kMonoTrack, 0, uint8_t(MachineId::va_mono));
    post(Cmd::set_midi_out, kMonoTrack, 0, 2);
    // The arp on, UP: MOD is the ARP page's first byte, six zones (core/arp.hpp).
    post(Cmd::set_param, kMonoTrack, 0, uint8_t(kArpFirst),
         uint8_t((int(ArpMode::up) * 128 + int(ArpMode::count) - 1) / int(ArpMode::count)));
    put_trig(kMonoTrack, 0);
    raw_step(kMonoTrack, 0).note = 60;
    raw_step(kMonoTrack, 0).length = 64;
    pump(2);
    host().clear_midi();
    set_transpose(7);
    play_tick(0);
    CHECK_EQ(ons(2, 67), 1);
    CHECK_EQ(ons(2, 60), 0);
    stop_and_settle();
}

TEST(transpose_engine_a_lock_holds_it) {
    start();
    set_transpose(5);
    pump();
    post(Cmd::lock_store);
    pump();
    set_transpose(-3);
    pump();
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(eng().transpose()), 5);
    post(Cmd::lock_tap);                           // swapped back
    pump();
    CHECK_EQ(int(eng().transpose()), -3);

    LiveState a{};
    LiveState b{};
    CHECK(live_state_same(a, b));
    b.transpose = 1;
    CHECK(!live_state_same(a, b));
    set_transpose(0);
    pump();
}

TEST(transpose_engine_a_byte_out_of_range_is_held_to_it) {
    start();
    set_transpose(100);
    pump();
    CHECK_EQ(int(eng().transpose()), kTransposeRange);
    set_transpose(0);
    pump();
}
