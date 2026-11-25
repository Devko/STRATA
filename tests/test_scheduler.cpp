// Tests for the sequencer core in core/scheduler.
//
// The failures that matter here are musical, not loud: a probability boundary that is wrong by
// one fires a step once an hour, a PRE chain that reads the probability result instead of the
// condition result makes a pattern that is almost right, and a non-deterministic draw makes a
// mutation that cannot be recalled. None of those are findable by ear on hardware. So, in the
// house style of tests/test_timing.cpp, most of what follows sweeps -- every B of A:B, every
// track and step and loop of a probability boundary, every ratchet count against every ramp --
// and aggregates, rather than spot-checking a few values.
#include "check.hpp"

#include "pattern.hpp"
#include "scheduler.hpp"
#include "timing.hpp"

using namespace strata;

namespace {

constexpr uint32_t kStepTicks = uint32_t(kTicksPerSixteenth);   // 24

// 130 BPM at 48 kHz, the tempo docs/BUDGET.md measures at.
constexpr float kSamplesPerTick = 48000.0f * 60.0f / (130.0f * float(kPpqn));

// --- building patterns ----------------------------------------------------

void blank(Pattern &pattern, uint8_t length = 16) {
    pattern_init(pattern);
    for (int t = 0; t < kTracks; t++) {
        pattern.tracks[t].length = length;
        pattern.tracks[t].speed = uint8_t(Speed::x1);
        pattern.tracks[t].swing = kSwingInherit;
    }
    pattern.master_length = length;
    pattern.swing = uint8_t(kSwingMin);
    pattern.seed = 0x5EEDu;
}

Step &put_trig(Pattern &pattern, int track, int step) {
    Step &s = pattern.tracks[track].steps[step];
    s.flags = kStepTrig;
    s.note = uint8_t(36 + track);
    s.velocity = 100;
    s.length = 8;
    s.micro = 0;
    s.probability = 100;
    s.condition = condition_pack(Condition::NONE);
    s.ratchet = ratchet_pack(1, RatchetRamp::none);
    return s;
}

// --- collecting what a run produces ---------------------------------------

struct Hit {
    uint32_t tick;
    StepEvent event;
};

// Big enough for every run below; the objects are static because 4,096 hits do not belong on
// a stack the device build limits to 16 KiB.
struct Log {
    static constexpr int kMax = 4096;
    Hit hits[kMax];
    int count;
    int dropped;          // hits this log had no room for, which no test expects
    int max_per_tick;

    void clear() {
        count = 0;
        dropped = 0;
        max_per_tick = 0;
    }

    int on_track(int track) const {
        int n = 0;
        for (int i = 0; i < count; i++) {
            if (hits[i].event.track == uint8_t(track)) n++;
        }
        return n;
    }
};

void run(Scheduler &sched, const Pattern &pattern, const PerformState &perform, uint32_t from,
         uint32_t to, Log &log, uint32_t frame = 0) {
    for (uint32_t tick = from; tick < to; tick++) {
        StepEvent events[kMaxEventsPerTick];
        const int n = sched.tick(tick, frame, pattern, perform, events, kMaxEventsPerTick);
        if (n > log.max_per_tick) log.max_per_tick = n;
        for (int i = 0; i < n; i++) {
            if (log.count >= Log::kMax) {
                log.dropped++;
                continue;
            }
            log.hits[log.count].tick = tick;
            log.hits[log.count].event = events[i];
            log.count++;
        }
    }
}

bool same_stream(const Log &a, const Log &b) {
    if (a.count != b.count) return false;
    for (int i = 0; i < a.count; i++) {
        const Hit &x = a.hits[i];
        const Hit &y = b.hits[i];
        if (x.tick != y.tick) return false;
        if (x.event.track != y.event.track || x.event.step != y.event.step) return false;
        if (x.event.note != y.event.note || x.event.velocity != y.event.velocity) return false;
        if (x.event.frame != y.event.frame) return false;
        if (x.event.ratchet_index != y.event.ratchet_index) return false;
        if (x.event.ratchet_count != y.event.ratchet_count) return false;
        if (x.event.trigless != y.event.trigless) return false;
    }
    return true;
}

// One track, one trig on step 0, one condition: how many loops of `loops` fire.
int fires_with_condition(uint8_t condition, bool fill, int loops, bool first_loop = true) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition;

    Scheduler sched;
    sched.configure(pattern);
    sched.reset(first_loop);

    PerformState perform;
    perform.fill = fill;

    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * uint32_t(loops), log);
    return log.count;
}

}  // namespace

// --- the plain case -------------------------------------------------------

TEST(four_on_the_floor_fires_once_per_step_per_loop) {
    static Pattern pattern;
    blank(pattern, 16);
    for (int step = 0; step < 16; step += 4) put_trig(pattern, 0, step);

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();

    const uint32_t loop_ticks = 16u * kStepTicks;
    const int loops = 4;
    run(sched, pattern, perform, 0, loop_ticks * uint32_t(loops), log);

    CHECK_EQ(log.count, 4 * loops);
    CHECK_EQ(log.max_per_tick, 1);
    CHECK_EQ(log.dropped, 0);

    for (int i = 0; i < log.count; i++) {
        const int loop = i / 4;
        const int which = i % 4;
        const StepEvent &event = log.hits[i].event;

        CHECK_EQ(int(event.track), 0);
        CHECK_EQ(int(event.step), which * 4);
        CHECK_EQ(log.hits[i].tick,
                 uint32_t(loop) * loop_ticks + uint32_t(which * 4) * kStepTicks);
        CHECK_EQ(int(event.note), 36);
        CHECK_EQ(int(event.velocity), 100);
        CHECK_EQ(int(event.length_index), 8);
        CHECK_EQ(int(event.ratchet_index), 0);
        CHECK_EQ(int(event.ratchet_count), 1);
        CHECK(!event.trigless);
        CHECK(!event.accent);
        CHECK_EQ(event.frame, 0u);
    }
}

TEST(events_inherit_the_ticks_own_frame) {
    static Pattern pattern;
    blank(pattern, 16);
    put_trig(pattern, 0, 0);

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 16u * kStepTicks, log, 37u);

    CHECK_EQ(log.count, 1);
    if (log.count == 1) CHECK_EQ(log.hits[0].event.frame, 37u);
}

TEST(a_step_that_is_off_produces_nothing) {
    static Pattern pattern;
    blank(pattern, 16);
    // Every step present in memory but with no trig bit, including accents and locks' worth of
    // other fields set, so that "off" really is decided by the flags alone.
    for (int step = 0; step < 16; step++) {
        Step &s = pattern.tracks[0].steps[step];
        s.flags = kStepAccent | kStepSlide;
        s.velocity = 127;
        s.probability = 100;
    }

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 16u * kStepTicks * 4u, log);

    CHECK_EQ(log.count, 0);
    CHECK_EQ(log.max_per_tick, 0);
}

TEST(a_trigless_step_emits_one_event_with_no_note) {
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = pattern.tracks[0].steps[0];
    s.flags = kStepTrigless;
    s.velocity = 90;
    s.length = 5;
    s.probability = 100;
    s.ratchet = ratchet_pack(6, RatchetRamp::up);   // a trigless step has nothing to ratchet

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 3u, log);

    CHECK_EQ(log.count, 3);              // one per loop, not six
    for (int i = 0; i < log.count; i++) {
        const StepEvent &event = log.hits[i].event;
        CHECK(event.trigless);
        CHECK_EQ(int(event.ratchet_count), 1);
        CHECK_EQ(int(event.ratchet_index), 0);
        CHECK_EQ(int(event.velocity), 90);
        CHECK_EQ(int(event.length_index), 5);
        CHECK_EQ(event.frame, 0u);
    }
}

TEST(a_trigless_step_still_obeys_its_condition_and_probability) {
    // The point of a trigless step is a conditional lock, so the gates must apply to it.
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = pattern.tracks[0].steps[0];
    s.flags = kStepTrigless;
    s.probability = 100;
    s.condition = condition_pack(Condition::FILL);

    PerformState perform;
    static Log log;

    for (int fill = 0; fill < 2; fill++) {
        perform.fill = fill != 0;
        Scheduler sched;
        sched.configure(pattern);
        sched.reset();
        log.clear();
        run(sched, pattern, perform, 0, 4u * kStepTicks * 5u, log);
        CHECK_EQ(log.count, fill != 0 ? 5 : 0);
    }

    // And the probability gate, which is what makes a lock fire "sometimes".
    s.condition = condition_pack(Condition::NONE);
    s.probability = 0;
    Scheduler sched;
    sched.configure(pattern);
    sched.reset();
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 5u, log);
    CHECK_EQ(log.count, 0);
}

TEST(both_trig_bits_set_plays_the_note) {
    // A corrupt state the UI cannot produce: resolve it loudly rather than silently.
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = put_trig(pattern, 0, 0);
    s.flags = uint8_t(kStepTrig | kStepTrigless);
    s.ratchet = ratchet_pack(3, RatchetRamp::none);

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log);

    CHECK_EQ(log.count, 3);
    for (int i = 0; i < log.count; i++) CHECK(!log.hits[i].event.trigless);
}

TEST(accent_and_slide_reach_the_event) {
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = put_trig(pattern, 0, 0);
    s.flags = uint8_t(kStepTrig | kStepAccent | kStepSlide);
    put_trig(pattern, 0, 1);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log);

    CHECK_EQ(log.count, 2);
    if (log.count == 2) {
        CHECK(log.hits[0].event.accent);
        CHECK(log.hits[0].event.slide);
        CHECK(!log.hits[1].event.accent);
        CHECK(!log.hits[1].event.slide);
    }
}

