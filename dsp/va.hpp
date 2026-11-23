// VA MONO: the bass voice, track 6 of the default techno kit.
//
// One monophonic virtual-analogue voice, built on the same primitives and in the same shape as
// Enaga's own `examples/synth` engine (two oscillators with hard sync, a sub, noise, a
// state-variable filter with a 24 dB cascade, ADSRs, MONO glide). That example is the measured
// reference the whole budget is built from -- 1,868 cycles per voice per block -- so following
// its structure is also how this file stays inside it.
//
// What the spec asks for (docs/SPEC.md, "Sound architecture"): "2 oscillators with sync, sub,
// noise, multimode filter, 2 envelopes, accent, slide, glide". All of it is here:
//
//   OSC 1   PolyBLEP saw or pulse (OSC page, WAV: 0-31 saw, 32 a square, up to 127 an 8%
//           pulse). The backbone.
//   OSC 2   Chosen on the OSC page's OS2: OFF; 1-63 a second saw DETUNED against OSC 1 (the
//           pair split around the pitch, up to 0.6 semitones apart: chorus, then Reese); 64-127
//           a PolyBLEP pulse HARD SYNCED to OSC 1, +0 to +24 semitones, the bright formant that
//           cannot drift out of tune. In sync mode an accent lifts it, so the accent brings the
//           bite. Off costs nothing: the old engine ran the synced pulse on every note.
//   SUB     Wave::triangle an octave down, its level the OSC page's SUB (0 = not rendered).
//           Triangle, not sine: Enaga's triangle is naive (piecewise linear, no PolyBLEP and
//           no polynomial) and so is a third the cost of its sine, while an octave below the
//           fundamental the difference is inaudible under the saw.
//   NOISE   A trace, lifted by an accent. Mostly removed again by the low-pass; it is there as
//           attack grit, not as a voice of its own.
//   DRIVE   The OSC page's DRV: a soft clip BEFORE the filter (strata_va_drive_filter), so the
//           resonance rides on a saturated, thicker source, plus more of the post-filter
//           drive. DRV 0 skips it and runs Enaga's own block filters.
//   FILTER  A multimode SVF in the VOICE (see below), swept by its own envelope: the acid
//           squelch. 24 dB uses Svf::cascade, which is two poles for about the price of one.
//           RES runs on an exponential curve to self-oscillation near 127, held in check by a
//           control-rate limiter on the resonant stage (va.cpp, "Resonance").
//   ENVS    Two: the amplitude envelope at the sample rate, and the filter envelope at the
//           control rate. One DEC knob shapes both, 303-style, with the filter's sweep shorter
//           than the amplitude's so the "wow" lands at the front of the note.
//
// **The voice filter is on the SRC page, not the AMP page.** docs/SPEC.md gives the insert
// chain its own multimode filter on the FLTR page already; that one is the track's utility
// filter. This one is the voice's *source character* -- the envelope sweep is the thing a
// listener calls "the bass" -- so MOD, CUT and RES sit next to TUN on SRC, where they are
// three adjacent encoders you can ride together, and the AMP page keeps the envelope's decay,
// how much of it reaches the filter, the glide and the level.
//
// Monophonic: one voice, so a new trig steals it. A stolen voice keeps its oscillator phases
// and its filter state and its amplitude envelope goes on from the level it is at, exactly as
// the reference does, because resetting any of those under a sounding note is a step in the
// output -- a click.
//
// It reads the SRC page (0-3), the AMP page (8-11), the ASR page and the OSC page (kOscFirst);
// FLTR and FX belong to the insert chain.
//
// --- the voice is shared with VA POLY -------------------------------------------------------
//
// docs/SPEC.md gives VA POLY "the same voice as VA MONO", six of them, and docs/BUDGET.md
// budgets it at six times the same measured figure. So the voice itself -- `VaVoice` below,
// with the control-rate block it reads, `VaSettings` -- is ONE implementation that both
// machines hold: VA MONO has one and VA POLY has six. dsp/vapoly.hpp's header records why that
// is shared here rather than duplicated, which is not the call core/project.cpp makes for its
// container code; the short version is that a spec which pins the two voices to be identical
// makes a drift between two copies a bug rather than the independence that buys.
//
// What stayed in the machines, because it is not voice arithmetic: the parameter mapping of
// slot 10 (GLD here, DET there), the pending-trig bookkeeping, voice allocation, and glide and
// slide, which docs/SPEC.md and dsp/track.hpp both make MONO-only.
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "track.hpp"
#include "waveloops.h"

