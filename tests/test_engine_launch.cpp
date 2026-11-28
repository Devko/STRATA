// Track Launch in the engine (app/engine.cpp, core/launch.hpp): queued launches landing on the
// boundary, a launched lane playing its part from its own step 1, the read-only guard, pattern
// changes and PIN, CAPTURE and the commands that drop queued launches.
#include "apprig.hpp"
#include "check.hpp"
#include "fakehost.hpp"

#include <enaga/clock.hpp>

using namespace strata;
using strata::test::FakeHost;

namespace {

using strata::test::fresh;
using strata::test::post;
using strata::test::pump;

Engine &engine() { return strata::test::engine(); }

uint8_t triggers(int track) {
    return engine().status().triggered[track].load(std::memory_order_relaxed);
}

void tick_at(uint32_t tick) {
    FakeHost::the().deliver_tick(tick, 0);
    pump();
}

int fired_on(int track, uint32_t tick) {
    const uint8_t before = triggers(track);
    tick_at(tick);
    return int(uint8_t(triggers(track) - before));
}

// A blank set: every lane on its own part, nothing queued, the transport stopped at tick 0, and
// pattern B07 holding a 16-step part on track 3 (index 2) with trigs on steps 1 and 2.
Engine &blank_set() {
    Engine &e = fresh();
    for (uint8_t t = 0; t < kTracks; t++) post(Cmd::set_mute, t, 0, 0, 0);
    post(Cmd::stop, 0, 0, 1);
    post(Cmd::cancel_queue);
    while (!e.commands().empty()) pump();
    Project &project = e.project_for_load();
    for (int t = 0; t < kTracks; t++) {
        project.settings.lane_source[t] = kLaneOwn;
        project.settings.lane_pinned = 0;
    }
    Pattern &b07 = project.patterns[1][6];
    pattern_init(b07);
    b07.tracks[2].length = 16;
    b07.tracks[2].steps[0].flags = kStepTrig;
    b07.tracks[2].steps[1].flags = kStepTrig;
    pattern_init(project.patterns[0][0]);
    pattern_init(project.patterns[0][1]);
    pattern_init(project.patterns[0][2]);
    project.bank = 0;
    project.slot = 0;
    post(Cmd::project_loaded);
    while (!e.commands().empty()) pump();
    FakeHost::the().set_running(true);
    tick_at(0);
    return e;
}

void launch(uint8_t lanes, uint8_t source, QueueMode mode) {
    post(Cmd::launch, 0, 0, lanes, source, uint8_t(mode));
    pump();
}

uint8_t launch_event() {
    return engine().status().launch_event.load(std::memory_order_relaxed);
}

}  // namespace

TEST(engine_launch_lands_at_the_end_of_the_pass_and_plays_from_step_1) {
    Engine &e = blank_set();
    tick_at(100);
    launch(0x04, lane_source(1, 6), QueueMode::end_of_pattern);
    CHECK(e.lane_queue(2).queue.pending);
    CHECK_EQ(int(e.lane_queue(2).queue.switch_tick), 1536);   // a blank pattern's 64 steps
    CHECK(!lane_borrowed(e.project(), 2));

    tick_at(1535);
    CHECK(!lane_borrowed(e.project(), 2));
    // On the boundary it lands, and its step 1 plays on that very tick.
    CHECK_EQ(fired_on(2, 1536), 1);
    CHECK(lane_borrowed(e.project(), 2));
    CHECK(!e.lane_queue(2).queue.pending);
    CHECK(&e.lane(2) == &e.project().patterns[1][6]);
    // Step 2 a sixteenth later, then nothing until its 16-step part comes round again.
    CHECK_EQ(fired_on(2, 1560), 1);
    int later = 0;
    for (uint32_t t = 1561; t < 1536 + 384; t++) later += fired_on(2, t);
    CHECK_EQ(later, 0);
    CHECK_EQ(fired_on(2, 1536 + 384), 1);
    // The other lanes still play A01, which is empty.
    CHECK(&e.lane(0) == &e.project().live());
}

