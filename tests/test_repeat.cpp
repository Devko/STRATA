// Beat repeat.
//
// The load figure for this is the one thing that matters most about it and the one thing a host
// cannot answer: it is a per-block tax on the master output, and docs/BUDGET.md needs it measured
// with and without cl6x. What a host CAN settle is whether it loops the right audio, and that is
// what is here.
//
// Three properties carry the weight. It must loop exactly the last N frames that played, because
// a repeat that is a frame out drifts audibly over a few passes. It must not capture while held,
// or the loop decays into a copy of its own first pass. And the pitch-down must be an exact half
// rate, so an octave is an octave.
//
// The ring is 2.93 MiB, so the BeatRepeat lives at file scope -- CLAUDE.md caps a stack frame at
// 16 KiB.
#include "check.hpp"

#include "repeat.hpp"

#include <initializer_list>

namespace {

using namespace strata;

constexpr float kRate = 48000.0f;
constexpr int kBlock = 64;              // ENAGA_BLOCK_FRAMES

BeatRepeat g_repeat;
bool g_ready = false;

float g_block[2 * kBlock];

void ensure_init() {
    if (g_ready) return;
    g_ready = g_repeat.init(kRate);
    CHECK(g_ready);
}

// A block whose every frame is identifiable: left carries the frame counter, right its negative.
// So a test can say exactly which frame came out.
void fill_counting(float *out, int frames, int first) {
    for (int i = 0; i < frames; i++) {
        out[2 * i + 0] = float(first + i);
        out[2 * i + 1] = -float(first + i);
    }
}

// Push `blocks` blocks of counting audio through, starting the counter at `first`.
int feed(int first, int blocks) {
    int at = first;
    for (int b = 0; b < blocks; b++) {
        fill_counting(g_block, kBlock, at);
        g_repeat.process(g_block, kBlock);
        at += kBlock;
    }
    return at;
}

}  // namespace

// --- the note values ------------------------------------------------------

TEST(repeat_lengths_are_note_values_at_96_ppqn) {
    // A quarter note is the whole 96, and each halving halves it. A triplet is two thirds of the
    // plain value.
    CHECK_EQ(repeat_length_ticks(RepeatLength::quarter), 96);
    CHECK_EQ(repeat_length_ticks(RepeatLength::eighth), 48);
    CHECK_EQ(repeat_length_ticks(RepeatLength::sixteenth), 24);
    CHECK_EQ(repeat_length_ticks(RepeatLength::thirty_second), 12);
    CHECK_EQ(repeat_length_ticks(RepeatLength::quarter_triplet), 64);
    CHECK_EQ(repeat_length_ticks(RepeatLength::eighth_triplet), 32);
    CHECK_EQ(repeat_length_ticks(RepeatLength::sixteenth_triplet), 16);

    // Three triplets fill the plain value they are a triplet of: 3 x 64 is two quarter notes,
    // which is what a quarter-note triplet means.
    CHECK_EQ(3 * repeat_length_ticks(RepeatLength::quarter_triplet),
             2 * repeat_length_ticks(RepeatLength::quarter));
    CHECK_EQ(3 * repeat_length_ticks(RepeatLength::eighth_triplet),
             2 * repeat_length_ticks(RepeatLength::eighth));

    // Every length divides the ring at the slowest tempo the clock allows, which is the claim
    // dsp/repeat.hpp makes about two bars at 60 BPM being headroom: a quarter note at 20 BPM is
    // 3 s against the ring's 8.
    const float seconds_per_tick_at_20bpm = 60.0f / (20.0f * 96.0f);
    const float longest =
        float(repeat_length_ticks(RepeatLength::quarter)) * seconds_per_tick_at_20bpm;
    CHECK(longest < kRepeatSeconds);
    CHECK_NEAR(longest, 3.0f, 0.001f);
}

TEST(repeat_length_names_fit_the_font) {
    for (int i = 0; i < int(RepeatLength::count); i++) {
        const char *name = repeat_length_name(RepeatLength(i));
        int length = 0;
        for (const char *p = name; *p != '\0'; p++) {
            const char c = *p;
            CHECK((c >= '0' && c <= '9') || c == '/' || c == 'T');
            length++;
        }
        CHECK(length >= 3);
        CHECK(length <= 6);
    }
    CHECK_EQ(repeat_length_name(RepeatLength::quarter), "1/4");
    CHECK_EQ(repeat_length_name(RepeatLength::sixteenth_triplet), "1/16T");
    CHECK_EQ(repeat_length_name(RepeatLength(99)), "?");
}

