// dsp/audition: the sample browser's audition player, fed by hand as the engine feeds it.
#include "audition.hpp"

#include "check.hpp"

#include <cmath>

using namespace strata;

namespace {

Audition g_aud;          // 16 KiB of fifo: file scope, never a local
float g_out[128];        // one block of interleaved stereo

constexpr float kOne = Audition::kGain / 32768.0f;   // one int16 count, as heard

// The value of file frame `f` in channel `ch`: distinct, small, signed.
int16_t value(uint32_t f, int ch) { return int16_t(ch == 0 ? int32_t(f % 1000u) : -int32_t(f % 1000u)); }

// Feed the next `count` frames, or as many as fit.
uint32_t feed(uint32_t count, int channels) {
    StreamFifo &fifo = g_aud.fifo();
    uint32_t written = 0;
    while (written < count) {
        uint32_t room = 0;
        int16_t *span = fifo.write_span(&room);
        if (room == 0) break;
        const uint32_t n = count - written < room ? count - written : room;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t f = fifo.next_frame() + i;
            for (int c = 0; c < channels; c++) span[uint32_t(channels) * i + uint32_t(c)] = value(f, c);
        }
        fifo.commit(n);
        written += n;
    }
    return written;
}

void clear_out() {
    for (float &x : g_out) x = 0.0f;
}

bool silent() {
    for (float x : g_out) {
        if (x != 0.0f) return false;
    }
    return true;
}

}  // namespace

TEST(audition_steps_are_the_file_rate_over_the_output_rate) {
    CHECK_EQ(Audition::step_for(48000, 48000), Audition::kStepOne);
    CHECK_EQ(Audition::step_for(96000, 48000), 2u * Audition::kStepOne);
    CHECK_EQ(Audition::step_for(24000, 48000), Audition::kStepOne / 2u);
    // 44.1 kHz: 0.91875, to within the last bit of Q24.
    const uint32_t s = Audition::step_for(44100, 48000);
    CHECK(s > 15414000u && s < 15414100u);
    // Outside the loader's rates, or no output rate: no step.
    CHECK_EQ(Audition::step_for(7999, 48000), 0u);
    CHECK_EQ(Audition::step_for(192000, 48000), 0u);
    CHECK_EQ(Audition::step_for(48000, 0), 0u);
}

TEST(audition_refuses_what_it_cannot_play) {
    CHECK(!g_aud.start(0, 1, Audition::kStepOne));
    CHECK(!g_aud.start(100, 3, Audition::kStepOne));
    CHECK(!g_aud.start(100, 0, Audition::kStepOne));
    CHECK(!g_aud.start(100, 1, 0));
    CHECK(!g_aud.start(100, 2, 5u * Audition::kStepOne));
    CHECK(!g_aud.playing());
    CHECK(!g_aud.take_finished());
}

