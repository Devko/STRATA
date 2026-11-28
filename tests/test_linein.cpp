// The line input as a mixer strip (core/project.hpp, InputField; Engine::run_input): off it
// costs nothing and changes nothing, on it plays audio()'s input through its own chain into the
// mix with a fader, pan, the sends and the duck, and it fades in and out rather than clicking.
#include "check.hpp"

#include <enaga/abi.h>

#include "apprig.hpp"
#include "commands.hpp"
#include "engine.hpp"

using namespace strata;
using namespace strata::test;

namespace {

constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);

float g_in[2 * kBlk];
float g_out[2 * kBlk];
float g_ref[2 * kBlk];

float absolute(float v) { return v < 0.0f ? -v : v; }

// A steady 0.2 square at 375 Hz (128 frames a period), left and right alike unless `right` is
// false, where the right side is silent.
void fill_input(int block, bool right = true) {
    for (int i = 0; i < kBlk; i++) {
        const int frame = block * kBlk + i;
        const float v = (frame / 64) % 2 == 0 ? 0.2f : -0.2f;
        g_in[2 * i] = v;
        g_in[2 * i + 1] = right ? v : 0.0f;
    }
}

uint8_t input_byte(InputField field) { return engine().project().settings.input[int(field)]; }

// One field to an exact byte, as the screen's encoder would get it there: a delta.
void set_input(InputField field, uint8_t target) {
    post_delta(Cmd::set_input, 0, 0, uint8_t(field), int(target) - int(input_byte(field)));
    pump();
}

void input_on(bool on) {
    if ((input_byte(InputField::on) != 0) != on) post(Cmd::set_input, 0, 0, uint8_t(InputField::on), 0, 1);
    pump();
}

// Blocks of the engine with the input fed, the last one left in g_out. Returns the loudest
// sample of all of them.
float run(int blocks, bool right = true) {
    float loudest = 0.0f;
    for (int b = 0; b < blocks; b++) {
        fill_input(b, right);
        engine().process(g_in, g_out, uint32_t(kBlk));
        const float p = peak_of(g_out, 2 * kBlk);
        loudest = p > loudest ? p : loudest;
    }
    return loudest;
}

// A blank project, quiet, with the master's glue and limiter out of the way so the input's level
// can be read straight off the output.
void quiet_rig() {
    fresh();
    set_master_byte(uint8_t(MasterField::compressor), 0);
    set_master_byte(uint8_t(MasterField::limit), 127);
    CHECK(settle());
}

}  // namespace

TEST(linein_off_by_default_the_input_is_not_heard_and_the_mix_is_untouched) {
    quiet_rig();
    CHECK_EQ(int(input_byte(InputField::on)), 0);
    // The same blocks with and without an input buffer come out bit for bit the same.
    for (int b = 0; b < 20; b++) {
        fill_input(b);
        engine().process(g_in, g_out, uint32_t(kBlk));
        engine().process(nullptr, g_ref, uint32_t(kBlk));
        for (int i = 0; i < 2 * kBlk; i++) CHECK_EQ(g_out[i], 0.0f);
        for (int i = 0; i < 2 * kBlk; i++) CHECK_EQ(g_ref[i], 0.0f);
    }
}

TEST(linein_on_the_input_plays_through_at_unity) {
    quiet_rig();
    input_on(true);
    run(20);
    // Centred at 0 dB: the constant-power pan puts cos(45 deg) of it on each side.
    const float want = 0.2f * 0.70710678f;
    for (int i = 0; i < kBlk; i++) {
        CHECK_NEAR(absolute(g_out[2 * i]), want, 0.002f);
        CHECK_NEAR(absolute(g_out[2 * i + 1]), want, 0.002f);
    }
    // And in time: the output follows the input's sign in the same block.
    for (int i = 0; i < kBlk; i++) CHECK((g_out[2 * i] > 0.0f) == (g_in[2 * i] > 0.0f));
}