// REPEAT + RIGHT is faster: every step shortens the length, the walk visits every length once,
// and it stops at both ends rather than wrapping from 1/32 to 1/4 under a held finger.
TEST(repeat_length_step_walks_by_ticks_and_stops_at_the_ends) {
    RepeatLength at = RepeatLength::quarter;
    int seen = 1;
    for (int i = 0; i < int(RepeatLength::count) - 1; i++) {
        const RepeatLength next = repeat_length_step(at, +1);
        CHECK(repeat_length_ticks(next) < repeat_length_ticks(at));
        CHECK(repeat_length_step(next, -1) == at);
        at = next;
        seen++;
    }
    CHECK_EQ(seen, int(RepeatLength::count));
    CHECK(at == RepeatLength::thirty_second);
    CHECK(repeat_length_step(RepeatLength::thirty_second, +1) == RepeatLength::thirty_second);
    CHECK(repeat_length_step(RepeatLength::quarter, -1) == RepeatLength::quarter);
    CHECK(repeat_length_step(RepeatLength::sixteenth, +1) == RepeatLength::sixteenth_triplet);
    CHECK(repeat_length_step(RepeatLength::sixteenth, -1) == RepeatLength::eighth_triplet);
    CHECK(repeat_length_step(RepeatLength::eighth, 0) == RepeatLength::eighth);
}

// --- the ring -------------------------------------------------------------

TEST(repeat_the_ring_is_two_bars_at_sixty_bpm) {
    ensure_init();
    // 8 seconds, which is what docs/archive/REVIEW.md sized at 2.93 MiB stereo float.
    CHECK_EQ(g_repeat.ring_frames(), uint32_t(kRate * kRepeatSeconds));
    const uint32_t bytes = g_repeat.ring_frames() * 2u * uint32_t(sizeof(float));
    CHECK_EQ(bytes, 3072000u);
    CHECK_NEAR(double(bytes) / (1024.0 * 1024.0), 2.93, 0.01);
}

TEST(repeat_is_silent_about_its_work_when_idle) {
    ensure_init();
    g_repeat.hold(false);
    g_repeat.set(1000, false);
    CHECK(!g_repeat.active());

    // Idle, the buffer passes through untouched.
    fill_counting(g_block, kBlock, 500);
    g_repeat.process(g_block, kBlock);
    for (int i = 0; i < kBlock; i++) {
        CHECK_NEAR(g_block[2 * i + 0], float(500 + i), 0.0f);
        CHECK_NEAR(g_block[2 * i + 1], -float(500 + i), 0.0f);
    }
}

TEST(repeat_cannot_loop_audio_it_was_never_given) {
    // A press a moment after the transport starts. Looping a ring of silence would be a gap in
    // the music that looks like a crash; active() says there is nothing to loop instead.
    BeatRepeat fresh;
    CHECK(fresh.init(kRate));
    CHECK_EQ(fresh.captured(), 0u);

    fresh.set(4800, false);                  // 100 ms
    fresh.hold(true);
    CHECK(!fresh.active());
    CHECK_EQ(fresh.loop_frames(), 0u);

    // The buffer is left alone while there is nothing to play.
    fill_counting(g_block, kBlock, 7);
    fresh.process(g_block, kBlock);
    for (int i = 0; i < kBlock; i++) CHECK_NEAR(g_block[2 * i + 0], float(7 + i), 0.0f);

    // Once enough has gone through, the same press works.
    fresh.hold(false);
    for (int b = 0; b < 200; b++) {          // 12,800 frames
        fill_counting(g_block, kBlock, b * kBlock);
        fresh.process(g_block, kBlock);
    }
    fresh.hold(true);
    CHECK(fresh.active());
    CHECK_EQ(fresh.loop_frames(), 4800u);
}

