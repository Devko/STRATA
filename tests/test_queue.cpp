// Tests for queued pattern changes and chains (core/queue).
//
// Almost everything here is an edge case, because the module is arithmetic and the arithmetic is
// the whole of it. Three families matter most:
//
//  - **`tick >= switch_tick`, never `==`.** The tests that prove it drive the queue from tick
//    streams with GAPS, including strides that never deliver the exact switch tick and a jump
//    that models a transport stopped and restarted. Under an equality test those changes would
//    be stranded forever, which on hardware looks like a pattern change the user pressed and
//    never got.
//  - **The grace window, and the strict round-up outside it.** A press up to `kQueueGraceTicks`
//    after a boundary lands on the next tick; one tick later than that waits for the next
//    boundary. Swept across every mode and a range of arming ticks, because an off-by-one there
//    is the difference between a bar of music and a tick of it.
//  - **Damaged input cannot divide by zero, hang, or arm a change that never becomes due.** A
//    `pattern_ticks` of 0, a negative one, an absurd one, and arming ticks at the very top of
//    the range where a round-up would wrap back into the past.
#include "check.hpp"

#include "queue.hpp"

#include <cstring>

using namespace strata;

namespace {

constexpr uint32_t kMaxTick = 0xFFFFFFFFu;

// 1x grid boundaries, repeated here rather than imported so that a change to the module has to
// disagree with a number written out by hand.
constexpr uint32_t k16Steps = 384u;      // 16 x 24 ticks
constexpr uint32_t k32Steps = 768u;

// A Project is about 1.03 MiB and a stack frame is capped at 16 KiB (CLAUDE.md), so the one the
// chain tests use is a file-scope object. Each test resets the part of it that it uses;
// `project_init` lives in core/project.cpp, which this module deliberately does not link against.
Project g_project;

bool same_text(const char *a, const char *b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// --- a synthetic tick stream ----------------------------------------------

struct Fire {
    int fires = 0;             // how many times the caller performed the switch
    uint32_t first_tick = 0;   // the tick it happened on
};

// Drive a queue from a stream that delivers only every `stride`-th tick, which is what a block
// holding several ticks looks like from here, and clear it once it fires -- exactly what the
// caller does. A stride that never lands on `switch_tick` is the point: the change must still
// arrive, and exactly once.
Fire drive(Queue &queue, uint32_t from, uint32_t to, uint32_t stride) {
    Fire fire;
    for (uint32_t tick = from; tick <= to; tick += stride) {
        if (queue_due(queue, tick)) {
            if (fire.fires == 0) fire.first_tick = tick;
            fire.fires++;
            queue_clear(queue);
        }
        if (to - tick < stride) break;      // stepping on would wrap past the end of the range
    }
    return fire;
}

// How many of the delivered ticks `queue_due` said true on, with nothing cleared: the queue has
// to keep saying true until the caller acts on it.
int due_count(const Queue &queue, uint32_t from, uint32_t to, uint32_t stride) {
    int count = 0;
    for (uint32_t tick = from; tick <= to; tick += stride) {
        if (queue_due(queue, tick)) count++;
        if (to - tick < stride) break;
    }
    return count;
}

// --- chains ---------------------------------------------------------------

struct Entry {
    uint8_t bank;
    uint8_t slot;
    uint8_t repeats;
};

// Lays a chain into the project the way a file would: `count` entries in use, the rest zeroed,
// which is what an unused entry means (core/project.hpp). `declared` overrides
// `settings.chain_length` for the tests that need the two to disagree.
void set_chain(Project &project, const Entry *entries, int count, int declared = -1) {
    for (int i = 0; i < kChainEntries; i++) project.chain[i] = ChainEntry{};
    for (int i = 0; i < count && i < kChainEntries; i++) {
        project.chain[i].bank = entries[i].bank;
        project.chain[i].slot = entries[i].slot;
        project.chain[i].repeats = entries[i].repeats;
    }
    project.settings.chain_length = uint8_t(declared < 0 ? count : declared);
    project.settings.chain_enabled = true;
}

}  // namespace

// --- mode names -----------------------------------------------------------

TEST(mode_names_are_the_four_labels_the_screen_shows) {
    CHECK(same_text(queue_mode_name(QueueMode::end_of_pattern), "END"));
    CHECK(same_text(queue_mode_name(QueueMode::after_16), "16"));
    CHECK(same_text(queue_mode_name(QueueMode::after_32), "32"));
    CHECK(same_text(queue_mode_name(QueueMode::immediate), "NOW"));

    // A mode outside the enum is a programming error, and a visible one: the screen shows that
    // something is wrong rather than a label that looks deliberate.
    CHECK(same_text(queue_mode_name(QueueMode::count), "?"));
    CHECK(same_text(queue_mode_name(QueueMode(200)), "?"));
}

// --- the switch tick ------------------------------------------------------

TEST(end_of_pattern_rounds_up_to_the_next_pattern_boundary) {
    const int pattern = int(k16Steps);      // a 16-step pattern at 1x

    // Pressed in the body of the pass: the pass plays out and the change lands on its end.
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 13, pattern), 384u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 200, pattern), 384u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 383, pattern), 384u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 397, pattern), 768u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 767, pattern), 768u);

    // Just after a boundary, including exactly on it: forgiven, so the next tick.
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 0, pattern), 1u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 12, pattern), 13u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 384, pattern), 385u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 396, pattern), 397u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 768, pattern), 769u);

    // An odd master length is the case a tick counter cannot be reset for: a 15-step pattern's
    // boundaries are multiples of 360, and the grid keeps running underneath it.
    const int fifteen = 15 * 24;
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 13, fifteen), 360u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 359, fifteen), 360u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 360, fifteen), 361u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 373, fifteen), 720u);
}

TEST(after_16_and_after_32_land_on_the_1x_grid_whatever_the_pattern_is) {
    // These two are boundaries of the grid, not durations from the press, so the pattern's own
    // length is irrelevant to them -- a reading of docs/SPEC.md the header writes down.
    const int lengths[] = {24, int(k16Steps), 15 * 24, 1536, 0, -7};

    for (const int pattern : lengths) {
        CHECK_EQ(queue_switch_tick(QueueMode::after_16, 13, pattern), k16Steps);
        CHECK_EQ(queue_switch_tick(QueueMode::after_16, 383, pattern), k16Steps);
        CHECK_EQ(queue_switch_tick(QueueMode::after_16, 1000, pattern), 1152u);
        CHECK_EQ(queue_switch_tick(QueueMode::after_16, 0, pattern), 1u);        // grace
        CHECK_EQ(queue_switch_tick(QueueMode::after_16, 384, pattern), 385u);    // grace

        CHECK_EQ(queue_switch_tick(QueueMode::after_32, 13, pattern), k32Steps);
        CHECK_EQ(queue_switch_tick(QueueMode::after_32, 384, pattern), k32Steps);
        CHECK_EQ(queue_switch_tick(QueueMode::after_32, 767, pattern), k32Steps);
        CHECK_EQ(queue_switch_tick(QueueMode::after_32, 1000, pattern), 1536u);
        CHECK_EQ(queue_switch_tick(QueueMode::after_32, 0, pattern), 1u);        // grace
        CHECK_EQ(queue_switch_tick(QueueMode::after_32, 768, pattern), 769u);    // grace
    }
}

