// dsp/lfo.hpp says what the four bytes mean and why the LFO runs at block rate.
#include "lfo.hpp"

namespace strata {

namespace {

// One synced cycle in ticks at 96 PPQN, longest first: a 16th is 24 ticks, a bar 384. Every entry
// is a whole number of ticks, triplets included, so a synced cycle never drifts off the grid.
constexpr uint32_t kSyncTicks[kLfoSyncDivisions] = {
    3072, 1536, 768, 384,      // 8, 4, 2 and 1 bars
    288, 192, 144, 128,        // 1/2 dotted, 1/2, 1/4 dotted, 1/2 triplet
    96, 72, 64, 48,            // 1/4, 1/8 dotted, 1/4 triplet, 1/8
    32, 24, 16, 12,            // 1/8 triplet, 1/16, 1/16 triplet, 1/32
};
// '.' is a dotted note: the font has no other dot to offer.
constexpr const char *kSyncNames[kLfoSyncDivisions] = {
    "8B",   "4B",  "2B",   "1B",
    "1/2.", "1/2", "1/4.", "1/2T",
    "1/4",  "1/8.", "1/4T", "1/8",
    "1/8T", "1/16", "1/16T", "1/32",
};

int sync_index(uint8_t rate) {
    const int r = rate > 127 ? 127 : int(rate);
    return (r - int(kLfoSyncFirst)) >> 2;
}

}  // namespace

uint32_t lfo_sync_ticks(uint8_t rate) {
    if (rate < kLfoSyncFirst) return 0;
    return kSyncTicks[sync_index(rate)];
}

const char *lfo_sync_name(uint8_t rate) {
    if (rate < kLfoSyncFirst) return "---";
    return kSyncNames[sync_index(rate)];
}

float lfo_free_hz(uint8_t rate) {
    // 1/16 Hz at 0, doubling every 7 counts: 32 Hz at 63. Control rate, only when the byte moves.
    const int r = rate >= kLfoSyncFirst ? int(kLfoSyncFirst) - 1 : int(rate);
    return 0.0625f * enaga::dsp::exp2(float(r) * (1.0f / 7.0f));
}

uint32_t lfo_free_centi_hz(uint8_t rate) {
    return uint32_t(lfo_free_hz(rate) * 100.0f + 0.5f);
}

const char *lfo_wave_name(LfoWave wave) {
    switch (wave) {
        case LfoWave::sine:        return "SIN";
        case LfoWave::triangle:    return "TRI";
        case LfoWave::ramp:        return "RMP";
        case LfoWave::saw:         return "SAW";
        case LfoWave::square:      return "SQR";
        case LfoWave::sample_hold: return "S+H";
        default:                   return "?";
    }
}

const char *lfo_trig_name(LfoTrig trig) {
    switch (trig) {
        case LfoTrig::retrig:   return "RETRIG";
        case LfoTrig::one_shot: return "ONE-SHOT";
        default:                return "FREE";
    }
}

const char *lfo_param_name(int index) {
    switch (index) {
        case kLfoWave:  return "WAV";
        case kLfoRate:  return "RAT";
        case kLfoDepth: return "DEP";
        case kLfoDest:  return "DST";
        default:        return "---";
    }
}

uint8_t lfo_apply(uint8_t base, float value, uint8_t depth, uint8_t floor, float span) {
    // `span` counts at DEP 127: span/127 per count of depth. Rounded half away from zero, so a
    // symmetrical wave moves the byte symmetrically. span x (1/127) is exactly the 64/127 this
    // used to multiply by when span is 64 (a power of two scales a float exactly), so every
    // destination but a cutoff moves bit for bit as it did.
    const float per_count = span * (1.0f / 127.0f);
    const float offset = value * float(depth > 127 ? 127 : depth) * per_count;
    const int rounded = offset >= 0.0f ? int(offset + 0.5f) : -int(-offset + 0.5f);
    const int next = int(base) + rounded;
    const int lowest = base < floor ? int(base) : int(floor);
    return uint8_t(next < lowest ? lowest : (next > 127 ? 127 : next));
}

uint8_t lfo_cutoff_floor(int dest, bool va_filter, bool insert_chain) {
    if (insert_chain && dest == page_first(Page::fltr) + 1) return kLfoInsertCutoffFloor;
    if (va_filter && dest == page_first(Page::src) + 2) return kLfoVaCutoffFloor;
    return 0;
}

float lfo_pitch_semis(float value, uint8_t depth) {
    // The same counts lfo_apply would move TUN by, unrounded, times TUN's 24/64 semitone a count.
    const float counts = value * float(depth > 127 ? 127 : depth) * (kLfoSpan * (1.0f / 127.0f));
    return counts * (24.0f / 64.0f);
}

int lfo_cutoff_param(bool va_filter, bool insert_chain) {
    if (va_filter) return page_first(Page::src) + 2;
    if (insert_chain) return page_first(Page::fltr) + 1;
    return -1;
}

float lfo_span(int dest, bool va_filter, bool insert_chain) {
    if (insert_chain && !va_filter && dest == page_first(Page::fltr) + 1) return kLfoInsertCutoffSpan;
    if (va_filter && dest == page_first(Page::src) + 2) return kLfoVaCutoffSpan;
    return kLfoSpan;
}

LfoSettings LfoSettings::make(const uint8_t bytes[kLfoParams]) {
    LfoSettings s;
    s.wave = lfo_wave(bytes[0]);
    s.sync_ticks = lfo_sync_ticks(bytes[1]);
    s.inv_sync_ticks = s.sync_ticks == 0 ? 0.0f : enaga::dsp::reciprocal(float(s.sync_ticks));
    s.hz = s.sync_ticks == 0 ? lfo_free_hz(bytes[1]) : 0.0f;
    s.depth = bytes[2] > 127 ? uint8_t(127) : bytes[2];
    s.dest = lfo_dest(bytes[3]);
    return s;
}

void TrackLfo::init(uint32_t seed) {
    lfo_.reset(0.0f);
    random_ = enaga::dsp::Random(seed);
    shot_ = 1.0f;
    done_ = true;
    fired_ = false;
    shot_held_ = 0.0f;
}

enaga::dsp::LfoShape TrackLfo::enaga_shape(LfoWave wave) {
    switch (wave) {
        case LfoWave::triangle:    return enaga::dsp::LfoShape::triangle;
        case LfoWave::ramp:
        case LfoWave::saw:         return enaga::dsp::LfoShape::saw;
        case LfoWave::square:      return enaga::dsp::LfoShape::square;
        case LfoWave::sample_hold: return enaga::dsp::LfoShape::sample_hold;
        default:                   return enaga::dsp::LfoShape::sine;
    }
}

float TrackLfo::shaped(LfoWave wave, float raw) const {
    // Enaga's saw rises; SAW is the falling one.
    return wave == LfoWave::saw ? -raw : raw;
}

void TrackLfo::restart() {
    // A whole cycle from phase 0 lands on phase 0 again and counts as a wrap, which is how
    // Enaga's S+H draws: so the note gets a new value through the same API a free cycle uses.
    lfo_.reset(0.0f);
    lfo_.step(enaga::dsp::LfoShape::sample_hold, 1.0f, random_);
    shot_ = 0.0f;
    done_ = false;
    fired_ = true;
    shot_held_ = random_.bipolar();
}

float TrackLfo::one_shot_value(LfoWave wave, float u, float held) {
    const bool rest = u >= 1.0f;
    const float x = u < 0.0f ? 0.0f : u;
    switch (wave) {
        case LfoWave::sine:     return rest ? 0.0f : enaga::dsp::sin_turns(x);
        case LfoWave::triangle: return rest ? 0.0f : (x < 0.5f ? 2.0f * x : 2.0f - 2.0f * x);
        case LfoWave::ramp:     return rest ? 1.0f : x;
        case LfoWave::saw:      return rest ? 0.0f : 1.0f - x;
        case LfoWave::square:   return rest ? 0.0f : (x < 0.5f ? 1.0f : 0.0f);
        case LfoWave::sample_hold: return held;
        default:                return 0.0f;
    }
}

float TrackLfo::one_shot(LfoWave wave, float cycles) {
    if (!fired_) return 0.0f;
    if (!done_) {
        shot_ += cycles;
        if (shot_ >= 1.0f) {
            shot_ = 1.0f;
            done_ = true;
        }
    }
    return one_shot_value(wave, shot_, shot_held_);
}

float TrackLfo::advance(LfoWave wave, float cycles) {
    return shaped(wave, lfo_.step(enaga_shape(wave), cycles, random_));
}

float TrackLfo::seek(LfoWave wave, float phase) {
    float dt = phase - lfo_.phase();
    if (dt < 0.0f) dt += 1.0f;
    return shaped(wave, lfo_.step(enaga_shape(wave), dt, random_));
}

}  // namespace strata