TEST(repeat_a_loop_longer_than_what_was_captured_is_shortened) {
    BeatRepeat fresh;
    CHECK(fresh.init(kRate));
    for (int b = 0; b < 10; b++) {           // 640 frames
        fill_counting(g_block, kBlock, b * kBlock);
        fresh.process(g_block, kBlock);
    }
    CHECK_EQ(fresh.captured(), 640u);

    fresh.set(48000, false);                 // a whole second, far more than exists
    fresh.hold(true);
    CHECK(fresh.active());
    CHECK_EQ(fresh.loop_frames(), 640u);     // exactly what there was
}

TEST(repeat_a_loop_longer_than_the_ring_is_clamped_to_it) {
    ensure_init();
    g_repeat.hold(false);
    feed(0, 100);
    g_repeat.set(0xFFFFFFFFu, false);
    g_repeat.hold(true);
    CHECK(g_repeat.loop_frames() <= g_repeat.ring_frames());
    g_repeat.hold(false);
}

// --- what it loops --------------------------------------------------------

TEST(repeat_loops_exactly_the_last_frames_that_played) {
    // The property that matters most: a repeat a frame out drifts audibly over a few passes.
    // The counting audio makes it exact -- the first frame out must be the first frame of the
    // last `length` that went in.
    ensure_init();
    g_repeat.hold(false);
    const int next = feed(0, 100);           // frames 0 .. 6399
    const int length = 256;
    g_repeat.set(uint32_t(length), false);
    g_repeat.hold(true);
    CHECK(g_repeat.active());
    CHECK_EQ(g_repeat.loop_frames(), uint32_t(length));

    // The loop is the last 256 frames: next-256 .. next-1.
    const int first = next - length;
    for (int block = 0; block < 8; block++) {
        for (int i = 0; i < 2 * kBlock; i++) g_block[i] = 999.0f;
        g_repeat.process(g_block, kBlock);
        for (int i = 0; i < kBlock; i++) {
            const int frame = block * kBlock + i;
            const int want = first + (frame % length);
            CHECK_NEAR(g_block[2 * i + 0], float(want), 0.0f);
            CHECK_NEAR(g_block[2 * i + 1], -float(want), 0.0f);
        }
    }
    g_repeat.hold(false);
}

TEST(repeat_wraps_at_the_loops_end_not_the_rings) {
    // A loop that straddles the ring's own wrap. The read has to turn round at the LOOP's end
    // while still following the ring's, which is two wraps in one expression and the easiest
    // thing here to get wrong.
    BeatRepeat fresh;
    CHECK(fresh.init(kRate));
    const int ring = int(fresh.ring_frames());
    const int length = 512;

    // Fill the ring most of the way round, so the next capture crosses zero and the loop sits
    // across the seam.
    int at = 0;
    const int blocks = (ring - length / 2) / kBlock;
    for (int b = 0; b < blocks; b++) {
        fill_counting(g_block, kBlock, at);
        fresh.process(g_block, kBlock);
        at += kBlock;
    }
    // And over the seam.
    for (int b = 0; b < length / kBlock; b++) {
        fill_counting(g_block, kBlock, at);
        fresh.process(g_block, kBlock);
        at += kBlock;
    }

    fresh.set(uint32_t(length), false);
    fresh.hold(true);
    CHECK_EQ(fresh.loop_frames(), uint32_t(length));
    const int first = at - length;
    for (int block = 0; block < 16; block++) {
        fresh.process(g_block, kBlock);
        for (int i = 0; i < kBlock; i++) {
            const int frame = block * kBlock + i;
            CHECK_NEAR(g_block[2 * i + 0], float(first + (frame % length)), 0.0f);
        }
    }
}

TEST(repeat_does_not_record_itself) {
    // dsp/repeat.hpp's rule. If the ring followed the output while held, the history would be
    // overwritten by the repeat and the loop would decay into a copy of its own first pass --
    // and the second pass would differ from the first. It must not.
    ensure_init();
    g_repeat.hold(false);
    feed(0, 100);
    const uint32_t captured_before = g_repeat.captured();
    const int length = 128;
    g_repeat.set(uint32_t(length), false);
    g_repeat.hold(true);

    // Four passes of the loop, all identical.
    float first_pass[2 * 128] = {};
    for (int pass = 0; pass < 4; pass++) {
        for (int i = 0; i < length; i += kBlock) {
            g_repeat.process(g_block, kBlock);
            for (int k = 0; k < 2 * kBlock; k++) {
                if (pass == 0) first_pass[2 * i + k] = g_block[k];
                else CHECK_NEAR(g_block[k], first_pass[2 * i + k], 0.0f);
            }
        }
    }
    // And the captured history did not move: nothing was written while held.
    CHECK_EQ(g_repeat.captured(), captured_before);
    g_repeat.hold(false);
}