TEST(a_corrupt_step_still_resolves_to_one_sane_event) {
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = pattern.tracks[0].steps[0];
    s.flags = kStepTrig;
    s.note = 200;                 // out of range
    s.velocity = 0;               // out of range
    s.length = 0;                 // the invalid sentinel
    s.micro = 100;                // out of range
    s.probability = 200;          // out of range
    s.condition = 137;            // not a condition this build knows: reads as NONE
    s.ratchet = 0;                // count 0 is not a thing

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 3u, log);

    CHECK_EQ(log.count, 3);
    for (int i = 0; i < log.count; i++) {
        const StepEvent &event = log.hits[i].event;
        CHECK(event.note <= 127);
        CHECK(event.velocity >= 1 && event.velocity <= 127);
        CHECK_EQ(int(event.ratchet_count), 1);
        CHECK_EQ(int(event.length_index), 0);     // passed through: the caller refuses it
    }
}

// --- conditions, as a pure function --------------------------------------

TEST(condition_none_always_passes) {
    const uint8_t packed = condition_pack(Condition::NONE);
    bool always = true;
    for (uint32_t loop = 0; loop < 32; loop++) {
        for (int bits = 0; bits < 16; bits++) {
            if (!condition_passes(packed, loop, (bits & 1) != 0, (bits & 2) != 0,
                                  (bits & 4) != 0, (bits & 8) != 0)) {
                always = false;
            }
        }
    }
    CHECK(always);
}

TEST(an_invalid_condition_byte_reads_as_none) {
    bool all_pass = true;
    int invalid = 0;
    for (int byte = 0; byte < 256; byte++) {
        if (condition_valid(uint8_t(byte))) continue;
        invalid++;
        for (int bits = 0; bits < 16; bits++) {
            if (!condition_passes(uint8_t(byte), 3, (bits & 1) != 0, (bits & 2) != 0,
                                  (bits & 4) != 0, (bits & 8) != 0)) {
                all_pass = false;
            }
        }
    }
    CHECK(all_pass);
    CHECK(invalid > 0);
    // 0-9 are the kinds and 16-50 the A:B pairs; everything else is invalid.
    CHECK_EQ(invalid, 256 - kConditionKinds - kConditionAbPairs);
}

TEST(each_condition_kind_follows_exactly_its_own_input) {
    // Sweeping all 16 combinations of the four inputs proves each kind is a function of one of
    // them and ignores the rest -- a copy-paste slip between PRE and NEI shows up here.
    for (int bits = 0; bits < 16; bits++) {
        const bool fill = (bits & 1) != 0;
        const bool previous = (bits & 2) != 0;
        const bool neighbour = (bits & 4) != 0;
        const bool first = (bits & 8) != 0;

        CHECK_EQ(condition_passes(condition_pack(Condition::FILL), 0, fill, previous, neighbour,
                                  first), fill);
        CHECK_EQ(condition_passes(condition_pack(Condition::NOT_FILL), 0, fill, previous,
                                  neighbour, first), !fill);
        CHECK_EQ(condition_passes(condition_pack(Condition::PRE), 0, fill, previous, neighbour,
                                  first), previous);
        CHECK_EQ(condition_passes(condition_pack(Condition::NOT_PRE), 0, fill, previous,
                                  neighbour, first), !previous);
        CHECK_EQ(condition_passes(condition_pack(Condition::NEI), 0, fill, previous, neighbour,
                                  first), neighbour);
        CHECK_EQ(condition_passes(condition_pack(Condition::NOT_NEI), 0, fill, previous,
                                  neighbour, first), !neighbour);
        CHECK_EQ(condition_passes(condition_pack(Condition::FIRST), 0, fill, previous, neighbour,
                                  first), first);
        CHECK_EQ(condition_passes(condition_pack(Condition::NOT_FIRST), 0, fill, previous,
                                  neighbour, first), !first);
    }
}

TEST(a_b_fires_on_loop_a_of_every_b) {
    // 840 is the lowest common multiple of 1..8, so every B divides the sweep exactly and the
    // fired count is an exact expectation rather than an approximate one.
    constexpr uint32_t kLoops = 840;

    for (uint8_t b = kConditionBMin; b <= kConditionBMax; b++) {
        for (uint8_t a = 1; a <= b; a++) {
            const uint8_t packed = condition_pack_a_b(a, b);
            CHECK_EQ(int(condition_a(packed)), int(a));
            CHECK_EQ(int(condition_b(packed)), int(b));

            uint32_t fired = 0;
            bool matches_modulo = true;
            for (uint32_t loop = 0; loop < kLoops; loop++) {
                const bool pass = condition_passes(packed, loop, false, false, false, false);
                if (pass != ((loop % b) + 1u == uint32_t(a))) matches_modulo = false;
                if (pass) fired++;
            }
            CHECK(matches_modulo);
            CHECK_EQ(fired, kLoops / uint32_t(b));
        }
    }
}

TEST(a_b_is_exact_at_the_largest_loop_numbers) {
    // The modulo is done with a reciprocal rather than a divide, which is exact only up to a
    // bound. The largest loop number a 32-bit tick count can reach is with the shortest
    // possible loop -- one step at 2x, 12 ticks -- so sweep the top of that range, and 2^30,
    // which is where the d <= 8 reciprocals stop being exact.
    const uint32_t bases[] = {0u,
                              1u,
                              0xFFFFu,
                              1u << 20,
                              100000000u,
                              0xFFFFFFFFu / 12u - 128u,
                              (1u << 30) - 128u};

    bool exact = true;
    for (uint8_t b = kConditionBMin; b <= kConditionBMax; b++) {
        for (uint8_t a = 1; a <= b; a++) {
            const uint8_t packed = condition_pack_a_b(a, b);
            for (unsigned k = 0; k < sizeof(bases) / sizeof(bases[0]); k++) {
                for (uint32_t d = 0; d < 128; d++) {
                    const uint32_t loop = bases[k] + d;
                    const bool pass = condition_passes(packed, loop, false, false, false, false);
                    if (pass != ((loop % b) + 1u == uint32_t(a))) exact = false;
                }
            }
        }
    }
    CHECK(exact);
}

TEST(a_bare_a_b_byte_reads_as_one_of_two) {
    // A hand-made file might hold the bare enum value; it must mean something sane.
    const uint8_t bare = uint8_t(Condition::A_B);
    CHECK_EQ(int(condition_a(bare)), 1);
    CHECK_EQ(int(condition_b(bare)), 2);
    for (uint32_t loop = 0; loop < 16; loop++) {
        CHECK_EQ(condition_passes(bare, loop, false, false, false, false), loop % 2u == 0u);
    }
}

// --- conditions, through the scheduler -----------------------------------

TEST(conditions_gate_steps_through_a_run) {
    const int loops = 8;

    CHECK_EQ(fires_with_condition(condition_pack(Condition::NONE), false, loops), loops);

    CHECK_EQ(fires_with_condition(condition_pack(Condition::FILL), true, loops), loops);
    CHECK_EQ(fires_with_condition(condition_pack(Condition::FILL), false, loops), 0);
    CHECK_EQ(fires_with_condition(condition_pack(Condition::NOT_FILL), true, loops), 0);
    CHECK_EQ(fires_with_condition(condition_pack(Condition::NOT_FILL), false, loops), loops);

    CHECK_EQ(fires_with_condition(condition_pack(Condition::FIRST), false, loops), 1);
    CHECK_EQ(fires_with_condition(condition_pack(Condition::NOT_FIRST), false, loops), loops - 1);

    // PRE on the only conditional step of a track feeds itself: it starts true on a fresh
    // pattern and so stays true. NOT PRE therefore alternates, which is a chain test in one step.
    CHECK_EQ(fires_with_condition(condition_pack(Condition::PRE), false, loops), loops);
    CHECK_EQ(fires_with_condition(condition_pack(Condition::NOT_PRE), false, loops), loops / 2);

    // Track 0 has no left neighbour: its neighbour result is false.
    CHECK_EQ(fires_with_condition(condition_pack(Condition::NEI), false, loops), 0);
    CHECK_EQ(fires_with_condition(condition_pack(Condition::NOT_NEI), false, loops), loops);

    for (uint8_t b = kConditionBMin; b <= kConditionBMax; b++) {
        for (uint8_t a = 1; a <= b; a++) {
            const int runs = 8 * int(b);
            int expected = 0;
            for (int loop = 0; loop < runs; loop++) {
                if (loop % int(b) + 1 == int(a)) expected++;
            }
            CHECK_EQ(fires_with_condition(condition_pack_a_b(a, b), false, runs), expected);
        }
    }
}

TEST(pre_chains_across_steps) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::FILL);
    put_trig(pattern, 0, 1).condition = condition_pack(Condition::PRE);
    put_trig(pattern, 0, 2).condition = condition_pack(Condition::NOT_PRE);

    PerformState perform;
    static Log log;

    // Fill off: step 0 fails, so PRE on step 1 fails and NOT PRE on step 2 passes.
    perform.fill = false;
    Scheduler cold;
    cold.configure(pattern);
    cold.reset();
    log.clear();
    run(cold, pattern, perform, 0, 4u * kStepTicks * 4u, log);
    CHECK_EQ(log.count, 4);
    for (int i = 0; i < log.count; i++) CHECK_EQ(int(log.hits[i].event.step), 2);

    // Fill on: step 0 passes, PRE follows it, and NOT PRE then fails.
    perform.fill = true;
    Scheduler hot;
    hot.configure(pattern);
    hot.reset();
    log.clear();
    run(hot, pattern, perform, 0, 4u * kStepTicks * 4u, log);
    CHECK_EQ(log.count, 8);
    for (int i = 0; i < log.count; i++) CHECK_EQ(int(log.hits[i].event.step), i % 2);
}