TEST(every_mode_but_now_restarts_the_pattern_on_its_boundary) {
    // docs/SPEC.md: only "at once" keeps the position, so the other three modes say where the new
    // pattern's step 1 is: the boundary, or for a forgiven late press the boundary just missed.
    Queue q;
    CHECK(queue_arm(q, 0, 1, QueueMode::after_16, 100, 1536));
    CHECK(q.restart);
    CHECK_EQ(q.switch_tick, 384u);
    CHECK_EQ(q.start_tick, 384u);

    CHECK(queue_arm(q, 0, 1, QueueMode::after_32, 770, 1536));     // 2 ticks late: forgiven
    CHECK_EQ(q.switch_tick, 771u);
    CHECK_EQ(q.start_tick, 768u);

    CHECK(queue_arm(q, 0, 1, QueueMode::immediate, 500, 1536));
    CHECK(!q.restart);
    CHECK_EQ(q.switch_tick, 501u);

    // The end of the pattern counts from the pattern's own start, `origin`.
    CHECK(queue_arm(q, 0, 1, QueueMode::end_of_pattern, 400, 360, 384));
    CHECK(q.restart);
    CHECK_EQ(q.switch_tick, 744u);
    CHECK_EQ(q.start_tick, 744u);
    CHECK(queue_arm(q, 0, 1, QueueMode::end_of_pattern, 750, 360, 384));   // 6 late: forgiven
    CHECK_EQ(q.switch_tick, 751u);
    CHECK_EQ(q.start_tick, 744u);
    // An origin after the armed tick (the transport relocated) reads as 0.
    CHECK(queue_arm(q, 0, 1, QueueMode::end_of_pattern, 100, 360, 384));
    CHECK_EQ(q.switch_tick, 360u);
    // And saturates rather than wrapping.
    CHECK(queue_arm(q, 0, 1, QueueMode::end_of_pattern, 0xFFFFFF00u, 1536, 0xFFFFFE00u));
    CHECK_EQ(q.switch_tick, 0xFFFFFFFFu);
}

TEST(the_grace_window_is_half_a_sixteenth) {
    CHECK_EQ(kQueueGraceTicks, 12);
    CHECK_EQ(kQueueGraceTicks, kTicksPerSixteenth / 2);

    // Narrower than a step at 1x, which is what makes the cost bounded: a forgiven press can
    // only ever lose the pass's step 0, never step 1 as well.
    CHECK(kQueueGraceTicks < kTicksPerSixteenth);
    CHECK(kQueueGraceTicks > 0);
}

TEST(a_late_press_inside_the_grace_window_lands_on_the_next_tick) {
    // The case the strict round-up got wrong: a few milliseconds of human lateness must not cost
    // a whole pattern. Swept over every lateness in the window, at several boundaries, for the
    // three modes that have boundaries at all.
    struct Case {
        QueueMode mode;
        uint32_t multiple;
    };
    const Case cases[] = {
        {QueueMode::end_of_pattern, k16Steps},
        {QueueMode::after_16, k16Steps},
        {QueueMode::after_32, k32Steps},
    };

    for (const Case &test_case : cases) {
        const uint32_t boundaries[] = {0u, test_case.multiple, test_case.multiple * 3u,
                                       test_case.multiple * 100u};
        for (const uint32_t boundary : boundaries) {
            for (uint32_t late = 0; late <= uint32_t(kQueueGraceTicks); late++) {
                const uint32_t armed = boundary + late;
                CHECK_EQ(queue_switch_tick(test_case.mode, armed, int(k16Steps)), armed + 1u);
            }
            // One tick past the window, the whole pass plays: that is the cliff, and it is
            // where it is meant to be.
            const uint32_t too_late = boundary + uint32_t(kQueueGraceTicks) + 1u;
            CHECK_EQ(queue_switch_tick(test_case.mode, too_late, int(k16Steps)),
                     boundary + test_case.multiple);
        }
    }
}

TEST(a_forgiven_press_lands_inside_the_new_passes_first_step) {
    // The documented COST of the grace window. Position is kept across a switch, so a switch
    // that lands a few ticks into a pass arrives after that pass's step 0 has already played --
    // from the old pattern. What this pins down is the bound that makes 12 the right number: a
    // forgiven switch always lands inside the first step of the pass, so the new pattern is
    // first heard on step 1 and never as late as step 2.
    for (uint32_t late = 0; late <= uint32_t(kQueueGraceTicks); late++) {
        const uint32_t armed = k16Steps + late;
        const uint32_t at = queue_switch_tick(QueueMode::after_16, armed, int(k16Steps));
        CHECK_EQ(at, armed + 1u);
        CHECK(at > k16Steps);                                   // after the boundary, always
        CHECK(at - k16Steps < uint32_t(kTicksPerSixteenth));    // still within step 0's span
    }
}

TEST(outside_the_grace_window_the_round_up_is_strict) {
    // Swept rather than spot-checked: the rule has to hold at every arming tick, and an
    // off-by-one here is a whole bar of music either way.
    struct Case {
        QueueMode mode;
        uint32_t multiple;
    };
    const Case cases[] = {
        {QueueMode::end_of_pattern, k16Steps},
        {QueueMode::after_16, k16Steps},
        {QueueMode::after_32, k32Steps},
    };

    for (const Case &test_case : cases) {
        for (uint32_t armed = 0; armed <= 3000; armed++) {
            const uint32_t at = queue_switch_tick(test_case.mode, armed, int(k16Steps));
            const uint32_t since = armed % test_case.multiple;

            CHECK(at > armed);                            // never in the past, never instant
            if (since <= uint32_t(kQueueGraceTicks)) {
                CHECK_EQ(at, armed + 1u);                 // forgiven
            } else {
                CHECK(at % test_case.multiple == 0);      // on a boundary
                CHECK_EQ(at, armed - since + test_case.multiple);
                CHECK(at - armed < test_case.multiple);   // and never more than one away
            }
        }
    }
}

TEST(immediate_is_the_next_tick_and_snaps_to_nothing) {
    // "At once keeping the position" (docs/SPEC.md): the switch point is the next tick and is
    // not rounded to any grid, so a change made in the middle of a bar stays there. There is
    // nothing in `Queue` to reset, which is the structural half of the same promise.
    const uint32_t ticks[] = {0u, 1u, 383u, 384u, 1000u, 123456u};
    const int lengths[] = {24, int(k16Steps), 1536, 1000000, 0, -1};

    for (const uint32_t armed : ticks) {
        for (const int pattern : lengths) {
            CHECK_EQ(queue_switch_tick(QueueMode::immediate, armed, pattern), armed + 1u);
        }
    }

    Queue queue;
    CHECK(queue_arm(queue, 2, 5, QueueMode::immediate, 1000, int(k16Steps)));
    CHECK_EQ(queue.switch_tick, 1001u);
    CHECK(!queue_due(queue, 1000));      // not on the arming tick itself
    CHECK(queue_due(queue, 1001));
}

