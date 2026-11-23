// The WAVE machines' wavetables: sixteen built-in tables, computed in init() and never touched
// again.
//
// Ported from PolyWerk (github.com/Devko/PolyWerk, dsp/wavetable.cpp; MIT, Copyright Devko), the
// same author's wavetable synth for the Akai Force. The recipes -- which spectrum each frame
// has, and how the frames morph -- are PolyWerk's. What changed for STRATA:
//
//   - Smaller. 16 frames a table, not 32, and 1,024 samples at the top level rather than 2,048
//     (512 harmonics: at 48 kHz that still reaches 20 kHz on a 40 Hz bass). 10,260 bytes a frame,
//     164 KB a table, 2.5 MiB for all sixteen. PolyWerk's layout would have been 4.4 MiB.
//   - No libm, no std::vector, no doubles. The builder runs once, in init(), in float, with
//     Enaga's math (sin_turns has an absolute error under 4e-6, so the FFT's twiddles put its
//     own error some 100 dB down) and with scratch it allocates once.
//   - The time-domain recipes (sync, fold, FM...) are sampled 4x finer than a frame instead of
//     8x. What the naive rendering aliases into the kept harmonics is then at most -71 dB
//     instead of -78, and the build takes half as long.
//   - Tables that need to be played octaves down (Organ, Bell) are left out: on STRATA a note
//     should sound at the note.
//
// The format is PolyWerk's. A frame is kWaveMipLevels pre-filtered copies of one cycle; level k
// keeps harmonics 1..(512 >> k) and is stored at its own length (short for the high levels,
// which have few harmonics), each followed by a guard sample equal to its first. Samples are
// int16 with one float scale per frame, so a quiet frame keeps its resolution. The oscillator
// (dsp/waveloops.ti.c) picks the level whose top harmonic stays clear of aliasing at the pitch
// it plays, so the inner loop is two lookups and no filtering.
#pragma once

#include <atomic>
#include <cstdint>

namespace strata {

constexpr int kWaveTables = 16;
constexpr int kWaveFrames = 16;
constexpr int kWaveTopBits = 10;
constexpr int kWaveTopLength = 1 << kWaveTopBits;   // samples of level 0
constexpr int kWaveHarmonics = kWaveTopLength / 2;  // 512
constexpr int kWaveMipLevels = 10;                  // 512, 256, ... 1 harmonics

// Samples of level k = 1 << kWaveMipBits[k] = clamp(8 * (512 >> k), 256, 1024): linear
// interpolation wants about eight samples a cycle of the top harmonic, capped at the top length.
constexpr int kWaveMipBits[kWaveMipLevels] = {10, 10, 10, 9, 8, 8, 8, 8, 8, 8};

constexpr int wave_mip_length(int k) { return 1 << kWaveMipBits[k]; }
constexpr int wave_mip_offset(int k) {
    return k == 0 ? 0 : wave_mip_offset(k - 1) + wave_mip_length(k - 1) + 1;
}
constexpr int kWaveFrameStride =
    wave_mip_offset(kWaveMipLevels - 1) + wave_mip_length(kWaveMipLevels - 1) + 1;
static_assert(kWaveFrameStride == 5130, "mip layout changed: update the header comment");

constexpr uint32_t kWaveLibraryBytes =
    uint32_t(kWaveTables) * kWaveFrames * kWaveFrameStride * sizeof(int16_t);   // all sixteen

// The highest harmonic h allowed at phase increment `inc` (cycles a sample) is the one whose
// alias folds back no lower than 18 kHz: h * inc <= (48 - 18) / 48. The top octave stays bright
// instead of every note being dulled to the strict Nyquist limit -- PolyWerk's rule, at 48 kHz.
constexpr float kWaveAliasLimit = 0.625f;

// The mip level to play at increment `inc`. Control rate; a loop of compares, no division.
inline int wave_mip_for(float inc) {
    int k = 0;
    while (k < kWaveMipLevels - 1 && float(kWaveHarmonics >> k) * inc > kWaveAliasLimit) k++;
    return k;
}

constexpr uint32_t kWaveTableBytes = uint32_t(kWaveFrames) * kWaveFrameStride * sizeof(int16_t);

// The tables a WAVE track can play, built on demand into a few slots reserved at init.
//
// Building all sixteen at boot outlasted Enaga's 1 s main-loop watchdog on the C6748, and spent
// 2.5 MiB and seconds of CPU on projects with no WAVE track. So nothing is built until a track
// sounds a table (Engine::step_waves asks for the tables the WAVE tracks are sounding, kit,
// locks, scenes and LFO in): the table is built into a free slot a small piece at a time from
// the main context, and the audio side plays the voice's sub alone until it is in. A slot whose
// table no track wants is reused, the least recently wanted first. More tables later -- built-in
// or the user's own from the card -- cost slots, not memory per table.
//
// Two contexts. The main context calls reserve() (init), want() and step(). The audio context
// calls slot_of(), frame_at(), scale_at() and audio_block(). A table is published in slot_of_
// only once all of its slot is written (release; acquire on the audio side), and a slot is only
// written again two audio blocks after its table was taken out of slot_of_, so no render that
// read the old mapping is still reading it.
class WaveLibrary {
public:
    // Reserves `slots` tables' memory and the builder's scratch. init() only.
    bool reserve(int slots);
    // Tests and tools: every table, built now (sixteen slots).
    bool init();