TEST(audition_plays_a_mono_file_at_its_rate_to_both_sides) {
    CHECK(g_aud.start(1000, 1, Audition::kStepOne));
    CHECK(g_aud.playing());
    CHECK(g_aud.wants_frames());
    CHECK_EQ(feed(1000, 1), 1000u);
    CHECK(!g_aud.wants_frames());
    clear_out();
    g_aud.render_add(g_out, 64);
    // At the file's own rate every read lands on a frame, which Catmull-Rom returns exactly.
    int wrong = 0;
    for (int i = 0; i < 64; i++) {
        const float want = float(value(uint32_t(i), 0)) * kOne;
        wrong += std::fabs(g_out[2 * i] - want) > 1e-7f ? 1 : 0;
        wrong += g_out[2 * i + 1] != g_out[2 * i] ? 1 : 0;
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(g_aud.position(), 64u);
}

TEST(audition_keeps_a_stereo_files_two_sides) {
    CHECK(g_aud.start(500, 2, Audition::kStepOne));
    feed(500, 2);
    clear_out();
    g_aud.render_add(g_out, 64);
    int wrong = 0;
    for (int i = 0; i < 64; i++) {
        wrong += std::fabs(g_out[2 * i] - float(value(uint32_t(i), 0)) * kOne) > 1e-7f ? 1 : 0;
        wrong += std::fabs(g_out[2 * i + 1] - float(value(uint32_t(i), 1)) * kOne) > 1e-7f ? 1 : 0;
    }
    CHECK_EQ(wrong, 0);
}

TEST(audition_adds_to_what_is_there_and_clamps) {
    CHECK(g_aud.start(200, 1, Audition::kStepOne));
    // Full scale: 32767 at half gain is about 0.5.
    StreamFifo &fifo = g_aud.fifo();
    uint32_t room = 0;
    int16_t *span = fifo.write_span(&room);
    for (uint32_t i = 0; i < 200; i++) span[i] = 32767;
    fifo.commit(200);
    for (float &x : g_out) x = 0.75f;
    g_aud.render_add(g_out, 64);
    int wrong = 0;
    for (int i = 0; i < 128; i++) wrong += g_out[i] != 1.0f ? 1 : 0;   // 0.75 + 0.5, clamped
    CHECK_EQ(wrong, 0);
}

TEST(audition_plays_a_44k_file_in_its_own_time) {
    // 4,410 frames at 44.1 kHz is 0.1 s: 4,800 output frames, 75 blocks.
    CHECK(g_aud.start(4410, 1, Audition::step_for(44100, 48000)));
    int blocks = 0;
    while (g_aud.playing() && blocks < 200) {
        feed(4410u - g_aud.fifo().next_frame(), 1);      // as the engine does, as room allows
        clear_out();
        g_aud.render_add(g_out, 64);
        blocks++;
    }
    CHECK(blocks >= 75 && blocks <= 76);
    CHECK(g_aud.take_finished());
    CHECK(!g_aud.take_finished());     // once
}

TEST(audition_holds_its_place_while_the_card_is_late) {
    CHECK(g_aud.start(10000, 1, Audition::kStepOne));
    feed(100, 1);
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(!silent());
    CHECK_EQ(g_aud.position(), 64u);
    const uint32_t late = g_aud.late();
    // The next block needs taps to frame 64 + 63 + 2 = 129; only 100 are in.
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(silent());
    CHECK_EQ(g_aud.position(), 64u);            // held, not skipped
    CHECK_EQ(g_aud.late(), late + 1u);
    CHECK(g_aud.playing());
    // The card catches up: it goes on from where it stopped.
    feed(200, 1);
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(std::fabs(g_out[0] - float(value(64, 0)) * kOne) < 1e-7f);
    CHECK_EQ(g_aud.position(), 128u);
}

TEST(audition_ends_at_the_files_last_frame) {
    CHECK(g_aud.start(100, 1, Audition::kStepOne));
    feed(100, 1);
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(g_aud.playing());
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(!g_aud.playing());
    CHECK(g_aud.take_finished());
    // 36 frames in the second block, then nothing.
    CHECK(std::fabs(g_out[2 * 35] - float(value(99, 0)) * kOne) < 1e-7f);
    CHECK_EQ(g_out[2 * 36], 0.0f);
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(silent());
}

TEST(audition_stop_fades_out_over_one_block) {
    CHECK(g_aud.start(10000, 1, Audition::kStepOne));
    // A steady level, so the fade is all that moves.
    StreamFifo &fifo = g_aud.fifo();
    uint32_t room = 0;
    int16_t *span = fifo.write_span(&room);
    for (uint32_t i = 0; i < room; i++) span[i] = 10000;
    fifo.commit(room);
    clear_out();
    g_aud.render_add(g_out, 64);
    g_aud.stop();
    CHECK(g_aud.playing());                 // until the fade is done
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(!g_aud.playing());
    CHECK(g_aud.take_finished());
    // Falling all the way: the first sample loud, every next one quieter, the last near zero.
    CHECK(std::fabs(g_out[0] - 10000.0f * kOne) < 1e-6f);
    int rising = 0;
    for (int i = 1; i < 64; i++) rising += g_out[2 * i] >= g_out[2 * (i - 1)] ? 1 : 0;
    CHECK_EQ(rising, 0);
    CHECK(g_out[2 * 63] < 10000.0f * kOne / 32.0f);
    // A stop with nothing playing does nothing.
    g_aud.stop();
    CHECK(!g_aud.playing());
    CHECK(!g_aud.take_finished());
}

TEST(audition_stopped_while_waiting_for_the_card_ends_at_once) {
    CHECK(g_aud.start(10000, 1, Audition::kStepOne));
    clear_out();
    g_aud.render_add(g_out, 64);            // nothing in yet: held
    CHECK(g_aud.playing());
    g_aud.stop();
    g_aud.render_add(g_out, 64);
    CHECK(!g_aud.playing());
    CHECK(g_aud.take_finished());
    CHECK(silent());
}
