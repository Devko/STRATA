// The ASR page: VA MONO and VA POLY holding a note (dsp/va.cpp, "ASR").
//
// What is pinned here:
//   - every ASR byte at 0 renders the same bits as a patch that never had the page, which is
//     what an older kit loads as (tests/test_golden.cpp pins the old output itself);
//   - SUS up holds a note at its level for as long as nothing lets it go, and REL is the tail
//     after the key comes up;
//   - the step's LEN, arriving as Trig::gate_frames, is the note's length on both machines once
//     SUS is up -- and on VA MONO only then, so an existing bass line keeps its decays;
//   - ATK is a real fade-in, and FSU keeps the filter open while the note sustains;
//   - a one-shot (no gate, no release coming) still ends if SUS is raised while it sounds.
#include "check.hpp"

#include <enaga/abi.h>

#include "va.hpp"
#include "vapoly.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kCapture = ENAGA_SAMPLE_RATE * 4;        // 4 s: well past the longest DEC

constexpr int kDec = 8, kEnv = 9, kLvl = 11;
constexpr int kAtk = kAsrFirst + 0, kSus = kAsrFirst + 1, kRel = kAsrFirst + 2,
              kFsu = kAsrFirst + 3;

float g_a[kCapture];
float g_b[kCapture];

void clear(float *x, int n) {
    for (int i = 0; i < n; i++) x[i] = 0.0f;
}

// A short-decay, open-filter patch: with SUS at 0 it is silent well inside half a second.
TrackParams patch() {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, 64);
    p.set(1, 70);         // LP12
    p.set(2, 127);        // wide open
    p.set(3, 0);
    p.set(kDec, 20);      // about 45 ms
    p.set(kEnv, 0);
    p.set(10, 0);         // no glide / no detune
    p.set(kLvl, 127);
    return p;
}

Trig hit(uint8_t note, uint32_t gate_frames = 0, bool slide = false, uint32_t frame = 0) {
    Trig t;
    t.note = note;
    t.velocity = 100;
    t.slide = slide;
    t.gate_frames = gate_frames;
    t.frame = frame;
    t.held = gate_frames == 0;   // no LEN here models a KEYS key: its release() comes
    return t;
}

int blocks_for(float seconds) { return int(seconds * kRate) / kBlk; }

// Renders `blocks` blocks; at block `release_block` (if >= 0) calls release(note) first.
int render(Machine &m, const TrackParams &p, float *out, int blocks, int release_block = -1,
           uint8_t note = 60) {
    for (int b = 0; b < blocks; b++) {
        if (b == release_block) m.release(note);
        m.prepare(p);
        m.render_add(out + b * kBlk, kBlk);
    }
    return blocks * kBlk;
}

float peak(const float *x, int from, int to) {
    float best = 0.0f;
    for (int i = from; i < to; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > best) best = a;
    }
    return best;
}

int frames_at(float seconds) { return int(seconds * kRate); }

}  // namespace

TEST(asr_every_byte_at_zero_is_the_envelope_the_vas_always_had) {
    // The curves' own values at 0 are the old constants exactly, not near them.
    CHECK_EQ(va_attack_seconds(0), 0.0025f);
    CHECK_EQ(va_release_seconds(0), 0.060f);
    CHECK(va_attack_seconds(127) > 4.9f && va_attack_seconds(127) < 5.1f);
    CHECK(va_release_seconds(127) > 7.9f && va_release_seconds(127) < 8.1f);
    CHECK(va_attack_seconds(64) > va_attack_seconds(63));

    // And a 0 page renders what it rendered: the same hit, with and without gates, against
    // itself with the page's bytes left at their zeroed default. (TrackParams{} zeroes them;
    // a garbage byte there would show up as a difference.)
    TrackParams p = patch();
    VaMono a, b;
    a.init(kRate);
    b.init(kRate);
    a.trigger(hit(60, 0));
    b.trigger(hit(60, 2000));          // a LEN, which SUS 0 must ignore on MONO
    const int blocks = blocks_for(0.5f);
    clear(g_a, blocks * kBlk);
    clear(g_b, blocks * kBlk);
    const int n = render(a, p, g_a, blocks);
    render(b, p, g_b, blocks);
    int differ = 0;
    for (int i = 0; i < n; i++) differ += g_a[i] != g_b[i];
    CHECK_EQ(differ, 0);
    CHECK(!a.active());                // a one-shot: over inside half a second
}

