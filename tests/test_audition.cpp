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

namespace {

// A file whose frames no cubic passes through: each side its own curve, so an interpolation
// that reads the wrong tap, the wrong side, or weighs a tap wrongly shows.
int16_t curve(uint32_t f, int ch) {
    const double x = double(f);
    return int16_t(ch == 0 ? 9000.0 * std::sin(x * 0.37) + 3000.0 * std::sin(x * 1.9)
                           : 7000.0 * std::cos(x * 0.21) - 2500.0 * std::sin(x * 2.6));
}

// Feeds file frames up to (not including) `upto`, as far as the fifo has room.
void feed_curve_to(uint32_t upto, int channels) {
    StreamFifo &fifo = g_aud.fifo();
    while (fifo.next_frame() < upto) {
        uint32_t room = 0;
        int16_t *span = fifo.write_span(&room);
        if (room == 0) break;
        const uint32_t want = upto - fifo.next_frame();
        const uint32_t n = want < room ? want : room;
        for (uint32_t i = 0; i < n; i++) {
            for (int c = 0; c < channels; c++) {
                span[uint32_t(channels) * i + uint32_t(c)] = curve(fifo.next_frame() + i, c);
            }
        }
        fifo.commit(n);
    }
}

// Catmull-Rom in double, on the taps p-1, p, p+1, p+2, each held inside the file.
double catmull(uint32_t p, uint32_t last, double t, int ch) {
    const double x0 = curve(p > 0 ? p - 1 : 0, ch), x1 = curve(p, ch);
    const double x2 = curve(p < last ? p + 1 : last, ch);
    const double x3 = curve(p + 2 < last ? p + 2 : last, ch);
    const double c1 = 0.5 * (x2 - x0);
    const double c2 = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3;
    const double c3 = 0.5 * (x3 - x0) + 1.5 * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

// Plays a file of `channels` at `rate`, sample by sample against a Catmull-Rom worked out here,
// over a pattern already in the output (the audition adds), with the card delivering exactly what
// each block needs: one frame short holds the block (nothing added, late, the place kept), and
// that frame arriving plays it. Through the file's end, where the taps are held at the last frame.
void play_against_catmull(int channels, uint32_t rate) {
    const uint32_t total = 700;
    const uint32_t last = total - 1;
    const uint32_t step = Audition::step_for(rate, 48000);
    CHECK(g_aud.start(total, channels, step));
    uint32_t pos = 0, frac = 0;
    int wrong = 0, blocks = 0, late_wrong = 0;
    const auto under = [](int k) { return 0.01f * float(k % 13) - 0.06f; };
    const auto fill = [&]() {
        for (int k = 0; k < 128; k++) g_out[k] = under(k);
    };
    while (g_aud.playing() && blocks < 40) {
        // The furthest tap this block reads: x3 of its last sample, inside the file.
        uint32_t p = pos, f = frac, far = 0;
        for (int i = 0; i < 64 && p < total; i++) {
            const uint32_t c = p + 2 < last ? p + 2 : last;
            far = c > far ? c : far;
            f += step & 0xFFFFFFu;
            p += (step >> 24) + (f >> 24);
            f &= 0xFFFFFFu;
        }
        if (far > 0 && g_aud.fifo().next_frame() < far) {
            feed_curve_to(far, channels);               // one frame short
            const uint32_t late = g_aud.late();
            fill();
            g_aud.render_add(g_out, 64);
            bool untouched = true;
            for (int k = 0; k < 128; k++) untouched = untouched && g_out[k] == under(k);
            late_wrong += (g_aud.late() == late + 1 && untouched && g_aud.position() == pos) ? 0 : 1;
        }
        feed_curve_to(far + 1, channels);
        fill();
        g_aud.render_add(g_out, 64);
        for (int i = 0; i < 64 && pos < total; i++) {
            const double t = double(frac) / 16777216.0;
            for (int ch = 0; ch < 2; ch++) {
                const double want = under(2 * i + ch) +
                                    catmull(pos, last, t, channels == 2 ? ch : 0) *
                                        (Audition::kGain / 32768.0);
                wrong += std::fabs(double(g_out[2 * i + ch]) - want) > 2e-6 ? 1 : 0;
            }
            frac += step & 0xFFFFFFu;
            pos += (step >> 24) + (frac >> 24);
            frac &= 0xFFFFFFu;
        }
        blocks++;
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(late_wrong, 0);
    CHECK(!g_aud.playing());
    CHECK(g_aud.take_finished());
    // Finished, more blocks add nothing and do not finish it again.
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(silent());
    CHECK(!g_aud.take_finished());
}

}  // namespace

// Below, at and above one file frame an output frame, stereo and mono.
TEST(audition_reads_every_sample_by_catmull_rom_from_exactly_the_frames_it_needs) {
    play_against_catmull(2, 44100);
    play_against_catmull(1, 44100);
    play_against_catmull(2, 88200);
    play_against_catmull(1, 96000);
    play_against_catmull(2, 32000);
}

TEST(audition_a_new_start_forgets_the_last_one) {
    // A stop not yet rendered, then a new start: the new one plays, it does not fade.
    CHECK(g_aud.start(1000, 1, Audition::kStepOne));
    g_aud.stop();
    CHECK(g_aud.start(1000, 1, Audition::kStepOne));
    feed(1000, 1);
    clear_out();
    g_aud.render_add(g_out, 64);
    CHECK(g_aud.playing());
    CHECK(std::fabs(g_out[2 * 10] - float(value(10, 0)) * kOne) < 1e-7f);
    // Played to its end without taking the flag, then started again: the old end is not news.
    CHECK(g_aud.start(64, 1, Audition::kStepOne));
    feed(64, 1);
    g_aud.render_add(g_out, 64);
    CHECK(!g_aud.playing());
    CHECK(g_aud.start(1000, 1, Audition::kStepOne));
    CHECK(!g_aud.take_finished());
    // A refused start leaves it stopped, even over one that was playing.
    CHECK(!g_aud.start(1000, 5, Audition::kStepOne));
    CHECK(!g_aud.playing());
}

TEST(audition_takes_the_edges_of_its_ranges) {
    CHECK(Audition::step_for(Audition::kMinRate, 48000) != 0u);
    CHECK(Audition::step_for(Audition::kMaxRate, 48000) == 2u * Audition::kStepOne);
    CHECK(g_aud.start(100, 2, 4u * Audition::kStepOne));     // a step of exactly 4
    CHECK(!g_aud.start(100, 2, 4u * Audition::kStepOne + 1u));
}