namespace strata {

// The voice filter's four settings. LP24 is a cascade of two poles (Svf::cascade); the other
// three are one pole each.
enum class VaMode : uint8_t { lp24 = 0, lp12, bp12, hp12 };

// MOD's zones. Deliberately unequal: see va.cpp.
VaMode va_mode_from(uint8_t value);

// The ASR page's knob curves (va.cpp has the table), for the screen as well as for `make`.
// 0 on every one of them is the envelope the VAs had before the page existed.
float va_attack_seconds(uint8_t value);    // ATK: 2.5 ms at 0, 5 s at 127
float va_release_seconds(uint8_t value);   // REL: 60 ms at 0, 8 s at 127

// The OSC page (va.cpp has the table). OS2's three zones, and the knob values the screen shows.
enum class VaOsc2 : uint8_t { off = 0, detune, sync };
VaOsc2 va_osc2_mode(uint8_t value);
float va_osc2_detune_cents(uint8_t value);   // the spread between the two in DET's zone
float va_osc2_sync_semitones(uint8_t value); // OSC 2 above OSC 1 in SYN's zone
int va_pulse_percent(uint8_t value);         // WAV's pulse width, 0 for the saw
// RES as the resonant stage's damping k (= 1/Q): 2 at 0, about 0.24 at 64, just below 0 (it
// self-oscillates, held to a level by the control-rate resonance limiter in va.cpp's run())
// over the last few counts.
float va_resonance_k(uint8_t value);

// The WAVE pair (VA MONO and VA POLY with a wavetable where the two oscillators were;
// dsp/wavetable.hpp has the tables). Their knobs, for the screen as well as for `make_wave`.
int va_wave_table(uint8_t value);            // TBL: 0-15, eight counts each
float va_wave_position(uint8_t value);       // POS: the frame, 0-15, fractional
float va_wave_motion(uint8_t value);         // MOT: frames the filter envelope adds, -15 to +15
float va_wave_detune_cents(uint8_t value);   // DET: the spread between the two readers, 0 is OFF

// --- the control-rate block -----------------------------------------------

// Everything the parameters decide, made once per block by `make` and read by every voice that
// renders. One struct rather than a set of members on the machine because six voices share one
// copy of it: VA POLY makes these once per block, not once per voice, so the four
// EnvelopeShape::make calls (twelve exp() between them, since accent has shapes of its own) and
// the knob curves cost the same at six voices as at one.
struct VaSettings {
    // Reads SRC 0-3 (TUN, MOD, CUT, RES), AMP 8, 9 and 11 (DEC, ENV, LVL) and the ASR page
    // (ATK SUS REL FSU). Slot 10 is deliberately NOT read here -- it is GLD on VA MONO and DET
    // on VA POLY -- so each machine maps that one itself. Control rate, once per block.
    static VaSettings make(const TrackParams &params, float rate, float inv_rate);
    // The WAVE pair's: SRC TBL POS CUT RES (the filter fixed at LP24, no TUN), AMP DEC ENV MOT
    // LVL, OSC WRP DET SUB and a fourth slot that is GLD on WAVE MONO (`mono`: the machine maps
    // it, so it is not drive) and DRV on WAVE POLY. The filter, the envelopes, the sub and the
    // levels are the VA's own, from `make`.
    static VaSettings make_wave(const TrackParams &params, float rate, float inv_rate, bool mono);

    enaga::dsp::EnvelopeShape amp_shape{}, amp_shape_accent{};
    enaga::dsp::EnvelopeShape fenv_shape{}, fenv_shape_accent{};
    VaMode mode = VaMode::lp12;
    // The OSC page: OSC 1's wave (saw, or a pulse of `width`), what OSC 2 does, and the two
    // oscillators' frequencies as ratios of the note's (a detune splits them; sync puts OSC 2
    // above). The sub is always an octave under the note itself.
    enaga::dsp::Wave wave = enaga::dsp::Wave::saw;
    float width = 0.5f;
    VaOsc2 osc2 = VaOsc2::off;
    float ratio1 = 1.0f;
    float ratio2 = 1.0f;
    float pre_drive = 0.0f;            // DRV: the gain into the filter's input clip, 0 for none
    float inv_pre_drive = 0.0f;        // and back out of it
    float rate = 48000.0f;
    float inv_rate = 1.0f / 48000.0f;
    float tune = 0.0f;                 // TUN, semitones
    float cutoff_hz = 650.0f;          // CUT
    float k_res = 1.0f;                // RES as the resonant stage's k = 1 / Q
    float env_semis = 0.0f;            // ENV, semitones of cutoff at envelope 1
    float decay_s = 0.2f;              // DEC
    float attack_s = 0.0025f;          // ATK
    float release_s = 0.060f;          // REL
    float sustain = 0.0f;              // SUS, 0-1 of the peak: 0 is a one-shot, DEC its length
    float filter_sustain = 0.0f;       // FSU, 0-1 of the filter envelope's sweep
    float level_osc1 = 0.0f;           // the oscillator levels, resonance makeup included
    float level_osc2 = 0.0f;
    float level_sub = 0.0f;
    float level_noise = 0.0f;
    float accent_osc2 = 0.0f;          // what an accent adds to those two
    float accent_noise = 0.0f;
    float drive = 1.0f;                // the post-filter soft clip's drive
    float out_gain = 0.25f;            // LVL