TEST(engine_launch_now_lands_at_once_and_stopped_lands_at_once_too) {
    Engine &e = blank_set();
    tick_at(50);
    launch(0x01, lane_source(1, 6), QueueMode::immediate);
    CHECK(!e.lane_queue(0).queue.pending);
    CHECK_EQ(int(e.project().settings.lane_source[0]), int(lane_source(1, 6)));

    FakeHost::the().set_running(false);
    launch(0x02, lane_source(0, 2), QueueMode::end_of_pattern);
    CHECK(!e.lane_queue(1).queue.pending);
    CHECK_EQ(int(e.project().settings.lane_source[1]), int(lane_source(0, 2)));
}

TEST(engine_launch_several_lanes_land_on_the_same_boundary) {
    Engine &e = blank_set();
    tick_at(30);                                   // past the grace window of bar 1
    launch(0x0B, lane_source(1, 6), QueueMode::after_16);
    CHECK_EQ(int(e.lane_queue(0).queue.switch_tick), 384);
    CHECK_EQ(int(e.lane_queue(1).queue.switch_tick), 384);
    CHECK_EQ(int(e.lane_queue(3).queue.switch_tick), 384);
    CHECK(!e.lane_queue(2).queue.pending);
    tick_at(384);
    CHECK_EQ(int(lane_borrowed_mask(e.project())), 0x0B);
}

TEST(engine_launch_a_borrowed_lane_is_read_only) {
    Engine &e = blank_set();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    const uint8_t before = launch_event();
    post(Cmd::toggle_trig, 2, 5);
    post(Cmd::set_track_length, 2, 0, 8);
    pump();
    CHECK_EQ(int(uint8_t(launch_event() - before)), 2);
    CHECK_EQ(int(e.status().launch_last.load()), int(LaunchEvent::read_only));
    CHECK_EQ(int(e.status().launch_arg.load()), 2);
    // Neither the borrowed part nor the hidden own part changed.
    CHECK((e.project().patterns[1][6].tracks[2].steps[5].flags & kStepTrig) == 0);
    CHECK((e.pattern().tracks[2].steps[5].flags & kStepTrig) == 0);
    CHECK_EQ(int(e.project().patterns[1][6].tracks[2].length), 16);
    // A lane on its own part edits as ever.
    post(Cmd::toggle_trig, 3, 5);
    pump();
    CHECK((e.pattern().tracks[3].steps[5].flags & kStepTrig) != 0);
}

TEST(engine_launch_a_pattern_change_sends_lanes_home_and_pin_keeps_one) {
    Engine &e = blank_set();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    launch(0x10, lane_source(1, 6), QueueMode::immediate);
    post(Cmd::launch_pin, 4);
    pump();
    tick_at(200);
    post(Cmd::queue_pattern, 0, 0, 0, 1, uint8_t(QueueMode::end_of_pattern));
    pump();
    tick_at(1536);
    CHECK_EQ(int(e.project().slot), 1);
    CHECK(!lane_borrowed(e.project(), 2));
    CHECK(lane_borrowed(e.project(), 4));
    CHECK_EQ(int(e.project().settings.lane_source[4]), int(lane_source(1, 6)));
}

TEST(engine_launch_a_launch_queued_with_a_pattern_change_plays_over_the_new_pattern) {
    Engine &e = blank_set();
    tick_at(200);
    post(Cmd::queue_pattern, 0, 0, 0, 1, uint8_t(QueueMode::end_of_pattern));
    launch(0x04, lane_source(1, 6), QueueMode::end_of_pattern);
    CHECK_EQ(fired_on(2, 1536), 1);
    CHECK_EQ(int(e.project().slot), 1);
    CHECK(lane_borrowed(e.project(), 2));
}