TEST(linein_turning_it_on_fades_in_and_off_fades_out_in_one_block) {
    quiet_rig();
    input_on(true);
    run(5);
    // The block the command lands in ramps down from where it was to silence: no step.
    post(Cmd::set_input, 0, 0, uint8_t(InputField::on), 0, 1);
    engine().process(g_in, g_out, uint32_t(kBlk));       // lands: off, ramping down
    CHECK(absolute(g_out[0]) > 0.1f);
    CHECK(absolute(g_out[2 * (kBlk - 1)]) < 0.01f);
    engine().process(g_in, g_out, uint32_t(kBlk));
    for (int i = 0; i < 2 * kBlk; i++) CHECK_EQ(g_out[i], 0.0f);
    post(Cmd::set_input, 0, 0, uint8_t(InputField::on), 0, 1);
    engine().process(g_in, g_out, uint32_t(kBlk));       // lands: on, ramping up from 0
    CHECK(absolute(g_out[0]) < 0.01f);
    CHECK(absolute(g_out[2 * (kBlk - 1)]) > 0.1f);
}

TEST(linein_the_fader_and_pan_place_it) {
    quiet_rig();
    input_on(true);
    set_input(InputField::level, 0);
    CHECK_EQ(run(10), 0.0f);
    set_input(InputField::level, kInputLevelUnity);
    set_input(InputField::pan, 0);                       // hard left
    run(10);
    for (int i = 0; i < kBlk; i++) {
        CHECK_NEAR(absolute(g_out[2 * i]), 0.2f, 0.002f);
        CHECK_EQ(g_out[2 * i + 1], 0.0f);
    }
    // A stereo source keeps its sides: a left-only input centred stays on the left.
    set_input(InputField::pan, 64);
    run(10, false);
    for (int i = 0; i < kBlk; i++) CHECK_EQ(g_out[2 * i + 1], 0.0f);
    CHECK(peak_of(g_out, 2 * kBlk) > 0.1f);
}

TEST(linein_the_delay_send_echoes_it_after_it_stops) {
    quiet_rig();
    input_on(true);
    set_input(InputField::level, kInputLevelUnity);
    set_input(InputField::delay, 127);
    run(40);
    // The input goes quiet and is turned off: the delay keeps ringing what it was sent.
    for (int i = 0; i < 2 * kBlk; i++) g_in[i] = 0.0f;
    input_on(false);
    float loudest = 0.0f;
    for (int b = 0; b < 600; b++) {
        engine().process(g_in, g_out, uint32_t(kBlk));
        const float p = peak_of(g_out, 2 * kBlk);
        loudest = p > loudest ? p : loudest;
    }
    CHECK(loudest > 0.01f);
}

TEST(linein_the_tone_knob_takes_the_top_off) {
    quiet_rig();
    input_on(true);
    const float flat = run(40);
    set_input(InputField::tone, 0);                     // the darkest low-pass
    run(40);
    const float dark = peak_of(g_out, 2 * kBlk);
    CHECK(dark < flat * 0.9f);
}

TEST(linein_commands_clamp_and_pushes_walk_the_choices) {
    quiet_rig();
    post_delta(Cmd::set_input, 0, 0, uint8_t(InputField::duck), 120);
    pump();
    CHECK_EQ(int(input_byte(InputField::duck)), int(kDuckDepthMax));
    post_delta(Cmd::set_input, 0, 0, uint8_t(InputField::level), 100);
    pump();
    CHECK_EQ(int(input_byte(InputField::level)), 127);
    post_delta(Cmd::set_input, 0, 0, uint8_t(InputField::source), 20);
    pump();
    CHECK_EQ(int(input_byte(InputField::source)), int(kInputSources) - 1);
    // A push on SRC goes round; on GAIN too, from NORMAL to HIGH to LOW.
    post(Cmd::set_input, 0, 0, uint8_t(InputField::source), 0, 1);
    pump();
    CHECK_EQ(int(input_byte(InputField::source)), 0);
    post(Cmd::set_input, 0, 0, uint8_t(InputField::gain), 0, 1);
    pump();
    CHECK_EQ(int(input_byte(InputField::gain)), 2);
    post(Cmd::set_input, 0, 0, uint8_t(InputField::gain), 0, 1);
    pump();
    CHECK_EQ(int(input_byte(InputField::gain)), 0);
    // A push on a knob is refused and moves nothing; so is a field this build does not have.
    const uint8_t pan = input_byte(InputField::pan);
    post(Cmd::set_input, 0, 0, uint8_t(InputField::pan), 0, 1);
    post(Cmd::set_input, 0, 0, uint8_t(kInputFields), 5);
    pump();
    CHECK_EQ(int(input_byte(InputField::pan)), int(pan));
}
