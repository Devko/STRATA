// The contract between a track's machine and its insert chain.
//
// This is the seam every machine and the chain are written against, so it is deliberately small
// and fixed. docs/SPEC.md, "Sound architecture" and "Parameters", is the authority for the
// shape; what follows pins down the parts that code has to agree on.
//
// A track owns 16 parameters on 4 pages of 4, mapped to encoders A-D, stored as 0-127 so that
// parameter locks stay one byte each. The pages split the work cleanly:
//
//     page 0  SRC   0-3    the MACHINE's sound source
//     page 1  FLTR  4-7    the insert chain's filter
//     page 2  AMP   8-11   the MACHINE's amplitude envelope and level
//     page 3  FX   12-15   the insert chain's drive, lo-fi, pan and sends
//
// So a machine reads SRC and AMP, the chain reads FLTR and FX, and neither needs the other's
// parameters. That is what lets machines and the chain be developed independently.
//
// Machines are MONO, all of them. Pan and the stereo spread happen in the insert chain, which
// halves the filter work and keeps every machine simpler.
//
// This comment used to go on to say that VA POLY and GRAIN "write both buffers of a stereo pair
// instead (see render_add_stereo below)". **There was no such function, here or anywhere.** The
// header promised an interface that did not exist, and the two machines it named have now been
// written mono: VA POLY spreads by voice DETUNE rather than stereo width, and GRAIN spreads
// position and pitch.
//
// Adding it is a real option, not a closed door -- but the cost is not the virtual. `InsertChain`
// is mono, so a stereo track doubles that track's insert-chain work, and it would do so on the
// two most expensive machines in the project while the CPU budget is still unmeasured on
// hardware. docs/UNIT_CHECKLIST.md carries it as a decision to take after a measurement rather
// than before one.
#pragma once

#include <cstdint>

namespace strata {

// --- parameters -----------------------------------------------------------

// The SOUND parameters: what a machine and its insert chain are prepared from.
constexpr int kParams = 16;
constexpr int kPages = 4;
constexpr int kParamsPerPage = 4;

// And a fifth page that is not sound: the track's LFO (dsp/lfo.hpp), four bytes after the 16,
// stored, locked, scened and macro'd like any other parameter, so every one of those works on
// it with no code of its own. No machine and no chain reads them, which is why `prepare` and
// its memo still look at the first kParams only: an LFO setting changing does not re-prepare a
// machine. What the LFO DOES to its destination arrives as an ordinary change to that byte.
constexpr int kLfoParams = 4;
// And a sixth: the arpeggiator (core/arp.hpp), MOD RAT OCT GATE, read by the engine's scheduler
// and by nothing that renders, for the same reasons.
constexpr int kArpParams = 4;
constexpr int kArpFirst = kParams + kLfoParams;
// And a seventh: ASR, the sustaining machines' note envelope -- ATK SUS REL FSU (dsp/va.cpp).
// Unlike the LFO and the arp this one IS sound, read by VA MONO and VA POLY in `prepare`, so the
// engine's prepare memo compares these four bytes as well as the first kParams. It sits after
// the arp rather than among the 16 because those are full: every VA slot on SRC and AMP is
// spoken for, and FLTR and FX belong to the insert chain. All four at 0 is the envelope the VAs
// always had, which is what an older kit loads as (core/project.hpp, kSectionKitAsr).
constexpr int kAsrParams = 4;
constexpr int kAsrFirst = kArpFirst + kArpParams;
// And an eighth: OSC, the VAs' oscillators -- WAV OS2 SUB DRV (dsp/va.cpp). Sound, like ASR, so
// the prepare memo compares it too. A kit from before it loads the values in
// core/project.hpp's kKitOscLegacy: a saw with the old fixed sub under it.
constexpr int kOscParams = 4;
constexpr int kOscFirst = kAsrFirst + kAsrParams;
constexpr int kTrackParams = kOscFirst + kOscParams;

enum class Page : uint8_t { src = 0, fltr = 1, amp = 2, fx = 3, lfo = 4, arp = 5, asr = 6, osc = 7 };

constexpr int page_first(Page page) { return int(page) * kParamsPerPage; }
static_assert(page_first(Page::lfo) == kParams, "the LFO page is the four bytes after the sound");
static_assert(kLfoParams == kParamsPerPage, "the LFO is one page of four");
static_assert(page_first(Page::arp) == kArpFirst, "the ARP page is the four bytes after the LFO");
static_assert(kArpParams == kParamsPerPage, "the arp is one page of four");
static_assert(page_first(Page::asr) == kAsrFirst, "the ASR page is the four bytes after the arp");
static_assert(kAsrParams == kParamsPerPage, "ASR is one page of four");
static_assert(page_first(Page::osc) == kOscFirst, "the OSC page is the four bytes after ASR");
static_assert(kOscParams == kParamsPerPage, "OSC is one page of four");

// All 32 of a track's parameters, as stored in a kit and overridden by locks. Values are 0-127;
// each machine and the chain map them to physical units themselves, so the sequencer never
// needs to know what a parameter means. Machines read v[0..kParams), the ASR page and the OSC page only.
struct TrackParams {
    uint8_t v[kTrackParams];

    uint8_t get(int index) const { return v[index]; }
    uint8_t get(Page page, int offset) const { return v[page_first(page) + offset]; }
    void set(int index, uint8_t value) { v[index] = value; }

