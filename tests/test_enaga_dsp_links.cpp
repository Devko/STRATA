// Proves Enaga's DSP library links and runs on the host.
//
// <enaga/dsp.hpp> is a C++ wrapper over block functions implemented in C under
// third_party/enaga/runtime/dsp. Without those objects, anything in dsp/ compiles but fails at
// link time -- a confusing failure to hit while writing a machine. This test fails loudly and
// early instead, and doubles as a smoke test that the library behaves on the host at all.
//
// It also pins down two conventions the machines in dsp/ must follow:
//   - Oscillators take dt (cycles per sample) AND inv_dt (its reciprocal), computed by the
//     caller at control rate, because nothing in the hot loop may divide.
//   - Filters take coefficients made at control rate (SvfCoefficients::make) and then run a
//     whole block through Svf::process. The headers say to prefer that over per-sample step().
#include "check.hpp"

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

using namespace enaga::dsp;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kFrames = int(ENAGA_BLOCK_FRAMES);   // 64: what audio() always gets

}  // namespace

TEST(enaga_dsp_math_is_available) {
    // There is no libm on the device, so the library brings its own.
    CHECK_NEAR(exp2(0.0f), 1.0f, 1e-6);
    CHECK_NEAR(exp2(1.0f), 2.0f, 1e-5);
    CHECK_NEAR(semitones(12.0f), 2.0f, 1e-4);   // an octave doubles the ratio
    CHECK_NEAR(sin_turns(0.0f), 0.0f, 1e-6);
    CHECK_NEAR(sin_turns(0.25f), 1.0f, 1e-5);   // a quarter turn is the peak
    CHECK_NEAR(clamp(5.0f, 0.0f, 1.0f), 1.0f, 1e-9);
    CHECK_NEAR(reciprocal(4.0f), 0.25f, 1e-6);
}

TEST(enaga_oscillator_runs_a_block) {
    float out[kFrames] = {};

    const float frequency = 1000.0f;
    const float dt = frequency / kRate;          // cycles per sample
    const float inv_dt = kRate / frequency;      // computed here, at control rate

    Oscillator osc;
    osc.reset();
    osc.render_add(out, kFrames, Wave::sine, dt, inv_dt, 0.5f);

    bool any_nonzero = false;
    bool in_range = true;
    for (int i = 0; i < kFrames; i++) {
        if (out[i] != 0.0f) any_nonzero = true;
        if (!(out[i] > -1.0f && out[i] < 1.0f)) in_range = false;
    }
    CHECK(any_nonzero);   // the C block function actually ran
    CHECK(in_range);      // and stayed inside full scale at level 0.5
}

TEST(enaga_oscillator_phase_advances_with_dt) {
    // 64 frames of a 750 Hz sine at 48 kHz is exactly one cycle, so the phase returns to 0.
    const float frequency = kRate / float(kFrames);
    const float dt = frequency / kRate;

    Oscillator osc;
    osc.reset();
    osc.advance(kFrames, dt);
    CHECK_NEAR(osc.phase(), 0.0f, 1e-4);
}

TEST(enaga_filter_takes_out_high_frequencies) {
    float in[kFrames], out[kFrames];
    for (int i = 0; i < kFrames; i++) in[i] = (i % 2) ? 1.0f : -1.0f;   // alternating: near Nyquist

    const SvfCoefficients coefficients = SvfCoefficients::make(500.0f, 0.0f, kRate);

    Svf filter;
    filter.reset();
    filter.process(in, out, kFrames, coefficients, Svf::low_pass);

    float peak = 0.0f;
    for (int i = 0; i < kFrames; i++) {
        const float a = out[i] < 0.0f ? -out[i] : out[i];
        if (a > peak) peak = a;
    }
    CHECK(peak < 0.5f);   // a 500 Hz low-pass should gut an alternating signal
}

TEST(enaga_envelope_opens_and_closes) {
    // A percussive AD with no sustain: what KICK, SNARE and HAT all want.
    const EnvelopeShape shape = EnvelopeShape::make(0.001f, 0.020f, 0.0f, 0.020f, kRate);

    Envelope envelope;
    envelope.kill();
    CHECK(!envelope.active());

    envelope.gate_on(true);
    CHECK(envelope.active());

    // Step per sample, as an amplitude envelope does.
    float peak = 0.0f;
    for (int i = 0; i < kFrames * 4; i++) {
        const float v = envelope.step(shape);
        if (v > peak) peak = v;
    }
    CHECK(peak > 0.5f);                 // it opened

    // With sustain at 0 the decay falls away on its own, no gate_off needed.
    for (int i = 0; i < kFrames * 64; i++) envelope.step(shape);
    CHECK(envelope.level() < 0.05f);    // and closed
}

TEST(enaga_envelope_renders_a_block) {
    // render() must give the same values as stepping one at a time: the block function is what
    // the machines will use, so the two paths staying equal is worth pinning down.
    const EnvelopeShape shape = EnvelopeShape::make(0.002f, 0.030f, 0.3f, 0.030f, kRate);

    float stepped[kFrames], rendered[kFrames];

    Envelope a;
    a.kill();
    a.gate_on(true);
    for (int i = 0; i < kFrames; i++) stepped[i] = a.step(shape);

    Envelope b;
    b.kill();
    b.gate_on(true);
    b.render(rendered, kFrames, shape);

    for (int i = 0; i < kFrames; i++) CHECK_NEAR(rendered[i], stepped[i], 1e-6);
}