TEST(engine_launch_cancel_jump_back_and_stop_drop_queued_launches) {
    Engine &e = blank_set();
    tick_at(100);
    launch(0x06, lane_source(1, 6), QueueMode::end_of_pattern);
    post(Cmd::launch_cancel, 0, 0, 0x02);
    pump();
    CHECK(!e.lane_queue(1).queue.pending);
    CHECK(e.lane_queue(2).queue.pending);
    post(Cmd::cancel_queue);
    pump();
    CHECK(!e.lane_queue(2).queue.pending);

    launch(0x01, lane_source(1, 6), QueueMode::end_of_pattern);
    CHECK(e.lane_queue(0).queue.pending);
    tick_at(0);                                    // the transport went back
    CHECK(!e.lane_queue(0).queue.pending);
    CHECK(!lane_borrowed(e.project(), 0));
}

TEST(engine_launch_capture_writes_the_mix_and_plays_it_with_every_lane_own) {
    Engine &e = blank_set();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    const uint8_t before = launch_event();
    post(Cmd::launch_capture, 0);                  // bank A: A02 is the first free slot
    pump();
    CHECK_EQ(int(uint8_t(launch_event() - before)), 1);
    CHECK_EQ(int(e.status().launch_last.load()), int(LaunchEvent::captured));
    CHECK_EQ(int(e.status().launch_arg.load()), 1);
    CHECK_EQ(int(e.project().slot), 1);
    CHECK_EQ(int(lane_borrowed_mask(e.project())), 0);
    CHECK((e.pattern().tracks[2].steps[1].flags & kStepTrig) != 0);
    CHECK_EQ(int(e.pattern().tracks[2].length), 16);
    // Now it is a part of its own, and edits.
    post(Cmd::toggle_trig, 2, 7);
    pump();
    CHECK((e.pattern().tracks[2].steps[7].flags & kStepTrig) != 0);
    CHECK((e.project().patterns[1][6].tracks[2].steps[7].flags & kStepTrig) == 0);
}

TEST(engine_launch_capture_says_when_the_bank_is_full) {
    Engine &e = blank_set();
    Project &project = e.project_for_load();
    for (int s = 0; s < kPatternsPerBank; s++) project.patterns[3][s].tracks[0].steps[0].flags = kStepTrig;
    post(Cmd::launch_capture, 3);
    pump();
    CHECK_EQ(int(e.status().launch_last.load()), int(LaunchEvent::no_slot));
    CHECK_EQ(int(e.project().bank), 0);
}

// LOCK keeps the lanes apart from the pattern (review H1, Roland 2026-11-28). A01's own track 3
// has a trig on step 5 that is silent while lane 3 plays B07; no jump or swap-back may write
// B07's part over it, and each puts the lane's source back.
namespace {

void a01_own_track_3() {
    Project &project = engine().project_for_load();
    project.patterns[0][0].tracks[2].steps[4].flags = kStepTrig;
}

bool a01_track_3_untouched() {
    const TrackPattern &own = engine().project().patterns[0][0].tracks[2];
    return (own.steps[4].flags & kStepTrig) != 0 && (own.steps[0].flags & kStepTrig) == 0 &&
           (own.steps[1].flags & kStepTrig) == 0;
}

uint8_t lock_state_settled() {
    pump(Engine::kLockCheckBlocks + 1);
    return engine().status().lock_state.load(std::memory_order_relaxed);
}

}  // namespace

TEST(engine_launch_a_lock_brings_back_the_launch_and_leaves_the_own_part) {
    Engine &e = blank_set();
    a01_own_track_3();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    post(Cmd::lock_store);
    pump();
    CHECK(a01_track_3_untouched());
    CHECK_EQ(int(lock_state_settled()), int(kLockHas));
    // Lane 3 home: what you hear differs from the lock.
    launch(0x04, kLaneOwn, QueueMode::immediate);
    CHECK(!lane_borrowed(e.project(), 2));
    CHECK_EQ(int(lock_state_settled()), int(kLockHas | kLockChanged));
    // The jump launches B07 on lane 3 again; A01's own part stays as it is.
    post(Cmd::lock_tap);
    pump();
    CHECK(lane_borrowed(e.project(), 2));
    CHECK_EQ(int(e.project().settings.lane_source[2]), int(lane_source(1, 6)));
    CHECK(a01_track_3_untouched());
    CHECK((e.lane(2).tracks[2].steps[1].flags & kStepTrig) != 0);   // B07's part plays
    // The swap-back is a round trip: lane 3 home, A01 untouched.
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(e.status().lock_last.load()), int(LockTap::swapped));
    CHECK(!lane_borrowed(e.project(), 2));
    CHECK(a01_track_3_untouched());
    CHECK((e.lane(2).tracks[2].steps[4].flags & kStepTrig) != 0);
}