TEST(asr_sus_holds_a_held_key_until_it_comes_up_and_rel_is_its_tail) {
    for (int poly = 0; poly < 2; poly++) {
        TrackParams p = patch();
        p.set(kSus, 100);
        p.set(kRel, 0);                 // 60 ms

        VaMono mono;
        VaPoly chord;
        Machine &m = poly ? static_cast<Machine &>(chord) : static_cast<Machine &>(mono);
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 0));          // a KEYS pad: no gate, a release when it comes up
        const int release_block = blocks_for(2.0f);
        const int blocks = blocks_for(3.0f);
        clear(g_a, blocks * kBlk);
        render(m, p, g_a, blocks, release_block, 60);

        // Two seconds in, forty decays later, still at the sustain level.
        const float early = peak(g_a, frames_at(0.3f), frames_at(0.4f));
        const float late = peak(g_a, frames_at(1.85f), frames_at(1.95f));
        CHECK(late > 0.1f);
        CHECK(late > early * 0.9f && late < early * 1.1f);
        // Let go: gone a fifth of a second later (REL 60 ms is to 1%).
        CHECK(peak(g_a, release_block * kBlk + frames_at(0.2f), blocks * kBlk) < late * 0.01f);
        CHECK(!m.active());
    }
}

TEST(asr_a_long_rel_rings_on_after_the_key) {
    TrackParams p = patch();
    p.set(kSus, 127);
    p.set(kRel, 90);                    // about 1.9 s
    CHECK(va_release_seconds(90) > 1.5f && va_release_seconds(90) < 2.5f);
    VaPoly m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, 0));
    const int release_block = blocks_for(0.5f);
    const int blocks = blocks_for(1.5f);
    clear(g_a, blocks * kBlk);
    render(m, p, g_a, blocks, release_block, 60);
    const float held = peak(g_a, frames_at(0.4f), frames_at(0.5f));
    // Half a second after the key, a long release is still clearly there and falling.
    const float after = peak(g_a, frames_at(0.95f), frames_at(1.05f));
    CHECK(after > held * 0.1f);
    CHECK(after < held * 0.9f);
}

TEST(asr_len_is_the_note_length_once_sus_is_up_on_both_machines) {
    for (int poly = 0; poly < 2; poly++) {
        TrackParams p = patch();
        p.set(kSus, 100);
        VaMono mono;
        VaPoly chord;
        Machine &m = poly ? static_cast<Machine &>(chord) : static_cast<Machine &>(mono);
        m.init(kRate);
        m.prepare(p);
        const uint32_t gate = uint32_t(frames_at(1.0f));     // a step's LEN
        m.trigger(hit(60, gate));
        const int blocks = blocks_for(2.0f);
        clear(g_a, blocks * kBlk);
        render(m, p, g_a, blocks);
        const float held = peak(g_a, frames_at(0.85f), frames_at(0.95f));
        CHECK(held > 0.1f);                                   // still held near the end of LEN
        CHECK(peak(g_a, frames_at(1.2f), blocks * kBlk) < held * 0.01f);   // and let go at it
        CHECK(!m.active());
    }
}

TEST(asr_mono_ignores_len_at_sus_zero_so_old_bass_lines_keep_their_decay) {
    // DEC long, SUS 0: the 303 bass. A LEN of one short step must not cut the decay -- it never
    // did, and every existing pattern has LEN 1 step.
    TrackParams p = patch();
    p.set(kDec, 127);                   // 2.4 s
    VaMono m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, uint32_t(frames_at(0.1f))));
    const int blocks = blocks_for(0.6f);
    clear(g_a, blocks * kBlk);
    render(m, p, g_a, blocks);
    CHECK(peak(g_a, frames_at(0.4f), frames_at(0.5f)) > 0.05f);
}

TEST(asr_a_slid_note_carries_the_held_one_on_rather_than_letting_it_go) {
    // Step 1 with a LEN a hair under the step, step 2 slid into from it: the first note's gate
    // must not let it go just before the slide arrives. If it did, the slid note would find a
    // voice in its release and start again -- back up to the peak, twice the sustain level here.
    TrackParams p = patch();
    p.set(kSus, 64);                              // about half the peak
    VaMono m;
    m.init(kRate);
    m.prepare(p);
    const uint32_t step = uint32_t(187 * kBlk + 40);
    m.trigger(hit(60, step - 8));                 // its gate ends 8 frames before the slide
    const int join_block = int(step) / kBlk;      // the slid trig lands inside this block
    const int blocks = blocks_for(0.8f);
    clear(g_a, blocks * kBlk);
    for (int b = 0; b < blocks; b++) {
        if (b == join_block) m.trigger(hit(67, step, true, step - uint32_t(b * kBlk)));
        m.prepare(p);
        m.render_add(g_a + b * kBlk, kBlk);
    }
    const float before = peak(g_a, frames_at(0.15f), frames_at(0.24f));
    const float across = peak(g_a, int(step), int(step) + frames_at(0.06f));
    CHECK(before > 0.05f);
    CHECK(across > before * 0.8f);
    CHECK(across < before * 1.3f);
    CHECK(m.pitch() > 66.0f);                     // and it did bend into the new pitch
}