TEST(a_condition_that_passed_but_a_probability_that_failed_still_feeds_pre) {
    // The case the header calls out: PRE chains off the CONDITION result, so a step silenced by
    // its probability still counts as a pass. Getting this wrong makes a pattern that is almost
    // right, which is the hardest kind of wrong to hear.
    static Pattern pattern;
    blank(pattern, 4);
    Step &first = put_trig(pattern, 0, 0);
    first.condition = condition_pack(Condition::NONE);
    first.probability = 0;                                   // condition passes, note does not
    put_trig(pattern, 0, 1).condition = condition_pack(Condition::PRE);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 6u, log);

    CHECK_EQ(log.count, 6);                                  // step 1 every loop, step 0 never
    for (int i = 0; i < log.count; i++) CHECK_EQ(int(log.hits[i].event.step), 1);
    CHECK(sched.last_condition(0));

    // The mirror: NOT PRE after the same step must never fire.
    pattern.tracks[0].steps[1].condition = condition_pack(Condition::NOT_PRE);
    Scheduler negated;
    negated.configure(pattern);
    negated.reset();
    log.clear();
    run(negated, pattern, perform, 0, 4u * kStepTicks * 6u, log);
    CHECK_EQ(log.count, 0);

    // And a condition that genuinely failed must not feed PRE.
    pattern.tracks[0].steps[0].probability = 100;
    pattern.tracks[0].steps[0].condition = condition_pack(Condition::FILL);   // fill is off
    pattern.tracks[0].steps[1].condition = condition_pack(Condition::PRE);
    Scheduler failing;
    failing.configure(pattern);
    failing.reset();
    log.clear();
    run(failing, pattern, perform, 0, 4u * kStepTicks * 6u, log);
    CHECK_EQ(log.count, 0);
    CHECK(!failing.last_condition(0));
}

TEST(nei_reads_the_neighbour_on_this_tick) {
    // Track 0's result flips step by step; track 1 mirrors it with NEI on every step. Tracks are
    // resolved in ascending order, so track 1 sees track 0's result from THIS tick -- if it saw
    // the previous tick's, the fired steps would be shifted by one.
    static Pattern pattern;
    blank(pattern, 4);
    for (int step = 0; step < 4; step++) {
        put_trig(pattern, 0, step).condition =
            (step % 2) == 0 ? condition_pack(Condition::NONE) : condition_pack(Condition::FILL);
        put_trig(pattern, 1, step).condition = condition_pack(Condition::NEI);
    }

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    perform.fill = false;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 3u, log);

    // Track 0 fires on its even steps, and track 1 fires on exactly the same ones.
    CHECK_EQ(log.on_track(0), 6);
    CHECK_EQ(log.on_track(1), 6);
    for (int i = 0; i < log.count; i++) {
        CHECK_EQ(int(log.hits[i].event.step) % 2, 0);
    }
}

TEST(nei_follows_a_neighbour_that_has_no_step_on_this_tick) {
    // Track 0 resolves only on step 0; track 1's steps 1-3 read the result it left behind.
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::FILL);
    for (int step = 0; step < 4; step++) {
        put_trig(pattern, 1, step).condition = condition_pack(Condition::NEI);
    }

    PerformState perform;
    static Log log;

    for (int fill = 0; fill < 2; fill++) {
        perform.fill = fill != 0;
        Scheduler sched;
        sched.configure(pattern);
        sched.reset();
        log.clear();
        run(sched, pattern, perform, 0, 4u * kStepTicks * 3u, log);

        CHECK_EQ(log.on_track(0), fill != 0 ? 3 : 0);
        CHECK_EQ(log.on_track(1), fill != 0 ? 12 : 0);
    }
}

TEST(nei_on_track_zero_never_fires_and_not_nei_always_does) {
    // Track 0 has no track to its left. The neighbour result is false rather than true, so a
    // NEI placed there goes silent -- a discoverable mistake -- and never wraps to track 7.
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::NEI);
    put_trig(pattern, 0, 1).condition = condition_pack(Condition::NOT_NEI);
    // Track 7 passes everything, which a wrap-around would let through on track 0.
    put_trig(pattern, 7, 0).condition = condition_pack(Condition::NONE);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 4u, log);

    CHECK_EQ(log.on_track(0), 4);
    CHECK_EQ(log.on_track(7), 4);
    for (int i = 0; i < log.count; i++) {
        if (log.hits[i].event.track == 0) CHECK_EQ(int(log.hits[i].event.step), 1);
    }
}

TEST(nei_chains_along_the_whole_kit) {
    // Every track from 1 up follows the one to its left, so flipping track 0 flips all eight.
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::FILL);
    for (int track = 1; track < kTracks; track++) {
        put_trig(pattern, track, 0).condition = condition_pack(Condition::NEI);
    }

    PerformState perform;
    static Log log;

    for (int fill = 0; fill < 2; fill++) {
        perform.fill = fill != 0;
        Scheduler sched;
        sched.configure(pattern);
        sched.reset();
        log.clear();
        run(sched, pattern, perform, 0, 4u * kStepTicks * 2u, log);
        CHECK_EQ(log.count, fill != 0 ? 2 * kTracks : 0);
    }
}

TEST(first_loop_is_per_track_and_survives_until_a_reset) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::FIRST);
    put_trig(pattern, 0, 1).condition = condition_pack(Condition::NOT_FIRST);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();

    const uint32_t loop_ticks = 4u * kStepTicks;
    const int loops = 6;
    run(sched, pattern, perform, 0, loop_ticks * uint32_t(loops), log);

    int firsts = 0;
    int laters = 0;
    for (int i = 0; i < log.count; i++) {
        if (log.hits[i].event.step == 0) firsts++;
        if (log.hits[i].event.step == 1) laters++;
    }
    CHECK_EQ(firsts, 1);
    CHECK_EQ(laters, loops - 1);

    // A reset mid-stream opens a new first pass, anchored to whatever loop the track is in.
    sched.reset();
    log.clear();
    run(sched, pattern, perform, loop_ticks * uint32_t(loops),
        loop_ticks * uint32_t(2 * loops), log);
    firsts = 0;
    laters = 0;
    for (int i = 0; i < log.count; i++) {
        if (log.hits[i].event.step == 0) firsts++;
        if (log.hits[i].event.step == 1) laters++;
    }
    CHECK_EQ(firsts, 1);
    CHECK_EQ(laters, loops - 1);

    // reset(false) says no pass counts as the first, so 1ST never passes at all.
    sched.reset(false);
    log.clear();
    run(sched, pattern, perform, loop_ticks * uint32_t(2 * loops),
        loop_ticks * uint32_t(3 * loops), log);
    firsts = 0;
    laters = 0;
    for (int i = 0; i < log.count; i++) {
        if (log.hits[i].event.step == 0) firsts++;
        if (log.hits[i].event.step == 1) laters++;
    }
    CHECK_EQ(firsts, 0);
    CHECK_EQ(laters, loops);
}

TEST(first_loop_follows_each_tracks_own_length) {
    // Polymeter: a 4-step track leaves its first pass four times sooner than a 16-step one, so
    // a global flag would be wrong for one of them.
    static Pattern pattern;
    blank(pattern, 16);
    pattern.tracks[0].length = 4;
    pattern.tracks[1].length = 16;
    for (int step = 0; step < 4; step++) {
        put_trig(pattern, 0, step).condition = condition_pack(Condition::FIRST);
    }
    for (int step = 0; step < 16; step++) {
        put_trig(pattern, 1, step).condition = condition_pack(Condition::FIRST);
    }

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 16u * kStepTicks * 4u, log);

    CHECK_EQ(log.on_track(0), 4);      // its first pass is 4 steps long
    CHECK_EQ(log.on_track(1), 16);     // and its neighbour's is 16
}

// --- probability ----------------------------------------------------------

TEST(probability_zero_never_fires_and_a_hundred_always_does) {
    // Swept across every track, every step and many loops. An off-by-one at either boundary is
    // a step that fires once an hour, or one that drops a hit once an hour.
    for (int which = 0; which < 2; which++) {
        const uint8_t probability = which == 0 ? uint8_t(0) : uint8_t(100);

        static Pattern pattern;
        blank(pattern, 16);
        for (int track = 0; track < kTracks; track++) {
            for (int step = 0; step < 16; step++) {
                put_trig(pattern, track, step).probability = probability;
            }
        }

        Scheduler sched;
        sched.configure(pattern, kSamplesPerTick);
        sched.reset();

        PerformState perform;
        int fired[kTracks][16] = {};
        const int loops = 24;
        const uint32_t end = 16u * kStepTicks * uint32_t(loops);
        for (uint32_t tick = 0; tick < end; tick++) {
            StepEvent events[kMaxEventsPerTick];
            const int n = sched.tick(tick, 0, pattern, perform, events, kMaxEventsPerTick);
            for (int i = 0; i < n; i++) fired[events[i].track][events[i].step]++;
        }

        for (int track = 0; track < kTracks; track++) {
            for (int step = 0; step < 16; step++) {
                CHECK_EQ(fired[track][step], which == 0 ? 0 : loops);
            }
        }
    }
}

TEST(probability_above_a_hundred_is_treated_as_always) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).probability = 255;

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 10u, log);
    CHECK_EQ(log.count, 10);
}

