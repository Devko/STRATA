// VA POLY: the chord voice in dsp/vapoly.cpp.
//
// The voice itself is VA MONO's (dsp/va.hpp's `VaVoice`, shared rather than duplicated:
// dsp/vapoly.hpp's header says why), and tests/test_va.cpp already pins its pitch, its filter
// modes, its accent and its envelopes note by note. So this file tests what is actually new
// here -- **voice allocation**, and that the parameters and the trig reach six voices instead
// of one -- rather than measuring the same filter twice.
//
// The case most of this file is about: **a chord arrives as several `trigger` calls on the same
// frame**, not as one call carrying several notes, because core/chord turns a step's chord byte
// into notes and the engine triggers each. An allocator that counts a voice as free until its
// trig's frame arrives gives every note of a chord the same voice, and the chord comes out as
// one note. Several tests here are that bug's alarm.
//
// Like tests/test_va.cpp these test measurable properties rather than sample values, and for
// the same reason: a property survives a later tidy-up, a sample value only pins the compiler.
#include "check.hpp"

#include <cstring>

#include <enaga/abi.h>

#include "vapoly.hpp"

using namespace strata;

namespace {

constexpr float kRate = float(ENAGA_SAMPLE_RATE);
constexpr int kBlk = int(ENAGA_BLOCK_FRAMES);
constexpr int kCapture = ENAGA_SAMPLE_RATE * 2;        // 2 s: longer than the longest DEC tail

// The eight parameters this machine owns. dsp/vapoly.cpp documents each mapping; seven of them
// are VA MONO's, and slot 10 is DET where VA MONO has GLD.
constexpr int kTun = 0, kMod = 1, kCut = 2, kRes = 3;
constexpr int kDec = 8, kEnv = 9, kDet = 10, kLvl = 11;

// MOD values inside each of the four zones (the zones are deliberately unequal: see va.cpp).
constexpr uint8_t kLp24 = 10, kLp12 = 70, kBp12 = 100, kHp12 = 120;

// Static, because two seconds of audio does not belong on a stack frame.
float g_a[kCapture];
float g_b[kCapture];

void clear(float *x, int n) {
    for (int i = 0; i < n; i++) x[i] = 0.0f;
}

void fill(float *x, int n, float value) {
    for (int i = 0; i < n; i++) x[i] = value;
}

TrackParams uniform(uint8_t value) {
    TrackParams p{};
    for (int i = 0; i < kParams; i++) p.set(i, value);
    return p;
}

// A usable starting patch: a long note, a wide open static low-pass, no detune, full level.
// Each test moves only the knobs it is about.
TrackParams patch() {
    TrackParams p = uniform(64);
    p.set(kTun, 64);      // no transposition
    p.set(kMod, kLp12);
    p.set(kCut, 127);     // wide open
    p.set(kRes, 0);
    p.set(kDec, 127);     // 2.4 s: it sustains through any window below
    p.set(kEnv, 0);       // a static filter, so the waveform does not move while measured
    p.set(kDet, 0);       // voices exactly in tune with each other unless a test says otherwise
    p.set(kLvl, 127);
    return p;
}

Trig hit(uint8_t note = 60, uint8_t velocity = 100, bool accent = false, uint32_t frame = 0,
         uint32_t gate_frames = 0) {
    Trig t;
    t.note = note;
    t.velocity = velocity;
    t.accent = accent;
    t.slide = false;      // MONO only (dsp/track.hpp); this machine ignores it
    t.frame = frame;
    t.gate_frames = gate_frames;   // 0 is "no gate": what every trig carried before the field
    return t;
}

// Render `blocks` blocks of 64 frames into out, re-reading the parameters every block exactly as
// audio() does. The buffer is the caller's to clear.
int render(Machine &m, const TrackParams &p, float *out, int blocks) {
    for (int b = 0; b < blocks; b++) {
        m.prepare(p);
        m.render_add(out + b * kBlk, kBlk);
    }
    return blocks * kBlk;
}

// `render`, but the transport stops part way: `release_at` and `kill_at` are block indices (-1
// for never) at whose start release_all() or kill_all() is called. A gate still counting when
// the sequencer stops is a state the engine really reaches, and the two calls mean different
// things -- release lets the tail ring, kill takes the voice away -- so both are rendered here.
int render_with_stop(VaPoly &va, const TrackParams &p, float *out, int blocks, int release_at,
                     int kill_at) {
    for (int b = 0; b < blocks; b++) {
        if (b == release_at) va.release_all();
        if (b == kill_at) va.kill_all();
        va.prepare(p);
        va.render_add(out + b * kBlk, kBlk);
    }
    return blocks * kBlk;
}

int blocks_for(float seconds) {
    return int(seconds * kRate) / kBlk;
}

float peak(const float *x, int n) {
    float best = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > best) best = a;
    }
    return best;
}

float energy(const float *x, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += x[i] * x[i];
    return sum;
}

// The first frame on which two renders disagree, or -1 if they never do.
//
// This is how the gate tests measure WHERE a gate acts, and it is exact rather than a
// threshold: a gate is a frame count, and nothing else in this machine changes a sample's
// value, so a gated note and an ungated one are bit-identical until the frame the gate closes
// on and different from it. A test that measured "it got quieter around there" would pass a
// gate that fired a control step early -- which is the bug worth catching, because the control
// step is 32 frames and the shortest gate the table can ask for is far smaller.
int first_difference(const float *x, const float *y, int n) {
    for (int i = 0; i < n; i++) {
        if (x[i] != y[i]) return i;
    }
    return -1;
}

// NaN fails x == x and an infinity fails the bounds, so this needs no <cmath>.
bool finite_within(const float *x, int n, float bound) {
    for (int i = 0; i < n; i++) {
        if (!(x[i] == x[i])) return false;
        if (!(x[i] > -bound && x[i] < bound)) return false;
    }
    return true;
}

// A proxy for brightness: scale-free, so it compares two signals of different levels directly.
int sign_changes(const float *x, int n) {
    int count = 0;
    for (int i = 1; i < n; i++) {
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) count++;
    }
    return count;
}

// How nearly the signal repeats itself after `lag` samples: 1 for an exact repeat, 0 for none.
// tests/test_va.cpp explains why this rather than a zero-crossing count is how to pin a pitch
// made of a saw, a synced pulse and a sub triangle at once. No sqrt on the host either.
float periodicity(const float *x, int n, int lag) {
    if (lag <= 0 || lag >= n) return 0.0f;
    double num = 0.0, ea = 0.0, eb = 0.0;
    for (int i = 0; i + lag < n; i++) {
        num += double(x[i]) * double(x[i + lag]);
        ea += double(x[i]) * double(x[i]);
        eb += double(x[i + lag]) * double(x[i + lag]);
    }
    const double product = ea * eb;
    if (product <= 1e-24) return 0.0f;
    const float root = enaga::dsp::exp2(0.5f * enaga::dsp::log2(float(product)));
    return float(num) * enaga::dsp::reciprocal(root);
}

// The composite period of one voice at a given note, in samples: the SUB's, an octave below the
// note, which is where the waveform as a whole repeats (tests/test_va.cpp, `period_of`).
int period_of(int note) {
    const float hz = 440.0f * enaga::dsp::semitones(float(note) - 69.0f);
    return int(2.0f * kRate * enaga::dsp::reciprocal(hz) + 0.5f);
}

// The first frame that is not exactly zero, or -1. A trig's frame is a sample-accurate promise
// (micro-timing and ratchets are built on it), so the tests pin where the sound starts rather
// than settling for "nothing before it": a note that starts a control step late passes that.
// The amplitude envelope's first step is 0, so a note triggered on frame f has its first
// non-zero sample on f + 1 -- hence `started_on` below allows exactly that one frame.
int first_sound(const float *x, int n) {
    for (int i = 0; i < n; i++) {
        if (x[i] != 0.0f) return i;
    }
    return -1;
}