TEST(asr_atk_fades_a_note_in) {
    TrackParams p = patch();
    p.set(kSus, 127);
    p.set(kAtk, 80);                    // about 0.3 s
    CHECK(va_attack_seconds(80) > 0.2f && va_attack_seconds(80) < 0.5f);
    VaPoly m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, 0));
    const int blocks = blocks_for(1.0f);
    clear(g_a, blocks * kBlk);
    render(m, p, g_a, blocks);
    const float start = peak(g_a, 0, frames_at(0.02f));
    const float full = peak(g_a, frames_at(0.8f), frames_at(0.9f));
    CHECK(full > 0.1f);
    CHECK(start < full * 0.2f);
}

TEST(asr_fsu_keeps_the_filter_open_while_the_note_sustains) {
    // CUT closed and ENV full: the sweep opens the filter, and at FSU 0 it closes again, so a
    // held note dulls; FSU up holds it bright. Bright here is loud, through a low-pass.
    TrackParams p = patch();
    p.set(2, 10);                       // CUT nearly closed
    p.set(kEnv, 127);
    p.set(kSus, 127);
    float level[2];
    for (int fsu = 0; fsu < 2; fsu++) {
        p.set(kFsu, fsu ? 127 : 0);
        VaPoly m;
        m.init(kRate);
        m.prepare(p);
        m.trigger(hit(60, 0));
        const int blocks = blocks_for(1.0f);
        clear(g_b, blocks * kBlk);
        render(m, p, g_b, blocks);
        level[fsu] = peak(g_b, frames_at(0.8f), frames_at(0.9f));
    }
    CHECK(level[1] > level[0] * 2.0f);
}

TEST(asr_poly_holds_a_long_len_past_the_old_ten_second_cap) {
    // A pad's LEN of 64 steps is 32 s at 120 BPM; the gate used to be capped at ten seconds.
    // Hold one for twelve, then let STOP-once end it: it must still be sounding at eleven.
    TrackParams p = patch();
    p.set(kSus, 127);
    VaPoly m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, uint32_t(frames_at(32.0f))));
    float block[kBlk];
    const int blocks = blocks_for(11.0f);
    for (int b = 0; b < blocks; b++) {
        clear(block, kBlk);
        m.prepare(p);
        m.render_add(block, kBlk);
    }
    CHECK(peak(block, 0, kBlk) > 0.05f);
    CHECK_EQ(m.sounding_voices(), 1);
}

TEST(asr_a_one_shot_still_ends_when_sus_is_raised_under_it) {
    // A pad hit at SUS 0: no gate and no release will ever come. Turning SUS up during its
    // decay (knob, scene morph, macro) must not leave it sustaining for ever.
    for (int poly = 0; poly < 2; poly++) {
        TrackParams p = patch();
        p.set(kDec, 127);                   // 2.4 s, so it is still sounding when SUS moves
        VaMono mono;
        VaPoly chord;
        Machine &m = poly ? static_cast<Machine &>(chord) : static_cast<Machine &>(mono);
        m.init(kRate);
        m.prepare(p);
        Trig t = hit(60, 0);
        t.held = false;                     // a pad hit, not a key
        m.trigger(t);
        const int raise_block = blocks_for(0.05f);
        const int blocks = blocks_for(3.0f);
        clear(g_a, blocks * kBlk);
        TrackParams up = p;
        up.set(kSus, 100);
        for (int b = 0; b < blocks; b++) {
            m.prepare(b < raise_block ? p : up);
            m.render_add(g_a + b * kBlk, kBlk);
        }
        CHECK(peak(g_a, 0, frames_at(0.04f)) > 0.1f);
        CHECK(peak(g_a, frames_at(1.0f), blocks * kBlk) < 1e-3f);
        CHECK(!m.active());
    }
}

TEST(asr_at_sus_zero_a_slide_still_bends_a_released_mono_note) {
    // The old behaviour, kept: a key let go, and a sequenced slide arriving inside its 60 ms
    // tail bends the fading note rather than starting a new one at the peak.
    TrackParams p = patch();
    p.set(kDec, 127);
    VaMono m;
    m.init(kRate);
    m.prepare(p);
    m.trigger(hit(60, 0));
    const int blocks = blocks_for(0.5f);
    clear(g_a, blocks * kBlk);
    const int release_block = blocks_for(0.2f);
    const int slide_block = release_block + 2;
    for (int b = 0; b < blocks; b++) {
        if (b == release_block) m.release(60);
        if (b == slide_block) m.trigger(hit(67, uint32_t(frames_at(1.0f)), true));
        m.prepare(p);
        m.render_add(g_a + b * kBlk, kBlk);
    }
    const float before = peak(g_a, frames_at(0.15f), release_block * kBlk);
    const float after = peak(g_a, slide_block * kBlk, slide_block * kBlk + frames_at(0.05f));
    CHECK(before > 0.05f);
    CHECK(after < before * 0.9f);          // no retrigger back up to the peak
}
