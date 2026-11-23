// dsp/capture: resampling's take buffer.
//
// The module is small and the things worth testing are its EDGES, because every one of them is a
// way to ruin a loop: a take one block too long does not loop, a take that restarts halfway
// through loses what was recorded, and a take the card has not had yet must not be overwritten.
//
// The conversion is tested against an exact oracle rather than a tolerance. int16 is a fixed
// point format and 32767 is a chosen constant, so "the right value" is a number and not a range
// -- and the one place that matters is a master sitting at exactly +/-1.0, which is where the
// limiter puts it and where a wrong clamp wraps full scale to silence.
#include "check.hpp"

#include <cstdint>

#include <enaga/abi.h>

#include "capture.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);

// A block of interleaved stereo, left and right distinguishable so a channel swap cannot pass.
void fill_block(float *out, int frames, float left, float right) {
    for (int i = 0; i < frames; i++) {
        out[2 * i] = left;
        out[2 * i + 1] = right;
    }
}

// A ramp, so a test can say WHICH frame a sample came from.
void fill_ramp(float *out, int frames, int first) {
    for (int i = 0; i < frames; i++) {
        const float v = float(first + i) * (1.0f / 100000.0f);
        out[2 * i] = v;
        out[2 * i + 1] = -v;
    }
}

float g_block[2 * kBlk];

}  // namespace

TEST(capture_starts_idle_and_holds_thirty_seconds) {
    Capture capture;
    CHECK(capture.init(kRate));

    CHECK(!capture.armed());
    CHECK(!capture.recording());
    CHECK(!capture.done());
    CHECK(!capture.clamped());
    CHECK_EQ(int(capture.frames()), 0);

    // 30 s, which is dsp/capture.hpp's kCaptureSeconds and is not a round guess: app/storage.hpp
    // sizes its loader from kMaxSampleSeconds, also 30, so a longer take could be written to the
    // card and then not loaded back into a track.
    CHECK_EQ(int(capture.capacity()), 30 * int(ENAGA_SAMPLE_RATE));
}

TEST(capture_records_exactly_the_frames_it_was_asked_for) {
    // The whole point of the module: a take of N frames is N frames, ending mid-block if that is
    // where N falls. A take a block too long does not loop, and nothing downstream can fix it.
    Capture capture;
    CHECK(capture.init(kRate));

    const uint32_t wanted = uint32_t(2 * kBlk + 17);   // deliberately not a block multiple
    CHECK(capture.arm(wanted));
    CHECK(capture.armed());
    CHECK_EQ(int(capture.wanted()), int(wanted));
    CHECK(!capture.recording());

    // Armed is not recording: a block that arrives before `begin()` is not in the take.
    fill_block(g_block, kBlk, 0.5f, 0.5f);
    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), 0);

    capture.begin();
    CHECK(capture.recording());

    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), kBlk);
    CHECK(capture.recording());

    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), 2 * kBlk);
    CHECK(capture.recording());

    // The third block overshoots, and the take stops on its frame rather than at the block's end.
    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), int(wanted));
    CHECK(capture.done());
    CHECK(!capture.recording());

    // And a block after that changes nothing.
    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), int(wanted));
}

TEST(capture_keeps_the_frames_in_order_and_the_channels_apart) {
    // A ramp says which frame each sample came from, and the right channel is the negation of the
    // left, so an off-by-one, a reversed block or a channel swap all fail here.
    Capture capture;
    CHECK(capture.init(kRate));

    const int blocks = 3;
    CHECK(capture.arm(uint32_t(blocks * kBlk)));
    capture.begin();
    for (int b = 0; b < blocks; b++) {
        fill_ramp(g_block, kBlk, b * kBlk);
        capture.process(g_block, kBlk);
    }
    CHECK(capture.done());
    CHECK_EQ(int(capture.frames()), blocks * kBlk);

    const int16_t *pcm = capture.data();
    bool ordered = true;
    bool mirrored = true;
    for (int i = 0; i < blocks * kBlk; i++) {
        const int16_t expect = int16_t(float(i) * (1.0f / 100000.0f) * 32767.0f);
        if (pcm[2 * i] != expect) ordered = false;
        if (pcm[2 * i + 1] != int16_t(-expect)) mirrored = false;
    }
    CHECK(ordered);
    CHECK(mirrored);
}