bool started_on(const float *x, int n, int frame) {
    const int first = first_sound(x, n);
    return first == frame || first == frame + 1;
}

// Is `note` held by any voice? The allocator is free to pick any slot, so the tests ask this
// rather than naming a slot, except where the slot itself is the point.
bool holds(const VaPoly &va, int note) {
    for (int i = 0; i < VaPoly::kVoices; i++) {
        if (va.voice_note(i) == note) return true;
    }
    return false;
}

// Six voices of a machine calibrated per voice (dsp/vapoly.cpp, LVL): a chord is louder than
// one note, by design, so the finite bound here is six voices' worth and not one's.
constexpr float kChordBound = 8.0f;

}  // namespace

// --- silence, sounding, dying ---------------------------------------------------------------

TEST(vapoly_is_silent_and_inactive_before_any_trigger) {
    VaPoly va;
    va.init(kRate);
    CHECK(!va.active());
    CHECK_EQ(va.sounding_voices(), 0);
    CHECK_EQ(va.busy_voices(), 0);
    CHECK_EQ(va.voice_note(0), -1);

    const TrackParams p = patch();
    clear(g_a, 8 * kBlk);
    render(va, p, g_a, 8);
    CHECK_EQ(peak(g_a, 8 * kBlk), 0.0f);
    CHECK(!va.active());
}

TEST(vapoly_trigger_sounds_and_sets_active) {
    VaPoly va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    va.trigger(hit());
    CHECK(va.active());            // true before the first render, or the engine would skip it
    CHECK_EQ(va.busy_voices(), 1);
    CHECK_EQ(va.voice_note(0), 60);   // slot 0 first: the one DET leaves in tune

    clear(g_a, 16 * kBlk);
    render(va, p, g_a, 16);
    CHECK(peak(g_a, 16 * kBlk) > 0.05f);
    CHECK(va.active());
    CHECK_EQ(va.sounding_voices(), 1);
    CHECK(finite_within(g_a, 16 * kBlk, 4.0f));
}

TEST(vapoly_trigger_frame_is_sample_accurate) {
    VaPoly va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    va.trigger(hit(60, 100, false, 40));

    clear(g_a, 4 * kBlk);
    render(va, p, g_a, 4);
    CHECK_EQ(peak(g_a, 40), 0.0f);                 // nothing before the trig's frame
    CHECK(peak(g_a + 40, 4 * kBlk - 40) > 0.01f);  // and something after it
    CHECK(started_on(g_a, 4 * kBlk, 40));          // on THAT frame, not the next control step
}

// --- a chord on one frame: the case this machine exists for ---------------------------------

TEST(vapoly_a_chord_on_one_frame_sounds_every_note) {
    // Three notes, all on frame 0, as core/chord's three notes for one step arrive. If the
    // allocator counted a voice as free until its trig started, all three would take the same
    // voice and this would be one note: so the energy against a single note, and the voice
    // count, are both the alarm for that bug.
    const TrackParams p = patch();
    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;

    VaPoly one, three;
    one.init(kRate);
    three.init(kRate);
    one.prepare(p);
    three.prepare(p);

    one.trigger(hit(60, 100, false, 0));
    three.trigger(hit(60, 100, false, 0));
    three.trigger(hit(64, 100, false, 0));
    three.trigger(hit(67, 100, false, 0));

    CHECK_EQ(one.busy_voices(), 1);
    CHECK_EQ(three.busy_voices(), 3);       // three voices, before a single frame is rendered
    CHECK(holds(three, 60));
    CHECK(holds(three, 64));
    CHECK(holds(three, 67));

    clear(g_a, n);
    clear(g_b, n);
    render(one, p, g_a, blocks);
    render(three, p, g_b, blocks);

    CHECK_EQ(one.sounding_voices(), 1);
    CHECK_EQ(three.sounding_voices(), 3);
    CHECK(energy(g_a, n) > 1.0f);                       // the single note has signal in it
    CHECK(energy(g_b, n) > 2.2f * energy(g_a, n));      // and the chord is about three of them
    CHECK(energy(g_b, n) < 4.5f * energy(g_a, n));
    CHECK(finite_within(g_b, n, kChordBound));
}

TEST(vapoly_two_notes_on_one_frame_are_both_audible) {
    // The same thing heard rather than counted. A fifth: note 60 alone repeats at its own
    // period and not at note 67's, and vice versa (tests/test_va.cpp pins both). Two notes at
    // once repeat cleanly at NEITHER, because each one dilutes the other's period -- which only
    // happens if both are actually sounding.
    const TrackParams p = patch();
    const int blocks = blocks_for(0.4f);
    const int from = int(0.05f * kRate);
    const int window = int(0.25f * kRate);
    const int lag60 = period_of(60), lag67 = period_of(67);

    VaPoly single, both;
    single.init(kRate);
    both.init(kRate);
    single.prepare(p);
    both.prepare(p);
    single.trigger(hit(60, 100, false, 0));
    both.trigger(hit(60, 100, false, 0));
    both.trigger(hit(67, 100, false, 0));

    clear(g_a, blocks * kBlk);
    clear(g_b, blocks * kBlk);
    render(single, p, g_a, blocks);
    render(both, p, g_b, blocks);

    CHECK(periodicity(g_a + from, window, lag60) > 0.85f);   // one note: its own period
    CHECK(periodicity(g_a + from, window, lag67) < 0.7f);    // and not the other's
    CHECK(periodicity(g_b + from, window, lag60) < 0.85f);   // two notes: neither is clean
    CHECK(periodicity(g_b + from, window, lag67) < 0.85f);
    CHECK(energy(g_b, blocks * kBlk) > 1.5f * energy(g_a, blocks * kBlk));
}

TEST(vapoly_a_chord_on_a_late_frame_starts_on_that_frame) {
    // Micro-timing and ratchets put a step's notes on an exact frame, and every note of the
    // chord has to land on the same one.
    const TrackParams p = patch();
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 100, false, 40));
    va.trigger(hit(64, 100, false, 40));
    va.trigger(hit(67, 100, false, 40));
    CHECK_EQ(va.busy_voices(), 3);

    const int blocks = blocks_for(0.2f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK_EQ(peak(g_a, 40), 0.0f);
    CHECK(peak(g_a + 40, blocks * kBlk - 40) > 0.05f);
    CHECK(started_on(g_a, blocks * kBlk, 40));       // all three on frame 40, not later
    CHECK_EQ(va.sounding_voices(), 3);
}

TEST(vapoly_six_notes_sound_together) {
    const TrackParams p = patch();
    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;
    const uint8_t notes[6] = {48, 55, 60, 64, 67, 72};

    VaPoly one, six;
    one.init(kRate);
    six.init(kRate);
    one.prepare(p);
    six.prepare(p);
    one.trigger(hit(60, 100, false, 0));
    for (int i = 0; i < 6; i++) six.trigger(hit(notes[i], 100, false, 0));

    CHECK_EQ(six.busy_voices(), 6);
    for (int i = 0; i < 6; i++) CHECK(holds(six, notes[i]));

    clear(g_a, n);
    clear(g_b, n);
    render(one, p, g_a, blocks);
    render(six, p, g_b, blocks);

    CHECK_EQ(six.sounding_voices(), 6);
    CHECK(energy(g_b, n) > 4.0f * energy(g_a, n));     // six voices, not one and not three
    CHECK(finite_within(g_b, n, kChordBound));
}

// --- stealing -------------------------------------------------------------------------------

