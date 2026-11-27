// Choke groups (CyDrums S4): KitTrack::choke, Cmd::set_choke, Engine::choke_group and the fade.
//
// What is pinned here: a note on a track in a group cuts the OTHER tracks of that group and no
// one else; the cut is a fade (strata_choke_fade), not a step; a note on the cut track in the
// same block keeps it, and two tracks of a group starting together both sound; and the command
// keeps the group inside 0-4. (A muted track needs no test of its own: muting already cuts it.)
#include "apprig.hpp"
#include "chainloops.h"
#include "check.hpp"
#include "fakehost.hpp"

using namespace strata;
using strata::test::FakeHost;
using strata::test::fresh;
using strata::test::post;
using strata::test::pump;

namespace {

// The blank kit's machines (core/project.cpp kit_clear): KICK on 1, VA MONO on 6, VA POLY on 7.
constexpr uint8_t kKick = 0;
constexpr uint8_t kSnare = 1;
constexpr uint8_t kMono = 5;
constexpr uint8_t kPoly = 6;

bool sounding(Engine &engine, int track) {
    const Machine *machine = engine.machine(track);
    return machine != nullptr && machine->active();
}

Engine &stopped() {
    Engine &engine = fresh();
    FakeHost::the().set_running(false);
    for (uint8_t t = 0; t < kTracks; t++) post(Cmd::set_choke, t, 0, 0);
    pump();
    return engine;
}

// A pad hit on `track`, applied by the next block.
void hit(uint8_t track, uint8_t note = 60) { post(Cmd::trigger_note, track, 0, note, 100); }

}  // namespace

TEST(choke_fade_ramps_down_from_its_gain_and_stops_at_zero) {
    float buf[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    // Steps BEFORE each sample, as strata_master_ramp does.
    const float left = strata_choke_fade(buf, 4, 1.0f, 0.25f);
    CHECK_EQ(buf[0], 0.75f);
    CHECK_EQ(buf[1], 0.5f);
    CHECK_EQ(buf[2], 0.25f);
    CHECK_EQ(buf[3], 0.0f);
    CHECK_EQ(left, 0.0f);
    // Past zero it holds at zero: a choke is silence, never a negative gain.
    const float more = strata_choke_fade(buf + 4, 4, 0.1f, 0.25f);
    CHECK_EQ(more, 0.0f);
    for (int i = 4; i < 8; i++) CHECK_EQ(buf[i], 0.0f);
    // Carried over from the last block: the next starts where it stopped.
    float tail[2] = {2, 2};
    CHECK_EQ(strata_choke_fade(tail, 2, 0.5f, 0.125f), 0.25f);
    CHECK_EQ(tail[0], 0.75f);
    CHECK_EQ(tail[1], 0.5f);
}

TEST(choke_a_note_cuts_the_other_tracks_of_its_group_only) {
    Engine &engine = stopped();
    post(Cmd::set_choke, kMono, 0, 1);
    post(Cmd::set_choke, kKick, 0, 1);
    post(Cmd::set_choke, kPoly, 0, 2);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kMono].choke), 1);

    hit(kMono);
    hit(kPoly);
    pump();
    CHECK(sounding(engine, kMono));
    CHECK(sounding(engine, kPoly));

    // The kick (group 1) cuts the mono (group 1) within its 64-frame fade, and leaves the poly
    // (group 2) alone.
    hit(kKick);
    pump(2);
    CHECK(sounding(engine, kKick));
    CHECK(!sounding(engine, kMono));
    CHECK(sounding(engine, kPoly));

    // A track in no group neither chokes nor is choked.
    hit(kMono);
    pump();
    CHECK(sounding(engine, kMono));
    hit(kSnare);
    pump(2);
    CHECK(sounding(engine, kMono));
}

TEST(choke_with_no_group_the_same_hits_ring_on) {
    // The control for the test above: the same notes, no groups.
    Engine &engine = stopped();
    hit(kMono);
    pump();
    hit(kKick);
    pump(2);
    CHECK(sounding(engine, kKick));
    CHECK(sounding(engine, kMono));
}