TEST(the_probability_draw_is_in_range_and_evenly_spread) {
    uint32_t buckets[10] = {};
    bool in_range = true;

    constexpr uint32_t kSeeds = 4;
    constexpr uint32_t kLoops = 32;
    for (uint32_t seed = 1; seed <= kSeeds; seed++) {
        for (int track = 0; track < kTracks; track++) {
            for (int step = 0; step < kStepsPerTrack; step++) {
                for (uint32_t loop = 0; loop < kLoops; loop++) {
                    const uint32_t roll =
                        probability_roll(seed * 2654435761u, track, step, loop);
                    if (roll >= 100u) in_range = false;
                    buckets[roll / 10u]++;
                }
            }
        }
    }

    CHECK(in_range);

    // A degenerate hash -- one that ignores an input, or whose low bits do not move -- empties
    // or overfills a bucket. The expectation is a tenth of the sweep in each.
    const uint32_t total = kSeeds * uint32_t(kTracks) * uint32_t(kStepsPerTrack) * kLoops;
    uint32_t counted = 0;
    for (int i = 0; i < 10; i++) {
        CHECK(buckets[i] > total / 20u);
        CHECK(buckets[i] < total / 5u);
        counted += buckets[i];
    }
    CHECK_EQ(counted, total);
}

TEST(the_probability_draw_moves_with_every_one_of_its_inputs) {
    // If it ignored one, a whole dimension of the pattern would share a draw.
    bool seed_matters = false;
    bool track_matters = false;
    bool step_matters = false;
    bool loop_matters = false;

    for (uint32_t i = 0; i < 64; i++) {
        if (probability_roll(1u + i, 0, 0, 0) != probability_roll(2u + i, 0, 0, 0)) {
            seed_matters = true;
        }
        if (probability_roll(7u, 0, int(i), 0) != probability_roll(7u, 1, int(i), 0)) {
            track_matters = true;
        }
        if (probability_roll(7u, 0, int(i), 0) != probability_roll(7u, 0, int(i) + 1, 0)) {
            step_matters = true;
        }
        if (probability_roll(7u, 0, 0, i) != probability_roll(7u, 0, 0, i + 1)) {
            loop_matters = true;
        }
    }

    CHECK(seed_matters);
    CHECK(track_matters);
    CHECK(step_matters);
    CHECK(loop_matters);
}

TEST(a_fifty_percent_step_fires_about_half_the_time) {
    static Pattern pattern;
    blank(pattern, 16);
    for (int track = 0; track < kTracks; track++) {
        for (int step = 0; step < 16; step++) {
            put_trig(pattern, track, step).probability = 50;
        }
    }

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    int fired[kTracks][16] = {};
    int total = 0;
    const int loops = 64;
    const uint32_t end = 16u * kStepTicks * uint32_t(loops);
    for (uint32_t tick = 0; tick < end; tick++) {
        StepEvent events[kMaxEventsPerTick];
        const int n = sched.tick(tick, 0, pattern, perform, events, kMaxEventsPerTick);
        for (int i = 0; i < n; i++) {
            fired[events[i].track][events[i].step]++;
            total++;
        }
    }

    const int opportunities = kTracks * 16 * loops;
    CHECK(total > opportunities * 40 / 100);
    CHECK(total < opportunities * 60 / 100);

    // And no individual step is stuck on or off across 64 passes, which is what a draw that
    // ignored the loop number would look like.
    bool all_vary = true;
    for (int track = 0; track < kTracks; track++) {
        for (int step = 0; step < 16; step++) {
            if (fired[track][step] == 0 || fired[track][step] == loops) all_vary = false;
        }
    }
    CHECK(all_vary);
}

TEST(the_probability_gate_fires_strictly_below_the_draw) {
    // 0% and 100% are decided without a draw, so those sweeps cannot see the comparison itself.
    // Pin it against the actual draw of every step: probability == roll must not fire, and
    // roll + 1 must. A > where a >= belongs is a step that fires once an hour too often.
    static Pattern pattern;
    blank(pattern, 16);

    uint32_t rolls[16];
    for (int step = 0; step < 16; step++) {
        put_trig(pattern, 0, step);
        rolls[step] = probability_roll(pattern.seed, 0, step, 0);
    }

    PerformState perform;
    for (int offset = 0; offset <= 1; offset++) {
        for (int step = 0; step < 16; step++) {
            pattern.tracks[0].steps[step].probability = uint8_t(rolls[step] + uint32_t(offset));
        }

        Scheduler sched;
        sched.configure(pattern);
        sched.reset();

        static Log log;
        log.clear();
        run(sched, pattern, perform, 0, 16u * kStepTicks, log);     // the first pass only
        CHECK_EQ(log.count, offset * 16);
    }
}

TEST(probability_is_resolved_after_the_condition) {
    // A step whose condition fails must not consume a draw's worth of meaning: the condition is
    // the gate, so the step never fires however the roll would have landed.
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = put_trig(pattern, 0, 0);
    s.condition = condition_pack(Condition::FILL);
    s.probability = 100;

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    perform.fill = false;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 16u, log);
    CHECK_EQ(log.count, 0);
}

// --- determinism ----------------------------------------------------------

TEST(the_same_pattern_plays_identically_on_every_pass) {
    static Pattern pattern;
    blank(pattern, 16);
    for (int track = 0; track < kTracks; track++) {
        for (int step = 0; step < 16; step++) {
            Step &s = put_trig(pattern, track, step);
            s.probability = uint8_t(20 + ((track * 16 + step) % 70));
            s.ratchet = ratchet_pack(uint8_t(1 + (step % 3)), RatchetRamp(step % 3));
        }
    }

    PerformState perform;
    const uint32_t end = 16u * kStepTicks * 8u;

    static Log first;
    static Log second;
    static Log reloaded;

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);

    sched.reset();
    first.clear();
    run(sched, pattern, perform, 0, end, first);
    CHECK(first.count > 200);
    CHECK_EQ(first.dropped, 0);

    // The same object, reset: the second pass must be the first pass.
    sched.reset();
    second.clear();
    run(sched, pattern, perform, 0, end, second);
    CHECK(same_stream(first, second));

    // A fresh object, as after a reload: still the same.
    Scheduler fresh;
    fresh.configure(pattern, kSamplesPerTick);
    fresh.reset();
    reloaded.clear();
    run(fresh, pattern, perform, 0, end, reloaded);
    CHECK(same_stream(first, reloaded));

    // A different seed is a different pattern.
    static Pattern other;
    other = pattern;
    other.seed = pattern.seed + 1u;
    Scheduler mutated;
    mutated.configure(other, kSamplesPerTick);
    mutated.reset();
    static Log different;
    different.clear();
    run(mutated, other, perform, 0, end, different);
    CHECK(!same_stream(first, different));

    // ... but still the same as itself, so "different" is not "random".
    Scheduler again;
    again.configure(other, kSamplesPerTick);
    again.reset();
    static Log repeat;
    repeat.clear();
    run(again, other, perform, 0, end, repeat);
    CHECK(same_stream(different, repeat));
}

TEST(a_later_pass_of_the_same_loop_repeats_itself) {
    // The draw takes the loop number, so passes differ from each other -- but tick n and
    // tick n of a re-run must agree. This is the property a host test relies on.
    static Pattern pattern;
    blank(pattern, 4);
    for (int step = 0; step < 4; step++) put_trig(pattern, 0, step).probability = 50;

    PerformState perform;
    const uint32_t end = 4u * kStepTicks * 64u;

    static Log a;
    static Log b;

    Scheduler one;
    one.configure(pattern);
    one.reset();
    a.clear();
    run(one, pattern, perform, 0, end, a);

    Scheduler two;
    two.configure(pattern);
    two.reset();
    b.clear();
    run(two, pattern, perform, 0, end, b);

    CHECK(same_stream(a, b));
    CHECK(a.count > 50);
    CHECK(a.count < 4 * 64);          // a 50% step really is gating something
}

// --- ratchets -------------------------------------------------------------

TEST(ratchet_velocity_stays_in_range_for_every_count_and_ramp) {
    bool in_range = true;
    for (int velocity = 0; velocity <= 255; velocity++) {
        for (uint8_t count = 1; count <= kRatchetMax; count++) {
            for (uint8_t index = 0; index < count; index++) {
                for (int ramp = 0; ramp <= int(RatchetRamp::down); ramp++) {
                    const uint8_t out = ratchet_velocity(uint8_t(velocity), index, count,
                                                         RatchetRamp(ramp));
                    if (out < 1 || out > 127) in_range = false;
                }
            }
        }
    }
    CHECK(in_range);

    // Out-of-range arguments clamp rather than read past the end of anything.
    CHECK(ratchet_velocity(100, 9, 4, RatchetRamp::up) ==
          ratchet_velocity(100, 3, 4, RatchetRamp::up));
    CHECK(ratchet_velocity(100, 0, 0, RatchetRamp::none) == 100);
    CHECK(ratchet_velocity(100, 0, 200, RatchetRamp::none) == 100);
    CHECK(ratchet_velocity(100, 0, 4, RatchetRamp(9)) == 100);   // an unknown ramp is none
}

TEST(ratchet_ramps_are_monotonic_and_mirror_each_other) {
    const uint8_t velocities[] = {1, 2, 8, 63, 100, 127};

    for (uint8_t count = 1; count <= kRatchetMax; count++) {
        for (unsigned v = 0; v < sizeof(velocities) / sizeof(velocities[0]); v++) {
            const uint8_t velocity = velocities[v];

            // none is flat.
            for (uint8_t index = 0; index < count; index++) {
                CHECK_EQ(int(ratchet_velocity(velocity, index, count, RatchetRamp::none)),
                         int(velocity));
            }

            uint8_t previous_up = 0;
            uint8_t previous_down = 128;
            for (uint8_t index = 0; index < count; index++) {
                const uint8_t up = ratchet_velocity(velocity, index, count, RatchetRamp::up);
                const uint8_t down = ratchet_velocity(velocity, index, count, RatchetRamp::down);
                CHECK(up >= previous_up);
                CHECK(down <= previous_down);
                // down is up read backwards.
                CHECK_EQ(int(up), int(ratchet_velocity(velocity, uint8_t(count - 1 - index),
                                                       count, RatchetRamp::down)));
                previous_up = up;
                previous_down = down;
            }

            // The ramp arrives at, or departs from, the step's own velocity.
            CHECK_EQ(int(ratchet_velocity(velocity, uint8_t(count - 1), count, RatchetRamp::up)),
                     int(velocity));
            CHECK_EQ(int(ratchet_velocity(velocity, 0, count, RatchetRamp::down)),
                     int(velocity));
        }
    }
}