    // --- main context ---
    // The tables wanted now, a bit each: built in turn, kept while wanted.
    void want(uint32_t tables) { wanted_ = tables & ((1u << kWaveTables) - 1u); }
    // One piece of work: a piece of the build in progress, or the start of the next one. False
    // when there is nothing to do (every wanted table is in, or waiting on a slot).
    bool step();
    // Whether `table` is playable, for tests and the screen.
    bool resident(int table) const { return slot_of(table) >= 0; }
    int slots() const { return slots_; }
    // Tables built since reserve(), for tests and the scenarios' log.
    int builds() const { return builds_; }
    int last_built() const { return last_built_; }

    // --- audio context ---
    // The slot holding `table`, or -1 while it is not built.
    int slot_of(int table) const {
        if (table < 0 || table >= kWaveTables) return -1;
        return slot_of_[table].load(std::memory_order_acquire);
    }
    // Level `mip` of `frame` in `slot`: wave_mip_length(mip) samples plus the guard sample.
    // Arguments are clamped, so a stale byte can never read outside the slots.
    const int16_t *frame_at(int slot, int frame, int mip) const;
    float scale_at(int slot, int frame) const;
    // The end of every audio block (Engine::process): what lets a slot be reused safely.
    void audio_block() { blocks_.fetch_add(1u, std::memory_order_release); }

    // By table, for tests: the table's frame, or a silent one when it is not built.
    const int16_t *frame(int table, int frame, int mip) const;
    float scale(int table, int frame) const;
    bool ready() const { return slots_ > 0; }

    // The table's name for the screen: three characters ("CLS", "PWM"...) and a long one.
    static const char *short_name(int table);
    static const char *long_name(int table);

    // Pieces a table: the recipe, and two frames at every mip level, plus up to six for each pair
    // of a time-domain recipe or a shaper.
    static constexpr int kStoreSteps = (kWaveFrames / 2) * kWaveMipLevels;
    static constexpr int kMostSteps = 1 + kStoreSteps + 6 * (kWaveFrames / 2);
    static constexpr int kMaxSlots = kWaveTables;

private:
    enum class Stage : uint8_t { idle, recipe, sample, shape, store };
    bool start_next();
    bool build_piece();

    int16_t *data_ = nullptr;                   // slots_ tables of kWaveTableBytes
    float scale_[kMaxSlots][kWaveFrames] = {};
    int slots_ = 0;
    std::atomic<int8_t> slot_of_[kWaveTables] = {};   // set to -1 by reserve()
    int8_t table_in_[kMaxSlots] = {};           // the table each slot holds or is building, -1
    uint32_t last_wanted_[kMaxSlots] = {};      // step count when its table was last wanted
    uint32_t freed_at_[kMaxSlots] = {};         // audio block when its table was taken out
    bool settling_[kMaxSlots] = {};             // taken out, waiting two blocks to be written
    std::atomic<uint32_t> blocks_{0};
    uint32_t wanted_ = 0;
    uint32_t steps_ = 0;
    int builds_ = 0;
    int last_built_ = -1;

    // The build in progress.
    int building_ = -1;                         // the table, or -1
    int build_slot_ = -1;
    int next_pair_ = 0;
    int next_level_ = 0;
    int next_part_ = 0;
    Stage stage_ = Stage::idle;
    bool normalise_ = true;
    // The builder (dsp/wavetable.cpp) lives here, one per library, so two libraries never share
    // a half-built table's spectra. Its scratch is on the heap, from reserve().
    alignas(8) unsigned char builder_[160] = {};
};

// The library is the same for every track and every instance: Engine::init reserves it and sets
// it here, and the machines read it through this pointer. nullptr without one, and a machine
// with no library, or whose table is not in yet, plays its sub alone rather than reading.
const WaveLibrary *wave_library();
// What reserve() takes besides the slots: the builder's scratch (tests/test_memory.cpp).
uint32_t wave_build_scratch_bytes();
void set_wave_library(const WaveLibrary *library);

}  // namespace strata