TEST(a_damaged_pattern_length_cannot_divide_by_zero_or_vanish) {
    // Zero or less is one 16th (queue.hpp): the change lands within a step rather than never.
    const int broken[] = {0, -1, -24, -1000000, -2147483647 - 1};
    for (const int pattern : broken) {
        CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 13, pattern), 24u);
        CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 23, pattern), 24u);
        CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 1000, pattern), 1008u);
        CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 0, pattern), 1u);     // grace
        CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 24, pattern), 25u);   // grace
    }

    // A loop shorter than the grace window forgives every press, which is the only sane reading
    // of a loop shorter than a human's reaction error -- and legal arithmetic either way.
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 5, 1), 6u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 5, 4), 6u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 5, 12), 6u);

    // Absurd but believable: a long wait, not an impossible one.
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 13, 1000000), 1000000u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 999999, 1000000), 1000000u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 1000000, 1000000), 1000001u);
}

TEST(an_absurd_pattern_length_still_becomes_due) {
    // The point of the previous test's arithmetic: a change armed against a 1,000,000-tick loop
    // is reachable from a real tick stream, with gaps, and fires exactly once.
    Queue queue;
    CHECK(queue_arm(queue, 0, 0, QueueMode::end_of_pattern, 100, 1000000));
    CHECK_EQ(queue.switch_tick, 1000000u);

    const Fire fire = drive(queue, 0, 1100000, 97);
    CHECK_EQ(fire.fires, 1);
    CHECK(fire.first_tick >= 1000000u);
    CHECK(fire.first_tick < 1000097u);
}

TEST(a_round_up_near_the_top_of_the_tick_range_saturates_instead_of_wrapping) {
    // 2^32 ticks is about 239 days of play, so this is unreachable -- but a wrap would not be
    // unreachable, it would be a switch tick in the past, and `tick >= switch_tick` would then
    // be true on every tick from the moment the change was armed.
    const uint32_t last_16_boundary = 4294967040u;        // 11184810 x 384
    CHECK_EQ(last_16_boundary % k16Steps, 0u);

    CHECK_EQ(queue_switch_tick(QueueMode::after_16, last_16_boundary - 1, 0),
             last_16_boundary);
    // On the last boundary the grace window applies and the next tick still exists.
    CHECK_EQ(queue_switch_tick(QueueMode::after_16, last_16_boundary, 0),
             last_16_boundary + 1u);
    CHECK_EQ(queue_switch_tick(QueueMode::after_16, kMaxTick, 0), kMaxTick);
    // A forgiven press on the very last tick has no next tick to land on: it saturates rather
    // than wrapping to 0, which would read as due forever.
    CHECK_EQ(kMaxTick % 5u, 0u);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, kMaxTick, 5), kMaxTick);
    CHECK_EQ(queue_switch_tick(QueueMode::after_32, kMaxTick, 0), kMaxTick);
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, kMaxTick - 10, 1536), kMaxTick);
    CHECK_EQ(queue_switch_tick(QueueMode::immediate, kMaxTick, 1536), kMaxTick);

    // Whatever the mode, the switch tick is never behind the arming tick.
    const uint32_t armed[] = {kMaxTick, kMaxTick - 1, kMaxTick - 100, kMaxTick - 1000};
    for (int mode = 0; mode < int(QueueMode::count); mode++) {
        for (const uint32_t tick : armed) {
            CHECK(queue_switch_tick(QueueMode(mode), tick, 1536) >= tick);
        }
    }

    // And a saturated change is not secretly due at the start of the session.
    Queue queue;
    CHECK(queue_arm(queue, 0, 0, QueueMode::after_16, kMaxTick - 1, 1536));
    CHECK_EQ(queue.switch_tick, kMaxTick);
    CHECK(!queue_due(queue, 0));
    CHECK(!queue_due(queue, 1000000));
    CHECK(!queue_due(queue, kMaxTick - 2));
    CHECK(queue_due(queue, kMaxTick));          // still reachable, just at the very end
}

TEST(a_switch_tick_is_always_in_the_future) {
    // The invariant every mode shares, and the one the caller relies on to avoid switching on
    // the tick it armed from -- which it has already played.
    const int lengths[] = {24, 360, int(k16Steps), 1536, 0, -5, 1000000};
    for (int mode = 0; mode < int(QueueMode::count); mode++) {
        for (const int pattern : lengths) {
            for (uint32_t armed = 0; armed < 800; armed++) {
                CHECK(queue_switch_tick(QueueMode(mode), armed, pattern) > armed);
            }
        }
    }
}

// --- the master loop ------------------------------------------------------

TEST(master_loop_ticks_clamps_a_damaged_master_length) {
    // One clamp in one place, because every caller of `queue_arm` needs it and two of them
    // would eventually disagree about what a master_length of 0 means.
    Pattern &pattern = g_project.patterns[0][0];
    CHECK_EQ(int(pattern.master_length), kStepsPerTrack);     // a fresh pattern is 64 steps
    CHECK_EQ(master_loop_ticks(pattern), 64 * 24);            // four bars at 1x

    pattern.master_length = 16;
    CHECK_EQ(master_loop_ticks(pattern), int(k16Steps));
    pattern.master_length = 15;
    CHECK_EQ(master_loop_ticks(pattern), 360);                // polymeter, not a power of two
    pattern.master_length = 1;
    CHECK_EQ(master_loop_ticks(pattern), 24);

    // Zeroed memory is one step, not a loop of no ticks at all: a 0 reaching
    // `queue_switch_tick` would have to be rescued there instead, in every mode.
    pattern.master_length = 0;
    CHECK_EQ(master_loop_ticks(pattern), 24);
    CHECK(master_loop_ticks(pattern) > 0);

    pattern.master_length = 255;                              // a damaged file
    CHECK_EQ(master_loop_ticks(pattern), 64 * 24);

    // And it really is what end_of_pattern rounds up to.
    pattern.master_length = 16;
    CHECK_EQ(queue_switch_tick(QueueMode::end_of_pattern, 100, master_loop_ticks(pattern)),
             k16Steps);

    pattern.master_length = uint8_t(kStepsPerTrack);          // left as it was found
}

// --- arming ---------------------------------------------------------------

TEST(arming_records_the_change_and_its_switch_tick) {
    Queue queue;
    CHECK(!queue.pending);

    CHECK(queue_arm(queue, 3, 7, QueueMode::after_32, 100, int(k16Steps)));
    CHECK(queue.pending);
    CHECK_EQ(int(queue.bank), 3);
    CHECK_EQ(int(queue.slot), 7);
    CHECK_EQ(int(queue.mode), int(QueueMode::after_32));
    CHECK_EQ(queue.switch_tick, k32Steps);

    // Every corner of the index space is armable.
    CHECK(queue_arm(queue, 0, 0, QueueMode::end_of_pattern, 0, int(k16Steps)));
    CHECK(queue_arm(queue, kBanks - 1, kPatternsPerBank - 1, QueueMode::immediate, 0, 24));
    CHECK_EQ(int(queue.bank), kBanks - 1);
    CHECK_EQ(int(queue.slot), kPatternsPerBank - 1);
}