TEST(capture_converts_full_scale_without_wrapping) {
    // Where the limiter puts a loud master: exactly +/-1.0. A clamp that lets 1.0 through as
    // 32768 wraps it to -32768, which turns the loudest part of a take into the loudest possible
    // noise in the opposite direction -- audible, and the kind of thing that only shows up on a
    // take someone pushed hard.
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(capture.arm(uint32_t(4 * kBlk)));
    capture.begin();

    fill_block(g_block, kBlk, 1.0f, -1.0f);
    capture.process(g_block, kBlk);
    fill_block(g_block, kBlk, 2.0f, -2.0f);          // past full scale, which a limiter can emit
    capture.process(g_block, kBlk);
    fill_block(g_block, kBlk, 0.0f, 0.0f);
    capture.process(g_block, kBlk);
    fill_block(g_block, kBlk, 1.0f / 32767.0f, -1.0f / 32767.0f);   // one quantum
    capture.process(g_block, kBlk);
    CHECK(capture.done());

    const int16_t *pcm = capture.data();
    CHECK_EQ(int(pcm[0]), 32767);
    CHECK_EQ(int(pcm[1]), -32767);
    CHECK_EQ(int(pcm[2 * kBlk]), 32767);             // clamped, not wrapped
    CHECK_EQ(int(pcm[2 * kBlk + 1]), -32768);        // the floor is genuinely -32768
    CHECK_EQ(int(pcm[4 * kBlk]), 0);
    CHECK_EQ(int(pcm[6 * kBlk]), 1);
    CHECK_EQ(int(pcm[6 * kBlk + 1]), -1);

    // Nothing in a take may be a wrapped sign: the loudest sample and its neighbour must agree.
    bool no_wrap = true;
    for (int i = 0; i < kBlk; i++) {
        if (pcm[2 * i] < 0 || pcm[2 * i + 1] > 0) no_wrap = false;
    }
    CHECK(no_wrap);
}

TEST(capture_leaves_what_the_user_hears_alone) {
    // A capture is a tap. If it wrote through `master` the take would be recorded AND the output
    // changed, which is the worst kind of bug to find by ear.
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(capture.arm(uint32_t(kBlk)));
    capture.begin();

    fill_ramp(g_block, kBlk, 0);
    float before[2 * kBlk];
    for (int i = 0; i < 2 * kBlk; i++) before[i] = g_block[i];

    capture.process(g_block, kBlk);

    bool untouched = true;
    for (int i = 0; i < 2 * kBlk; i++) {
        if (g_block[i] != before[i]) untouched = false;
    }
    CHECK(untouched);
}

TEST(capture_refuses_to_restart_a_take_in_flight) {
    // A second press during a take must not throw away what has been recorded. There is no undo
    // for a lost take: the audio is gone.
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(capture.arm(uint32_t(4 * kBlk)));
    capture.begin();
    fill_block(g_block, kBlk, 0.5f, 0.5f);
    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), kBlk);

    CHECK(!capture.arm(uint32_t(8 * kBlk)));     // refused
    CHECK(capture.recording());                  // and the take carries on
    CHECK_EQ(int(capture.wanted()), 4 * kBlk);
    CHECK_EQ(int(capture.frames()), kBlk);

    // An ARMED take, on the other hand, is replaceable: nothing has been recorded yet.
    Capture other;
    CHECK(other.init(kRate));
    CHECK(other.arm(uint32_t(4 * kBlk)));
    CHECK(other.arm(uint32_t(8 * kBlk)));
    CHECK_EQ(int(other.wanted()), 8 * kBlk);
}