TEST(vapoly_a_seventh_note_steals_instead_of_being_dropped) {
    // Six voices are all there are, so the seventh note has to take one. Being dropped is the
    // other possible answer and the wrong one: a chord change would lose its top note.
    const TrackParams p = patch();          // DEC 127: nothing has decayed away
    const uint8_t notes[6] = {48, 55, 60, 64, 67, 72};
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    for (int i = 0; i < 6; i++) va.trigger(hit(notes[i], 100, false, 0));

    clear(g_a, blocks_for(0.1f) * kBlk);
    render(va, p, g_a, blocks_for(0.1f));
    CHECK_EQ(va.sounding_voices(), 6);

    va.trigger(hit(79, 100, false, 0));
    CHECK(holds(va, 79));                   // the new note is held ...
    CHECK_EQ(va.busy_voices(), 6);          // ... and still only six voices exist

    int surviving = 0;
    for (int i = 0; i < 6; i++) surviving += holds(va, notes[i]) ? 1 : 0;
    CHECK_EQ(surviving, 5);                 // exactly one of the six gave up its voice

    const int blocks = blocks_for(0.3f);
    clear(g_b, blocks * kBlk);
    render(va, p, g_b, blocks);
    CHECK_EQ(va.sounding_voices(), 6);
    CHECK(finite_within(g_b, blocks * kBlk, kChordBound));
}

TEST(vapoly_steals_the_quietest_voice) {
    // The quietest voice is the one furthest through its decay, which in a chord progression is
    // the oldest note. Here voice 0's note is 150 ms older than the other five, so it is the
    // one the seventh note takes.
    TrackParams p = patch();
    p.set(kDec, 80);        // about 0.35 s: long enough to hold, short enough to decay visibly
    VaPoly va;
    va.init(kRate);
    va.prepare(p);

    va.trigger(hit(48, 100, false, 0));
    clear(g_a, blocks_for(0.15f) * kBlk);
    render(va, p, g_a, blocks_for(0.15f));
    CHECK_EQ(va.voice_note(0), 48);

    const uint8_t rest[5] = {55, 60, 64, 67};
    for (int i = 0; i < 4; i++) va.trigger(hit(rest[i], 100, false, 0));
    va.trigger(hit(72, 100, false, 0));
    clear(g_b, 2 * kBlk);
    render(va, p, g_b, 2);
    CHECK_EQ(va.busy_voices(), 6);

    va.trigger(hit(79, 100, false, 0));
    CHECK_EQ(va.voice_note(0), 79);     // the oldest, quietest voice is the one that went
    CHECK(!holds(va, 48));
    for (int i = 0; i < 4; i++) CHECK(holds(va, rest[i]));
    CHECK(holds(va, 72));
}

TEST(vapoly_a_chord_change_that_steals_keeps_all_of_its_notes) {
    // Two notes arriving on one frame when every voice is busy and the voices are at DIFFERENT
    // levels. The first of the two steals the quietest voice; the second must then leave that
    // voice alone even though it is still the quietest thing in the machine, or it takes the
    // same voice and the first of the two notes is lost. This is the same-frame bug again, on
    // the steal path rather than the free-voice path, and it is why the steal scan skips a
    // voice that is already holding a trig.
    TrackParams p = patch();
    p.set(kDec, 80);                    // about 0.35 s, so the voices decay at different rates
    VaPoly va;
    va.init(kRate);
    va.prepare(p);

    va.trigger(hit(48, 100, false, 0));            // the oldest, so the quietest
    clear(g_a, blocks_for(0.12f) * kBlk);
    render(va, p, g_a, blocks_for(0.12f));
    va.trigger(hit(50, 100, false, 0));            // the next oldest
    clear(g_a, blocks_for(0.08f) * kBlk);
    render(va, p, g_a, blocks_for(0.08f));
    const uint8_t rest[4] = {55, 60, 64, 67};
    for (int i = 0; i < 4; i++) va.trigger(hit(rest[i], 100, false, 0));
    clear(g_a, 2 * kBlk);
    render(va, p, g_a, 2);
    CHECK_EQ(va.busy_voices(), 6);

    va.trigger(hit(72, 100, false, 0));            // both on the same frame, both must land
    va.trigger(hit(76, 100, false, 0));
    CHECK(holds(va, 72));
    CHECK(holds(va, 76));
    CHECK(!holds(va, 48));                         // the two quietest gave up their voices
    CHECK(!holds(va, 50));
    for (int i = 0; i < 4; i++) CHECK(holds(va, rest[i]));
    CHECK_EQ(va.busy_voices(), 6);

    const int blocks = blocks_for(0.2f);
    clear(g_b, blocks * kBlk);
    render(va, p, g_b, blocks);
    CHECK_EQ(va.sounding_voices(), 6);
    CHECK(finite_within(g_b, blocks * kBlk, kChordBound));
}

TEST(vapoly_equal_voices_are_stolen_in_turn) {
    // Six voices triggered on the same frame with the same patch sit at exactly the same level,
    // so "the quietest" cannot choose between them: the rotating cursor does. Three more notes
    // on one frame must therefore take three DIFFERENT voices -- including not re-stealing the
    // voice the previous one of them just took, which would make a three-note chord change
    // sound as one note.
    const TrackParams p = patch();
    const uint8_t notes[6] = {48, 50, 52, 53, 55, 57};
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    for (int i = 0; i < 6; i++) va.trigger(hit(notes[i], 100, false, 0));
    clear(g_a, blocks_for(0.1f) * kBlk);
    render(va, p, g_a, blocks_for(0.1f));

    va.trigger(hit(72, 100, false, 0));
    va.trigger(hit(76, 100, false, 0));
    va.trigger(hit(79, 100, false, 0));
    CHECK(holds(va, 72));
    CHECK(holds(va, 76));
    CHECK(holds(va, 79));
    CHECK_EQ(va.busy_voices(), 6);

    int survivors = 0;
    for (int i = 0; i < 6; i++) survivors += holds(va, notes[i]) ? 1 : 0;
    CHECK_EQ(survivors, 3);     // three went, three stayed: one voice each, not three in one

    const int blocks = blocks_for(0.3f);
    clear(g_b, blocks * kBlk);
    render(va, p, g_b, blocks);
    CHECK_EQ(va.sounding_voices(), 6);
    CHECK(finite_within(g_b, blocks * kBlk, kChordBound));
}

TEST(vapoly_more_notes_than_voices_on_one_frame_stay_finite) {
    // Eight notes on one frame from silence: six voices take six of them and the last two have
    // to displace a note that has not started yet. Something audibly goes, which is the only
    // possible answer; what must not happen is a hang in the per-voice slicing loop or a voice
    // left waiting for a frame that never comes.
    const TrackParams p = patch();
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    const uint8_t notes[8] = {36, 43, 48, 55, 60, 64, 67, 72};
    for (int i = 0; i < 8; i++) va.trigger(hit(notes[i], 100, false, 0));
    CHECK_EQ(va.busy_voices(), 6);
    CHECK(holds(va, 67));       // the last two arrivals are the ones that are held
    CHECK(holds(va, 72));

    const int blocks = blocks_for(0.3f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK_EQ(va.sounding_voices(), 6);
    CHECK(finite_within(g_a, blocks * kBlk, kChordBound));
    CHECK(peak(g_a, blocks * kBlk) > 0.05f);
}

TEST(vapoly_the_same_note_retriggers_its_own_voice) {
    // A repeated chord reuses its own voices instead of stealing fresh ones, and the retrigger
    // behaves as VA MONO's does: the envelope starts again from the level it is at, so the
    // repeat is louder than the tail it interrupted but costs no second voice.
    TrackParams p = patch();
    p.set(kDec, 90);            // about 0.55 s, so there is a decay to interrupt
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 100, false, 0));

    const int first = blocks_for(0.15f);
    const int window = int(0.015f * kRate);
    clear(g_a, first * kBlk);
    const int n = render(va, p, g_a, first);
    const float before = peak(g_a + n - window, window);
    CHECK(before > 0.01f);
    CHECK_EQ(va.sounding_voices(), 1);

    va.trigger(hit(60, 100, false, 0));
    CHECK_EQ(va.busy_voices(), 1);          // the same voice, not a second one
    clear(g_b, blocks_for(0.1f) * kBlk);
    render(va, p, g_b, blocks_for(0.1f));
    CHECK_EQ(va.sounding_voices(), 1);
    CHECK(peak(g_b + int(0.006f * kRate), window) > 1.5f * before);   // it did restart
}