TEST(arming_refuses_every_index_no_pattern_exists_at) {
    Queue queue;
    CHECK(queue_arm(queue, 2, 9, QueueMode::after_16, 10, int(k16Steps)));
    const Queue before = queue;

    struct Bad {
        int bank;
        int slot;
    };
    const Bad bad[] = {
        {-1, 0}, {kBanks, 0}, {255, 0}, {1000, 0},        // no such bank
        {0, -1}, {0, kPatternsPerBank}, {0, 255},         // no such slot
        {kBanks - 1, kPatternsPerBank},                   // in range for the bank, not the slot
        {kBanks, kPatternsPerBank - 1},                   // and the other way round
        {-1, -1}, {kBanks, kPatternsPerBank},
    };

    for (const Bad &index : bad) {
        CHECK(!queue_arm(queue, index.bank, index.slot, QueueMode::immediate, 999, 24));
        // Untouched, not cleared: a mistyped index must not cancel a change already queued.
        CHECK_EQ(queue.pending, before.pending);
        CHECK_EQ(int(queue.bank), int(before.bank));
        CHECK_EQ(int(queue.slot), int(before.slot));
        CHECK_EQ(int(queue.mode), int(before.mode));
        CHECK_EQ(queue.switch_tick, before.switch_tick);
    }
}

TEST(arming_over_a_pending_change_replaces_it) {
    // Pressing a second pattern pad before the first change lands picks the second: there is one
    // queue, not a stack, which is what "the next pattern is queued" means.
    Queue queue;
    CHECK(queue_arm(queue, 1, 1, QueueMode::end_of_pattern, 0, int(k16Steps)));
    CHECK(queue_arm(queue, 4, 2, QueueMode::immediate, 50, int(k16Steps)));
    CHECK_EQ(int(queue.bank), 4);
    CHECK_EQ(int(queue.slot), 2);
    CHECK_EQ(queue.switch_tick, 51u);
}

// --- becoming due ---------------------------------------------------------

TEST(an_empty_queue_is_never_due) {
    Queue queue;
    CHECK(!queue_due(queue, 0));
    CHECK(!queue_due(queue, 1));
    CHECK(!queue_due(queue, kMaxTick));

    // Not even with a switch tick left over from a previous life: `pending` decides.
    queue.switch_tick = 10;
    CHECK(!queue_due(queue, 1000));
}

TEST(due_is_greater_or_equal_not_equal) {
    Queue queue;
    // Armed at 100, which is past the grace window, so the switch is the next grid boundary.
    CHECK(queue_arm(queue, 0, 1, QueueMode::after_16, 100, int(k16Steps)));
    CHECK_EQ(queue.switch_tick, k16Steps);

    CHECK(!queue_due(queue, 0));
    CHECK(!queue_due(queue, 383));
    CHECK(queue_due(queue, 384));            // the exact tick
    CHECK(queue_due(queue, 385));            // and every tick after it
    CHECK(queue_due(queue, 100000));
    CHECK(queue_due(queue, kMaxTick));
}

TEST(due_keeps_saying_true_until_the_caller_clears_it) {
    Queue queue;
    CHECK(queue_arm(queue, 0, 1, QueueMode::after_16, 100, int(k16Steps)));

    // Every delivered tick from the switch tick on reads true, so a caller that is a tick late
    // still gets its change.
    CHECK_EQ(due_count(queue, 380, 400, 1), 17);     // ticks 384..400
    CHECK(queue.pending);                            // reading it changes nothing

    queue_clear(queue);
    CHECK_EQ(due_count(queue, 0, 100000, 1), 0);
}

TEST(clearing_leaves_nothing_behind) {
    Queue queue;
    CHECK(queue_arm(queue, 5, 11, QueueMode::after_32, 1000, int(k16Steps)));
    queue_clear(queue);

    // The whole struct, so a stale bank and slot that still name a real pattern cannot be read
    // by accident later.
    CHECK(!queue.pending);
    CHECK_EQ(int(queue.bank), 0);
    CHECK_EQ(int(queue.slot), 0);
    CHECK_EQ(int(queue.mode), int(QueueMode::end_of_pattern));
    CHECK_EQ(queue.switch_tick, 0u);

    queue_clear(queue);                       // idempotent
    CHECK(!queue.pending);
}

TEST(a_tick_stream_with_gaps_still_delivers_the_change_exactly_once) {
    // THE test for `>=`. A block holds several ticks, so the sequencer sees a stream with gaps;
    // under an equality test every stride that steps over tick 384 would strand the change.
    const uint32_t strides[] = {1u, 2u, 3u, 5u, 7u, 24u, 64u, 97u, 384u, 1000u};

    for (const uint32_t stride : strides) {
        Queue queue;
        CHECK(queue_arm(queue, 2, 2, QueueMode::after_16, 100, int(k16Steps)));
        CHECK_EQ(queue.switch_tick, k16Steps);

        const Fire fire = drive(queue, 0, 4000, stride);
        CHECK_EQ(fire.fires, 1);                        // exactly once, never twice
        CHECK(fire.first_tick >= k16Steps);             // never early
        CHECK(fire.first_tick < k16Steps + stride);     // and on the first delivered tick after
    }

    // A stride of 7 never delivers tick 384 at all (384 is not a multiple of 7), so this case
    // only passes because the test is `>=`.
    CHECK(k16Steps % 7u != 0u);
    Queue queue;
    CHECK(queue_arm(queue, 2, 2, QueueMode::after_16, 100, int(k16Steps)));
    const Fire fire = drive(queue, 0, 4000, 7);
    CHECK_EQ(fire.fires, 1);
    CHECK_EQ(fire.first_tick, 385u);
    CHECK(fire.first_tick != k16Steps);
}

TEST(a_transport_that_jumps_still_delivers_the_change) {
    // Stopped and restarted, or a clock that resumed somewhere else entirely: the queue is not
    // allowed to care where the stream went, only that it got past the switch tick.
    Queue queue;
    CHECK(queue_arm(queue, 6, 3, QueueMode::end_of_pattern, 100, int(k16Steps)));
    CHECK_EQ(queue.switch_tick, k16Steps);

    const Fire early = drive(queue, 0, 100, 1);
    CHECK_EQ(early.fires, 0);                 // nothing before the switch tick
    CHECK(queue.pending);

    const Fire late = drive(queue, 500000, 500010, 1);
    CHECK_EQ(late.fires, 1);
    CHECK_EQ(late.first_tick, 500000u);
    CHECK(!queue.pending);                    // and the caller's clear stuck
}

TEST(each_mode_becomes_due_from_a_real_stream) {
    // Nothing arms a change that never arrives, for any mode or pattern length.
    const int lengths[] = {24, 360, int(k16Steps), 1536};
    for (int mode = 0; mode < int(QueueMode::count); mode++) {
        for (const int pattern : lengths) {
            Queue queue;
            CHECK(queue_arm(queue, 1, 1, QueueMode(mode), 200, pattern));
            const Fire fire = drive(queue, 200, 200 + 4000, 3);
            CHECK_EQ(fire.fires, 1);
            CHECK(fire.first_tick > 200u);
        }
    }
}