TEST(capture_refuses_to_overwrite_a_take_the_card_has_not_had) {
    // `done` means the main context owns the buffer. Arming over it would hand a save a buffer
    // being rewritten by the audio side -- a torn WAV, and the two contexts' one rule broken.
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(capture.arm(uint32_t(kBlk)));
    capture.begin();
    fill_block(g_block, kBlk, 0.5f, 0.5f);
    capture.process(g_block, kBlk);
    CHECK(capture.done());

    CHECK(!capture.arm(uint32_t(kBlk)));
    CHECK(capture.done());
    CHECK_EQ(int(capture.frames()), kBlk);

    // Released, it takes a new one.
    capture.release();
    CHECK(!capture.done());
    CHECK_EQ(int(capture.frames()), 0);
    CHECK(capture.arm(uint32_t(2 * kBlk)));
    CHECK(capture.armed());
}

TEST(capture_cancel_drops_an_armed_or_running_take_but_not_a_finished_one) {
    // A stop, a pattern change or a card removal cancels. A finished take is the main context's
    // and must survive all three, or a save in progress loses its buffer.
    Capture capture;
    CHECK(capture.init(kRate));

    CHECK(capture.arm(uint32_t(4 * kBlk)));
    capture.cancel();
    CHECK(!capture.armed());
    CHECK_EQ(int(capture.wanted()), 0);

    CHECK(capture.arm(uint32_t(4 * kBlk)));
    capture.begin();
    fill_block(g_block, kBlk, 0.5f, 0.5f);
    capture.process(g_block, kBlk);
    capture.cancel();
    CHECK(!capture.recording());
    CHECK_EQ(int(capture.frames()), 0);

    // And a block arriving after a cancel is not recorded, which is what makes the cancel safe
    // to call from a command rather than only at a block boundary.
    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), 0);

    CHECK(capture.arm(uint32_t(kBlk)));
    capture.begin();
    capture.process(g_block, kBlk);
    CHECK(capture.done());
    capture.cancel();
    CHECK(capture.done());                       // survived
    CHECK_EQ(int(capture.frames()), kBlk);
}

TEST(capture_clamps_a_take_longer_than_the_buffer_and_says_so) {
    // 8 bars below about 64 BPM is longer than 30 s. The take is shortened rather than refused --
    // a short loop is usable and a refusal in the middle of a performance is not -- but the
    // screen has to be able to say it happened, so the flag is sticky until the next arm.
    Capture capture;
    CHECK(capture.init(kRate));

    CHECK(capture.arm(capture.capacity() + 1u));
    CHECK(capture.clamped());
    CHECK_EQ(int(capture.wanted()), int(capture.capacity()));

    // Sticky: still true after the take, which is when the screen gets to draw it.
    capture.begin();
    CHECK(capture.clamped());

    // And cleared by the next arm that fits.
    capture.cancel();
    CHECK(capture.arm(uint32_t(kBlk)));
    CHECK(!capture.clamped());

    // A take of EXACTLY the buffer's length is NOT clamped -- one frame either side of the
    // boundary. A `>=` here would tell the user their longest usable take had been shortened
    // when it had not. Found by mutation; nothing else in this file reached it.
    capture.cancel();
    CHECK(capture.arm(capture.capacity()));
    CHECK(!capture.clamped());
    CHECK_EQ(int(capture.wanted()), int(capture.capacity()));
}

TEST(capture_refuses_a_zero_length_take) {
    // 0 frames would be `done` on the first block with nothing in it, and a 0-frame WAV is a
    // file nothing can load. The command carries a bar count, so 0 means a tempo read that has
    // not settled rather than a request anyone made.
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(!capture.arm(0));
    CHECK(!capture.armed());
    CHECK(!capture.done());
}

