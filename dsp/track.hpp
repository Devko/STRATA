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
// Machines are MONO. Pan and the stereo spread happen in the insert chain, which halves the
// filter work and keeps every machine simpler. Only VA POLY and GRAIN produce stereo, and they
// write both buffers of a stereo pair instead (see render_add_stereo below).
#pragma once

#include <cstdint>

namespace strata {

// --- parameters -----------------------------------------------------------

constexpr int kParams = 16;
constexpr int kPages = 4;
constexpr int kParamsPerPage = 4;

enum class Page : uint8_t { src = 0, fltr = 1, amp = 2, fx = 3 };

constexpr int page_first(Page page) { return int(page) * kParamsPerPage; }

// All 16 of a track's parameters, as stored in a kit and overridden by locks. Values are 0-127;
// each machine and the chain map them to physical units themselves, so the sequencer never
// needs to know what a parameter means.
struct TrackParams {
    uint8_t v[kParams];

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
    uint32_t frame = 0;       // which frame of the block it starts on: sample accuracy
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

    // Add this block's mono output into out[0..frames).
    virtual void render_add(float *out, int frames) = 0;

    // True while anything is sounding, so the mixer can skip silent tracks and the UI can show
    // activity. Must be cheap.
    virtual bool active() const = 0;

    // How many of the 16 parameters this machine actually uses on its pages, for the UI to grey
    // out the rest. Machines that use all of SRC and AMP return 8.
    virtual int used_params() const { return 8; }

    // Three-letter names for the 16 parameter slots, for the encoder labels. Index 0-15; a
    // machine returns "---" for a slot it does not use. The font has upper case and digits only.
    virtual const char *param_name(int index) const = 0;
};

}  // namespace strata