// --- chain length ---------------------------------------------------------

TEST(chain_length_stops_at_the_first_unused_entry) {
    // `repeats == 0` is what zeroed memory means, so it ends the chain rather than meaning an
    // entry that plays no times (core/project.hpp).
    const Entry entries[] = {{0, 0, 1}, {1, 2, 2}, {2, 4, 3}};
    set_chain(g_project, entries, 3, 8);          // claims 8, only 3 are usable
    CHECK_EQ(chain_length(g_project), 3);

    // A hole stops it even when later entries look fine.
    g_project.chain[1].repeats = 0;
    CHECK_EQ(chain_length(g_project), 1);

    g_project.chain[0].repeats = 0;
    CHECK_EQ(chain_length(g_project), 0);
}

TEST(chain_length_is_bounded_by_the_declared_length) {
    Entry entries[kChainEntries];
    for (int i = 0; i < kChainEntries; i++) {
        entries[i] = Entry{uint8_t(i / kPatternsPerBank), uint8_t(i % kPatternsPerBank), 1};
    }

    set_chain(g_project, entries, kChainEntries, 0);
    CHECK_EQ(chain_length(g_project), 0);         // nothing declared: no chain, however full

    g_project.settings.chain_length = 1;
    CHECK_EQ(chain_length(g_project), 1);
    g_project.settings.chain_length = 5;
    CHECK_EQ(chain_length(g_project), 5);
    g_project.settings.chain_length = uint8_t(kChainEntries);
    CHECK_EQ(chain_length(g_project), kChainEntries);

    // A declared length past the array cannot read past it: a byte from a file can say 200.
    g_project.settings.chain_length = 200;
    CHECK_EQ(chain_length(g_project), kChainEntries);
    g_project.settings.chain_length = 255;
    CHECK_EQ(chain_length(g_project), kChainEntries);
}

TEST(chain_length_drops_entries_naming_patterns_that_cannot_exist) {
    const Entry entries[] = {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}, {3, 3, 1}};
    set_chain(g_project, entries, 4);
    CHECK_EQ(chain_length(g_project), 4);

    g_project.chain[2].bank = uint8_t(kBanks);              // no bank I
    CHECK_EQ(chain_length(g_project), 2);

    set_chain(g_project, entries, 4);
    g_project.chain[2].slot = uint8_t(kPatternsPerBank);    // no 17th pattern
    CHECK_EQ(chain_length(g_project), 2);

    set_chain(g_project, entries, 4);
    g_project.chain[0].bank = 255;
    CHECK_EQ(chain_length(g_project), 0);

    set_chain(g_project, entries, 4);
    g_project.chain[3].slot = 200;                          // only the last one is damaged
    CHECK_EQ(chain_length(g_project), 3);
}

TEST(chain_length_ignores_whether_the_chain_is_enabled) {
    // `chain_enabled` is the app's decision about whether to follow the chain at all; the chain
    // itself is as long as it is, and the screen shows that length either way.
    const Entry entries[] = {{0, 0, 1}, {0, 1, 1}};
    set_chain(g_project, entries, 2);
    CHECK_EQ(chain_length(g_project), 2);
    g_project.settings.chain_enabled = false;
    CHECK_EQ(chain_length(g_project), 2);
}

// --- the caller's half ------------------------------------------------------
//
// `chain_advance` reports a change against the LIVE pattern, `project.bank` / `.slot`, so these
// tests play the engine's part: starting a chain puts its first entry live, and a reported change
// is switched to. Without that the project's live pattern would be whatever an earlier test left.
static bool start_chain(ChainPlayer &player) {
    if (!chain_start(g_project, player)) return false;
    g_project.bank = g_project.chain[player.entry].bank;
    g_project.slot = g_project.chain[player.entry].slot;
    return true;
}

static bool advance(ChainPlayer &player, uint8_t &bank, uint8_t &slot) {
    if (!chain_advance(g_project, player, bank, slot)) return false;
    g_project.bank = bank;
    g_project.slot = slot;
    return true;
}

// --- starting -------------------------------------------------------------

TEST(chain_start_on_an_empty_chain_leaves_the_player_inactive) {
    set_chain(g_project, nullptr, 0);
    ChainPlayer player;
    player.active = true;                  // whatever was there before
    player.entry = 9;
    player.played = 4;

    uint8_t bank = 99, slot = 99;
    CHECK(!start_chain(player));
    CHECK(!player.active);
    CHECK_EQ(int(player.entry), 0);
    CHECK_EQ(int(player.played), 0);
    CHECK(!chain_current(g_project, player, bank, slot));
    CHECK_EQ(int(bank), 99);               // nothing written on a false return
    CHECK_EQ(int(slot), 99);

    // A chain whose first entry is unused is empty too, however long it claims to be.
    const Entry entries[] = {{0, 0, 0}, {1, 1, 4}};
    set_chain(g_project, entries, 2, 2);
    CHECK(!start_chain(player));
    CHECK(!player.active);
}

TEST(chain_start_puts_the_player_on_the_first_entry) {
    const Entry entries[] = {{5, 9, 2}, {1, 1, 1}};
    set_chain(g_project, entries, 2);

    ChainPlayer player;
    CHECK(start_chain(player));
    CHECK(player.active);
    CHECK_EQ(int(player.entry), 0);
    CHECK_EQ(int(player.played), 0);

    uint8_t bank = 99, slot = 99;
    CHECK(chain_current(g_project, player, bank, slot));
    CHECK_EQ(int(bank), 5);
    CHECK_EQ(int(slot), 9);
}

TEST(chain_current_says_false_while_the_player_is_inactive) {
    const Entry entries[] = {{1, 1, 1}};
    set_chain(g_project, entries, 1);

    ChainPlayer player;                    // never started
    uint8_t bank = 99, slot = 99;
    CHECK(!chain_current(g_project, player, bank, slot));
    CHECK_EQ(int(bank), 99);
    CHECK_EQ(int(slot), 99);
}

// --- walking a chain ------------------------------------------------------