TEST(ratchet_frames_spread_evenly_from_the_steps_own_frame) {
    const float step_samples = float(kStepTicks) * kSamplesPerTick;

    for (uint32_t count = 1; count <= uint32_t(kRatchetMax); count++) {
        CHECK_EQ(ratchet_frame(0, count, step_samples), 0u);     // the first hit is the step

        const float spacing = step_samples / float(count);
        uint32_t previous = 0;
        for (uint32_t index = 1; index < count; index++) {
            const uint32_t offset = ratchet_frame(index, count, step_samples);
            CHECK(offset > previous);
            CHECK(offset < uint32_t(step_samples));             // still inside the step
            CHECK_NEAR(float(offset), float(index) * spacing, 1.0);
            previous = offset;
        }
    }

    // An unconfigured or nonsensical step length puts every hit on the step's own frame rather
    // than somewhere unpredictable.
    CHECK_EQ(ratchet_frame(3, 4, 0.0f), 0u);
    CHECK_EQ(ratchet_frame(3, 4, -1000.0f), 0u);

    // Out-of-range arguments clamp.
    CHECK_EQ(ratchet_frame(9, 4, step_samples), ratchet_frame(3, 4, step_samples));
    CHECK_EQ(ratchet_frame(0, 0, step_samples), 0u);
    CHECK_EQ(ratchet_frame(0, 200, step_samples), 0u);
}

TEST(a_ratchet_emits_its_count_with_the_right_velocities_and_frames) {
    const float step_samples = float(kStepTicks) * kSamplesPerTick;

    for (uint8_t count = kRatchetMin; count <= kRatchetMax; count++) {
        for (int ramp = 0; ramp <= int(RatchetRamp::down); ramp++) {
            static Pattern pattern;
            blank(pattern, 4);
            Step &s = put_trig(pattern, 0, 0);
            s.velocity = 120;
            s.ratchet = ratchet_pack(count, RatchetRamp(ramp));

            Scheduler sched;
            sched.configure(pattern, kSamplesPerTick);
            sched.reset();

            PerformState perform;
            static Log log;
            log.clear();
            run(sched, pattern, perform, 0, 4u * kStepTicks, log, 11u);

            CHECK_EQ(log.count, int(count));
            if (log.count != int(count)) continue;

            for (int i = 0; i < log.count; i++) {
                const StepEvent &event = log.hits[i].event;
                CHECK_EQ(int(event.ratchet_index), i);
                CHECK_EQ(int(event.ratchet_count), int(count));
                CHECK_EQ(int(event.step), 0);
                CHECK(event.velocity >= 1 && event.velocity <= 127);
                CHECK(!event.trigless);
                // Every hit of a step arrives on the one tick, so they share a tick number and
                // differ only in their frame.
                CHECK_EQ(log.hits[i].tick, 0u);
            }

            // The first hit sits on the step's own frame and the rest are evenly spread.
            CHECK_EQ(log.hits[0].event.frame, 11u);
            for (int i = 1; i < log.count; i++) {
                CHECK(log.hits[i].event.frame > log.hits[i - 1].event.frame);
                CHECK_NEAR(float(log.hits[i].event.frame - 11u),
                           float(i) * step_samples / float(count), 1.0);
            }
            CHECK(float(log.hits[log.count - 1].event.frame - 11u) < step_samples);

            // The ramp runs the right way round.
            if (RatchetRamp(ramp) == RatchetRamp::none) {
                for (int i = 0; i < log.count; i++) CHECK_EQ(int(log.hits[i].event.velocity), 120);
            } else if (RatchetRamp(ramp) == RatchetRamp::up) {
                CHECK_EQ(int(log.hits[log.count - 1].event.velocity), 120);
                for (int i = 1; i < log.count; i++) {
                    CHECK(log.hits[i].event.velocity >= log.hits[i - 1].event.velocity);
                }
            } else {
                CHECK_EQ(int(log.hits[0].event.velocity), 120);
                for (int i = 1; i < log.count; i++) {
                    CHECK(log.hits[i].event.velocity <= log.hits[i - 1].event.velocity);
                }
            }
        }
    }
}

TEST(a_ratchet_of_one_is_a_plain_step) {
    static Pattern pattern;
    blank(pattern, 4);
    Step &s = put_trig(pattern, 0, 0);
    s.ratchet = ratchet_pack(1, RatchetRamp::up);

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log, 5u);

    CHECK_EQ(log.count, 1);
    if (log.count == 1) {
        CHECK_EQ(log.hits[0].event.frame, 5u);
        CHECK_EQ(int(log.hits[0].event.velocity), 100);
        CHECK_EQ(int(log.hits[0].event.ratchet_count), 1);
    }
}

TEST(a_ratchet_without_a_configured_tempo_still_emits_its_hits) {
    // configure() left at its default knows no tempo, so the hits cannot be spread. They must
    // still all be there, with their velocities, rather than silently collapsing to one.
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).ratchet = ratchet_pack(5, RatchetRamp::down);

    Scheduler sched;
    sched.configure(pattern);           // no samples_per_tick
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log, 3u);

    CHECK_EQ(log.count, 5);
    for (int i = 0; i < log.count; i++) {
        CHECK_EQ(log.hits[i].event.frame, 3u);
        CHECK_EQ(int(log.hits[i].event.ratchet_index), i);
    }
}

// --- timing comes from core/timing ---------------------------------------

TEST(swing_is_resolved_against_the_pattern_and_the_track) {
    static Pattern pattern;
    blank(pattern, 16);
    pattern.swing = 75;
    pattern.tracks[0].swing = kSwingInherit;     // follows the pattern
    pattern.tracks[1].swing = uint8_t(kSwingMin);  // overrides it
    pattern.tracks[2].swing = 200;               // out of range: clamped to the maximum
    for (int track = 0; track < 3; track++) put_trig(pattern, track, 1);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 16u * kStepTicks, log);

    CHECK_EQ(log.count, 3);
    uint32_t at[kTracks] = {};
    for (int i = 0; i < log.count; i++) at[log.hits[i].event.track] = log.hits[i].tick;

    CHECK_EQ(at[0], kStepTicks + 12u);     // 75%: half a step late
    CHECK_EQ(at[1], kStepTicks);           // 50%: straight
    CHECK_EQ(at[2], kStepTicks + 12u);     // clamped to 75%
}

TEST(micro_timing_moves_a_step_without_a_reconfigure) {
    // Micro-timing is a step edit, not a timing change, so it must be read per tick.
    static Pattern pattern;
    blank(pattern, 16);
    put_trig(pattern, 0, 4);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 16u * kStepTicks, log);
    CHECK_EQ(log.count, 1);
    if (log.count == 1) CHECK_EQ(log.hits[0].tick, 4u * kStepTicks);

    pattern.tracks[0].steps[4].micro = int8_t(-kMicroRange);   // no configure() in between
    sched.reset();
    log.clear();
    run(sched, pattern, perform, 0, 16u * kStepTicks, log);
    CHECK_EQ(log.count, 1);
    if (log.count == 1) {
        // core/timing owns the arithmetic; the scheduler must agree with it rather than carry
        // its own copy.
        TrackTiming timing;
        timing.length = 16;
        timing.speed = Speed::x1;
        timing.swing = uint8_t(kSwingMin);
        CHECK_EQ(log.hits[0].tick,
                 uint32_t(effective_tick(4, timing, nullptr)) - uint32_t(kMicroRange));
    }
}

TEST(speed_and_length_are_clamped_by_configure) {
    static Pattern pattern;
    blank(pattern, 16);
    pattern.tracks[0].speed = 99;        // not a speed: falls back to 1x
    pattern.tracks[1].length = 0;        // not a length: clamped to one step
    pattern.tracks[2].length = 200;      // clamped to 64
    put_trig(pattern, 0, 1);
    put_trig(pattern, 1, 0);
    put_trig(pattern, 2, 0);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log);

    // Track 0 at 1x fires step 1 at tick 24; track 1 is a one-step loop, so it fires on every
    // step boundary; track 2's step 0 fires once in this window.
    CHECK_EQ(log.on_track(0), 1);
    CHECK_EQ(log.on_track(1), 4);
    CHECK_EQ(log.on_track(2), 1);
}