TEST(repeat_releasing_returns_the_live_master_at_once) {
    ensure_init();
    g_repeat.hold(false);
    feed(0, 100);
    g_repeat.set(128, false);
    g_repeat.hold(true);
    g_repeat.process(g_block, kBlock);
    CHECK(g_repeat.active());

    g_repeat.hold(false);
    CHECK(!g_repeat.active());
    fill_counting(g_block, kBlock, 70000);
    g_repeat.process(g_block, kBlock);
    for (int i = 0; i < kBlock; i++) {
        CHECK_NEAR(g_block[2 * i + 0], float(70000 + i), 0.0f);
    }
}

TEST(repeat_a_second_press_takes_a_fresh_loop) {
    // The length `set` asks for while held applies to the NEXT press, and each press takes the
    // audio that has just played rather than the one before.
    ensure_init();
    g_repeat.hold(false);
    const int first_next = feed(0, 100);
    g_repeat.set(128, false);
    g_repeat.hold(true);
    g_repeat.process(g_block, kBlock);
    CHECK_NEAR(g_block[0], float(first_next - 128), 0.0f);
    g_repeat.hold(false);

    const int second_next = feed(first_next, 20);
    g_repeat.hold(true);
    g_repeat.process(g_block, kBlock);
    CHECK_NEAR(g_block[0], float(second_next - 128), 0.0f);
    g_repeat.hold(false);
}

TEST(repeat_holding_is_a_transition_not_a_state_to_reassert) {
    // `hold(true)` every block is the obvious way to wire a held pad -- the engine knows whether
    // the pad is down and says so once a block. If each call re-armed, the loop would restart
    // every 64 frames and stutter at the block rate instead of looping at its own length. So
    // only a CHANGE arms it, and this is the test that says so, because nothing else here calls
    // hold twice in a row.
    ensure_init();
    g_repeat.hold(false);
    const int next = feed(0, 100);
    const int length = 256;
    g_repeat.set(uint32_t(length), false);

    g_repeat.hold(true);
    const float first = float(next - length);

    // Four blocks, re-asserting the hold before each one as a per-block wiring would.
    for (int block = 0; block < 4; block++) {
        g_repeat.hold(true);
        g_repeat.process(g_block, kBlock);
        for (int i = 0; i < kBlock; i++) {
            const int frame = block * kBlock + i;
            CHECK_NEAR(g_block[2 * i + 0], first + float(frame % length), 0.0f);
        }
    }

    // And re-asserting the release is equally harmless.
    g_repeat.hold(false);
    g_repeat.hold(false);
    CHECK(!g_repeat.active());
    fill_counting(g_block, kBlock, 90000);
    g_repeat.process(g_block, kBlock);
    CHECK_NEAR(g_block[0], 90000.0f, 0.0f);
}

TEST(repeat_a_length_change_while_held_waits_for_the_next_press) {
    // Moving the start under a running loop would jump the read position, which is a click
    // rather than a musical change.
    ensure_init();
    g_repeat.hold(false);
    feed(0, 100);
    g_repeat.set(256, false);
    g_repeat.hold(true);
    CHECK_EQ(g_repeat.loop_frames(), 256u);
    g_repeat.set(64, false);
    CHECK_EQ(g_repeat.loop_frames(), 256u);     // still this pass's length
    g_repeat.hold(false);
    g_repeat.hold(true);
    CHECK_EQ(g_repeat.loop_frames(), 64u);      // and the next press takes it
    g_repeat.hold(false);
}

// --- pitch down -----------------------------------------------------------