TEST(a_chain_plays_each_entry_its_repeats_times_and_wraps) {
    // Three entries with repeats 1, 2 and 3, walked through three full cycles against a
    // hand-written expectation. A count alone would not catch an entry played in the wrong
    // order or a wrap that skipped one.
    const Entry entries[] = {{0, 0, 1}, {1, 2, 2}, {2, 4, 3}};
    set_chain(g_project, entries, 3);

    ChainPlayer player;
    CHECK(start_chain(player));

    struct Expected {
        bool changed;       // does the pattern change on this pass?
        uint8_t bank;       // the pattern the chain is on afterwards
        uint8_t slot;
        uint8_t playing;    // which entry was playing during the pass that just ended
    };
    // One row per finished pass. A01 plays once, then B03 twice, then C05 three times, then it
    // all comes round again -- a chain loops rather than stopping.
    const Expected expected[] = {
        {true,  1, 2, 0},   // A01's single pass done -> B03
        {false, 1, 2, 1},   // B03, pass 1 of 2
        {true,  2, 4, 1},   // B03 done -> C05
        {false, 2, 4, 2},   // C05, pass 1 of 3
        {false, 2, 4, 2},   // pass 2 of 3
        {true,  0, 0, 2},   // C05 done -> wraps back to A01
    };
    const int period = int(sizeof(expected) / sizeof(expected[0]));

    int passes[3] = {};
    for (int pass = 0; pass < period * 3; pass++) {
        const Expected &want = expected[pass % period];

        // Which entry's pattern is sounding during this pass, for the repeat count.
        uint8_t bank = 99, slot = 99;
        CHECK(chain_current(g_project, player, bank, slot));
        CHECK_EQ(int(bank), int(entries[want.playing].bank));
        CHECK_EQ(int(slot), int(entries[want.playing].slot));
        passes[want.playing]++;

        uint8_t next_bank = 99, next_slot = 99;
        const bool changed = advance(player, next_bank, next_slot);
        CHECK_EQ(changed, want.changed);
        if (changed) {
            CHECK_EQ(int(next_bank), int(want.bank));
            CHECK_EQ(int(next_slot), int(want.slot));
        } else {
            CHECK_EQ(int(next_bank), 99);        // untouched when nothing changes
            CHECK_EQ(int(next_slot), 99);
        }

        // And wherever the walk is, the screen agrees with it.
        uint8_t now_bank = 99, now_slot = 99;
        CHECK(chain_current(g_project, player, now_bank, now_slot));
        CHECK_EQ(int(now_bank), int(want.bank));
        CHECK_EQ(int(now_slot), int(want.slot));
    }

    // Three cycles: each entry played exactly `repeats` times per cycle.
    CHECK_EQ(passes[0], 3);
    CHECK_EQ(passes[1], 6);
    CHECK_EQ(passes[2], 9);
}

TEST(a_one_entry_chain_never_reports_a_change) {
    // The decision core/queue.cpp documents: `chain_advance` reports a PATTERN change, not an
    // ENTRY change. A one-entry chain loops the same pattern, so it returns FALSE on every
    // pass -- a pattern change would clear the undo history and restart the `1ST` conditions
    // for no musical reason.
    const Entry one[] = {{3, 6, 1}};
    set_chain(g_project, one, 1);

    ChainPlayer player;
    CHECK(start_chain(player));

    for (int pass = 0; pass < 10; pass++) {
        uint8_t bank = 99, slot = 99;
        CHECK(!advance(player, bank, slot));
        CHECK_EQ(int(bank), 99);
        CHECK_EQ(int(slot), 99);
        CHECK(player.active);
        CHECK_EQ(int(player.entry), 0);
        CHECK_EQ(int(player.played), 0);      // used up and wrapped, not accumulating

        uint8_t at_bank = 99, at_slot = 99;
        CHECK(chain_current(g_project, player, at_bank, at_slot));
        CHECK_EQ(int(at_bank), 3);
        CHECK_EQ(int(at_slot), 6);
    }
}

TEST(a_one_entry_chain_with_several_repeats_counts_its_passes) {
    const Entry one[] = {{0, 5, 3}};
    set_chain(g_project, one, 1);

    ChainPlayer player;
    CHECK(start_chain(player));

    // played goes 1, 2, then wraps to 0 as the entry is used up and starts again.
    const int expected_played[] = {1, 2, 0};
    for (int pass = 0; pass < 9; pass++) {
        uint8_t bank = 99, slot = 99;
        CHECK(!advance(player, bank, slot));      // same pattern throughout
        CHECK_EQ(int(player.played), expected_played[pass % 3]);
        CHECK_EQ(int(player.entry), 0);
    }
}

TEST(two_entries_naming_one_pattern_report_no_change_but_still_advance) {
    // The entry moves, the pattern does not. The player has to follow the entries -- a third
    // entry naming something else must still arrive on time -- while reporting no change.
    const Entry entries[] = {{1, 1, 1}, {1, 1, 1}, {2, 2, 1}};
    set_chain(g_project, entries, 3);

    ChainPlayer player;
    CHECK(start_chain(player));

    uint8_t bank = 99, slot = 99;
    CHECK(!advance(player, bank, slot));     // entry 0 -> entry 1, same A02
    CHECK_EQ(int(player.entry), 1);
    CHECK_EQ(int(bank), 99);

    CHECK(advance(player, bank, slot));      // entry 1 -> entry 2, C03
    CHECK_EQ(int(player.entry), 2);
    CHECK_EQ(int(bank), 2);
    CHECK_EQ(int(slot), 2);

    CHECK(advance(player, bank, slot));      // wraps to entry 0, back to A02
    CHECK_EQ(int(player.entry), 0);
    CHECK_EQ(int(bank), 1);
    CHECK_EQ(int(slot), 1);
}

TEST(a_full_chain_of_64_entries_walks_every_one_and_wraps) {
    Entry entries[kChainEntries];
    for (int i = 0; i < kChainEntries; i++) {
        // 64 distinct patterns, so every step of the walk must report a change.
        entries[i] = Entry{uint8_t(i / kPatternsPerBank), uint8_t(i % kPatternsPerBank), 1};
    }
    set_chain(g_project, entries, kChainEntries);
    CHECK_EQ(chain_length(g_project), kChainEntries);

    ChainPlayer player;
    CHECK(start_chain(player));

    int seen[kChainEntries] = {};
    for (int pass = 0; pass < kChainEntries * 2; pass++) {
        uint8_t bank = 99, slot = 99;
        CHECK(advance(player, bank, slot));
        const int entry = (pass + 1) % kChainEntries;       // starts on 0, so pass 0 lands on 1
        CHECK_EQ(int(player.entry), entry);
        CHECK_EQ(int(bank), int(entries[entry].bank));
        CHECK_EQ(int(slot), int(entries[entry].slot));
        seen[entry]++;
    }
    for (int i = 0; i < kChainEntries; i++) CHECK_EQ(seen[i], 2);   // two full cycles, no skips
}

TEST(a_long_chain_with_mixed_repeats_holds_its_totals) {
    // Every entry played exactly `repeats` times per cycle, over many cycles, which is the
    // property a single hand-written sequence cannot cover for long chains.
    const int count = 7;
    Entry entries[7];
    for (int i = 0; i < count; i++) {
        entries[i] = Entry{uint8_t(i % kBanks), uint8_t(i), uint8_t(i + 1)};   // repeats 1..7
    }
    set_chain(g_project, entries, count);

    ChainPlayer player;
    CHECK(start_chain(player));

    const int cycles = 5;
    int total_repeats = 0;
    for (int i = 0; i < count; i++) total_repeats += int(entries[i].repeats);   // 28

    int passes[7] = {};
    int changes = 0;
    for (int pass = 0; pass < total_repeats * cycles; pass++) {
        passes[player.entry]++;
        uint8_t bank = 99, slot = 99;
        if (advance(player, bank, slot)) changes++;
    }
    for (int i = 0; i < count; i++) CHECK_EQ(passes[i], int(entries[i].repeats) * cycles);

    // Seven distinct patterns, so the pattern changes once per entry boundary: 7 per cycle.
    CHECK_EQ(changes, count * cycles);
    CHECK(player.active);
}