TEST(polymeter_tracks_drift_against_each_other) {
    const uint8_t lengths[kTracks] = {16, 12, 7, 5, 3, 11, 13, 9};

    static Pattern pattern;
    blank(pattern, 16);
    for (int track = 0; track < kTracks; track++) {
        pattern.tracks[track].length = lengths[track];
        put_trig(pattern, track, 0);
    }

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    const uint32_t end = 16u * kStepTicks * 40u;    // 40 passes of the longest track

    int fired[kTracks] = {};
    bool ticks_ok = true;
    for (uint32_t tick = 0; tick < end; tick++) {
        StepEvent events[kMaxEventsPerTick];
        const int n = sched.tick(tick, 0, pattern, perform, events, kMaxEventsPerTick);
        for (int i = 0; i < n; i++) {
            const int track = events[i].track;
            const uint32_t loop_ticks = uint32_t(lengths[track]) * kStepTicks;
            if (tick % loop_ticks != 0u) ticks_ok = false;      // step 0 of its own grid
            fired[track]++;
        }
    }

    CHECK(ticks_ok);
    for (int track = 0; track < kTracks; track++) {
        const uint32_t loop_ticks = uint32_t(lengths[track]) * kStepTicks;
        CHECK_EQ(fired[track], int((end + loop_ticks - 1u) / loop_ticks));
    }

    // The point of polymeter: the 16-step and 12-step tracks line up only every 4 bars.
    CHECK_EQ(fired[0], 40);
    CHECK_EQ(fired[1], 40 * 16 / 12 + (40 * 16 % 12 ? 1 : 0));
}

TEST(every_step_of_every_length_fires_once_per_pass) {
    // The same sweep tests/test_timing.cpp runs on the mapping, but through the scheduler, so
    // that a trig on any step of any length is reachable.
    for (int length = 1; length <= 24; length++) {
        static Pattern pattern;
        blank(pattern, uint8_t(length));
        for (int step = 0; step < length; step++) put_trig(pattern, 0, step);
        pattern.tracks[0].swing = 67;
        for (int step = 0; step < length; step++) {
            pattern.tracks[0].steps[step].micro = int8_t((step % 23) - kMicroRange);
        }

        Scheduler sched;
        sched.configure(pattern);
        sched.reset();

        PerformState perform;
        const int loops = 3;
        const uint32_t end = uint32_t(length) * kStepTicks * uint32_t(loops);

        int fired[kStepsPerTrack] = {};
        for (uint32_t tick = 0; tick < end; tick++) {
            StepEvent events[kMaxEventsPerTick];
            const int n = sched.tick(tick, 0, pattern, perform, events, kMaxEventsPerTick);
            for (int i = 0; i < n; i++) fired[events[i].step]++;
        }

        // Step 1 sits at micro -11 here, so it is pulled before each pass's start: the first
        // pass's lands on tick 0 (it has no earlier tick) and the pass after the last one's
        // lands inside this window too. Every other step fires exactly once per pass.
        for (int step = 0; step < length; step++) {
            CHECK_EQ(fired[step], loops + (step == 0 ? 1 : 0));
        }
        for (int step = length; step < kStepsPerTrack; step++) CHECK_EQ(fired[step], 0);
    }
}

// --- the playhead and the chaining state ---------------------------------

TEST(the_playhead_follows_time_over_empty_steps) {
    static Pattern pattern;
    blank(pattern, 16);
    // No trigs at all: the playhead is where the sequencer is, not where it sounded.

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    bool playhead_ok = true;
    for (uint32_t step = 0; step < 16; step++) {
        sched.tick(step * kStepTicks, 0, pattern, perform, nullptr, 0);
        if (sched.playhead(0) != uint8_t(step)) playhead_ok = false;
        // Between steps it holds its value.
        sched.tick(step * kStepTicks + 1u, 0, pattern, perform, nullptr, 0);
        if (sched.playhead(0) != uint8_t(step)) playhead_ok = false;
    }
    CHECK(playhead_ok);

    CHECK_EQ(int(sched.playhead(-1)), 0);          // out of range, not a read past the array
    CHECK_EQ(int(sched.playhead(kTracks)), 0);
    CHECK(sched.last_condition(-1));
    CHECK(sched.last_condition(kTracks));
}

TEST(reset_forgets_the_chain_but_keeps_the_timing) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::FILL);

    Scheduler sched;
    sched.configure(pattern);
    sched.reset();

    PerformState perform;
    perform.fill = false;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log);
    CHECK(!sched.last_condition(0));
    CHECK_EQ(log.count, 0);

    sched.reset();
    CHECK(sched.last_condition(0));                // PRE reads true on a fresh pattern
    CHECK_EQ(int(sched.playhead(0)), 0);

    // The timing survived, so the pattern plays without a second configure().
    pattern.tracks[0].steps[0].condition = condition_pack(Condition::NONE);
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks * 2u, log);
    CHECK_EQ(log.count, 2);
}

// --- the buffer limit -----------------------------------------------------

TEST(max_is_never_written_past) {
    static Pattern pattern;
    blank(pattern, 16);
    for (int track = 0; track < kTracks; track++) {
        put_trig(pattern, track, 0).ratchet = ratchet_pack(kRatchetMax, RatchetRamp::up);
    }

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);

    PerformState perform;
    constexpr int kGuard = 4;
    constexpr int kSize = kMaxEventsPerTick + kGuard;

    // What THIS pattern demands of one tick: one step per track, each fully ratcheted. That is
    // the number the expectation below is about, and it is deliberately not kMaxEventsPerTick --
    // the two used to coincide, and tying the test to the global constant made it fail when the
    // constant was corrected upwards to cover same-track collisions.
    constexpr int kDemand = kTracks * kRatchetMax;

    for (int max = 0; max <= kMaxEventsPerTick; max++) {
        StepEvent buffer[kSize];
        for (int i = 0; i < kSize; i++) buffer[i].note = 255;   // not a note any step can emit

        sched.reset();
        const int n = sched.tick(0, 0, pattern, perform, buffer, max);

        CHECK(n <= max);
        CHECK_EQ(n, max < kDemand ? max : kDemand);
        for (int i = n; i < kSize; i++) CHECK_EQ(int(buffer[i].note), 255);
    }

    // The real bound is larger than this pattern's demand, because core/timing can land several
    // steps of ONE track on a single tick (see the collision test below).
    CHECK_EQ(kMaxEventsPerTick, kTracks * kMaxFiringsPerTick * kRatchetMax);
    CHECK(kMaxEventsPerTick >= kDemand);
}

TEST(a_null_buffer_resolves_the_tick_without_writing) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0).condition = condition_pack(Condition::NOT_PRE);
    put_trig(pattern, 1, 0).condition = condition_pack(Condition::NEI);

    PerformState perform;

    Scheduler written;
    written.configure(pattern);
    written.reset();
    Scheduler silent;
    silent.configure(pattern);
    silent.reset();

    bool identical = true;
    for (uint32_t tick = 0; tick < 4u * kStepTicks * 8u; tick++) {
        StepEvent events[kMaxEventsPerTick];
        written.tick(tick, 0, pattern, perform, events, kMaxEventsPerTick);
        const int n = silent.tick(tick, 0, pattern, perform, nullptr, 0);
        if (n != 0) identical = false;
        for (int track = 0; track < kTracks; track++) {
            if (written.playhead(track) != silent.playhead(track)) identical = false;
            if (written.last_condition(track) != silent.last_condition(track)) {
                identical = false;
            }
        }
    }
    CHECK(identical);

    // A negative max is treated as no room, not as a licence to write.
    StepEvent one;
    one.note = 255;
    CHECK_EQ(silent.tick(0, 0, pattern, perform, &one, -1), 0);
    CHECK_EQ(int(one.note), 255);
}

TEST(truncation_does_not_change_the_music) {
    // Every track is resolved whatever the buffer size, so the PRE/NEI chain and the playheads
    // cannot depend on how much room the caller gave.
    static Pattern pattern;
    blank(pattern, 16);
    for (int track = 0; track < kTracks; track++) {
        for (int step = 0; step < 16; step++) {
            Step &s = put_trig(pattern, track, step);
            s.condition = (step % 3) == 0 ? condition_pack(Condition::NOT_PRE)
                                          : condition_pack(Condition::NEI);
            s.ratchet = ratchet_pack(uint8_t(1 + (step % 8)), RatchetRamp::up);
        }
    }

    PerformState perform;

    Scheduler roomy;
    roomy.configure(pattern, kSamplesPerTick);
    roomy.reset();
    Scheduler cramped;
    cramped.configure(pattern, kSamplesPerTick);
    cramped.reset();

    bool state_matches = true;
    bool prefix_matches = true;
    for (uint32_t tick = 0; tick < 16u * kStepTicks * 4u; tick++) {
        StepEvent full[kMaxEventsPerTick];
        StepEvent few[3];
        const int n = roomy.tick(tick, 7, pattern, perform, full, kMaxEventsPerTick);
        const int m = cramped.tick(tick, 7, pattern, perform, few, 3);

        if (m != (n < 3 ? n : 3)) prefix_matches = false;
        for (int i = 0; i < m; i++) {
            if (few[i].track != full[i].track || few[i].step != full[i].step ||
                few[i].velocity != full[i].velocity || few[i].frame != full[i].frame) {
                prefix_matches = false;
            }
        }
        for (int track = 0; track < kTracks; track++) {
            if (roomy.playhead(track) != cramped.playhead(track)) state_matches = false;
            if (roomy.last_condition(track) != cramped.last_condition(track)) {
                state_matches = false;
            }
        }
    }
    CHECK(prefix_matches);
    CHECK(state_matches);
}

