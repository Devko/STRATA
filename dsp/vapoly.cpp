// VA POLY, the chord voice. vapoly.hpp has the topology, the share-or-duplicate decision and
// the voice allocation rules; dsp/va.cpp has the voice itself and what dominates inside it.
//
// --- what is expected to dominate -----------------------------------------------------------
//
// Six voices of dsp/va.cpp's `VaVoice`, so the per-voice accounting is that file's and is not
// repeated here. What this file adds on top of six voices is small and all of it is control
// rate or per block:
//
//   stage                                                       cost
//   six voices of VaVoice                           6 x ~1,660 cycles/block (dsp/va.cpp's
//                                                   derivation), so about 9,960 cycles =
//                                                   2.0% of the 496,000-cycle block
//   prepare: one VaSettings::make                   ONCE a block, not once a voice: the four
//                                                   EnvelopeShape::make and the knob curves
//                                                   are shared, so this is the same ~2.5
//                                                   flops/sample VA MONO pays
//   allocate: at most three passes over 6 slots     per TRIG, so at most a few hundred cycles
//                                                   on a step, and nothing between steps
//   render_add: six busy tests                      one envelope stage read and one level
//                                                   compare each; a silent machine returns on
//                                                   `active_` alone
//
// So about **2.0% of the block with all six voices sounding**, against docs/SPEC.md's budget of
// 6 x 1,868 = 11,208 cycles (2.26%) and docs/BUDGET.md's expectation that replacing the mono
// stand-in on track 7 adds "about 1.9 points". That is a derivation, not a measurement: see the
// note at the end of this comment.
//
// Three things about the shape of it are worth knowing before anyone tunes it:
//
// **The six voices share one 16-float slice.** Each renders into `work_` and mixes out of it
// before the next one runs, so the working set is one cache line's worth however many voices
// sound, and six voices of scratch never reach the audio stack (CLAUDE.md caps a frame at
// 16 KiB; six voices of state live in the machine, allocated with it).
//
// **The control-rate work is the part that scales worst.** Each voice does its own
// `semitones`, `tan_pi` and three `reciprocal` calls per 16 frames, because each voice has its
// own pitch and therefore its own coefficients -- six times VA MONO's ~4.5 flops/sample. The
// library has the answer to exactly this: `enaga_tan_pi_buffer`, `enaga_reciprocal_buffer` and
// `enaga_svf_coefficients_buffer` take an array of per-voice control values, and
// <enaga/dsp.h> says in as many words that a short buffer should be padded because "8 voices'
// values plus 8 spare cost less than 8". Taking it means splitting `VaVoice::run` into a
// control pass and a render pass so all six voices' values can be gathered first. That is the
// optimization to reach for if this machine ever needs one, and it is deliberately NOT taken
// here: it would restructure the voice VA MONO shares, for a saving nothing has measured yet.
//
// **The cheap win above it is the one dsp/va.cpp already names**: a fused `soft clip ->
// multiply by the envelope -> ramped add into the bus` pass in a *.ti.c file, which is worth
// six times here what it is worth there.
//
// **None of these figures is measured.** There is no load scenario for a track hosting this
// machine yet (`app/` wires the machines and this agent does not touch it), so the 2.0% above
// is six times dsp/va.cpp's own derivation and carries all of its assumptions -- ~2.5 flops per
// cycle on this core, and cl6x compiling Enaga's block functions. CLAUDE.md requires the load
// scenario and docs/BUDGET.md figures with and without cl6x after a DSP change; that has to
// happen when the machine is registered on a track, with the real numbers replacing these.
#include "vapoly.hpp"