TEST(vapoly_a_ratchet_keeps_both_of_its_hits) {
    // A ratcheted step fires the same note several times at different frames of one block. A
    // voice that is still WAITING for its frame is not a match for the same note, so the second
    // hit takes a second voice and both sound; matching it would replace the first hit and only
    // one of the two would ever be heard.
    const TrackParams p = patch();
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 100, false, 0));
    va.trigger(hit(60, 100, false, 32));
    CHECK_EQ(va.busy_voices(), 2);

    const int blocks = blocks_for(0.2f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK_EQ(va.sounding_voices(), 2);
    CHECK(peak(g_a, 32) > 0.01f);           // the first hit sounded before the second's frame
    CHECK(finite_within(g_a, blocks * kBlk, kChordBound));
}

TEST(vapoly_a_finished_voice_is_reclaimed) {
    // A voice whose note has decayed away is free again. If it were not, a pattern would run
    // out of voices after six steps and every later chord would be a steal.
    TrackParams p = patch();
    p.set(kDec, 0);             // the shortest decay: 15 ms
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    const uint8_t notes[6] = {48, 55, 60, 64, 67, 72};
    for (int i = 0; i < 6; i++) va.trigger(hit(notes[i], 100, false, 0));

    const int blocks = blocks_for(0.5f);
    clear(g_a, blocks * kBlk);
    const int n = render(va, p, g_a, blocks);
    CHECK(peak(g_a, 2 * kBlk) > 0.05f);                  // they did sound
    CHECK(peak(g_a + n - 4 * kBlk, 4 * kBlk) < 1e-4f);   // and then they did not
    CHECK(!va.active());
    CHECK_EQ(va.sounding_voices(), 0);
    CHECK_EQ(va.busy_voices(), 0);
    for (int i = 0; i < 6; i++) CHECK_EQ(va.voice_note(i), -1);

    // And six fresh notes find six free voices rather than stealing.
    for (int i = 0; i < 6; i++) va.trigger(hit(notes[i], 100, false, 0));
    CHECK_EQ(va.busy_voices(), 6);
    clear(g_b, 8 * kBlk);
    render(va, p, g_b, 8);
    CHECK_EQ(va.sounding_voices(), 6);
    CHECK(peak(g_b, 8 * kBlk) > 0.05f);
}

// --- the buffer contract --------------------------------------------------------------------

TEST(vapoly_render_add_accumulates_and_never_clears) {
    // Six voices and every one of them adds: the machine shares the track's buffer with
    // whatever the engine has already put there.
    const TrackParams p = patch();
    const int blocks = 24;
    const int n = blocks * kBlk;
    const uint8_t notes[3] = {60, 64, 67};

    VaPoly a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(p);
    b.prepare(p);
    for (int i = 0; i < 3; i++) {
        a.trigger(hit(notes[i], 100, false, 0));
        b.trigger(hit(notes[i], 100, false, 0));
    }

    clear(g_a, n);
    fill(g_b, n, 0.25f);
    render(a, p, g_a, blocks);
    render(b, p, g_b, blocks);

    for (int i = 0; i < n; i += 37) {
        CHECK_NEAR(g_b[i] - 0.25f, g_a[i], 1e-6f);
    }
    CHECK(peak(g_a, n) > 0.05f);       // and there was something to add
}

TEST(vapoly_kill_all_silences_every_voice_at_once) {
    const TrackParams p = patch();
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    for (int i = 0; i < 6; i++) va.trigger(hit(uint8_t(48 + 4 * i), 100, false, 0));
    clear(g_a, blocks_for(0.1f) * kBlk);
    render(va, p, g_a, blocks_for(0.1f));
    CHECK(va.active());
    CHECK_EQ(va.sounding_voices(), 6);

    va.kill_all();
    CHECK(!va.active());
    CHECK_EQ(va.sounding_voices(), 0);
    CHECK_EQ(va.busy_voices(), 0);
    clear(g_b, 16 * kBlk);
    render(va, p, g_b, 16);
    CHECK_EQ(peak(g_b, 16 * kBlk), 0.0f);
}

TEST(vapoly_kill_all_drops_a_trig_that_has_not_started) {
    // A hard cut is a hard cut: a note still waiting for its frame does not sound through it,
    // and -- the part that is easy to get wrong -- it does not come back later either. A kill
    // that forgot to drop the pending flag leaves a voice holding a trig that the NEXT trigger
    // makes active again, so a killed note sounds a step late.
    const TrackParams p = patch();
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    va.trigger(hit(60, 100, false, 40));
    va.kill_all();
    CHECK(!va.active());
    CHECK_EQ(va.busy_voices(), 0);          // the trig is gone, not merely silenced
    CHECK_EQ(va.voice_note(0), -1);
    clear(g_a, 8 * kBlk);
    render(va, p, g_a, 8);
    CHECK_EQ(peak(g_a, 8 * kBlk), 0.0f);

    // A later note brings the machine back, and only that note sounds.
    va.trigger(hit(67, 100, false, 0));
    CHECK_EQ(va.busy_voices(), 1);
    clear(g_b, 8 * kBlk);
    render(va, p, g_b, 8);
    CHECK_EQ(va.sounding_voices(), 1);
    CHECK(holds(va, 67));
    CHECK(!holds(va, 60));
    CHECK(started_on(g_b, 8 * kBlk, 0));
}

TEST(vapoly_release_all_leaves_a_tail_on_every_voice) {
    const TrackParams p = patch();      // DEC 127, so the chord would hold for seconds

    VaPoly released, killed;
    released.init(kRate);
    killed.init(kRate);
    released.prepare(p);
    killed.prepare(p);
    for (int i = 0; i < 3; i++) {
        released.trigger(hit(uint8_t(60 + 4 * i), 100, false, 0));
        killed.trigger(hit(uint8_t(60 + 4 * i), 100, false, 0));
    }
    clear(g_a, blocks_for(0.1f) * kBlk);
    clear(g_b, blocks_for(0.1f) * kBlk);
    render(released, p, g_a, blocks_for(0.1f));
    render(killed, p, g_b, blocks_for(0.1f));

    released.release_all();
    killed.kill_all();

    const int blocks = blocks_for(0.05f);
    const int n = blocks * kBlk;
    clear(g_a, n);
    clear(g_b, n);
    render(released, p, g_a, blocks);
    render(killed, p, g_b, blocks);

    CHECK(energy(g_a, n) > 0.0f);           // a tail, not a cut
    CHECK(peak(g_a, 2 * kBlk) > 0.01f);
    CHECK_EQ(peak(g_b, n), 0.0f);

    // And the tail ends: the release reaches idle, which is what frees the voices.
    render(released, p, g_a, blocks_for(0.5f));
    CHECK(!released.active());
    CHECK_EQ(released.sounding_voices(), 0);
}

// --- the extremes ---------------------------------------------------------------------------