TEST(two_steps_of_one_track_on_one_tick_still_respect_max) {
    // At 2x a step is 12 ticks and micro-timing is +/-11, so two steps of the same track CAN
    // land on one tick -- core/timing allows up to kMaxFiringsPerTick of them. With a full
    // ratchet on each, one tick wants 8 tracks x 2 steps x 8 hits = 128 events.
    //
    // This test originally proved that the caller's limit held when the buffer was too small for
    // that, because kMaxEventsPerTick was then only 64. It was right that the header's
    // arithmetic did not cover this case, and the constant has since been corrected to
    // kTracks x kMaxFiringsPerTick x kRatchetMax. So the collision now FITS, and what this test
    // proves is the stronger property: a correctly sized buffer loses nothing even here.
    static Pattern pattern;
    blank(pattern, 4);
    for (int track = 0; track < kTracks; track++) {
        pattern.tracks[track].speed = uint8_t(Speed::x2);
        pattern.tracks[track].swing = uint8_t(kSwingMin);
        put_trig(pattern, track, 0).ratchet = ratchet_pack(kRatchetMax, RatchetRamp::none);
        put_trig(pattern, track, 1).ratchet = ratchet_pack(kRatchetMax, RatchetRamp::none);
        pattern.tracks[track].steps[0].micro = 1;     // step 0 pushed to tick 1
        pattern.tracks[track].steps[1].micro = int8_t(-kMicroRange);   // step 1 pulled to tick 1
    }

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    constexpr int kGuard = 8;
    StepEvent buffer[kMaxEventsPerTick + kGuard];
    for (int i = 0; i < kMaxEventsPerTick + kGuard; i++) buffer[i].note = 255;

    const int n = sched.tick(1, 0, pattern, perform, buffer, kMaxEventsPerTick);

    // Both steps of all eight tracks want eight hits each, and all of them now fit.
    constexpr int kDemand = kTracks * 2 * kRatchetMax;      // 128
    CHECK_EQ(n, kDemand);
    CHECK(n <= kMaxEventsPerTick);
    for (int i = n; i < kMaxEventsPerTick + kGuard; i++) CHECK_EQ(int(buffer[i].note), 255);

    // The two colliding steps really are both there, so this is a genuine collision and not a
    // dropped step.
    bool saw_step_0 = false;
    bool saw_step_1 = false;
    for (int i = 0; i < n; i++) {
        if (buffer[i].track == 0 && buffer[i].step == 0) saw_step_0 = true;
        if (buffer[i].track == 0 && buffer[i].step == 1) saw_step_1 = true;
    }
    CHECK(saw_step_0);
    CHECK(saw_step_1);
}

// --- the long run ---------------------------------------------------------

TEST(a_long_run_with_everything_active_stays_consistent) {
    // Conditions, probability, ratchets, swing, micro-timing and polymeter all at once, for
    // several thousand ticks: about a minute of music at 130 BPM.
    const uint8_t lengths[kTracks] = {16, 12, 7, 5, 16, 11, 13, 9};
    const uint8_t speeds[kTracks] = {
        uint8_t(Speed::x1),   uint8_t(Speed::x1),   uint8_t(Speed::x1_2), uint8_t(Speed::x3_4),
        uint8_t(Speed::x1_4), uint8_t(Speed::x1),   uint8_t(Speed::x1_8), uint8_t(Speed::x1),
    };

    static Pattern pattern;
    blank(pattern, 16);
    pattern.swing = 68;
    for (int track = 0; track < kTracks; track++) {
        pattern.tracks[track].length = lengths[track];
        pattern.tracks[track].speed = speeds[track];
        pattern.tracks[track].swing = (track % 2) == 0 ? kSwingInherit : uint8_t(72);

        for (int step = 0; step < int(lengths[track]); step++) {
            const int seed = track * 31 + step * 7;
            Step &s = put_trig(pattern, track, step);
            if ((seed % 11) == 0) s.flags = kStepTrigless;
            if ((seed % 13) == 0) s.flags = 0;                 // an off step in the mix
            s.micro = int8_t((seed % 23) - kMicroRange);
            s.probability = uint8_t(seed % 101);
            s.ratchet = ratchet_pack(uint8_t(1 + (seed % kRatchetMax)),
                                     RatchetRamp(seed % 3));
            switch (seed % 7) {
                case 0: s.condition = condition_pack_a_b(uint8_t(1 + (seed % 3)),
                                                         uint8_t(2 + (seed % 7))); break;
                case 1: s.condition = condition_pack(Condition::FILL); break;
                case 2: s.condition = condition_pack(Condition::NOT_FILL); break;
                case 3: s.condition = condition_pack(Condition::PRE); break;
                case 4: s.condition = condition_pack(Condition::NEI); break;
                case 5: s.condition = condition_pack(Condition::NOT_FIRST); break;
                default: s.condition = condition_pack(Condition::NONE); break;
            }
        }
    }

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();

    PerformState perform;
    const uint32_t end = 12000;      // about a minute at 130 BPM

    int total = 0;
    int max_per_tick = 0;
    bool fields_sane = true;
    bool ratchets_sane = true;
    int trigless_seen = 0;

    for (uint32_t tick = 0; tick < end; tick++) {
        perform.fill = ((tick / (16u * kStepTicks)) % 4u) == 3u;    // a fill every fourth bar

        StepEvent events[kMaxEventsPerTick];
        const int n = sched.tick(tick, 64, pattern, perform, events, kMaxEventsPerTick);
        if (n > max_per_tick) max_per_tick = n;
        total += n;

        for (int i = 0; i < n; i++) {
            const StepEvent &event = events[i];
            if (event.track >= kTracks) fields_sane = false;
            if (event.step >= lengths[event.track]) fields_sane = false;
            if (event.note > 127) fields_sane = false;
            if (event.velocity < 1 || event.velocity > 127) fields_sane = false;
            if (event.frame < 64u) fields_sane = false;
            if (event.ratchet_count < kRatchetMin || event.ratchet_count > kRatchetMax) {
                ratchets_sane = false;
            }
            if (event.ratchet_index >= event.ratchet_count) ratchets_sane = false;
            if (event.trigless) {
                trigless_seen++;
                if (event.ratchet_count != 1 || event.ratchet_index != 0) ratchets_sane = false;
            }
        }
    }

    CHECK(fields_sane);
    CHECK(ratchets_sane);
    CHECK(trigless_seen > 0);
    CHECK(total > 500);                              // the run really did play something
    CHECK(max_per_tick <= kMaxEventsPerTick);        // the documented bound holds here

    // And the whole run repeats exactly, which is what makes a mutation recallable.
    Scheduler again;
    again.configure(pattern, kSamplesPerTick);
    again.reset();
    int repeat_total = 0;
    for (uint32_t tick = 0; tick < end; tick++) {
        perform.fill = ((tick / (16u * kStepTicks)) % 4u) == 3u;
        StepEvent events[kMaxEventsPerTick];
        repeat_total += again.tick(tick, 64, pattern, perform, events, kMaxEventsPerTick);
    }
    CHECK_EQ(repeat_total, total);

    bool identical = true;
    for (int track = 0; track < kTracks; track++) {
        if (again.playhead(track) != sched.playhead(track)) identical = false;
        if (again.last_condition(track) != sched.last_condition(track)) identical = false;
    }
    CHECK(identical);
}

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(the_probability_draw_is_pinned_so_a_saved_pattern_plays_the_same_after_an_update) {
    // The other tests here prove the draw is well spread and deterministic; neither notices if
    // the hash itself changes. It must not: a pattern's seed is saved with it, and a changed
    // hash would make every saved probability step play a different sequence after an update.
    struct Draw {
        uint32_t seed;
        int track;
        int step;
        uint32_t loop;
        uint32_t roll;
    };
    const Draw draws[] = {
        {0x0u, 0, 0, 0u, 31u},
        {0x0u, 0, 0, 999u, 2u},
        {0x0u, 0, 63, 0u, 48u},
        {0x0u, 0, 63, 999u, 4u},
        {0x0u, 7, 0, 0u, 4u},
        {0x0u, 7, 0, 999u, 30u},
        {0x0u, 7, 63, 0u, 8u},
        {0x0u, 7, 63, 999u, 71u},
        {0x5EEDu, 0, 0, 0u, 36u},
        {0x5EEDu, 0, 0, 999u, 34u},
        {0x5EEDu, 0, 63, 0u, 96u},
        {0x5EEDu, 0, 63, 999u, 94u},
        {0x5EEDu, 7, 0, 0u, 14u},
        {0x5EEDu, 7, 0, 999u, 24u},
        {0x5EEDu, 7, 63, 0u, 81u},
        {0x5EEDu, 7, 63, 999u, 51u},
        {0xDEADBEEFu, 0, 0, 0u, 6u},
        {0xDEADBEEFu, 0, 0, 999u, 31u},
        {0xDEADBEEFu, 0, 63, 0u, 42u},
        {0xDEADBEEFu, 0, 63, 999u, 41u},
        {0xDEADBEEFu, 7, 0, 0u, 34u},
        {0xDEADBEEFu, 7, 0, 999u, 61u},
        {0xDEADBEEFu, 7, 63, 0u, 4u},
        {0xDEADBEEFu, 7, 63, 999u, 0u},
    };
    for (const Draw &d : draws) {
        CHECK_EQ(probability_roll(d.seed, d.track, d.step, d.loop), d.roll);
    }
}

TEST(a_ratchet_index_at_or_past_the_count_is_the_last_hit) {
    for (int ramp = 0; ramp <= int(RatchetRamp::down); ramp++) {
        const RatchetRamp r = RatchetRamp(ramp);
        CHECK_EQ(int(ratchet_velocity(100, 4, 4, r)), int(ratchet_velocity(100, 3, 4, r)));
        CHECK_EQ(int(ratchet_velocity(100, 9, 4, r)), int(ratchet_velocity(100, 3, 4, r)));
    }
    CHECK_EQ(ratchet_frame(4, 4, 1000.0f), ratchet_frame(3, 4, 1000.0f));
    CHECK_EQ(ratchet_frame(3, 4, 1000.0f), 750u);
}

TEST(a_speed_byte_one_past_the_table_plays_at_normal_speed) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 1);
    pattern.tracks[0].speed = uint8_t(Speed::count);
    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();
    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log);
    CHECK_EQ(log.count, 1);
    CHECK_EQ(log.hits[0].tick, kStepTicks);
}

