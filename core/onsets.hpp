// Slice points: where the hits are in a sample, found once when it loads.
//
// SLC cuts the START/LEN window into equal slices, which only fits a loop whose hits sit on an
// even grid. A drum break played by a person does not, so its slices start before or after the
// hit they were meant to hold. SLC AUTO (docs/SPEC.md, "SAMPLE") cuts at the hits instead, and
// this file finds them.
//
// ## How
//
// The sample is read in hops of kHop frames (5.3 ms at 48 kHz). A hop's energy is the sum of
// |L| + |R| (or |M|) over its frames. A hop is a hit when its energy is at least kRise times the
// mean of the kHistory hops before it and above kFloor, a level that keeps the hiss in a fade
// from counting. Its strength is how far it rose above that mean.
//
// A hit lasts more than one hop (the attack spreads over two or three), and a flam is two hits
// close together that the ear hears as one. So a hit within kMinGapHops of the previous one is
// part of it: the run keeps its FIRST hop as the place to cut (the cut goes before the attack,
// never inside it) and the strongest hop as its strength.
//
// The kPointsMax strongest hits are kept, one per run. finish() sorts them by place and ranks
// them by strength, so a track asking for n slices cuts at the n - 1 strongest
// (dsp/sample.hpp, set_slice_points). Hop 0 is never a point: a cut there makes a slice of
// nothing.
//
// ## Cost and context
//
// Main context only (app/storage.cpp feeds it the PCM as each slice of a load is written into
// the pool), and incremental, so a 349 s file costs no more per pass than the load itself: one
// add and one abs per sample. Integer only; no divide, no float, no allocation.
//
// core/ never includes Enaga headers, and this is plain data in, plain data out.
#pragma once

#include <cstdint>

namespace strata {

struct SlicePoints {
    static constexpr int kMax = 15;            // 16 slices, SLC's top
    uint32_t at[kMax] = {};                    // frames from the sample's start, ascending
    uint8_t rank[kMax] = {};                   // 0 = the strongest hit
    uint8_t count = 0;
};

class OnsetFinder {
public:
    static constexpr int kPointsMax = SlicePoints::kMax;
    static constexpr uint32_t kHop = 256;
    static constexpr int kHistory = 8;         // a power of two: the mean is a shift
    static constexpr uint32_t kRise = 2;       // a hit at twice the recent mean
    // Mean |sample| over a hop, per channel, below which nothing is a hit: about -40 dBFS.
    static constexpr uint32_t kFloorPerFrame = 330;
    // 50 ms at 48 kHz: closer than this is the same hit.
    static constexpr uint32_t kMinGapHops = 9;

    // Starts over. Any feed with `first` == 0 also starts over, so a loader does not have to.
    void reset(int channels);

    // The next `frames` frames of interleaved int16, `first` being the number of the first of
    // them. Frames must come in order: a feed that does not start where the last one ended marks
    // the analysis broken, and finish() then reports no points rather than wrong ones.
    void feed(const int16_t *pcm, uint32_t first, uint32_t frames, int channels);

    // The points of everything fed so far, for a sample of `total` frames (the count the loader
    // hands the engine). `total` is checked against what was fed: a mismatch is no points.
    void finish(uint32_t total, SlicePoints &out);

    uint32_t fed() const { return next_; }

private:
    void end_hop();
    void close_run();
    void keep(uint32_t at, uint32_t strength);

    int channels_ = 1;
    bool broken_ = false;
    uint32_t next_ = 0;                        // the frame number the next feed must start at
    uint32_t hop_ = 0;                         // hops completed
    uint32_t in_hop_ = 0;                      // frames of the current hop so far
    uint32_t energy_ = 0;                      // of the current hop

    uint32_t history_[kHistory] = {};
    uint32_t history_sum_ = 0;
    int history_at_ = 0;

    bool run_open_ = false;
    uint32_t run_start_hop_ = 0;
    uint32_t run_strength_ = 0;

    uint32_t kept_at_[kPointsMax] = {};
    uint32_t kept_strength_[kPointsMax] = {};
    int kept_ = 0;
};

}  // namespace strata