TEST(engine_launch_a_jump_from_a_launch_keeps_it_for_the_swap_back) {
    Engine &e = blank_set();
    a01_own_track_3();
    post(Cmd::lock_store);    // every lane on its own part
    pump();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    post(Cmd::lock_tap);
    pump();
    CHECK_EQ(int(e.status().lock_last.load()), int(LockTap::jumped));
    CHECK(!lane_borrowed(e.project(), 2));
    CHECK(a01_track_3_untouched());   // B07's part was not baked in by the jump's back state
    post(Cmd::lock_tap);
    pump();
    CHECK(lane_borrowed(e.project(), 2));
    CHECK_EQ(int(e.project().settings.lane_source[2]), int(lane_source(1, 6)));
    CHECK(a01_track_3_untouched());
    // And B07 itself is untouched by any of it.
    CHECK(e.project().patterns[1][6].tracks[2].steps[4].flags == 0);
}

TEST(engine_launch_a_jump_keeps_the_pins) {
    Engine &e = blank_set();
    post(Cmd::lock_store);
    pump();
    post(Cmd::launch_pin, 2);
    pump();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    post(Cmd::lock_tap);
    pump();
    CHECK(!lane_borrowed(e.project(), 2));
    CHECK_EQ(int(e.project().settings.lane_pinned), 0x04);
}

TEST(engine_launch_a_landing_is_reported_so_the_project_saves) {
    Engine &e = blank_set();
    tick_at(100);
    const uint8_t before = e.status().launch_landed.load();
    launch(0x04, lane_source(1, 6), QueueMode::end_of_pattern);
    CHECK_EQ(int(e.status().launch_landed.load()), int(before));   // queued, not landed
    tick_at(1536);
    CHECK_EQ(int(uint8_t(e.status().launch_landed.load() - before)), 1);
}

TEST(engine_launch_a_slot_rewritten_under_a_borrowing_lane_restarts_it) {
    Engine &e = blank_set();
    Project &project = e.project_for_load();
    // A03: track 3 an 8-step part with a trig on step 1 only.
    Pattern &a03 = project.patterns[0][2];
    a03.tracks[2].length = 8;
    a03.tracks[2].steps[0].flags = kStepTrig;
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    tick_at(300);
    // COPY A03 onto B07, the slot the lane plays: the new part from its step 1, 8 steps long.
    post(Cmd::pattern_copy, 1, 6, 0, 2);
    pump();
    CHECK_EQ(int(e.lane(2).tracks[2].length), 8);
    CHECK_EQ(fired_on(2, 301), 1);
    int between = 0;
    for (uint32_t t = 302; t < 301 + 192; t++) between += fired_on(2, t);
    CHECK_EQ(between, 0);
    CHECK_EQ(fired_on(2, 301 + 192), 1);
}

// --- ARR REC (core/arrrec.hpp, "Track Launch") ------------------------------------------------