TEST(repeat_pitch_down_reads_at_exactly_half_rate) {
    // Half rate is an exact (0, 2^31) step, so an octave is an octave and not an approximation.
    // With counting audio, frame k out must be the loop's frame k/2 -- and the odd frames must
    // be the midpoint, since the interpolation is linear.
    ensure_init();
    g_repeat.hold(false);
    const int next = feed(0, 100);
    const int length = 256;
    g_repeat.set(uint32_t(length), true);
    g_repeat.hold(true);
    CHECK(g_repeat.pitch_down());

    const int first = next - length;
    g_repeat.process(g_block, kBlock);
    for (int i = 0; i < kBlock; i++) {
        // Position advances by 0.5 a frame: frame i reads loop position i/2.
        const float want = float(first) + float(i) * 0.5f;
        CHECK_NEAR(g_block[2 * i + 0], want, 0.001f);
    }
    g_repeat.hold(false);
}

TEST(repeat_pitch_down_takes_twice_as_long_to_come_round) {
    // The other half of halving the rate: the loop plays through in twice the frames, which is
    // what makes it musically a repeat an octave down rather than a shorter one.
    ensure_init();
    g_repeat.hold(false);
    const int next = feed(0, 100);
    const int length = 128;
    g_repeat.set(uint32_t(length), true);
    g_repeat.hold(true);
    const float first_out = float(next - length);

    // Every frame of two whole passes, against the position a half-rate read should be at. The
    // LAST frame of each pass is the seam and is checked separately below, because there the
    // interpolation closes the circle rather than continuing the ramp.
    int wrong = 0;
    for (int frame = 0; frame < 2 * length; frame++) {
        if (frame % kBlock == 0) g_repeat.process(g_block, kBlock);
        const int i = frame % kBlock;
        const int offset = frame / 2;
        const bool half = (frame & 1) != 0;
        if (offset == length - 1 && half) continue;        // the seam
        const float want = first_out + float(offset) + (half ? 0.5f : 0.0f);
        const float got = g_block[2 * i + 0];
        if (got < want - 0.001f || got > want + 0.001f) wrong++;
    }
    CHECK_EQ(wrong, 0);

    // And after exactly 2 * length frames it is back at the loop's first frame.
    g_repeat.process(g_block, kBlock);
    CHECK_NEAR(g_block[0], first_out, 0.001f);
    g_repeat.hold(false);
}

TEST(repeat_the_loops_seam_closes_the_circle) {
    // At the loop's last frame the interpolation partner is the loop's FIRST frame, not whatever
    // the ring holds after it. That is what makes a pitched-down loop seamless instead of
    // clicking once per pass, and it is only visible at a fractional rate -- at unity the
    // fraction is always zero and the seam never interpolates.
    //
    // With counting audio the seam value is therefore the midpoint of the loop's ends, which
    // reads as a jump backwards and is correct: the ramp is an artefact of the test signal, and
    // real audio has no reason to be continuous across a loop point.
    ensure_init();
    g_repeat.hold(false);
    const int next = feed(0, 100);
    const int length = 128;
    g_repeat.set(uint32_t(length), true);
    g_repeat.hold(true);
    const float first = float(next - length);
    const float last = first + float(length - 1);

    // Frame 2 * length - 1 is the seam: offset length-1 with a half fraction.
    for (int frame = 0; frame < 2 * length; frame++) {
        if (frame % kBlock == 0) g_repeat.process(g_block, kBlock);
        if (frame != 2 * length - 1) continue;
        const int i = frame % kBlock;
        CHECK_NEAR(g_block[2 * i + 0], (last + first) * 0.5f, 0.001f);
        CHECK_NEAR(g_block[2 * i + 1], -(last + first) * 0.5f, 0.001f);
    }
    g_repeat.hold(false);
}

