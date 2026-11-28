// Tests for Track Launch's core: core/launch (sources, pins, pattern changes, the per-lane queue,
// CAPTURE) and the scheduler's per-lane patterns and ticks.
#include "check.hpp"

#include "launch.hpp"
#include "pattern.hpp"
#include "project.hpp"
#include "queue.hpp"
#include "scheduler.hpp"

using namespace strata;

namespace {

Project g_project;     // 1 MiB: never on a stack

void blank(Pattern &pattern, uint8_t length = 16, uint32_t seed = 0x5EEDu) {
    pattern_init(pattern);
    for (int t = 0; t < kTracks; t++) pattern.tracks[t].length = length;
    pattern.master_length = length;
    pattern.seed = seed;
}

Step &put_trig(Pattern &pattern, int track, int step, uint8_t note = 60) {
    Step &s = pattern.tracks[track].steps[step];
    s.flags = kStepTrig;
    s.note = note;
    return s;
}

void fresh(int bank = 0, int slot = 0) {
    project_init(g_project);
    g_project.bank = uint8_t(bank);
    g_project.slot = uint8_t(slot);
}

// Runs `ticks` ticks of a launched scheduler and returns how many events track `track` produced,
// with the note of the first in `first_note`.
struct Run {
    int count = 0;
    uint8_t first_note = 0;
    uint8_t first_step = 0;
};

}  // namespace

// --- sources ------------------------------------------------------------------