    // The wavetable, on the WAVE pair: `table` -1 is the two VA oscillators instead. OSC 2's
    // detune (osc2, ratio1, ratio2, level_osc2) is read as a second reader of the same frames.
    int table = -1;
    float position = 0.0f;             // POS, in frames
    float motion = 0.0f;               // MOT: frames added at filter envelope 1
    bool warped = false;               // WRP above 0
    strata_wave_warp warp{0.5f, 1.0f, 1.0f};

    // The pitch slew per control step, as a one-pole coefficient; 1 means arrive at once. The
    // MACHINE sets these, because glide is monophonic: VA MONO from GLD and from its slide
    // floor, VA POLY not at all -- a chord has nothing to glide from, so 1 is what it wants.
    float glide_coef = 1.0f;
    float slide_coef = 1.0f;
};

// --- one voice ------------------------------------------------------------

// Two oscillators, the sub, the noise, the filter and the two envelopes: everything that is per
// NOTE rather than per block. VA MONO holds one of these and VA POLY six, and neither machine
// contains any of the voice's arithmetic itself.
//
// Starting a note is a few small calls rather than one `start(...)`, because the two machines
// want different combinations of them and one call would have taken five booleans in an order
// no compiler would check. Both machines do this:
//
//     if (!v.sounding()) v.reset_phases();   // ONLY when silent: resetting under a note clicks
//     v.set_note(velocity, accent, slide);
//     v.set_target(note + tune);             // where the pitch is heading ...
//     v.set_pitch(target);                   // ... and, unless it should glide there, from
//     v.gate_on();

// Filter coefficients shared between the voices of ONE machine, within one block.
//
// Why it is possible at all: the voice's filter has NO KEY TRACKING, which va.cpp argues for on
// musical grounds -- "a bass line wants the same squelch at every note of a pattern". So `g` and
// `k` depend on the settings, on the filter envelope's value and on the accent, and NOT on the
// note. The voices of a chord are gated on the same frame with the same shapes, so their filter
// envelopes run bit-identically and all six were computing the same `semitones` and the same
// `tan_pi` -- the two most expensive things in the control step -- six times over.
//
// Keyed on the envelope value by EXACT EQUALITY, which is what makes a hit bit-exact rather than
// merely close: identical inputs, identical outputs, so a shared coefficient is the same number
// the voice would have computed. A float key is right here for the same reason it is usually
// wrong -- the question being asked is "are these the same bits", not "are these about equal".
//
// The settings are the other half of the key, and they are not stored: `VaPoly::prepare` clears
// the cache, which is the one thing that must happen for a stale entry to be impossible.
// docs/BUDGET.md has what it saves.
struct VaFilterCache {
    // Four control slices to a block, and a gate closing mid-slice can cut one more, so eight is
    // one block's worth with room to spare. A full cache simply stops storing: a miss costs what
    // it always cost.
    static constexpr int kEntries = 8;

    struct Entry {
        float fenv;
        float g;
        float k;
        bool accent;
    };

    Entry entries[kEntries] = {};
    int count = 0;

    void clear() { count = 0; }

    // The entry for this envelope value and accent, or -1. A linear scan of at most eight float
    // compares against an exp2 and a tangent, so the scan is the cheap side by a wide margin.
    int find(float fenv, bool accent) const {
        for (int i = 0; i < count; i++) {
            if (entries[i].fenv == fenv && entries[i].accent == accent) return i;
        }
        return -1;
    }