TEST(a_high_repeat_count_cannot_overflow_the_pass_counter) {
    // `played` is a byte, and a count that reached 256 would wrap to 0 and repeat the entry
    // forever -- an entry you could not get out of.
    const Entry entries[] = {{0, 0, 255}, {1, 0, 1}};
    set_chain(g_project, entries, 2);

    ChainPlayer player;
    CHECK(start_chain(player));

    for (int pass = 1; pass < 255; pass++) {
        uint8_t bank = 99, slot = 99;
        CHECK(!advance(player, bank, slot));
        CHECK_EQ(int(player.entry), 0);
        CHECK_EQ(int(player.played), pass);
    }

    uint8_t bank = 99, slot = 99;
    CHECK(advance(player, bank, slot));    // the 255th pass ends the entry
    CHECK_EQ(int(player.entry), 1);
    CHECK_EQ(int(bank), 1);
    CHECK_EQ(int(slot), 0);
}

// --- the chain edited while it plays --------------------------------------

TEST(chain_advance_on_an_inactive_player_does_nothing) {
    const Entry entries[] = {{1, 1, 1}, {2, 2, 1}};
    set_chain(g_project, entries, 2);

    ChainPlayer player;                    // never started
    uint8_t bank = 99, slot = 99;
    CHECK(!advance(player, bank, slot));
    CHECK(!player.active);
    CHECK_EQ(int(bank), 99);
    CHECK_EQ(int(slot), 99);
}

TEST(a_chain_emptied_while_it_plays_stops_being_followed) {
    const Entry entries[] = {{1, 1, 2}, {2, 2, 2}};
    set_chain(g_project, entries, 2);

    ChainPlayer player;
    CHECK(start_chain(player));

    g_project.settings.chain_length = 0;              // the user cleared the chain

    uint8_t bank = 99, slot = 99;
    CHECK(!advance(player, bank, slot));
    CHECK(!player.active);                            // rather than reporting a dropped entry
    CHECK_EQ(int(bank), 99);
    CHECK(!chain_current(g_project, player, bank, slot));
}

TEST(a_chain_shortened_while_it_plays_wraps_to_an_entry_that_still_exists) {
    const Entry entries[] = {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}, {3, 3, 1}};
    set_chain(g_project, entries, 4);

    ChainPlayer player;
    CHECK(start_chain(player));
    uint8_t bank = 99, slot = 99;
    CHECK(advance(player, bank, slot));
    CHECK(advance(player, bank, slot));
    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(player.entry), 3);

    g_project.settings.chain_length = 2;              // entries 2 and 3 are gone

    // The player is past the end, so it reads as being on the first entry rather than on memory
    // that is no longer in the chain.
    CHECK(chain_current(g_project, player, bank, slot));
    CHECK_EQ(int(bank), 0);
    CHECK_EQ(int(slot), 0);

    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(player.entry), 1);
    CHECK_EQ(int(bank), 1);
    CHECK_EQ(int(slot), 1);

    CHECK(advance(player, bank, slot));     // and wraps within the new length
    CHECK_EQ(int(player.entry), 0);
    CHECK_EQ(int(bank), 0);
}

TEST(an_entry_damaged_while_it_plays_truncates_the_chain_under_the_player) {
    const Entry entries[] = {{0, 0, 1}, {1, 1, 1}, {2, 2, 1}};
    set_chain(g_project, entries, 3);

    ChainPlayer player;
    CHECK(start_chain(player));
    g_project.chain[1].bank = uint8_t(kBanks);        // entry 1 now names nothing
    CHECK_EQ(chain_length(g_project), 1);

    // One usable entry left, and it is the one playing: the chain loops it and reports nothing.
    uint8_t bank = 99, slot = 99;
    CHECK(!advance(player, bank, slot));
    CHECK_EQ(int(player.entry), 0);
    CHECK(player.active);
    CHECK(chain_current(g_project, player, bank, slot));
    CHECK_EQ(int(bank), 0);
    CHECK_EQ(int(slot), 0);
}

// --- the two halves together ----------------------------------------------

TEST(a_chain_change_is_armed_and_lands_like_any_other) {
    // How app/ puts the two together: the chain says which pattern is next, the queue says when.
    // The chain's own switch is the end of the pattern, so the tick stream still decides.
    const Entry entries[] = {{0, 0, 1}, {1, 3, 1}};
    set_chain(g_project, entries, 2);

    ChainPlayer player;
    CHECK(start_chain(player));

    uint8_t bank = 99, slot = 99;
    CHECK(advance(player, bank, slot));

    Queue queue;
    CHECK(queue_arm(queue, int(bank), int(slot), QueueMode::end_of_pattern, 100, int(k16Steps)));
    CHECK_EQ(queue.switch_tick, k16Steps);

    const Fire fire = drive(queue, 100, 2000, 64);    // a block of 64 ticks at a time
    CHECK_EQ(fire.fires, 1);
    CHECK(fire.first_tick >= k16Steps);
    CHECK_EQ(int(queue.bank), 0);                     // cleared after the switch
    CHECK(!queue.pending);
}

// --- editing a chain --------------------------------------------------------

TEST(chain_insert_set_and_remove_keep_the_chain_a_prefix) {
    Project &p = g_project;
    set_chain(p, nullptr, 0);
    CHECK_EQ(chain_length(p), 0);

    CHECK(chain_insert(p, 0, 0, 2, 1));               // A03
    CHECK(chain_insert(p, 1, 1, 0, 4));               // B01 x4, after it
    CHECK(chain_insert(p, 0, 0, 0, 2));               // A01 x2, in front
    CHECK_EQ(chain_length(p), 3);
    CHECK_EQ(int(p.settings.chain_length), 3);
    CHECK_EQ(int(p.chain[0].slot), 0);
    CHECK_EQ(int(p.chain[1].slot), 2);
    CHECK_EQ(int(p.chain[2].bank), 1);
    CHECK_EQ(int(p.chain[2].repeats), 4);

    CHECK(chain_set(p, 1, 7, 15, 16));
    CHECK_EQ(int(p.chain[1].bank), 7);
    CHECK_EQ(int(p.chain[1].repeats), 16);

    CHECK(chain_remove(p, 0));
    CHECK_EQ(chain_length(p), 2);
    CHECK_EQ(int(p.chain[0].bank), 7);
    CHECK_EQ(int(p.chain[2].repeats), 0);             // the vacated entry is zeroed
    CHECK(chain_remove(p, 1));
    CHECK(chain_remove(p, 0));
    CHECK_EQ(chain_length(p), 0);
    CHECK_EQ(int(p.settings.chain_length), 0);
}