TEST(vapoly_all_zero_parameters_are_silent_not_broken) {
    TrackParams p = uniform(0);          // LVL 0
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    for (int i = 0; i < 6; i++) va.trigger(hit(uint8_t(i), 127, true, 0));
    const int blocks = blocks_for(0.2f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK(finite_within(g_a, blocks * kBlk, kChordBound));
    CHECK_EQ(peak(g_a, blocks * kBlk), 0.0f);
}

TEST(vapoly_all_127_parameters_stay_finite) {
    TrackParams p = uniform(127);        // LVL 127, RES 127, DET 127, HP12, the lot
    VaPoly va;
    va.init(kRate);
    va.prepare(p);
    for (int i = 0; i < 6; i++) va.trigger(hit(uint8_t(120 + i), 127, true, uint32_t(8 * i)));
    const int blocks = blocks_for(0.3f);
    clear(g_a, blocks * kBlk);
    render(va, p, g_a, blocks);
    CHECK(finite_within(g_a, blocks * kBlk, kChordBound));

    // And again over the top of a sounding chord, with every note at both ends of the range.
    va.trigger(hit(0, 1, false, 0));
    va.trigger(hit(127, 127, true, 1));
    va.trigger(hit(60, 64, true, 63));
    clear(g_b, blocks * kBlk);
    render(va, p, g_b, blocks);
    CHECK(finite_within(g_b, blocks * kBlk, kChordBound));
}

TEST(vapoly_full_resonance_is_finite_in_every_mode) {
    const uint8_t modes[4] = {kLp24, kLp12, kBp12, kHp12};
    for (int i = 0; i < 4; i++) {
        TrackParams p = patch();
        p.set(kMod, modes[i]);
        p.set(kRes, 127);
        p.set(kCut, 40);
        p.set(kEnv, 100);        // and sweeping across the cutoff while six voices ring

        VaPoly va;
        va.init(kRate);
        va.prepare(p);
        for (int v = 0; v < 6; v++) va.trigger(hit(uint8_t(48 + 4 * v), 127, true, 0));
        const int blocks = blocks_for(0.4f);
        clear(g_a, blocks * kBlk);
        render(va, p, g_a, blocks);
        CHECK(finite_within(g_a, blocks * kBlk, kChordBound));
    }
}

// --- the parameters reach the voices --------------------------------------------------------

TEST(vapoly_pitch_follows_the_note_and_the_tuning_knob) {
    // Six voices, so the test is that EVERY voice transposes, not just the first.
    TrackParams flat = patch(), up = patch();
    up.set(kTun, 96);        // (96 - 64) * 24/64 = +12 semitones exactly

    VaPoly a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(flat);
    b.prepare(up);
    const uint8_t notes[3] = {60, 64, 67};
    for (int i = 0; i < 3; i++) {
        a.trigger(hit(notes[i], 100, false, 0));
        b.trigger(hit(notes[i], 100, false, 0));
    }
    clear(g_a, 4 * kBlk);
    clear(g_b, 4 * kBlk);
    render(a, flat, g_a, 4);
    render(b, up, g_b, 4);

    for (int i = 0; i < 3; i++) {
        CHECK_NEAR(a.voice_pitch(i), float(notes[i]), 0.01f);
        CHECK_NEAR(b.voice_pitch(i), float(notes[i]) + 12.0f, 0.01f);
    }

    // And heard: one note of the chord, an octave up, repeats at the octave's period.
    const int blocks = blocks_for(0.4f);
    const int from = int(0.05f * kRate);
    const int window = int(0.25f * kRate);
    VaPoly low, high;
    low.init(kRate);
    high.init(kRate);
    low.prepare(flat);
    high.prepare(up);
    low.trigger(hit(60, 100, false, 0));
    high.trigger(hit(60, 100, false, 0));
    clear(g_a, blocks * kBlk);
    clear(g_b, blocks * kBlk);
    render(low, flat, g_a, blocks);
    render(high, up, g_b, blocks);
    CHECK(periodicity(g_a + from, window, period_of(60)) > 0.85f);
    CHECK(periodicity(g_b + from, window, period_of(72)) > 0.85f);
    // The reverse direction only: a note an octave UP also repeats at twice its own period, so
    // the discriminator has to be the long-period signal measured at the short lag.
    CHECK(periodicity(g_a + from, window, period_of(72)) < 0.7f);
}

TEST(vapoly_detune_spreads_the_voice_slots_and_leaves_slot_zero_in_tune) {
    // DET is the slot VA MONO spends on GLD. It detunes the voices against each other, which is
    // what thickens a chord; slot 0 stays in tune, because a single note always lands there.
    TrackParams none = patch(), wide = patch();
    wide.set(kDet, 127);

    VaPoly flat, spread;
    flat.init(kRate);
    spread.init(kRate);
    flat.prepare(none);
    spread.prepare(wide);
    CHECK_EQ(flat.detune_semitones(), 0.0f);
    CHECK(spread.detune_semitones() > 0.1f);
    CHECK(spread.detune_semitones() < 0.5f);     // a quarter tone at most: still in tune

    for (int i = 0; i < 6; i++) {
        flat.trigger(hit(60, 100, false, 0));    // the same note six times: a ratchet's worth,
        spread.trigger(hit(60, 100, false, 0));  // so every slot holds the same note number
    }
    clear(g_a, 4 * kBlk);
    clear(g_b, 4 * kBlk);
    render(flat, none, g_a, 4);
    render(spread, wide, g_b, 4);

    CHECK_EQ(flat.busy_voices(), 6);
    CHECK_EQ(spread.busy_voices(), 6);
    for (int i = 0; i < 6; i++) CHECK_NEAR(flat.voice_pitch(i), 60.0f, 0.001f);

    CHECK_NEAR(spread.voice_pitch(0), 60.0f, 0.001f);          // slot 0: exactly in tune
    CHECK(spread.voice_pitch(1) > 60.0f);                      // and the pairs beyond it spread
    CHECK(spread.voice_pitch(2) < 60.0f);
    CHECK_NEAR(spread.voice_pitch(1) - 60.0f, 60.0f - spread.voice_pitch(2), 0.001f);
    CHECK_NEAR(spread.voice_pitch(1) - 60.0f, spread.detune_semitones(), 0.001f);

    // Six voices a quarter tone apart beat against each other, so the chord is not just six
    // copies of one waveform: the energy differs from the in-tune stack's.
    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;
    clear(g_a, n);
    clear(g_b, n);
    render(flat, none, g_a, blocks);
    render(spread, wide, g_b, blocks);
    CHECK(energy(g_a, n) > 0.0f);
    CHECK(energy(g_b, n) > 0.0f);
    const float ratio = energy(g_b, n) * enaga::dsp::reciprocal(energy(g_a, n));
    CHECK(ratio < 0.98f || ratio > 1.02f);
    CHECK(finite_within(g_b, n, kChordBound));
}

TEST(vapoly_each_voice_has_its_own_noise_stream) {
    // Six voices sharing one noise seed would add six copies of the same signal in lockstep.
    // Two voices playing the SAME note with DET 0 are otherwise identical, sample for sample,
    // so the difference between two of them and twice one of them is exactly the noise -- zero
    // if the seeds were shared, audible if they are not.
    const TrackParams p = patch();
    const int blocks = blocks_for(0.2f);
    const int n = blocks * kBlk;

    VaPoly single, pair;
    single.init(kRate);
    pair.init(kRate);
    single.prepare(p);
    pair.prepare(p);
    single.trigger(hit(60, 100, false, 0));
    pair.trigger(hit(60, 100, false, 0));
    pair.trigger(hit(60, 100, false, 0));      // a second voice, the same note, the same frame
    CHECK_EQ(pair.busy_voices(), 2);

    clear(g_a, n);
    clear(g_b, n);
    render(single, p, g_a, blocks);
    render(pair, p, g_b, blocks);

    float worst = 0.0f;
    for (int i = 0; i < n; i++) {
        const float d = g_b[i] - 2.0f * g_a[i];
        const float a = d < 0.0f ? -d : d;
        if (a > worst) worst = a;
    }
    CHECK(worst > 1e-4f);                      // the two voices are not the same signal
    CHECK(worst < 0.2f);                       // ... but they are the same note: only noise
    CHECK(peak(g_a, n) > 0.05f);
}

TEST(vapoly_accent_reaches_the_voices) {
    TrackParams p = patch();
    p.set(kMod, kLp24);
    p.set(kCut, 30);         // closed enough that opening it is audible
    p.set(kRes, 60);
    p.set(kEnv, 70);
    p.set(kDec, 70);

    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;

    VaPoly plain, accented;
    plain.init(kRate);
    accented.init(kRate);
    plain.prepare(p);
    accented.prepare(p);
    plain.trigger(hit(60, 100, false, 0));
    accented.trigger(hit(60, 100, true, 0));
    clear(g_a, n);
    clear(g_b, n);
    render(plain, p, g_a, blocks);
    render(accented, p, g_b, blocks);

    const float pa = peak(g_a, n);
    const float pb = peak(g_b, n);
    CHECK(pa > 0.01f);
    CHECK(pb > pa);                      // louder, which is the least of what an accent does

    // Matched for level, the accented note is still brighter: the accent opens the filter and
    // lifts the synced oscillator and the noise (dsp/va.cpp).
    const float scale = pb * enaga::dsp::reciprocal(pa);
    for (int i = 0; i < n; i++) g_a[i] *= scale;
    CHECK(sign_changes(g_b, n) > sign_changes(g_a, n) * 6 / 5);
}

TEST(vapoly_velocity_reaches_the_voices) {
    const TrackParams p = patch();
    const int blocks = blocks_for(0.2f);
    const int n = blocks * kBlk;

    VaPoly soft, hard;
    soft.init(kRate);
    hard.init(kRate);
    soft.prepare(p);
    hard.prepare(p);
    soft.trigger(hit(60, 1, false, 0));
    hard.trigger(hit(60, 127, false, 0));
    clear(g_a, n);
    clear(g_b, n);
    render(soft, p, g_a, blocks);
    render(hard, p, g_b, blocks);

    CHECK(peak(g_a, n) > 0.0f);                    // velocity 1 is quiet, not silent
    CHECK(peak(g_b, n) > 2.0f * peak(g_a, n));
}

TEST(vapoly_the_filter_knobs_reach_the_voices) {
    // The voice filter is tests/test_va.cpp's subject; what this pins is that MOD and CUT get
    // from this machine's parameter page to all of its voices.
    TrackParams open = patch(), closed = patch();
    closed.set(kMod, kLp24);
    closed.set(kCut, 0);        // 35 Hz, far below the chord
    closed.set(kEnv, 0);

    const int blocks = blocks_for(0.3f);
    const int n = blocks * kBlk;
    const uint8_t notes[3] = {60, 64, 67};

    VaPoly a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(open);
    b.prepare(closed);
    for (int i = 0; i < 3; i++) {
        a.trigger(hit(notes[i], 100, false, 0));
        b.trigger(hit(notes[i], 100, false, 0));
    }
    clear(g_a, n);
    clear(g_b, n);
    render(a, open, g_a, blocks);
    render(b, closed, g_b, blocks);

    CHECK(energy(g_a, n) > 1.0f);
    CHECK(energy(g_b, n) < 0.05f * energy(g_a, n));
    CHECK(sign_changes(g_b, n) < sign_changes(g_a, n));
    CHECK_NEAR(b.base_cutoff_hz(), 35.0f, 1.0f);
    CHECK(a.base_cutoff_hz() > 10000.0f);
    CHECK(b.mode() == VaMode::lp24);
    CHECK(a.mode() == VaMode::lp12);
}

TEST(vapoly_decay_knob_spans_its_range_and_lengthens_a_chord) {
    VaPoly va;
    va.init(kRate);
    TrackParams p = patch();

    p.set(kDec, 0);
    va.prepare(p);
    const float shortest = va.decay_seconds();
    p.set(kDec, 127);
    va.prepare(p);
    const float longest = va.decay_seconds();
    CHECK_NEAR(shortest, 0.015f, 0.002f);
    CHECK(longest > 2.0f && longest < 3.0f);

    TrackParams shorter = patch(), longer = patch();
    shorter.set(kDec, 20);
    longer.set(kDec, 90);
    const int blocks = blocks_for(0.6f);
    const int n = blocks * kBlk;

    VaPoly a, b;
    a.init(kRate);
    b.init(kRate);
    a.prepare(shorter);
    b.prepare(longer);
    for (int i = 0; i < 3; i++) {
        a.trigger(hit(uint8_t(60 + 4 * i), 100, false, 0));
        b.trigger(hit(uint8_t(60 + 4 * i), 100, false, 0));
    }
    clear(g_a, n);
    clear(g_b, n);
    render(a, shorter, g_a, blocks);
    render(b, longer, g_b, blocks);

    CHECK(energy(g_b, n) > 2.0f * energy(g_a, n));
    CHECK(!a.active());      // the short chord has finished inside the window
    CHECK(b.active());       // the long one has not
}

// --- Step::length, as a gate ----------------------------------------------------------------
//
// `Step::length` has been stored and serialised since M3 and had never made a sound: nothing
// read it. The engine now converts it to frames (app/engine.cpp, `gate_frames_for`) and hands
// it over as `Trig::gate_frames`, and this machine counts it down per voice and releases on its
// exact frame. core/timing.hpp's own comment sets the rule these tests hold it to: "gate length
// does not have to land on the tick grid: it only decides when the voice is released".
//
// The countdown lives in VaPoly's Slot rather than in the shared VaVoice on purpose
// (dsp/vapoly.hpp says why): VA MONO's arithmetic has to stay bit-identical, and
// tests/test_va.cpp is what proves it still is.
//
// Mutation-tested, and three mutations of the gate path survive because they cannot be heard.
// They are recorded here so nobody writes a test that cannot fail:
//
//   * `s.gate <= n` for `s.gate < n` in the slice cut -- when they are equal the assignment is
//     the value it already had.
//   * dropping `!s.pending` from the slice cut -- re-slicing a voice that has already started is
//     bit-transparent, so this costs control-rate work and changes no sample (dsp/vapoly.cpp
//     says the same at the line).
//   * dropping the kGateMax clamp -- it only ever LOWERS a count above ten seconds, and the
//     longest DEC is 2.4 s, so no note survives to where the two differ. A count so large it
//     wraps the int lands negative, which this machine reads as no gate: inert either way. The
//     clamp's magnitude IS tested (`..._is_clamped_rather_than_wrapping` fails if kGateMax
//     drops into an audible window); its existence cannot be.

TEST(vapoly_a_trig_with_no_gate_is_left_to_its_own_envelope) {
    // 0 is what every trig carried before the field existed and what a step with no length
    // asks for. DEC 127 is a 2.4 s decay, so the note is still sounding a second in.
    VaPoly va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    va.trigger(hit(60, 100, false, 0, 0));

    const int blocks = blocks_for(1.5f);
    clear(g_a, blocks * kBlk);
    const int n = render(va, p, g_a, blocks);
    CHECK(va.active());
    CHECK(peak(g_a + ENAGA_SAMPLE_RATE, n - ENAGA_SAMPLE_RATE) > 0.01f);
}

TEST(vapoly_a_gate_changes_nothing_before_the_frame_it_closes_on) {
    // The whole contract in one measurement, at six lengths spanning the table: a gated note is
    // the ungated note until its gate's last frame, and differs from it on that frame. Not one
    // control step early, not one late.
    const TrackParams p = patch();
    const int blocks = blocks_for(1.2f);

    const uint32_t gates[] = {1u, 64u, 1000u, 4000u, 12000u, 48000u};
    for (uint32_t gate : gates) {
        VaPoly open;
        open.init(kRate);
        open.prepare(p);
        open.trigger(hit(60, 100, false, 0, 0));
        clear(g_a, blocks * kBlk);
        const int n = render(open, p, g_a, blocks);

        VaPoly gated;
        gated.init(kRate);
        gated.prepare(p);
        gated.trigger(hit(60, 100, false, 0, gate));
        clear(g_b, blocks * kBlk);
        render(gated, p, g_b, blocks);

        CHECK_EQ(first_difference(g_a, g_b, n), int(gate));
    }
}

TEST(vapoly_the_gate_starts_when_the_note_starts_not_when_the_trig_was_taken) {
    // A trig is committed to a voice as soon as it arrives but starts on its own frame, which is
    // what micro-timing and ratchets are built on. The gate is a NOTE length, so it has to be
    // counted from the frame the note starts on -- otherwise a step nudged late would be
    // shortened by exactly the nudge, and the last frames tested here are a whole block away.
    const TrackParams p = patch();
    const int blocks = blocks_for(1.2f);
    constexpr uint32_t kGate = 4000u;

    const uint32_t frames[] = {0u, 13u, 63u, 200u};
    for (uint32_t frame : frames) {
        VaPoly open;
        open.init(kRate);
        open.prepare(p);
        open.trigger(hit(60, 100, false, frame, 0));
        clear(g_a, blocks * kBlk);
        const int n = render(open, p, g_a, blocks);

        VaPoly gated;
        gated.init(kRate);
        gated.prepare(p);
        gated.trigger(hit(60, 100, false, frame, kGate));
        clear(g_b, blocks * kBlk);
        render(gated, p, g_b, blocks);

        CHECK_EQ(first_difference(g_a, g_b, n), int(frame + kGate));
    }
}

TEST(vapoly_each_voice_counts_its_own_gate) {
    // Three notes on one frame, each carrying a different length -- a chord whose notes do not
    // all end together, which is the reason the counter is per slot. Gating one note at a time
    // and asking where the chord first changes says which voice the count belongs to: a gate
    // kept per machine instead of per voice would answer with the same frame all three times.
    const TrackParams p = patch();
    const int blocks = blocks_for(1.2f);
    const uint8_t notes[] = {60, 64, 67};
    const uint32_t gates[] = {1000u, 2000u, 3000u};

    VaPoly open;
    open.init(kRate);
    open.prepare(p);
    for (uint8_t note : notes) open.trigger(hit(note, 100, false, 0, 0));
    CHECK_EQ(open.voice_note(0), 60);   // in slot order, so the loop below knows which is which
    CHECK_EQ(open.voice_note(1), 64);
    CHECK_EQ(open.voice_note(2), 67);
    clear(g_a, blocks * kBlk);
    const int n = render(open, p, g_a, blocks);

    for (int which = 0; which < 3; which++) {
        VaPoly gated;
        gated.init(kRate);
        gated.prepare(p);
        for (int i = 0; i < 3; i++) {
            gated.trigger(hit(notes[i], 100, false, 0, i == which ? gates[i] : 0u));
        }
        clear(g_b, blocks * kBlk);
        render(gated, p, g_b, blocks);
        CHECK_EQ(first_difference(g_a, g_b, n), int(gates[which]));
    }

    // And all three at once: the chord first changes where the shortest note ends.
    VaPoly all;
    all.init(kRate);
    all.prepare(p);
    for (int i = 0; i < 3; i++) all.trigger(hit(notes[i], 100, false, 0, gates[i]));
    clear(g_b, blocks * kBlk);
    render(all, p, g_b, blocks);
    CHECK_EQ(first_difference(g_a, g_b, n), int(gates[0]));
}

TEST(vapoly_a_steal_gates_the_new_note_and_not_the_one_it_replaces) {
    // All six voices sounding, so the next trig has to steal one. The stealing trig commits to
    // its slot at once but starts on its own frame, and the note already there keeps sounding
    // until it does -- with the shortest gate the field can carry, one frame, counting all the
    // while. Nothing may change before the NEW note's own end: a countdown that ran during the
    // wait would release the note being replaced 40 frames early, and a gate booked against the
    // wrong slot would shorten a note nobody gated.
    const TrackParams p = patch();
    constexpr int kWarm = 8;                       // blocks: long enough that all six sound
    constexpr int kFrame = 40;
    const int blocks = blocks_for(0.5f);
    const int start = kWarm * kBlk + kFrame;       // the stealing note's first frame

    float *buffers[2] = {g_a, g_b};
    for (int pass = 0; pass < 2; pass++) {
        VaPoly va;
        va.init(kRate);
        va.prepare(p);
        for (int i = 0; i < VaPoly::kVoices; i++) va.trigger(hit(uint8_t(48 + i), 100, false, 0));
        clear(buffers[pass], (kWarm + blocks) * kBlk);
        render(va, p, buffers[pass], kWarm);
        CHECK_EQ(va.sounding_voices(), VaPoly::kVoices);   // nothing free: the next trig steals

        va.trigger(hit(72, 100, false, kFrame, pass == 0 ? 0u : 1u));
        CHECK(holds(va, 72));                              // the stolen slot took the new note
        for (int i = 49; i <= 53; i++) CHECK(holds(va, i)); // and the others kept theirs
        render(va, p, buffers[pass] + kWarm * kBlk, blocks);
    }
    CHECK_EQ(first_difference(g_a, g_b, (kWarm + blocks) * kBlk), start + 1);
}

TEST(vapoly_a_gate_releases_the_voice_it_does_not_cut_it) {
    // The release, not a cut: `kill()` on the gate's last frame would put a step in the output,
    // which is a click. So the note is still audible just after its gate closes, decays, and is
    // exactly silent well before the ungated note would be -- and the voice is given back.
    VaPoly va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    constexpr int kGate = 4000;
    va.trigger(hit(60, 100, false, 0, uint32_t(kGate)));

    const int blocks = blocks_for(1.5f);
    clear(g_a, blocks * kBlk);   // the capture buffers are shared and render_add ADDS into them
    const int n = render(va, p, g_a, blocks);
    const float held = peak(g_a, kGate);
    CHECK(held > 0.1f);
    CHECK(peak(g_a + kGate, 64) > 0.1f * held);        // still ringing one block later
    CHECK(peak(g_a + kGate, 2000) < held);             // and on the way down
    CHECK_EQ(peak(g_a + ENAGA_SAMPLE_RATE, n - ENAGA_SAMPLE_RATE), 0.0f);
    CHECK(!va.active());                               // the slot is free again
    CHECK_EQ(va.sounding_voices(), 0);
    CHECK(finite_within(g_a, n, 4.0f));
}

TEST(vapoly_an_absurd_gate_is_clamped_rather_than_wrapping) {
    // `gate_frames` is a uint32_t and the countdown is an int, so a length computed from a tempo
    // read before the clock has settled must not land in the counter as a negative number or as
    // an immediate release. These four are "longer than anything this machine can still be heard
    // through", so each has to behave exactly as no gate at all over the whole capture.
    //
    // The clamp's VALUE (kGateMax, ten seconds) is not audible and is not tested: the longest
    // DEC is 2.4 s, so no note survives to ten. What is tested is that nothing wraps and nothing
    // fires early -- a kGateMax small enough to land inside this window would fail here.
    const TrackParams p = patch();
    const int blocks = blocks_for(1.9f);

    VaPoly open;
    open.init(kRate);
    open.prepare(p);
    open.trigger(hit(60, 100, false, 0, 0));
    clear(g_a, blocks * kBlk);
    const int n = render(open, p, g_a, blocks);

    const uint32_t gates[] = {480000u, 600000u, 0x80000001u, 0xFFFFFFFFu};
    for (uint32_t gate : gates) {
        VaPoly gated;
        gated.init(kRate);
        gated.prepare(p);
        gated.trigger(hit(60, 100, false, 0, gate));
        clear(g_b, blocks * kBlk);
        render(gated, p, g_b, blocks);
        CHECK_EQ(first_difference(g_a, g_b, n), -1);
    }
}

TEST(vapoly_a_gate_that_closes_after_a_stop_changes_nothing) {
    // STOP releases every voice. A gate still counting then closes on a voice that is already
    // releasing, and its gate_off() has to be a second one that does nothing: a release that
    // restarted from the current level would stretch the tail, and one that reset it would
    // click. Bit-identical to the same stop with no gate at all is the whole statement.
    const TrackParams p = patch();
    const int blocks = blocks_for(1.5f);
    constexpr int kStopBlock = 20;                    // frame 1280, before either gate closes

    VaPoly open;
    open.init(kRate);
    open.prepare(p);
    open.trigger(hit(60, 100, false, 0, 0));
    clear(g_a, blocks * kBlk);
    const int n = render_with_stop(open, p, g_a, blocks, kStopBlock, -1);

    VaPoly late;
    late.init(kRate);
    late.prepare(p);
    late.trigger(hit(60, 100, false, 0, 4000));       // closes after the stop: no effect
    clear(g_b, blocks * kBlk);
    render_with_stop(late, p, g_b, blocks, kStopBlock, -1);
    CHECK_EQ(first_difference(g_a, g_b, n), -1);

    // The control: a gate that closes BEFORE the stop does change the output, on its own frame.
    // Without this the test above would pass just as well on a machine that ignored the field.
    VaPoly early;
    early.init(kRate);
    early.prepare(p);
    early.trigger(hit(60, 100, false, 0, 1000));
    clear(g_b, blocks * kBlk);
    render_with_stop(early, p, g_b, blocks, kStopBlock, -1);
    CHECK_EQ(first_difference(g_a, g_b, n), 1000);
}

TEST(vapoly_kill_all_ends_a_gated_note_and_its_countdown) {
    // A pattern change takes the voices away outright. The countdown has to go with them: a
    // slot left counting would release a voice that another note had since been given.
    VaPoly va;
    va.init(kRate);
    const TrackParams p = patch();
    va.prepare(p);
    va.trigger(hit(60, 100, false, 0, 4000));         // 4000 frames = block 62

    constexpr int kKillBlock = 20;                    // frame 1280, while the gate still counts
    const int blocks = blocks_for(1.0f);
    clear(g_a, blocks * kBlk);
    const int n = render_with_stop(va, p, g_a, blocks, -1, kKillBlock);

    CHECK(peak(g_a, kKillBlock * kBlk) > 0.1f);                                 // it sounded
    CHECK_EQ(peak(g_a + kKillBlock * kBlk, n - kKillBlock * kBlk), 0.0f);       // then stopped
    CHECK(!va.active());
    CHECK_EQ(va.voice_note(0), -1);

    // And the slot is clean enough to take a new note, which the stale gate must not shorten.
    va.trigger(hit(72, 100, false, 0, 0));
    clear(g_b, blocks * kBlk);
    render(va, p, g_b, blocks);
    CHECK(va.active());
    CHECK(peak(g_b + ENAGA_SAMPLE_RATE / 2, n - ENAGA_SAMPLE_RATE / 2) > 0.01f);
}

// --- the parameter surface ------------------------------------------------------------------

TEST(vapoly_parameter_names_are_three_upper_case_letters) {
    VaPoly va;
    va.init(kRate);
    CHECK_EQ(va.used_params(), 8);

    const char *const expected[kParams] = {
        "TUN", "MOD", "CUT", "RES", "---", "---", "---", "---",
        "DEC", "ENV", "DET", "LVL", "---", "---", "---", "---",
    };
    for (int i = 0; i < kParams; i++) {
        const char *name = va.param_name(i);
        CHECK_EQ(int(std::strlen(name)), 3);
        CHECK(std::strcmp(name, expected[i]) == 0);
        for (int c = 0; c < 3; c++) {
            const char ch = name[c];
            CHECK(ch == '-' || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
        }
    }
    CHECK(std::strcmp(va.param_name(-1), "---") == 0);
    CHECK(std::strcmp(va.param_name(kParams), "---") == 0);
}

TEST(vapoly_mode_zones_match_va_mono) {
    // The same knob mapping as the bass machine, because it is the same function: a chord
    // machine whose MOD knob meant something else would be a surprise, not a feature.
    CHECK(VaPoly::mode_from(0) == VaMode::lp24);
    CHECK(VaPoly::mode_from(63) == VaMode::lp24);
    CHECK(VaPoly::mode_from(64) == VaMode::lp12);   // the default kit's untouched knob
    CHECK(VaPoly::mode_from(95) == VaMode::lp12);
    CHECK(VaPoly::mode_from(96) == VaMode::bp12);
    CHECK(VaPoly::mode_from(111) == VaMode::bp12);
    CHECK(VaPoly::mode_from(112) == VaMode::hp12);
    CHECK(VaPoly::mode_from(127) == VaMode::hp12);
}

TEST(vapoly_is_safe_to_trigger_before_the_first_prepare) {
    // init() derives a usable patch from mid knobs, as every STRATA machine does, so a trig
    // arriving in the same block as the engine's first prepare() cannot read uninitialised
    // coefficients.
    VaPoly va;
    va.init(kRate);
    for (int i = 0; i < 6; i++) va.trigger(hit(uint8_t(60 + i), 100, false, 0));
    const int blocks = 8;
    clear(g_a, blocks * kBlk);
    for (int b = 0; b < blocks; b++) va.render_add(g_a + b * kBlk, kBlk);
    CHECK(finite_within(g_a, blocks * kBlk, kChordBound));
    CHECK(peak(g_a, blocks * kBlk) > 0.0f);
}

// --- release(note): KEYS mode letting a key go --------------------------------------------------

TEST(vapoly_releasing_a_note_is_its_gate_closing_on_that_frame) {
    // A key held for K blocks then let go must sound exactly like the same note carrying a gate of
    // K blocks: same release, same tail, from the same frame. And only that note of a chord.
    const TrackParams p = patch();
    constexpr int kHeld = 20;
    const int blocks = blocks_for(1.5f);
    const uint8_t notes[] = {60, 64, 67};

    VaPoly gated;
    gated.init(kRate);
    gated.prepare(p);
    for (uint8_t note : notes) {
        gated.trigger(hit(note, 100, false, 0, note == 64 ? uint32_t(kHeld * kBlk) : 0u));
    }
    clear(g_a, blocks * kBlk);
    const int n = render(gated, p, g_a, blocks);

    VaPoly held;
    held.init(kRate);
    held.prepare(p);
    for (uint8_t note : notes) held.trigger(hit(note, 100, false, 0, 0));
    clear(g_b, blocks * kBlk);
    render(held, p, g_b, kHeld);
    held.release(64);
    held.release(50);                                  // a note nothing holds: nothing happens
    render(held, p, g_b + kHeld * kBlk, blocks - kHeld);

    CHECK_EQ(first_difference(g_a, g_b, n), -1);
    CHECK(holds(held, 60));
    CHECK(holds(held, 67));
}

TEST(vapoly_a_note_released_before_its_frame_still_starts_and_is_let_go) {
    // Pressed and released inside one block: the note must not be lost and must not hang. It
    // starts on its frame and is released one frame later, exactly as a one-frame gate is.
    const TrackParams p = patch();
    const int blocks = blocks_for(1.0f);

    VaPoly gated;
    gated.init(kRate);
    gated.prepare(p);
    gated.trigger(hit(60, 100, false, 40, 1));
    clear(g_a, blocks * kBlk);
    const int n = render(gated, p, g_a, blocks);

    VaPoly tapped;
    tapped.init(kRate);
    tapped.prepare(p);
    tapped.trigger(hit(60, 100, false, 40, 0));
    tapped.release(60);
    clear(g_b, blocks * kBlk);
    render(tapped, p, g_b, blocks);

    CHECK_EQ(first_difference(g_a, g_b, n), -1);
    CHECK(peak(g_b, n) > 0.0f);
    CHECK(!tapped.active());
}