TEST(engine_arr_rec_captures_a_launched_lane_into_a_slot_of_its_own) {
    Engine &e = blank_set();
    const uint32_t loop = uint32_t(master_loop_ticks(e.project().live()));
    const Status &status = e.status();
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    const uint8_t captures = status.arr_captures.load();
    post(Cmd::arr_rec, 0, 0, 1);
    pump();
    tick_at(1);
    tick_at(loop);
    // The pass with B07's track 3 under A01 went in as A02, the first free slot, and nothing
    // switched: A01 still plays with the lane borrowed.
    CHECK_EQ(chain_length(e.project()), 1);
    CHECK_EQ(int(e.project().chain[0].bank), 0);
    CHECK_EQ(int(e.project().chain[0].slot), 1);
    CHECK_EQ(int(status.arr_last.load()), int(ArrEvent::launch_kept));
    CHECK_EQ(int(status.arr_arg.load()), 1);
    CHECK_EQ(int(uint8_t(status.arr_captures.load() - captures)), 1);
    CHECK_EQ(int(status.arr_capture_banks.load()), 0x01);
    CHECK_EQ(int(e.project().slot), 0);
    CHECK(lane_borrowed(e.project(), 2));
    const Pattern &a02 = e.project().patterns[0][1];
    CHECK((a02.tracks[2].steps[1].flags & kStepTrig) != 0);
    CHECK_EQ(int(a02.tracks[2].length), 16);

    // A second pass of the same mix is a repeat of A02, with no second capture.
    tick_at(loop + 1);
    tick_at(2 * loop);
    CHECK_EQ(chain_length(e.project()), 1);
    CHECK_EQ(int(e.project().chain[0].repeats), 2);
    CHECK_EQ(int(uint8_t(status.arr_captures.load() - captures)), 1);
    CHECK(!pattern_has_steps(e.project().patterns[0][2]));

    // Track 3 home: A01 alone is a section of its own.
    launch(0x04, kLaneOwn, QueueMode::immediate);
    tick_at(2 * loop + 1);
    tick_at(3 * loop);
    post(Cmd::arr_rec, 0, 0, 0);
    pump();
    CHECK_EQ(chain_length(e.project()), 2);
    CHECK_EQ(int(e.project().chain[1].slot), 0);
    CHECK_EQ(int(status.arr_last.load()), int(ArrEvent::ended));

    // The chain replays it: A02 with every lane on its own part.
    post(Cmd::set_chain, 0, 0, 1);
    pump();
    CHECK_EQ(int(e.project().slot), 1);
    CHECK_EQ(int(lane_borrowed_mask(e.project())), 0);
    CHECK((e.pattern().tracks[2].steps[1].flags & kStepTrig) != 0);
    post(Cmd::set_chain, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(engine_arr_rec_says_when_a_launch_cannot_be_kept) {
    Engine &e = blank_set();
    Project &project = e.project_for_load();
    for (int s = 1; s < kPatternsPerBank; s++) project.patterns[0][s].tracks[7].steps[9].flags = kStepTrig;
    const uint32_t loop = uint32_t(master_loop_ticks(e.project().live()));
    launch(0x04, lane_source(1, 6), QueueMode::immediate);
    const uint8_t captures = e.status().arr_captures.load();
    post(Cmd::arr_rec, 0, 0, 1);
    pump();
    tick_at(1);
    tick_at(loop);
    CHECK_EQ(int(e.status().arr_last.load()), int(ArrEvent::launch_lost));
    CHECK_EQ(int(e.project().chain[0].slot), 0);
    CHECK_EQ(int(e.status().arr_captures.load()), int(captures));
    post(Cmd::arr_rec, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
}

TEST(engine_arr_rec_a_launch_landing_on_the_boundary_belongs_to_the_pass_it_starts) {
    Engine &e = blank_set();
    const uint32_t loop = uint32_t(master_loop_ticks(e.project().live()));
    post(Cmd::arr_rec, 0, 0, 1);
    pump();
    tick_at(1);
    // Queued for the end of A01's first pass: it lands on the tick that starts the second, after
    // ARR REC has taken that pass's state, and still belongs to it.
    launch(0x04, lane_source(1, 6), QueueMode::end_of_pattern);
    tick_at(loop);
    CHECK(lane_borrowed(e.project(), 2));
    CHECK_EQ(chain_length(e.project()), 1);
    CHECK_EQ(int(e.project().chain[0].slot), 0);   // the first pass: A01 alone
    tick_at(loop + 1);
    tick_at(2 * loop);
    CHECK_EQ(chain_length(e.project()), 2);
    CHECK_EQ(int(e.project().chain[1].slot), 1);   // the second: captured into A02
    CHECK((e.project().patterns[0][1].tracks[2].steps[0].flags & kStepTrig) != 0);
    post(Cmd::arr_rec, 0, 0, 0);
    FakeHost::the().set_running(false);
    pump();
}