    void add(float fenv, bool accent, float g, float k) {
        if (count >= kEntries) return;
        entries[count].fenv = fenv;
        entries[count].accent = accent;
        entries[count].g = g;
        entries[count].k = k;
        count++;
    }
};

class VaVoice {
public:
    // Frames per control step. 16 is the reference's figure: modulation at 3 kHz, which is fast
    // enough that a filter sweep does not step audibly, and slow enough that the tangent and
    // the reciprocals behind a set of SVF coefficients cost 1/16 of what they would per sample.
    // A trig landing mid-step shortens one slice so a note still starts on its exact frame.
    static constexpr int kControl = 16;

    // The level below which the amplitude envelope counts as finished. An Envelope with
    // sustain 0 parks in the `sustain` stage rather than reaching `idle` on its decay path
    // (docs/ENAGA_API.md, "Gotchas"), so the level is what says a one-shot note is over.
    static constexpr float kSilent = 1e-4f;

    void reset();          // everything: phases, filters, envelopes, and the pitch to note 60
    void reset_phases();   // the oscillators, the filter state and the sync carry
    void kill();           // silence now: the envelopes and the filter state, no tail

    // Each voice of a polyphonic machine needs its own noise stream. Six voices sharing one
    // seed would add six copies of the same signal coherently -- six times one voice's noise
    // instead of the root of six, and correlated rather than noise.
    void seed(uint32_t value) { rng_ = enaga::dsp::Random(value); }

    void set_note(float velocity, bool accent, bool slide);
    void set_pitch(float note) { pitch_ = note; }
    // Tests: drop the cached oscillator increments, so the next control step computes them.
    // A voice copied and then made to forget must render the same bits as the original.
    void forget_increments() { osc_inv_rate_ = 0.0f; }
    void set_target(float note) { target_ = note; }
    void gate_on();
    void gate_off() {
        amp_.gate_off();
        fenv_.gate_off();   // so the filter closes as the level falls: no bright ghost
    }
    // Let go of already: the amplitude envelope is in its release tail.
    bool released() const { return amp_.stage() == enaga::dsp::Envelope::release; }

    // Is this voice making sound? The LEVEL decides, not `Envelope::active()`, which stays true
    // forever on a sustain-0 decay path (kSilent above).
    bool sounding() const {
        const enaga::dsp::Envelope::Stage s = amp_.stage();
        return s != enaga::dsp::Envelope::idle &&
               (s == enaga::dsp::Envelope::attack || amp_.level() >= kSilent);
    }

    float amp_level() const { return amp_.level(); }

    // Fade out over kChokeFrames and stop: how the shared voice budget takes a voice from
    // another track (va.cpp says why not a kill or a release). A no-op on a silent voice or one
    // already fading. gate_on() cancels it, so a choked voice can be taken for a new note.
    static constexpr int kChokeFrames = 64;
    void choke();
    bool choking() const { return choke_left_ > 0; }

    float pitch() const { return pitch_; }
    float target_pitch() const { return target_; }
    bool accent() const { return accent_; }

    // One control step and its n samples (n must be 1..kControl), ADDED into out. `work` and
    // `env` are kControl-float scratch buffers the CALLER owns and every voice shares, so six
    // voices of slice never reach the audio stack.
    //
    // `shared`, when given, is the filter-coefficient cache above: the voices of one machine
    // pass the same one, and a hit skips the control step's `semitones` and `tan_pi`. A caller
    // with one voice passes nothing, since there is nobody to share with.
    void run(const VaSettings &settings, float *out, int n, float *work, float *env,
             VaFilterCache *shared = nullptr);

private:
    enaga::dsp::Oscillator osc1_, osc2_, sub_;
    enaga::dsp::Svf svf_a_, svf_b_;     // svf_a_ is LP24's first (non-resonant) stage
    enaga::dsp::Envelope amp_, fenv_;
    enaga::dsp::Random rng_{0x1F3B9A7Du};

    float pitch_ = 60.0f;               // the glide's current value, in note numbers
    float target_ = 60.0f;              // where it is heading
    float sync_carry_ = 0.0f;           // enaga_oscillator_sync_add's sample in hand
    float gain_ = 0.0f;                 // the output gain the last slice ended on
    float velocity_ = 1.0f;             // this note's velocity, 0-1
    bool accent_ = false;               // this note's accent flag
    bool slide_ = false;                // this note arrived on a slid step

    // The oscillators' increments and their reciprocals, as of the last control step, and the
    // pitch and rate they came from. A held note does not glide -- VA POLY never does, VA MONO
    // only while GLD is moving it -- so most control steps would recompute a semitones() and two
    // reciprocals into the bits already here. Keyed by exact float equality on both inputs, so a
    // hit is bit-exact by construction (va.cpp, run()).
    float osc_pitch_ = 0.0f;
    float osc_inv_rate_ = 0.0f;         // 0 never matches a real rate: the first step computes
    float osc_ratio1_ = 0.0f, osc_ratio2_ = 0.0f;
    float dt0_ = 0.0f, inv0_ = 0.0f;    // the note itself: the sub's, at half
    float dt1_ = 0.0f, inv1_ = 0.0f, dt2_ = 0.0f, inv2_ = 0.0f;