TEST(launch_sources_name_every_pattern_and_split_back) {
    CHECK_EQ(int(lane_source(0, 0)), 1);
    CHECK_EQ(int(lane_source(7, 15)), 128);
    CHECK_EQ(int(lane_source(8, 0)), int(kLaneOwn));
    CHECK_EQ(int(lane_source(0, 16)), int(kLaneOwn));
    int bad = 0;
    for (int b = 0; b < kBanks; b++) {
        for (int s = 0; s < kPatternsPerBank; s++) {
            const uint8_t source = lane_source(b, s);
            if (lane_source_bank(source) != b || lane_source_slot(source) != s) bad++;
            if (!lane_source_valid(source)) bad++;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(!lane_source_valid(129));
}

TEST(launch_a_lane_plays_its_source_and_the_rest_the_live_pattern) {
    fresh(0, 0);
    CHECK(launch_set(g_project, 2, lane_source(1, 6)));       // T3 < B07
    CHECK(lane_borrowed(g_project, 2));
    CHECK(!lane_borrowed(g_project, 3));
    CHECK_EQ(int(lane_borrowed_mask(g_project)), 0x04);
    CHECK(&lane_pattern(g_project, 2) == &g_project.patterns[1][6]);
    CHECK(&lane_pattern(g_project, 3) == &g_project.live());
    CHECK_EQ(int(lane_playing_source(g_project, 2)), int(lane_source(1, 6)));
    CHECK_EQ(int(lane_playing_source(g_project, 3)), int(lane_source(0, 0)));
}

TEST(launch_onto_the_live_pattern_is_the_own_part) {
    fresh(2, 3);
    CHECK(launch_set(g_project, 0, lane_source(2, 3)));
    CHECK_EQ(int(g_project.settings.lane_source[0]), int(kLaneOwn));
    CHECK(!launch_set(g_project, 8, 1));
    CHECK(!launch_set(g_project, 0, 200));
    CHECK_EQ(int(g_project.settings.lane_source[0]), int(kLaneOwn));
}

TEST(launch_a_damaged_source_reads_as_the_live_pattern) {
    fresh(0, 0);
    g_project.settings.lane_source[5] = 250;
    CHECK(!lane_borrowed(g_project, 5));
    CHECK(&lane_pattern(g_project, 5) == &g_project.live());
}

// --- pattern changes ------------------------------------------------------------

TEST(launch_a_pattern_change_sends_unpinned_lanes_home) {
    fresh(0, 0);
    launch_set(g_project, 2, lane_source(1, 6));
    launch_set(g_project, 6, lane_source(1, 1));
    g_project.slot = 1;                                       // A01 -> A02
    const uint8_t restarted = launch_pattern_changed(g_project, 0, 0);
    CHECK_EQ(int(lane_borrowed_mask(g_project)), 0);
    CHECK_EQ(int(restarted), 0xFF);                           // every lane plays A02's part now
}

TEST(launch_a_pinned_own_lane_keeps_the_pattern_left) {
    fresh(0, 0);
    launch_toggle_pin(g_project, 2);
    g_project.slot = 1;                                       // A01 -> A02
    const uint8_t restarted = launch_pattern_changed(g_project, 0, 0);
    CHECK(lane_borrowed(g_project, 2));
    CHECK_EQ(int(g_project.settings.lane_source[2]), int(lane_source(0, 0)));
    CHECK_EQ(int(restarted), 0xFB);                           // T3 keeps its place in A01
}

TEST(launch_a_pinned_borrowed_lane_keeps_its_source) {
    fresh(0, 0);
    launch_set(g_project, 4, lane_source(3, 3));
    launch_toggle_pin(g_project, 4);
    g_project.slot = 1;
    const uint8_t restarted = launch_pattern_changed(g_project, 0, 0);
    CHECK_EQ(int(g_project.settings.lane_source[4]), int(lane_source(3, 3)));
    CHECK_EQ(int(restarted & 0x10), 0);
    // Unpinned again, the next change brings it home.
    launch_toggle_pin(g_project, 4);
    g_project.slot = 2;
    CHECK_EQ(int(launch_pattern_changed(g_project, 0, 1) & 0x10), 0x10);
    CHECK(!lane_borrowed(g_project, 4));
}

TEST(launch_a_change_onto_the_borrowed_slot_makes_it_the_own_part_without_a_restart) {
    fresh(0, 0);
    launch_set(g_project, 1, lane_source(1, 6));
    launch_set(g_project, 3, lane_source(1, 6));
    launch_toggle_pin(g_project, 3);
    g_project.bank = 1;
    g_project.slot = 6;                                       // onto B07
    const uint8_t restarted = launch_pattern_changed(g_project, 0, 0);
    CHECK_EQ(int(g_project.settings.lane_source[1]), int(kLaneOwn));
    CHECK_EQ(int(g_project.settings.lane_source[3]), int(kLaneOwn));
    CHECK_EQ(int(restarted & 0x0A), 0);                       // both already played B07's part
    CHECK_EQ(int(restarted & 0x01), 0x01);
}

// --- the per-lane queue --------------------------------------------------------

TEST(launch_a_queued_launch_lands_on_the_queue_boundaries) {
    LaneQueue lane;
    // END on a 16-step pattern, armed at tick 100: the next 384.
    CHECK(lane_queue_arm(lane, 7, QueueMode::end_of_pattern, 100, 384, 0));
    CHECK(lane.queue.pending);
    CHECK_EQ(int(lane.source), 7);
    CHECK(!lane_queue_due(lane, 383));
    CHECK(lane_queue_due(lane, 384));
    CHECK(lane.queue.restart);
    CHECK_EQ(int(lane.queue.start_tick), 384);
    // 32: the 768 grid whatever the pattern.
    CHECK(lane_queue_arm(lane, 9, QueueMode::after_32, 100, 384, 0));
    CHECK_EQ(int(lane.queue.switch_tick), 768);
    // NOW keeps the position.
    CHECK(lane_queue_arm(lane, 9, QueueMode::immediate, 100, 384, 0));
    CHECK_EQ(int(lane.queue.switch_tick), 101);
    CHECK(!lane.queue.restart);
    // A late press is forgiven onto the boundary just missed.
    CHECK(lane_queue_arm(lane, 9, QueueMode::end_of_pattern, 390, 384, 0));
    CHECK_EQ(int(lane.queue.switch_tick), 391);
    CHECK_EQ(int(lane.queue.start_tick), 384);
    // Own is a source; 129 is not, and the queue is left as it was.
    CHECK(lane_queue_arm(lane, kLaneOwn, QueueMode::after_16, 10, 384, 0));
    CHECK(!lane_queue_arm(lane, 129, QueueMode::after_16, 10, 384, 0));
    CHECK_EQ(int(lane.source), int(kLaneOwn));
    CHECK(lane.queue.pending);
    lane_queue_clear(lane);
    CHECK(!lane.queue.pending);
}

// --- the scheduler, lane by lane -----------------------------------------------

namespace {

Scheduler g_scheduler;
StepEvent g_events[kMaxEventsPerTick];

// Plays ticks [from, to) with lane `track` launched at `origin` from `lane_pattern` (the rest on
// `master` from tick 0) and returns what that lane did.
Run run_lane(const Pattern &master, const Pattern &borrowed, int track, uint32_t origin,
             uint32_t from, uint32_t to) {
    const Pattern *lanes[kTracks];
    for (int t = 0; t < kTracks; t++) lanes[t] = &master;
    lanes[track] = &borrowed;
    g_scheduler.reset(true);
    g_scheduler.configure(master, lanes, 1000.0f);
    Run run;
    PerformState perform;
    for (uint32_t tick = from; tick < to; tick++) {
        uint32_t ticks[kTracks];
        for (int t = 0; t < kTracks; t++) ticks[t] = tick;
        ticks[track] = tick - origin;
        const int n = g_scheduler.tick(ticks, 0, master, lanes, perform, g_events,
                                       kMaxEventsPerTick);
        for (int i = 0; i < n; i++) {
            if (g_events[i].track != uint8_t(track)) continue;
            if (run.count == 0) {
                run.first_note = g_events[i].note;
                run.first_step = g_events[i].step;
            }
            run.count++;
        }
    }
    return run;
}

}  // namespace

TEST(launch_scheduler_a_lane_plays_its_own_pattern_from_its_own_start) {
    static Pattern master, borrowed;
    blank(master);
    blank(borrowed);
    put_trig(master, 2, 0, 40);
    put_trig(borrowed, 2, 0, 50);       // step 1 of the borrowed part
    put_trig(borrowed, 2, 4, 51);
    // Launched at tick 100: its step 1 is tick 100, not tick 0 or 384.
    const Run run = run_lane(master, borrowed, 2, 100, 100, 100 + 384);
    CHECK_EQ(run.count, 2);
    CHECK_EQ(int(run.first_note), 50);
    CHECK_EQ(int(run.first_step), 0);
}

TEST(launch_scheduler_the_other_lanes_keep_the_master_pattern) {
    static Pattern master, borrowed;
    blank(master);
    blank(borrowed);
    put_trig(master, 0, 0, 36);
    put_trig(borrowed, 0, 0, 99);       // not launched on lane 0: never heard
    put_trig(borrowed, 2, 0, 50);
    const Run run = run_lane(master, borrowed, 2, 0, 0, 384);
    CHECK_EQ(run.count, 1);
    CHECK_EQ(int(run.first_note), 50);
    // Lane 0 plays the master: B's note 99 on track 1 is never heard.
    const Run own = run_lane(master, master, 0, 0, 0, 384);
    CHECK_EQ(own.count, 1);
    CHECK_EQ(int(own.first_note), 36);
}

TEST(launch_scheduler_a_part_brings_its_length_and_speed) {
    static Pattern master, borrowed;
    blank(master, 16);
    blank(borrowed, 16);
    borrowed.tracks[1].length = 4;
    borrowed.tracks[1].speed = uint8_t(Speed::x2);
    put_trig(borrowed, 1, 0, 60);
    // 4 steps at 2x is 48 ticks a pass: 8 passes in 384 ticks.
    const Run run = run_lane(master, borrowed, 1, 0, 0, 384);
    CHECK_EQ(run.count, 8);
}

TEST(launch_scheduler_first_counts_from_the_lanes_launch) {
    static Pattern master, borrowed;
    blank(master, 16);
    blank(borrowed, 16);
    Step &s = put_trig(borrowed, 3, 0, 60);
    s.condition = condition_pack(Condition::FIRST);
    // Launched at tick 1000, far into the master's passes: its first pass is still its first.
    const Run run = run_lane(master, borrowed, 3, 1000, 1000, 1000 + 3 * 384);
    CHECK_EQ(run.count, 1);
}

TEST(launch_scheduler_probability_draws_from_the_parts_own_seed) {
    static Pattern master, borrowed, alone;
    blank(master, 16, 0x1111u);
    blank(borrowed, 16, 0x2222u);
    for (int i = 0; i < 16; i++) put_trig(borrowed, 5, i, 60).probability = 50;
    alone = borrowed;
    // The part launched over another pattern fires exactly as it does in its own.
    const Run launched = run_lane(master, borrowed, 5, 0, 0, 8 * 384);
    const Run home = run_lane(alone, alone, 5, 0, 0, 8 * 384);
    CHECK_EQ(launched.count, home.count);
    CHECK(launched.count > 0 && launched.count < 128);
}

TEST(launch_scheduler_reset_lane_leaves_the_others_running) {
    static Pattern master;
    blank(master, 16);
    for (int t = 0; t < kTracks; t++) put_trig(master, t, 0, 60);
    g_scheduler.reset(true);
    g_scheduler.configure(master, 1000.0f);
    PerformState perform;
    for (uint32_t tick = 0; tick < 200; tick++) {
        g_scheduler.tick(tick, 0, master, perform, g_events, kMaxEventsPerTick);
    }
    const uint8_t before = g_scheduler.playhead(4);
    g_scheduler.reset_lane(2);
    CHECK_EQ(int(g_scheduler.playhead(2)), 0);
    CHECK_EQ(int(g_scheduler.playhead(4)), int(before));
    g_scheduler.reset_lane(-1);       // nothing to reset, nothing broken
    g_scheduler.reset_lane(8);
}

// --- CAPTURE ---------------------------------------------------------------------

TEST(launch_capture_writes_what_every_lane_plays) {
    fresh(0, 0);
    Pattern &live = g_project.patterns[0][0];
    Pattern &b07 = g_project.patterns[1][6];
    blank(live, 16, 0xAAAAu);
    blank(b07, 32, 0xBBBBu);
    live.swing = 60;
    live.root = 3;
    put_trig(live, 0, 0, 36);
    pattern_set_lock(live, 0, 0, 2, 77);
    put_trig(live, 2, 0, 40);                                 // replaced by B07's part
    put_trig(b07, 2, 5, 52);
    pattern_set_lock(b07, 2, 5, 4, 11);
    pattern_set_lock(b07, 2, 5, 6, 22);
    launch_set(g_project, 2, lane_source(1, 6));

    const int slot = launch_capture_slot(g_project, 0);
    CHECK_EQ(slot, 1);                                        // A01 plays, A02 is empty
    CHECK_EQ(int(launch_capture(g_project, 0, slot)), int(CaptureResult::ok));
    const Pattern &out = g_project.patterns[0][1];
    CHECK_EQ(int(out.swing), 60);
    CHECK_EQ(int(out.root), 3);
    CHECK_EQ(int(out.seed), 0xAAAA);
    CHECK_EQ(int(out.master_length), 16);
    CHECK_EQ(int(out.tracks[2].length), 32);                  // the part's own length
    CHECK((out.tracks[2].steps[0].flags & kStepTrig) == 0);
    CHECK_EQ(int(out.tracks[2].steps[5].note), 52);
    uint8_t value = 0;
    CHECK(pattern_get_lock(out, 0, 0, 2, value));
    CHECK_EQ(int(value), 77);
    CHECK(pattern_get_lock(out, 2, 5, 4, value));
    CHECK_EQ(int(value), 11);
    CHECK(pattern_get_lock(out, 2, 5, 6, value));
    CHECK_EQ(int(value), 22);
    CHECK_EQ(int(out.locks_used), 3);                         // compacted
    CHECK(g_project.bank_dirty[0]);
    // The sources are untouched.
    CHECK_EQ(int(b07.tracks[2].steps[5].note), 52);
    CHECK(lane_borrowed(g_project, 2));
}

TEST(launch_capture_never_writes_over_a_pattern_or_a_source) {
    fresh(0, 0);
    put_trig(g_project.patterns[0][1], 0, 0);                 // A02 holds steps
    launch_set(g_project, 1, lane_source(0, 2));              // A03 (empty) feeds T2
    CHECK_EQ(launch_capture_slot(g_project, 0), 3);
    CHECK_EQ(int(launch_capture(g_project, 0, 0)), int(CaptureResult::no_slot));   // live
    CHECK_EQ(int(launch_capture(g_project, 0, 1)), int(CaptureResult::no_slot));   // has steps
    CHECK_EQ(int(launch_capture(g_project, 0, 2)), int(CaptureResult::no_slot));   // a source
    CHECK_EQ(int(launch_capture(g_project, 9, 0)), int(CaptureResult::no_slot));
    CHECK_EQ(launch_capture_slot(g_project, 9), -1);
    // A full bank has no slot.
    for (int s = 0; s < kPatternsPerBank; s++) put_trig(g_project.patterns[2][s], 0, 0);
    CHECK_EQ(launch_capture_slot(g_project, 2), -1);
}

TEST(launch_capture_refuses_when_the_locks_do_not_fit_and_writes_nothing) {
    fresh(0, 0);
    Pattern &live = g_project.patterns[0][0];
    Pattern &other = g_project.patterns[0][5];
    // 64 steps x 8 locks on one track of each: 512 + 512 = 1,024 fits exactly; one more does not.
    for (int s = 0; s < kStepsPerTrack; s++) {
        put_trig(live, 0, s);
        put_trig(other, 1, s);
        for (int p = 0; p < kLocksPerStep; p++) {
            pattern_set_lock(live, 0, s, uint8_t(p), 1);
            pattern_set_lock(other, 1, s, uint8_t(p), 2);
        }
    }
    launch_set(g_project, 1, lane_source(0, 5));
    CHECK_EQ(int(launch_capture(g_project, 0, 1)), int(CaptureResult::ok));
    CHECK_EQ(int(g_project.patterns[0][1].locks_used), kLockPool);

    put_trig(other, 2, 0);
    pattern_set_lock(other, 2, 0, 0, 5);
    launch_set(g_project, 2, lane_source(0, 5));
    CHECK_EQ(int(launch_capture(g_project, 0, 2)), int(CaptureResult::locks_full));
    CHECK(!pattern_has_steps(g_project.patterns[0][2]));
}