TEST(capture_is_safe_before_init) {
    // Every machine in dsp/ is safe to drive before its first prepare; this is the same promise.
    // An app whose init failed half way must not fault on the next block.
    Capture capture;
    CHECK(!capture.arm(uint32_t(kBlk)));
    capture.begin();
    capture.cancel();
    fill_block(g_block, kBlk, 0.5f, 0.5f);
    capture.process(g_block, kBlk);
    CHECK(!capture.recording());
    CHECK(!capture.done());
    CHECK_EQ(int(capture.frames()), 0);
}

TEST(capture_ignores_a_null_or_empty_block) {
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(capture.arm(uint32_t(kBlk)));
    capture.begin();
    capture.process(nullptr, kBlk);
    capture.process(g_block, 0);
    capture.process(g_block, -1);
    CHECK_EQ(int(capture.frames()), 0);
    CHECK(capture.recording());
}

TEST(capture_records_a_one_frame_block) {
    // A block of one frame is still audio: the master can hand over a short block at the end of
    // a take, and dropping it would make the take a frame short.
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(capture.arm(3));
    capture.begin();
    fill_ramp(g_block, 1, 100);
    capture.process(g_block, 1);
    CHECK_EQ(int(capture.frames()), 1);
    fill_ramp(g_block, 2, 101);
    capture.process(g_block, 2);
    CHECK_EQ(int(capture.frames()), 3);
    CHECK(capture.done());
    CHECK_EQ(int(capture.data()[0]), int(int16_t(100.0f / 100000.0f * 32767.0f)));
    CHECK_EQ(int(capture.data()[4]), int(int16_t(102.0f / 100000.0f * 32767.0f)));
}

// --- a bounce: one track, mono (2026-11-23) ---------------------------------------------------

TEST(capture_a_mono_take_is_one_sample_a_frame_and_ends_on_its_frame) {
    Capture capture;
    CHECK(capture.init(kRate));
    const uint32_t wanted = uint32_t(kBlk + 9);
    CHECK(capture.arm(wanted, 1));
    CHECK_EQ(capture.channels(), 1);
    capture.begin();

    static float mono[kBlk];
    for (int i = 0; i < kBlk; i++) mono[i] = float(i) * (1.0f / 1000.0f);
    // A stereo block is not this take's width: nothing is written.
    fill_block(g_block, kBlk, 0.5f, 0.5f);
    capture.process(g_block, kBlk);
    CHECK_EQ(int(capture.frames()), 0);

    capture.process_mono(mono, kBlk);
    capture.process_mono(mono, kBlk);
    CHECK(capture.done());
    CHECK_EQ(int(capture.frames()), int(wanted));
    // One int16 a frame, packed: the WAV is mono, half a stereo take's size.
    CHECK_EQ(int(capture.data_bytes()), int(wanted) * 2);
    CHECK_EQ(int(capture.file_bytes()), int(Capture::kHeaderBytes) + int(wanted) * 2);
    for (int i = 0; i < kBlk; i++) {
        CHECK_EQ(int(capture.data()[i]), int(int16_t(float(i) * (1.0f / 1000.0f) * 32767.0f)));
    }
    CHECK_EQ(int(capture.data()[kBlk]), 0);               // the second block's first frame
    CHECK_EQ(int(capture.data()[kBlk + 8]), int(int16_t(8.0f / 1000.0f * 32767.0f)));

    // And the next take is stereo again, as asked.
    capture.release();
    CHECK(capture.arm(uint32_t(kBlk)));
    CHECK_EQ(capture.channels(), 2);
    capture.begin();
    capture.process_mono(mono, kBlk);                     // the wrong width: nothing
    CHECK_EQ(int(capture.frames()), 0);
}

TEST(capture_refuses_a_width_it_cannot_write) {
    Capture capture;
    CHECK(capture.init(kRate));
    CHECK(!capture.arm(uint32_t(kBlk), 0));
    CHECK(!capture.arm(uint32_t(kBlk), 3));
    CHECK(!capture.armed());
}