namespace strata {
namespace {

// The longest gate a slot will count, in frames: ten seconds at 48 kHz. The table's longest
// entry is 64 steps, which at 20 BPM is over three minutes, and a counter that large is a note
// nobody is waiting for -- but the clamp exists so the countdown cannot be handed something
// absurd by a tempo read before the clock has settled.
constexpr int kGateMax = 480000;


using enaga::dsp::clamp;

// --- DET ------------------------------------------------------------------------------------
//
// The voice slots are detuned against each other, which is what thickens a chord: it is the
// voice-to-voice tuning error of an analogue polysynth, and the part of docs/SPEC.md's "spread"
// that survives a mono output bus (vapoly.hpp says why stereo spread is not here).
//
// A quarter of a semitone at DET 127. That is wide enough to beat audibly on a held chord
// (25 cents is about 1.9 Hz of beating on a 130 Hz root) and narrow enough that a melodic line
// still plays in tune.
constexpr float kDetuneSemis = 0.25f;

// Per-slot offsets, in units of DET. Slot 0 is exactly in tune, because the allocator hands out
// free voices in order and so a single note -- a melody on this machine, or the root of a
// chord -- always lands there. The pairs beyond it are symmetric so a full six-voice chord's
// mean pitch moves by 0.05 of DET, about 1 cent: a thicker chord, not a sharper one.
constexpr float kDetune[6] = {0.0f, 1.0f, -1.0f, 0.6f, -0.6f, 0.3f};
static_assert(int(sizeof(kDetune) / sizeof(kDetune[0])) == VaPoly::kVoices,
              "one detune offset per voice slot");

// A distinct noise seed per voice. Six voices sharing one seed would add six copies of the same
// xorshift stream in lockstep: six times one voice's noise instead of the root of six, and a
// correlated signal rather than noise. These are the odd 32-bit constants VA MONO's seed came
// from the same way -- any distinct non-zero values do.
constexpr uint32_t kSeeds[6] = {0x1F3B9A7Du, 0x2D9A4C1Bu, 0x3BF17E05u,
                                0x51C6A93Fu, 0x6E2D5B11u, 0x7A4F18C9u};
static_assert(int(sizeof(kSeeds) / sizeof(kSeeds[0])) == VaPoly::kVoices,
              "one noise seed per voice slot");

// A trig's frame, clamped so a silly value cannot leave a voice waiting for ever and holding
// itself busy. The scheduler only ever sends a frame inside the block.
constexpr uint32_t kWaitMax = 65535u;

}  // namespace

// --- parameters -----------------------------------------------------------
//
// VA MONO's eight, with one slot spent differently. The seven that are shared are shared
// exactly -- same curves, same code (dsp/va.cpp's `VaSettings::make`), because a chord machine
// whose CUT knob did not mean what the bass machine's CUT knob means would be a surprise, not a
// feature.
//
// SRC -- the voice: its tuning and its filter, which is this machine's source character.
//   0  TUN  tune, (knob - 64) * 24/64 semitones: +-24, exactly 0 at 64.
//   1  MOD  voice filter mode in four unequal zones: LP24 0-63, LP12 64-95, BP12 96-111,
//           HP12 112-127. Per voice, so a chord's notes each get their own filter.
//   2  CUT  base cutoff, 35 Hz to 12 kHz exponential, about 650 Hz at 64.
//   3  RES  resonance, 0 to 0.93 (0.84 on LP24). Six resonant filters at once is a lot of
//           level, which is why every voice carries VA MONO's post-filter soft clip: the
//           machine stays bounded per voice rather than relying on the master limiter.
// AMP -- the envelopes and the output.
//   8  DEC  decay, 15 ms to 2.4 s exponential. One knob for both envelopes, and with no
//           note-off in the Machine interface it is also the chord's length: short for a stab,
//           long for a pad that rings into the next step.
//   9  ENV  how much of the filter envelope reaches the cutoff, 0 to +66 semitones.
//  10  DET  voice detune, 0 to a quarter of a semitone (see kDetune above). This is the slot
//           VA MONO spends on GLD, which is monophonic; a chord has nothing to glide from.
//  11  LVL  output level, square law. Calibrated per voice as VA MONO's is, so a six-note
//           chord is louder than one note -- as a chord should be. The track fader and the
//           master limiter are what hold a dense pattern down, not a hidden voice scaling,
//           which would make a chord quieter than the note it was built from.

void VaPoly::init(float sample_rate) {
    rate_ = sample_rate > 1.0f ? sample_rate : 48000.0f;
    inv_rate_ = enaga::dsp::reciprocal(rate_);

    for (int i = 0; i < kVoices; i++) {
        Slot &s = slots_[i];
        s.voice.reset();
        s.voice.seed(kSeeds[i]);
        s.next = Trig{};
        s.wait = 0;
        s.gate = -1;
        s.pending = false;
        s.note = -1;
    }
    steal_cursor_ = 0;
    detune_ = 0.0f;
    active_ = false;

    // A machine must be safe to trigger before the first prepare(), so derive from mid values.
    TrackParams defaults{};
    for (int i = 0; i < kParams; i++) defaults.set(i, 64);
    prepare(defaults);
}

void VaPoly::prepare(const TrackParams &params) {
    // Once a block for all six voices, which is the whole reason the control-rate values are a
    // struct and not a set of members (va.hpp).
    settings_ = VaSettings::make(params, rate_, inv_rate_);

    // Everything in the filter cache was derived from the settings that have just been replaced,
    // so it goes with them. This is the ONE thing that has to happen for a stale coefficient to
    // be impossible, which is why it sits on the line after the settings rather than anywhere
    // more convenient (dsp/va.hpp, VaFilterCache).
    filter_cache_.clear();

    // Slot 10 is DET here, so VaSettings::make leaves it alone. The glide coefficients stay at
    // their default of 1 -- arrive at once -- which is what a machine with no glide wants.
    detune_ = kDetuneSemis * params.unit(10);
}

void VaPoly::trigger(const Trig &trig) {
    const int pick = allocate(trig.note);
    Slot &s = slots_[pick];

    // The slot is committed to the new note from here, before its frame arrives: that is what
    // makes the rest of a chord land on other voices (vapoly.hpp, "voice allocation").
    s.note = int(trig.note);
    s.next = trig;
    s.wait = int(trig.frame > kWaitMax ? kWaitMax : trig.frame);
    // The gate starts counting when the note STARTS, not when the trig was taken, so a note
    // scheduled late in the block still sounds for its full length.
    //
    // 0 could be stored as 0 -- the countdown asks `gate > 0`, so it would be just as inert --
    // but the header states the invariant as "-1 means no gate" and init() and the countdown's
    // own reset both write -1. One spelling for one meaning, so a reader never has to work out
    // whether a 0 in a debugger is a length or the absence of one.
    s.gate = trig.gate_frames == 0 ? -1 : int(trig.gate_frames > kGateMax ? kGateMax
                                                                         : trig.gate_frames);
    s.pending = true;
    active_ = true;
}

void VaPoly::release(uint8_t note) {
    for (Slot &s : slots_) {
        if (s.note != int(note)) continue;
        if (s.pending) {
            // Not started yet: a one-frame gate, so it starts and is let go on the next frame
            // through the same path as a step's length.
            s.gate = 1;
        } else if (s.voice.sounding()) {
            s.voice.gate_off();
            s.gate = -1;
        }
    }
}

int VaPoly::allocate(uint8_t note) {
    // 1. The voice already sounding this note, if it has started. A repeated chord reuses its
    //    own voices; a waiting voice is not a match, so a ratchet keeps both of its hits.
    for (int i = 0; i < kVoices; i++) {
        const Slot &s = slots_[i];
        if (!s.pending && s.note == int(note) && s.voice.sounding()) return i;
    }

    // 2. A free voice: nothing pending, nothing sounding. In slot order, so a single note takes
    //    slot 0 and plays in tune whatever DET says.
    for (int i = 0; i < kVoices; i++) {
        if (!slots_[i].busy()) return i;
    }

    // 3. The quietest voice that has started, which is the one furthest through its decay. The
    //    scan starts at a rotating cursor so voices held at the same level -- a long DEC, the
    //    usual chord setting -- are stolen in turn rather than always the first one. kVoices is
    //    6, so the cursor wraps with a compare: the C674x has no integer divide instruction, so
    //    a `%` would be a libgcc call even at control rate.
    int pick = -1;
    float lowest = 0.0f;
    int i = steal_cursor_;
    for (int n = 0; n < kVoices; n++) {
        if (!slots_[i].pending) {
            const float level = slots_[i].voice.amp_level();
            if (pick < 0 || level < lowest) {
                pick = i;
                lowest = level;
            }
        }
        if (++i >= kVoices) i = 0;
    }

    // 4. Every voice is committed to a note that has not started: more notes on one frame than
    //    there are voices. One of them has to go, and the cursor decides which.
    if (pick < 0) pick = steal_cursor_;

    steal_cursor_ = pick + 1 >= kVoices ? 0 : pick + 1;
    return pick;
}

bool VaPoly::would_add_voice(uint8_t note) const {
    for (const Slot &s : slots_) {
        if (!s.pending && s.note == int(note) && s.voice.sounding() && !s.voice.choking()) {
            return false;                    // allocate() retriggers this voice in place
        }
    }
    for (const Slot &s : slots_) {
        if (!s.busy()) return true;          // a free slot: one more voice sounding
    }
    // Every slot busy: allocate() steals one of this machine's own. Stealing a choking slot
    // brings a voice back into the count; any other steal is one out and one in.
    for (const Slot &s : slots_) {
        if (s.voice.choking()) return true;
    }
    return false;
}

int VaPoly::quietest_voice(float &level) const {
    int pick = -1;
    for (int i = 0; i < kVoices; i++) {
        const Slot &s = slots_[i];
        if (s.pending || !s.voice.sounding() || s.voice.choking()) continue;
        const float l = s.voice.amp_level();
        if (pick < 0 || l < level) {
            pick = i;
            level = l;
        }
    }
    return pick;
}

void VaPoly::release_all() {
    // STOP once: every sounding voice gets its release tail, filter envelope included so no
    // bright ghost is left behind. A pending trig still starts -- it was scheduled before the
    // stop, which is dsp/sample.cpp's rule for the same situation.
    for (Slot &s : slots_) s.voice.gate_off();
}

void VaPoly::kill_all() {
    for (Slot &s : slots_) {
        s.pending = false;
        s.wait = 0;
        s.note = -1;
        s.voice.kill();
    }
    active_ = false;
}

void VaPoly::render_add(float *out, int frames) {
    if (!active_) return;   // nothing sounding and nothing pending: an idle machine costs a test

    // Once a block, here as well as in prepare(). The entries are keyed on this block's filter
    // envelope values, so a cache carried across blocks fills with last block's keys and stops
    // taking new ones -- still correct, but every voice would miss. prepare() used to be the
    // only clear, and it ran every block; app/engine.cpp now skips it when the parameters have
    // not moved, which leaves the settings, and so every entry, valid but the cache full.
    filter_cache_.clear();

    bool busy = false;
    for (int i = 0; i < kVoices; i++) {
        if (!slots_[i].busy()) continue;   // an idle voice costs one envelope read and a compare
        render_slot(i, out, frames);
        busy = busy || slots_[i].busy();   // ... and it may have finished inside this block
    }
    active_ = busy;
}

const char *VaPoly::param_name(int index) const {
    static const char *const names[kParams] = {
        "TUN", "MOD", "CUT", "RES",      // SRC: the voice and its filter
        "---", "---", "---", "---",      // FLTR: the insert chain's
        "DEC", "ENV", "DET", "LVL",      // AMP: the envelopes, the detune and the output
        "---", "---", "---", "---",      // FX: the insert chain's
    };
    return index >= 0 && index < kParams ? names[index] : "---";
}

// --- the voices -----------------------------------------------------------

void VaPoly::render_slot(int slot, float *out, int frames) {
    Slot &s = slots_[slot];
    // VA MONO's loop, per voice: control steps of at most kControl frames, cut short at the
    // pending trig's frame so a note still starts on its exact sample. Each voice walks the
    // whole block on its own, so six voices starting on six different frames need no
    // block-wide slicing -- and the voices ADD into the caller's buffer, which is never
    // cleared here.
    int i = 0;
    while (i < frames) {
        if (s.pending && s.wait <= 0) {
            start(slot);
            continue;
        }
        int n = frames - i;
        if (n > VaVoice::kControl) n = VaVoice::kControl;
        if (s.pending && s.wait < n) n = s.wait;   // cut the slice short at the trig's frame
        // And short at the gate's end, so a note is released on its exact frame rather than at
        // the next control step -- the same reason the trig's frame cuts the slice.
        //
        // `!s.pending` here is a COST guard, not an audible one, and the difference matters to
        // anyone tempted to delete it or to write a test for it. A slot whose trig is still
        // waiting is rendering the note it is about to replace, and the gate belongs to the new
        // note, so counting it yet would be wrong -- but re-slicing a voice that has already
        // started is bit-transparent (measured: only the slice that STARTS a note is sensitive
        // to its length, because that is where gate_on and the envelope's first step land). So
        // without this guard a one-frame gate on a waiting trig would render the sounding note a
        // frame at a time -- sixteen times the control-rate work for an identical result.
        // tests/test_vapoly.cpp's steal test pins the audible half; this comment is the rest.
        if (!s.pending && s.gate > 0 && s.gate < n) n = s.gate;
        s.voice.run(settings_, out + i, n, work_, env_, &filter_cache_);
        if (s.pending) s.wait -= n;
        if (!s.pending && s.gate > 0) {
            s.gate -= n;
            if (s.gate <= 0) {
                // The gate closed: release, and stop counting. The tail is the envelope's
                // release, so the note fades rather than cutting -- `kill()` would click.
                s.voice.gate_off();
                s.gate = -1;
            }
        }
        i += n;
    }
}

void VaPoly::start(int slot) {
    Slot &s = slots_[slot];
    const Trig t = s.next;
    s.pending = false;
    s.wait = 0;
    s.note = int(t.note);

    // A voice taken while it is still sounding keeps its oscillator phases, its filter state
    // and its amplitude envelope's level, exactly as VA MONO's does: resetting any of them
    // under a sounding note puts a step in the output, which is a click. A silent voice starts
    // from a clean one, so a chord's notes do not inherit a stale filter.
    if (!s.voice.sounding()) s.voice.reset_phases();

    // No glide and no slide (vapoly.hpp): the pitch starts where it is heading. TUN transposes
    // every voice and DET detunes the slots against each other.
    //
    // `set_pitch` is redundant as long as this machine leaves `glide_coef` at 1, because the
    // voice's first control step would arrive at the target before it rendered a sample: a
    // mutation that drops this line changes no output, and mutation testing found exactly that.
    // It stays because "a chord note starts at its pitch" should be this file's statement, not
    // a consequence of a default in a struct a future glide knob might change.
    const float pitch =
        clamp(float(t.note) + settings_.tune + detune_ * kDetune[slot], 0.0f, 132.0f);
    s.voice.set_note(float(t.velocity) * (1.0f / 127.0f), t.accent, false);
    s.voice.set_target(pitch);
    s.voice.set_pitch(pitch);
    s.voice.gate_on();
}

}  // namespace strata