    // reciprocal(float(n)) for the slice length the last slice had: n is kControl on all but
    // the slices a trig or a gate cuts short.
    int ramp_n_ = 0;
    float inv_n_ = 0.0f;

    int choke_left_ = 0;                // frames of the choke fade still to run, 0 when none
    float choke_step_ = 0.0f;           // the output gain it takes off per frame

    // The wavetable readers' phases, and the position the last slice ended on, so a moving
    // position ramps across the slice instead of stepping (-1: a new note, no ramp).
    void render_wave(const VaSettings &s, float *work, int n, float fenv, float level2);
    uint32_t wave_phase1_ = 0, wave_phase2_ = 0;
    float wave_pos_ = -1.0f;
};

// --- the machine ----------------------------------------------------------

class VaMono final : public Machine {
public:
    // The enum moved to namespace scope so VA POLY shares it; VaMono::Mode still names it.
    using Mode = VaMode;

    void init(float sample_rate) override;
    void prepare(const TrackParams &params) override;
    void trigger(const Trig &trig) override;
    void release_all() override;
    void release(uint8_t note) override;
    void kill_all() override;
    void render_add(float *out, int frames) override;
    bool active() const override { return active_; }
    const char *param_name(int index) const override;

    // --- for the UI, and for tests that need to pin a mapping rather than guess at it -------

    // MOD's zones. Deliberately unequal: see va.cpp.
    static Mode mode_from(uint8_t value) { return va_mode_from(value); }
    Mode mode() const { return settings_.mode; }
    // CUT in Hz, before the filter envelope and the accent move it.
    float base_cutoff_hz() const { return settings_.cutoff_hz; }
    // The glide's current value and where it is heading, as note numbers (TUN included).
    float pitch() const { return voice_.pitch(); }
    float target_pitch() const { return voice_.target_pitch(); }
    // The amplitude envelope's level: what a slide must not reset.
    float amp_level() const { return voice_.amp_level(); }
    float decay_seconds() const { return settings_.decay_s; }
    float glide_seconds() const { return glide_s_; }
    // SUS as the envelope holds it, 0-1: what the ASR page's prepare left.
    float sustain_level() const { return settings_.sustain; }

    // WAVE MONO: the same machine with the wavetable (make_wave), GLD on the OSC page. Set
    // before init(); the engine's pool holds one of each.
    void set_wave(bool wave) { wave_ = wave; }
    bool wave() const { return wave_; }
    const VaSettings &settings() const { return settings_; }

private:
    // A slide is always audible: below this it would be a jump, not a slide. The 303's own
    // slide time is fixed at about this, and GLD only ever makes a slide longer.
    static constexpr float kSlideFloorS = 0.055f;

    void start();                       // begin the pending trig, on its exact frame

    VaVoice voice_;
    VaSettings settings_{};

    float rate_ = 48000.0f;
    float inv_rate_ = 1.0f / 48000.0f;
    float control_rate_ = 3000.0f;

    float glide_s_ = 0.0f;              // GLD, for the accessor
    bool wave_ = false;                 // WAVE MONO
    bool first_note_ = true;            // nothing to glide from yet
    bool active_ = false;

    Trig next_{};                       // the pending trig and the frames until it starts
    int wait_ = 0;
    bool pending_ = false;
    bool release_on_start_ = false;     // released before its frame: let go as soon as it starts

    // Frames of gate left before the sounding note is released: `Trig::gate_frames`, the
    // step's LEN, as VA POLY's slots count it (vapoly.hpp). -1 is no gate. Counted only when SUS
    // is up: at SUS 0 this machine is the one-shot bass it always was, DEC its note length, and
    // a LEN of one step cutting a long decay short would change every existing bass line.
    int gate_ = -1;
    // The sounding note has no gate and no release coming (Trig::held): a one-shot. At SUS 0
    // its envelope ends it; if SUS is raised while it sounds it would hold for ever, so the
    // render loop lets it go the moment the settings sustain.
    bool one_shot_ = false;

    float work_[VaVoice::kControl] = {};    // the slice being built
    float env_[VaVoice::kControl] = {};
};

}  // namespace strata