TEST(repeat_never_leaves_the_ring_whatever_it_is_asked_for) {
    // The one failure that would not be audible as a glitch but as garbage: an out-of-range read
    // on this chip does not fault, it silently returns whatever is there. Driven over every
    // length, both rates, and many blocks, with the output checked to stay inside the range the
    // counting audio can produce.
    BeatRepeat fresh;
    CHECK(fresh.init(kRate));
    int at = 0;
    for (int b = 0; b < 400; b++) {
        fill_counting(g_block, kBlock, at);
        fresh.process(g_block, kBlock);
        at += kBlock;
    }

    int outside = 0;
    for (int kind = 0; kind < int(RepeatLength::count); kind++) {
        for (bool down : {false, true}) {
            // 130 BPM: samples per tick is 48000 * 60 / (130 * 96).
            const float samples_per_tick = kRate * 60.0f / (130.0f * 96.0f);
            const uint32_t frames =
                uint32_t(float(repeat_length_ticks(RepeatLength(kind))) * samples_per_tick);
            fresh.hold(false);
            fresh.set(frames, down);
            fresh.hold(true);
            for (int b = 0; b < 40; b++) {
                fresh.process(g_block, kBlock);
                for (int i = 0; i < kBlock; i++) {
                    const float l = g_block[2 * i + 0];
                    const float r = g_block[2 * i + 1];
                    if (l < 0.0f || l > float(at)) outside++;
                    if (r > 0.0f || r < -float(at)) outside++;
                }
            }
        }
    }
    CHECK_EQ(outside, 0);
    fresh.hold(false);
}

TEST(repeat_survives_a_block_of_no_frames_and_an_uninitialised_object) {
    // audio() is never called with 0 frames, but a scenario script can, and an uninitialised
    // object is what a failed init leaves.
    BeatRepeat bare;
    bare.set(100, true);
    bare.hold(true);
    CHECK(!bare.active());
    fill_counting(g_block, kBlock, 1);
    bare.process(g_block, kBlock);
    for (int i = 0; i < kBlock; i++) CHECK_NEAR(g_block[2 * i + 0], float(1 + i), 0.0f);

    ensure_init();
    g_repeat.process(g_block, 0);
    g_repeat.process(g_block, -4);
}

// --- gaps a mutation run found (docs/MUTATION.md) -------------------------------------------

namespace {

BeatRepeat g_fresh;
bool g_fresh_ready = false;

BeatRepeat &fresh() {
    if (!g_fresh_ready) g_fresh_ready = g_fresh.init(kRate);
    CHECK(g_fresh_ready);
    return g_fresh;
}

}  // namespace

TEST(repeat_a_one_frame_loop_repeats_that_frame) {
    // The shortest loop is one frame, and a request for none is that frame too, not a ring of
    // silence and not two frames.
    BeatRepeat &r = fresh();
    for (uint32_t ask : {0u, 1u}) {
        r.hold(false);
        r.set(ask, false);
        fill_counting(g_block, kBlock, 5000);
        r.process(g_block, kBlock);                 // the last frame captured is 5063
        r.hold(true);
        CHECK(r.active());
        fill_counting(g_block, kBlock, 0);
        r.process(g_block, kBlock);
        for (int i = 0; i < kBlock; i++) CHECK_EQ(g_block[2 * i], 5063.0f);
        r.hold(false);
    }
}

TEST(repeat_a_new_press_starts_on_a_whole_frame_after_a_pitched_down_one) {
    // An octave down advances half a frame a sample, so a release mid-way leaves half a frame
    // behind. The next press starts on the loop's first frame exactly, not between two.
    BeatRepeat &r = fresh();
    r.hold(false);
    r.set(16, true);
    fill_counting(g_block, kBlock, 100);
    r.process(g_block, kBlock);                     // the loop is 148..163
    r.hold(true);
    fill_counting(g_block, 3, 0);
    r.process(g_block, 3);                          // three samples: 1.5 frames in
    r.hold(false);
    r.set(16, false);
    r.hold(true);
    fill_counting(g_block, 1, 0);
    r.process(g_block, 1);
    CHECK_EQ(g_block[0], 148.0f);
    r.hold(false);
}

TEST(repeat_captures_a_one_frame_block) {
    BeatRepeat &r = fresh();
    r.hold(false);
    r.set(1, false);
    fill_counting(g_block, kBlock, 0);
    r.process(g_block, kBlock);
    fill_counting(g_block, 1, 777);
    r.process(g_block, 1);                          // idle: captured
    r.hold(true);
    fill_counting(g_block, kBlock, 0);
    r.process(g_block, kBlock);
    CHECK_EQ(g_block[0], 777.0f);
    r.hold(false);
}