TEST(shortening_a_track_to_its_playhead_puts_the_playhead_back_on_step_one) {
    static Pattern pattern;
    blank(pattern, 8);
    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();
    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks + 1u, log);
    CHECK_EQ(int(sched.playhead(0)), 4);
    blank(pattern, 4);
    sched.configure(pattern, kSamplesPerTick);
    CHECK_EQ(int(sched.playhead(0)), 0);
}

TEST(the_draw_follows_the_seed_of_the_pattern_handed_to_tick_not_the_configured_one) {
    // Two seeds that disagree about one 50% step on loop 0.
    static Pattern pattern;
    blank(pattern, 1);
    Step &s = put_trig(pattern, 0, 0);
    s.probability = 50;
    uint32_t seed_yes = 0, seed_no = 0;
    for (uint32_t seed = 1; seed < 1000 && (seed_yes == 0 || seed_no == 0); seed++) {
        if (probability_roll(seed, 0, 0, 0) < 50) {
            if (seed_yes == 0) seed_yes = seed;
        } else if (seed_no == 0) {
            seed_no = seed;
        }
    }
    CHECK(seed_yes != 0 && seed_no != 0);
    pattern.seed = seed_no;
    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();
    pattern.seed = seed_yes;                    // a mutation reseeded it; no configure() since
    PerformState perform;
    StepEvent events[kMaxEventsPerTick];
    CHECK_EQ(sched.tick(0, 0, pattern, perform, events, kMaxEventsPerTick), 1);
}

TEST(tick_with_no_buffer_or_a_full_one_writes_nothing_past_it) {
    static Pattern pattern;
    blank(pattern, 4);
    for (int t = 0; t < 3; t++) {
        Step &s = pattern.tracks[t].steps[0];
        s.flags = kStepTrigless;
        s.probability = 100;
    }
    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();
    PerformState perform;
    CHECK_EQ(sched.tick(0, 0, pattern, perform, nullptr, 4), 0);

    sched.reset();
    StepEvent events[3];
    events[1].track = 99;
    events[2].track = 99;
    CHECK_EQ(sched.tick(0, 0, pattern, perform, events, 1), 1);
    CHECK_EQ(int(events[1].track), 99);
    CHECK_EQ(int(events[2].track), 99);
}

TEST(a_trigless_event_carries_its_track_step_note_and_flags) {
    static Pattern pattern;
    blank(pattern, 4);
    for (int v = 0; v < 2; v++) {
        Step &s = pattern.tracks[5].steps[2];
        s.flags = uint8_t(kStepTrigless | kStepAccent | kStepSlide);
        s.note = uint8_t(0x80 | 61);              // the top bit is not part of a note
        s.velocity = uint8_t(v);
        s.probability = 100;
        Scheduler sched;
        sched.configure(pattern, kSamplesPerTick);
        sched.reset();
        PerformState perform;
        StepEvent events[kMaxEventsPerTick];
        int n = 0;
        for (uint32_t t = 0; t <= 2 * kStepTicks && n == 0; t++) {
            n = sched.tick(t, 7, pattern, perform, events, kMaxEventsPerTick);
        }
        CHECK_EQ(n, 1);
        CHECK(events[0].trigless);
        CHECK_EQ(int(events[0].track), 5);
        CHECK_EQ(int(events[0].step), 2);
        CHECK_EQ(int(events[0].note), 61);
        CHECK_EQ(int(events[0].velocity), 1);   // 0 and 1 both read as the quietest hit
        CHECK(events[0].accent);
        CHECK(events[0].slide);
        CHECK_EQ(events[0].frame, 7u);
    }
}

// --- play modes -------------------------------------------------------------

namespace {

// Four trigs, notes 40..43, so the order they sound in names the steps.
void four_named_steps(Pattern &pattern, PlayMode mode) {
    blank(pattern, 4);
    for (int s = 0; s < 4; s++) put_trig(pattern, 0, s).note = uint8_t(40 + s);
    pattern.tracks[0].play = uint8_t(mode);
}

}  // namespace

TEST(reverse_and_ping_pong_play_the_steps_in_their_order_and_the_playhead_follows) {
    static Pattern pattern;
    four_named_steps(pattern, PlayMode::rev);
    Scheduler sched;
    sched.configure(pattern);
    sched.reset();
    PerformState perform;
    static Log log;
    log.clear();
    for (uint32_t slot = 0; slot < 4; slot++) {
        run(sched, pattern, perform, slot * kStepTicks, (slot + 1) * kStepTicks, log);
        CHECK_EQ(int(sched.playhead(0)), 3 - int(slot));    // the light is on the step heard
    }
    CHECK_EQ(log.count, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(int(log.hits[i].event.note), 43 - i);
        CHECK_EQ(int(log.hits[i].event.step), 3 - i);
        CHECK_EQ(log.hits[i].tick, uint32_t(i) * kStepTicks);   // on the grid, as before
    }

    four_named_steps(pattern, PlayMode::ping);
    sched.configure(pattern);
    sched.reset();
    log.clear();
    run(sched, pattern, perform, 0, 8u * kStepTicks, log);
    const int ping[8] = {40, 41, 42, 43, 43, 42, 41, 40};
    CHECK_EQ(log.count, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(int(log.hits[i].event.note), ping[i]);
}

// A step under a play mode brings its condition, its ratchet and its probability with it.
TEST(a_step_played_out_of_order_keeps_its_condition_and_ratchet) {
    static Pattern pattern;
    blank(pattern, 4);
    Step &fill = put_trig(pattern, 0, 0);          // sounds in slot 3 under REV
    fill.condition = condition_pack(Condition::FILL);
    fill.ratchet = ratchet_pack(3, RatchetRamp::none);
    pattern.tracks[0].play = uint8_t(PlayMode::rev);

    Scheduler sched;
    sched.configure(pattern, kSamplesPerTick);
    sched.reset();
    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 4u * kStepTicks, log);
    CHECK_EQ(log.count, 0);                         // no FILL, no hit

    perform.fill = true;
    run(sched, pattern, perform, 4u * kStepTicks, 8u * kStepTicks, log);
    CHECK_EQ(log.count, 3);                         // the ratchet, in slot 3 of the pass
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(log.hits[i].tick, 7u * kStepTicks);
        CHECK_EQ(int(log.hits[i].event.step), 0);
        CHECK_EQ(int(log.hits[i].event.ratchet_count), 3);
    }
}

// PRE reads the step that sounded before it, in time. Forward, step 1's PRE follows step 0
// (passes); reversed, it follows step 2 (a FILL that fails without the pad).
TEST(pre_chains_in_the_order_the_steps_sound) {
    static Pattern pattern;
    blank(pattern, 4);
    put_trig(pattern, 0, 0);
    put_trig(pattern, 0, 1).condition = condition_pack(Condition::PRE);
    put_trig(pattern, 0, 2).condition = condition_pack(Condition::FILL);
    PerformState perform;
    static Log log;

    for (int mode = 0; mode < 2; mode++) {
        pattern.tracks[0].play = uint8_t(mode == 0 ? PlayMode::fwd : PlayMode::rev);
        Scheduler sched;
        sched.configure(pattern);
        sched.reset();
        log.clear();
        run(sched, pattern, perform, 0, 4u * kStepTicks, log);
        bool step1 = false;
        for (int i = 0; i < log.count; i++) step1 = step1 || log.hits[i].event.step == 1;
        CHECK_EQ(step1, mode == 0);
    }
}

// The mode is read on every tick, like micro-timing: an edit (or an undo) is heard on the next
// step with no configure().
TEST(a_play_mode_change_is_heard_on_the_next_step_without_a_configure) {
    static Pattern pattern;
    four_named_steps(pattern, PlayMode::fwd);
    Scheduler sched;
    sched.configure(pattern);
    sched.reset();
    PerformState perform;
    static Log log;
    log.clear();
    run(sched, pattern, perform, 0, 2u * kStepTicks, log);
    pattern.tracks[0].play = uint8_t(PlayMode::rev);
    run(sched, pattern, perform, 2u * kStepTicks, 4u * kStepTicks, log);
    CHECK_EQ(log.count, 4);
    CHECK_EQ(int(log.hits[2].event.note), 41);     // slot 2 reversed is step 1
    CHECK_EQ(int(log.hits[3].event.note), 40);
}

// RND is seeded by the pattern: the same pattern plays the same stream every time it is
// played (an emulator scenario can count on it), another seed plays another one.
TEST(random_play_repeats_exactly_for_one_seed_and_differs_for_another) {
    static Pattern pattern;
    blank(pattern, 16);
    for (int t = 0; t < 3; t++) {
        for (int s = 0; s < 16; s++) put_trig(pattern, t, s).note = uint8_t(30 + s);
        pattern.tracks[t].play = uint8_t(int(PlayMode::rnd1) + t);
    }
    PerformState perform;
    static Log first, again, other;
    Log *const logs[3] = {&first, &again, &other};
    for (Log *log : logs) {
        pattern.seed = log == &other ? 0x1234u : 0x5EEDu;
        Scheduler sched;
        sched.configure(pattern);
        sched.reset();
        log->clear();
        run(sched, pattern, perform, 0, 8u * 16u * kStepTicks, *log);
    }
    CHECK_EQ(first.count, 3 * 16 * 8);
    CHECK(same_stream(first, again));
    CHECK(!same_stream(first, other));
    // And it is not forward: some slot of the first track plays another step.
    bool moved = false;
    for (int i = 0; i < first.count; i++) {
        const Hit &h = first.hits[i];
        if (h.event.track == 0 && h.event.step != uint8_t((h.tick / kStepTicks) % 16u)) moved = true;
    }
    CHECK(moved);
}