TEST(chain_edits_refuse_what_cannot_be_in_a_chain) {
    Project &p = g_project;
    const Entry two[] = {{0, 0, 1}, {0, 1, 1}};
    set_chain(p, two, 2);

    CHECK(!chain_set(p, 2, 0, 0, 1));                 // past the end
    CHECK(!chain_set(p, -1, 0, 0, 1));
    CHECK(!chain_set(p, 0, 0, 0, 0));                 // no repeats
    CHECK(!chain_set(p, 0, uint8_t(kBanks), 0, 1));   // no such bank
    CHECK(!chain_insert(p, 3, 0, 0, 1));              // a gap
    CHECK(!chain_remove(p, 2));
    CHECK_EQ(chain_length(p), 2);
    CHECK_EQ(int(p.chain[1].slot), 1);

    // Full.
    set_chain(p, nullptr, 0);
    for (int i = 0; i < kChainEntries; i++) CHECK(chain_insert(p, i, 0, uint8_t(i % 16), 1));
    CHECK(!chain_insert(p, 0, 0, 0, 1));
    CHECK_EQ(chain_length(p), kChainEntries);
}

TEST(chain_edits_first_cut_a_damaged_chain_back_to_what_plays) {
    // Declared length 5, but entry 2 has no repeats: only two entries play, and an insert at the
    // end lands after those two, not after the garbage.
    Project &p = g_project;
    const Entry broken[] = {{0, 0, 1}, {0, 1, 1}, {0, 2, 0}, {0, 3, 1}, {0, 4, 1}};
    set_chain(p, broken, 5);
    CHECK_EQ(chain_length(p), 2);
    CHECK(chain_insert(p, 2, 1, 1, 1));
    CHECK_EQ(chain_length(p), 3);
    CHECK_EQ(int(p.chain[2].bank), 1);
    CHECK_EQ(int(p.chain[3].repeats), 0);
    CHECK_EQ(int(p.settings.chain_length), 3);
}

TEST(a_refused_chain_edit_leaves_a_damaged_chain_as_it_was) {
    // queue.hpp: a refused edit leaves the project as it was. The cut-back used to run before
    // the index was checked, so a refused remove still rewrote a damaged chain's tail.
    Project &p = g_project;
    const Entry broken[] = {{0, 0, 1}, {0, 1, 1}, {0, 2, 0}, {0, 3, 1}};
    set_chain(p, broken, 4);
    CHECK(!chain_remove(p, 3));
    CHECK(!chain_set(p, 2, 0, 0, 1));
    CHECK(!chain_insert(p, 3, 0, 0, 1));
    CHECK_EQ(int(p.settings.chain_length), 4);
    CHECK_EQ(int(p.chain[3].slot), 3);
    CHECK_EQ(int(p.chain[3].repeats), 1);
}

// --- editing a chain while it plays -------------------------------------------
//
// `ChainPlayer::entry` is an index, and the CHAIN page renumbers entries under it.

TEST(an_entry_added_above_the_playing_one_does_not_move_the_chain) {
    // A01, A02, A03. Playing A02 (entry 1); an entry is added at the top.
    const Entry entries[] = {{0, 0, 1}, {0, 1, 1}, {0, 2, 1}};
    set_chain(g_project, entries, 3);
    ChainPlayer player;
    uint8_t bank = 0, slot = 0;
    CHECK(start_chain(player));
    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(slot), 1);

    CHECK(chain_insert(g_project, 0, 0, 9, 1));       // A10 in front
    chain_player_inserted(player, 0);
    CHECK_EQ(int(player.entry), 2);                   // still on A02

    CHECK(advance(player, bank, slot));               // A02 -> A03, not A02 again
    CHECK_EQ(int(slot), 2);
    CHECK(advance(player, bank, slot));               // and wraps to the new first entry
    CHECK_EQ(int(slot), 9);

    // An entry added BELOW the playing one leaves the index alone.
    chain_player_inserted(player, 1);
    CHECK_EQ(int(player.entry), 0);
}

TEST(removing_the_playing_entry_plays_the_next_one_rather_than_skipping_it) {
    const Entry entries[] = {{0, 0, 1}, {0, 1, 2}, {0, 2, 1}, {0, 3, 1}};
    set_chain(g_project, entries, 4);
    ChainPlayer player;
    uint8_t bank = 0, slot = 0;
    CHECK(start_chain(player));
    CHECK(advance(player, bank, slot));               // A02, first of two passes
    CHECK_EQ(int(slot), 1);

    CHECK(chain_remove(g_project, 1));
    chain_player_removed(player, 1);
    CHECK(!chain_current(g_project, player, bank, slot));   // nothing in the chain plays now

    CHECK(advance(player, bank, slot));               // A03 next, not A04
    CHECK_EQ(int(slot), 2);
    CHECK_EQ(int(player.entry), 1);
    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(slot), 3);

    // Removing an entry above the playing one moves the index down with it.
    chain_player_removed(player, 0);
    CHECK_EQ(int(player.entry), 1);
}

TEST(removing_the_last_entry_while_it_plays_wraps_to_the_first) {
    const Entry entries[] = {{0, 0, 1}, {0, 1, 1}};
    set_chain(g_project, entries, 2);
    ChainPlayer player;
    uint8_t bank = 0, slot = 0;
    CHECK(start_chain(player));
    CHECK(advance(player, bank, slot));               // A02, entry 1
    CHECK(chain_remove(g_project, 1));
    chain_player_removed(player, 1);
    CHECK(advance(player, bank, slot));               // back to A01
    CHECK_EQ(int(slot), 0);
}

TEST(editing_the_playing_entry_still_switches_to_the_next_one) {
    // A01, A02. Playing A01, the user turns entry 0 to A02. The next entry is A02 -- the same as
    // the EDITED entry but not the pattern sounding -- so the chain must still switch.
    const Entry entries[] = {{0, 0, 1}, {0, 1, 1}};
    set_chain(g_project, entries, 2);
    ChainPlayer player;
    uint8_t bank = 0, slot = 0;
    CHECK(start_chain(player));
    CHECK(chain_set(g_project, 0, 0, 1, 1));
    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(slot), 1);
}

TEST(chain_resume_keeps_the_live_pattern_and_continues_from_the_first_entry) {
    // A project reopened with its chain on (Cmd::project_loaded).
    const Entry entries[] = {{0, 0, 2}, {0, 1, 1}};
    set_chain(g_project, entries, 2);
    ChainPlayer player;
    uint8_t bank = 0, slot = 0;

    g_project.bank = 3;                                // saved on D06, not an entry
    g_project.slot = 5;
    CHECK(chain_resume(g_project, player));
    CHECK(player.active);
    CHECK(advance(player, bank, slot));                // D06's pass ends: A01, the first entry
    CHECK_EQ(int(slot), 0);
    CHECK(!advance(player, bank, slot));               // its second pass
    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(slot), 1);

    // Saved on the first entry: the chain simply runs from there.
    g_project.bank = 0;
    g_project.slot = 0;
    CHECK(chain_resume(g_project, player));
    CHECK(!player.orphaned);
    CHECK(!advance(player, bank, slot));               // A01's second pass
    CHECK(advance(player, bank, slot));
    CHECK_EQ(int(slot), 1);

    set_chain(g_project, nullptr, 0);
    CHECK(!chain_resume(g_project, player));
    CHECK(!player.active);
}