TEST(choke_is_mutual_and_the_later_note_wins) {
    Engine &engine = stopped();
    post(Cmd::set_choke, kMono, 0, 3);
    post(Cmd::set_choke, kPoly, 0, 3);
    pump();
    hit(kMono);
    pump();
    hit(kPoly);
    pump(2);
    CHECK(sounding(engine, kPoly));
    CHECK(!sounding(engine, kMono));
    hit(kMono);
    pump(2);
    CHECK(sounding(engine, kMono));
    CHECK(!sounding(engine, kPoly));
}

TEST(choke_two_tracks_of_a_group_on_the_same_frame_both_sound) {
    // A block is rendered whole, so a track cannot be cut before a frame and not after it: two
    // notes on one frame both play, whichever comes first in the block's order.
    Engine &engine = stopped();
    post(Cmd::set_choke, kMono, 0, 1);
    post(Cmd::set_choke, kKick, 0, 1);
    pump();
    hit(kKick);
    hit(kMono);
    pump(2);
    CHECK(sounding(engine, kKick));
    CHECK(sounding(engine, kMono));
}

TEST(choke_a_cut_track_keeps_no_stored_energy_for_its_next_note) {
    // A choke ends in kill_all, and a track whose machine has gone idle is not rendered, so its
    // insert chain stood still with the filter's ringing and the drive's DC blocker mid-swing in
    // it -- and played that out as a transient on the track's next note, the stored transient
    // Engine::silence_track was written to clear. A cut by a choke is a cut like any other.
    // Measured here as a kick at LVL 0 after the choke: nothing should come out.
    constexpr uint8_t kDrv = 4, kCut = 5, kRes = 6, kFilterMod = 7, kLvl = 11;
    constexpr uint8_t kSendA = 14, kSendB = 15;
    Engine &engine = stopped();
    for (uint8_t t = 0; t < kTracks; t++) {
        post(Cmd::set_param, t, 0, kSendA, 0);
        post(Cmd::set_param, t, 0, kSendB, 0);
    }
    post(Cmd::set_choke, kKick, 0, 1);
    post(Cmd::set_choke, kSnare, 0, 1);
    post(Cmd::set_param, kKick, 0, kDrv, 127);
    post(Cmd::set_param, kKick, 0, kFilterMod, 40);    // LP24
    post(Cmd::set_param, kKick, 0, kCut, 40);
    post(Cmd::set_param, kKick, 0, kRes, 127);
    pump();
    hit(kKick);
    pump(4);
    CHECK(sounding(engine, kKick));
    hit(kSnare);                                      // chokes the kick
    pump(2);
    CHECK(!sounding(engine, kKick));
    post(Cmd::set_mute, kSnare, 0, 1, 0);             // and the snare out of the way
    post(Cmd::set_param, kKick, 0, kLvl, 0);
    pump(2);

    hit(kKick);
    float total = 0.0f;
    for (int b = 0; b < 4; b++) {
        pump();
        const float *block = strata::test::last_block();
        for (int i = 0; i < strata::test::kRigStereo; i++) total += block[i] * block[i];
    }
    CHECK_NEAR(total, 0.0f, 1e-6f);
    post(Cmd::set_mute, kSnare, 0, 0, 0);
    pump();
}

TEST(choke_the_command_keeps_the_group_in_range) {
    Engine &engine = stopped();
    post(Cmd::set_choke, kSnare, 0, 4);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kSnare].choke), 4);
    post(Cmd::set_choke, kSnare, 0, 5);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kSnare].choke), 0);
    post(Cmd::set_choke, kSnare, 0, 255);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kSnare].choke), 0);

    // c = 1 steps from the kit's own byte, so two posts in one block are two steps, and 4 goes
    // round to none.
    post(Cmd::set_choke, kSnare, 0, 0, 0, 1);
    post(Cmd::set_choke, kSnare, 0, 0, 0, 1);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kSnare].choke), 2);
    post(Cmd::set_choke, kSnare, 0, 0, 0, 1);
    post(Cmd::set_choke, kSnare, 0, 0, 0, 1);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kSnare].choke), 4);
    post(Cmd::set_choke, kSnare, 0, 0, 0, 1);
    pump();
    CHECK_EQ(int(engine.project().kit.tracks[kSnare].choke), 0);
}