    // 0-127 to 0..1. The usual first step in mapping a parameter.
    float unit(int index) const { return float(v[index]) * (1.0f / 127.0f); }
    float unit(Page page, int offset) const { return unit(page_first(page) + offset); }
};

// --- a trigger ------------------------------------------------------------

// One note-on for a machine. The sequencer fills this from a Step; the scheduler has already
// resolved probability, conditions and ratchets, so a Trig always sounds.
struct Trig {
    uint8_t note = 60;        // 0-127
    uint8_t velocity = 100;   // 1-127
    bool accent = false;
    bool slide = false;       // MONO machines only

    // How long the note should sound for, in frames from `frame`. **0 means no gate**: the
    // machine's own envelope decides when the note ends, which is what every drum wants and what
    // every machine did before this field existed.
    //
    // This is `Step::length` finally arriving. core/timing.hpp's note-length table is in
    // SIXTEENTHS OF A STEP and deliberately offers no ticks conversion, because -- as its own
    // comment says -- "gate length does not have to land on the tick grid: it only decides when
    // a voice is released, so the caller converts to SAMPLES at control rate, where the tempo is
    // known". `Engine::fire` is that caller.
    //
    // A DURATION rather than a note-off, for the sequencer: the machine owns the voice, so it
    // owns when the voice stops, and nothing has to schedule anything. A HELD key has no
    // duration to give, so KEYS mode triggers with 0 and ends the note with `Machine::release`
    // when the key comes up.
    //
    // A machine may ignore it entirely. The one-shot drums do.
    uint32_t gate_frames = 0;

    // A release WILL come for this note: a held KEYS key or a MIDI IN note, which end with
    // `Machine::release`. A trig with no gate and no release is a one-shot, and a machine whose
    // notes can sustain (the VA ASR page) must end it by itself even if SUS is raised while it
    // sounds -- otherwise it would hold for ever. Machines that never sustain ignore it.
    bool held = false;

    // Which frame of the block this starts on: sample accuracy, and the field micro-timing and
    // ratchets are built on.
    //
    // **It is always INSIDE the block.** `core/scheduler` can produce a frame far past the end of
    // one -- a ratchet's later hits sit thousands of frames into the step, and its header says
    // "spreading them is the caller's to defer, not the scheduler's to withhold" -- and
    // `app/engine.cpp` is the caller that defers them. A machine therefore never has to think
    // about a frame it cannot reach, and `trigger` may schedule on this value directly.
    //
    // Three machines clamp it to 65,535 anyway (dsp/sample.cpp, dsp/grain.cpp, dsp/vapoly.cpp)
    // and five take it raw. That inconsistency is harmless now the engine bounds it, and it is
    // recorded here rather than tidied in eight files: if the deferral is ever removed, the
    // clamping three would place a late hit 1.37 s early and the other five would not.
    uint32_t frame = 0;
};

// --- the machine interface ------------------------------------------------

// Implementations live in dsp/ and are allocated once, in App::init(), never in audio().
//
// Rules for every implementation (CLAUDE.md has the full set):
//   - no allocation, no files, no blocking, no division in a hot loop
//   - render_add ADDS into the buffer; it never clears it
//   - a voice that is not sounding must cost nothing measurable
//   - parameters are re-read once per block at control rate, not per sample
class Machine {
public:
    virtual ~Machine() = default;

    // Called once, in App::init(). Any buffer or delay line is taken here.
    virtual void init(float sample_rate) = 0;

    // Control rate, once per block, before render_add. Cheap: no allocation, and this is where
    // coefficients and envelope shapes are made.
    virtual void prepare(const TrackParams &params) = 0;

    // Start a note. May steal a voice; machines with 2 voices retrigger with a short fade.
    virtual void trigger(const Trig &trig) = 0;

    // Release any sounding voice with its tail (STOP once). Not a hard cut.
    virtual void release_all() = 0;

    // Hard cut, no tail (STOP twice, or a choke group).
    virtual void kill_all() = 0;

    // Let go of `note`: every voice playing it goes into its release, and one still waiting for
    // its frame is released as soon as it starts. A key coming up in KEYS mode
    // (`Cmd::release_note`). The sustaining machines -- VA, VA POLY, SAMPLE and GRAIN --
    // implement it; a one-shot drum has nothing to let go of, so the default does nothing.
    //
    // It is by NOTE, not by who started it: a key let go on a track whose sequencer is playing
    // the same note releases the sequencer's voice too. Accepted by Roland on 2026-11-22 -- the
    // alternative is a per-voice owner tag in every machine for a case that is mostly a player
    // doubling the part.
    virtual void release(uint8_t note) { (void)note; }

    // Add this block's mono output into out[0..frames).
    virtual void render_add(float *out, int frames) = 0;

    // True while anything is sounding, so the mixer can skip silent tracks and the UI can show
    // activity. Must be cheap.
    virtual bool active() const = 0;

    // How many of the 16 parameters this machine actually uses on its pages, for the UI to grey
    // out the rest. Machines that use all of SRC and AMP return 8.
    virtual int used_params() const { return 8; }

    // The amplitude envelope of the note most worth showing, for the dot EDIT's AMP and ASR
    // pictures ride on their curve: its stage (enaga::dsp::Envelope::Stage as a byte, 0 idle) and
    // level, 0-1 of the peak. Audio context, read by the engine every few blocks. False when the
    // machine does not say, which is every machine but SAMPLE, GRAIN and the MONO synths.
    virtual bool envelope_now(uint8_t &stage, float &level) const {
        (void)stage;
        (void)level;
        return false;
    }

    // Three-letter names for the 16 parameter slots, for the encoder labels. Index 0-15; a
    // machine returns "---" for a slot it does not use. The font has upper case and digits only.
    virtual const char *param_name(int index) const = 0;
};

}  // namespace strata
