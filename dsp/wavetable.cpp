// The wavetable builder. init() time only: nothing here runs in audio().
//
// The recipes are PolyWerk's (dsp/wavetable.cpp there, MIT, Copyright Devko); dsp/wavetable.hpp
// says what changed on the way here. Every number in a recipe below is PolyWerk's unless its
// comment says otherwise.
#include "wavetable.hpp"

#include <cstdint>
#include <new>

#include <enaga/dsp.hpp>

namespace strata {
namespace {

using enaga::dsp::cos_turns;
using enaga::dsp::exp;
using enaga::dsp::exp2;
using enaga::dsp::log2;
using enaga::dsp::sin_turns;

constexpr int kTop = kWaveHarmonics - 1;    // harmonics 1..511 are stored
constexpr int kSampled = 4 * kWaveTopLength;  // the time-domain recipes' resolution
constexpr int kFftMax = kSampled;

// --- float helpers without libm (init time, so plain and exact enough) -----------------------

float absf(float x) { return x < 0.0f ? -x : x; }
float maxf(float a, float b) { return a > b ? a : b; }
float floorf_(float x) { return float(enaga_floor_int(x)); }
float fracf(float x) { return x - floorf_(x); }
float roundf_(float x) { return floorf_(x + 0.5f); }
float powf_(float base, float e) { return exp2(e * log2(base)); }
// sqrt by Newton from a bit guess; x >= 0.
float sqrtf_(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = enaga_bits_float(0x5F3759DFu - (enaga_float_bits(x) >> 1));   // ~1/sqrt(x)
    for (int i = 0; i < 4; i++) r = r * (1.5f - 0.5f * x * r * r);
    return x * r;
}
float tanhf_(float x) {
    if (x > 9.0f) return 1.0f;
    if (x < -9.0f) return -1.0f;
    const float e = exp(2.0f * x);
    return (e - 1.0f) / (e + 1.0f);
}
float sinf_turns(float t) { return sin_turns(t); }

// Where frame f of n sits along a table, 0..1.
float at(int f, int n) { return n > 1 ? float(f) / float(n - 1) : 0.0f; }

// --- spectra -----------------------------------------------------------------------------------

// Cosine (a) and sine (b) amplitudes of harmonics 1..kTop; index 0 is unused (no DC).
struct Spectrum {
    float a[kWaveHarmonics];
    float b[kWaveHarmonics];
    void clear() {
        for (int h = 0; h < kWaveHarmonics; h++) a[h] = b[h] = 0.0f;
    }
};

// The builder's scratch, carved from memory it borrows for the build (WaveLibrary::init), or
// from the heap when it is given none. Every piece is float-aligned; nothing is freed.
struct Arena {
    uint8_t *at = nullptr;
    uint32_t left = 0;
    template <typename T>
    T *take(uint32_t count) {
        if (at == nullptr) return new (std::nothrow) T[count];
        const uint32_t pad = uint32_t(-reinterpret_cast<uintptr_t>(at)) & uint32_t(alignof(T) - 1);
        const uint32_t bytes = uint32_t(sizeof(T)) * count;
        if (pad + bytes > left) return nullptr;
        T *out = new (at + pad) T[count];
        at += pad + bytes;
        left -= pad + bytes;
        return out;
    }
};

void mix_into(Spectrum &out, const Spectrum &x, const Spectrum &y, float t) {
    for (int h = 0; h < kWaveHarmonics; h++) {
        out.a[h] = x.a[h] + (y.a[h] - x.a[h]) * t;
        out.b[h] = x.b[h] + (y.b[h] - x.b[h]) * t;
    }
}

constexpr float kPi = 3.14159265358979f;

void make_sine(Spectrum &s) {
    s.clear();
    s.b[1] = 1.0f;
}
void make_saw(Spectrum &s) {
    s.clear();
    for (int h = 1; h <= kTop; h++) s.b[h] = 2.0f / (kPi * float(h)) * ((h & 1) ? 1.0f : -1.0f);
}
void make_square(Spectrum &s) {
    s.clear();
    for (int h = 1; h <= kTop; h += 2) s.b[h] = 4.0f / (kPi * float(h));
}
void make_triangle(Spectrum &s) {
    s.clear();
    for (int h = 1; h <= kTop; h += 2)
        s.b[h] = 8.0f / (kPi * kPi * float(h) * float(h)) * ((((h - 1) / 2) & 1) ? -1.0f : 1.0f);
}
// +1 for the first `duty` of the cycle, -1 after, DC removed.
void make_pulse(Spectrum &s, float duty) {
    s.clear();
    for (int h = 1; h <= kTop; h++) {
        const float turns = float(h) * duty;
        s.a[h] = 2.0f * sin_turns(turns) / (kPi * float(h));
        s.b[h] = 2.0f * (1.0f - cos_turns(turns)) / (kPi * float(h));
    }
}
// A soft 1/h body plus one resonant peak around harmonic `centre`.
void make_formant(Spectrum &s, float centre) {
    s.clear();
    const float width = 0.25f * centre + 1.5f;
    for (int h = 1; h <= kTop; h++) {
        const float z = (float(h) - centre) / width;
        s.b[h] = 0.3f / float(h) + exp(-z * z);
    }
}

// A resonance's magnitude at f: 1 at its centre, 1/sqrt(2) half a bandwidth away.
float peak(float f, float centre, float bw) {
    const float z = (f - centre) / (0.5f * bw);
    return 1.0f / sqrtf_(1.0f + z * z);
}

// --- sung vowels: a glottal source through five formants (PolyWerk, after the Csound manual) ----

struct Formant {
    float hz, db, bw;
};
struct Vowel {
    Formant f[5];
};
constexpr Vowel kBassA{{{600, 0, 60}, {1040, -7, 70}, {2250, -9, 110}, {2450, -9, 120}, {2750, -20, 130}}};
constexpr Vowel kBassE{{{400, 0, 40}, {1620, -12, 80}, {2400, -9, 100}, {2800, -12, 120}, {3100, -18, 120}}};
constexpr Vowel kBassI{{{250, 0, 60}, {1750, -30, 90}, {2600, -16, 100}, {3050, -22, 120}, {3340, -28, 120}}};
constexpr Vowel kBassO{{{400, 0, 40}, {750, -11, 80}, {2400, -21, 100}, {2600, -20, 120}, {2900, -40, 120}}};
constexpr Vowel kBassU{{{350, 0, 40}, {600, -20, 80}, {2400, -32, 100}, {2675, -28, 120}, {2950, -36, 120}}};
constexpr Vowel kTenorA{{{650, 0, 80}, {1080, -6, 90}, {2650, -7, 120}, {2900, -8, 130}, {3250, -22, 140}}};
constexpr Vowel kTenorE{{{400, 0, 70}, {1700, -14, 80}, {2600, -12, 100}, {3200, -14, 120}, {3580, -20, 120}}};
constexpr Vowel kTenorI{{{290, 0, 40}, {1870, -15, 90}, {2800, -18, 100}, {3250, -20, 120}, {3540, -30, 120}}};
constexpr Vowel kTenorO{{{400, 0, 40}, {800, -10, 80}, {2600, -12, 100}, {2800, -12, 120}, {3000, -26, 120}}};
constexpr Vowel kTenorU{{{350, 0, 40}, {600, -20, 60}, {2700, -17, 100}, {2900, -14, 120}, {3300, -26, 120}}};

struct Singer {
    float f0, tilt, floor, widen;   // fundamental (Hz), source tilt, unformanted floor, bandwidth scale
};

// The formants glide from vowel to vowel (frequency, level and width move), rather than one
// vowel's spectrum crossfading into the next.
void sing(Spectrum &s, const Vowel &va, const Vowel &vb, float t, const Singer &c) {
    Formant m[5];
    for (int k = 0; k < 5; k++) {
        m[k].hz = va.f[k].hz * powf_(vb.f[k].hz / va.f[k].hz, t);
        m[k].db = va.f[k].db + (vb.f[k].db - va.f[k].db) * t;
        m[k].bw = va.f[k].bw + (vb.f[k].bw - va.f[k].bw) * t;
    }
    float gain[5];
    for (int k = 0; k < 5; k++) gain[k] = exp2(m[k].db * (3.32192809f / 20.0f));   // 10^(dB/20)
    s.clear();
    for (int h = 1; h <= kTop; h++) {
        const float f = float(h) * c.f0;
        if (f > 12000.0f) break;
        float g = c.floor;
        for (int k = 0; k < 5; k++) g += gain[k] * peak(f, m[k].hz, maxf(100.0f, m[k].bw * c.widen));
        s.b[h] = g / powf_(float(h), c.tilt) * ((h & 1) ? 1.0f : -1.0f);   // a saw's phases
    }
}

// --- the FFT -----------------------------------------------------------------------------------

// Radix 2, in place, any power of two up to kFftMax, unscaled both ways. Twiddles come from one
// table for kFftMax points, read at a stride for the smaller sizes.
class Fft {
public:
    float *re = nullptr;
    float *im = nullptr;

    bool init(Arena &arena) {
        re = arena.take<float>(kFftMax);
        im = arena.take<float>(kFftMax);
        wr_ = arena.take<float>(kFftMax / 2);
        wi_ = arena.take<float>(kFftMax / 2);
        if (!re || !im || !wr_ || !wi_) return false;
        for (int k = 0; k < kFftMax / 2; k++) {
            const float t = float(k) / float(kFftMax);
            wr_[k] = cos_turns(t);
            wi_[k] = -sin_turns(t);   // e^(-2 pi i k / N)
        }
        return true;
    }

    void run(int n, bool inverse) {
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                const float tr = re[i], ti = im[i];
                re[i] = re[j];
                im[i] = im[j];
                re[j] = tr;
                im[j] = ti;
            }
        }
        const float sign = inverse ? -1.0f : 1.0f;
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1;
            const int step = kFftMax / len;
            for (int i = 0; i < n; i += len) {
                for (int k = 0; k < half; k++) {
                    const float wr = wr_[k * step], wi = sign * wi_[k * step];
                    const int p = i + k, q = p + half;
                    const float vr = re[q] * wr - im[q] * wi;
                    const float vi = re[q] * wi + im[q] * wr;
                    re[q] = re[p] - vr;
                    im[q] = im[p] - vi;
                    re[p] += vr;
                    im[p] += vi;
                }
            }
        }
    }

private:
    float *wr_ = nullptr;
    float *wi_ = nullptr;
};

// --- the builder -------------------------------------------------------------------------------

// One cycle as a function of the frame's place along the table (x, 0..1) and the time in the
// cycle (t, 0..1): the time-domain recipes.
using Cycle = float (*)(float x, float t);
// A waveshaper applied to frame f's cycle, rendered at peak 1.
using Shape = float (*)(int f, float v);

class Builder {
public:
    bool init(Arena &arena) {
        specs_ = arena.take<Spectrum>(kWaveFrames);
        keys_ = arena.take<Spectrum>(kKeys);
        f32_ = arena.take<float>(2 * kWaveFrameStride);
        return specs_ && keys_ && f32_ && fft_.init(arena);
    }

    // Frame f's spectrum, and key spectra a recipe morphs between.
    static constexpr int kKeys = 8;
    Spectrum &spec(int f) { return specs_[f]; }
    Spectrum &key(int k) { return keys_[k]; }

    // Spectra of time-domain cycles, two frames per FFT.
    void sample(Cycle cycle) {
        for (int f = 0; f < kWaveFrames; f += 2) {
            const float xa = at(f, kWaveFrames), xb = at(f + 1, kWaveFrames);
            for (int s = 0; s < kSampled; s++) {
                const float t = float(s) * (1.0f / float(kSampled));
                fft_.re[s] = cycle(xa, t);
                fft_.im[s] = cycle(xb, t);
            }
            analyse(kSampled, specs_[f], specs_[f + 1]);
        }
    }

    // Every frame through a waveshaper: rendered at peak 1, shaped, measured again.
    void shape(Shape fn) {
        for (int f = 0; f < kWaveFrames; f += 2) {
            synthesise(kSampled, specs_[f], specs_[f + 1], kTop);
            float pa = 0.0f, pb = 0.0f;
            for (int s = 0; s < kSampled; s++) {
                pa = maxf(pa, absf(fft_.re[s]));
                pb = maxf(pb, absf(fft_.im[s]));
            }
            const float ka = pa > 1e-12f ? 1.0f / pa : 1.0f, kb = pb > 1e-12f ? 1.0f / pb : 1.0f;
            for (int s = 0; s < kSampled; s++) {
                fft_.re[s] = fn(f, fft_.re[s] * ka);
                fft_.im[s] = fn(f + 1, fft_.im[s] * kb);
            }
            analyse(kSampled, specs_[f], specs_[f + 1]);
        }
    }

    // The sixteen spectra into table `table` of `data`, every mip level, as int16 with a scale
    // per frame. `normalise`: each frame to its own level-0 peak, so frames don't jump in level
    // as the position moves. Otherwise the whole table to one peak (normalise_whole).
    void store(int16_t *data, float *scales, bool normalise) {
        for (int f = 0; f < kWaveFrames; f += 2) {
            float gain[2] = {1.0f, 1.0f};
            for (int k = 0; k < kWaveMipLevels; k++) {
                const int n = wave_mip_length(k);
                const int top = (kWaveHarmonics >> k) < kTop ? (kWaveHarmonics >> k) : kTop;
                synthesise(n, specs_[f], specs_[f + 1], top);
                if (k == 0 && normalise) {
                    float pk[2] = {0.0f, 0.0f};
                    for (int s = 0; s < n; s++) {
                        pk[0] = maxf(pk[0], absf(fft_.re[s]));
                        pk[1] = maxf(pk[1], absf(fft_.im[s]));
                    }
                    for (int i = 0; i < 2; i++) gain[i] = pk[i] > 1e-9f ? 1.0f / pk[i] : 1.0f;
                }
                for (int i = 0; i < 2; i++) {
                    float *dst = f32_ + i * kWaveFrameStride + wave_mip_offset(k);
                    const float *src = i ? fft_.im : fft_.re;
                    for (int s = 0; s < n; s++) dst[s] = src[s] * gain[i];
                    dst[n] = dst[0];
                }
            }
            for (int i = 0; i < 2; i++)
                quantise(f32_ + i * kWaveFrameStride, data + (f + i) * kWaveFrameStride, scales[f + i]);
        }
        if (!normalise) normalise_whole(data, scales);
    }

private:
    // x = first + i * second, n real samples each, into their spectra (n a power of two).
    void analyse(int n, Spectrum &first, Spectrum &second) {
        fft_.run(n, false);
        first.clear();
        second.clear();
        const int top = n / 2 - 1 < kTop ? n / 2 - 1 : kTop;
        const float scale = 2.0f / float(n);
        for (int h = 1; h <= top; h++) {
            const float xr = fft_.re[h], xi = fft_.im[h];
            const float mr = fft_.re[n - h], mi = -fft_.im[n - h];    // conj(X[n - h])
            const float ar = 0.5f * (xr + mr), ai = 0.5f * (xi + mi);  // the real part's spectrum
            const float ur = xr - mr, ui = xi - mi;
            const float br = 0.5f * ui, bi = -0.5f * ur;               // -i/2 (X - conj): the imaginary part's
            first.a[h] = scale * ar;
            first.b[h] = -scale * ai;
            second.a[h] = scale * br;
            second.b[h] = -scale * bi;
        }
    }

    // Two spectra, harmonics 1..top, into n samples each: re = first, im = second.
    void synthesise(int n, const Spectrum &first, const Spectrum &second, int top) {
        for (int s = 0; s < n; s++) fft_.re[s] = fft_.im[s] = 0.0f;
        for (int h = 1; h <= top && h < n / 2; h++) {
            const float a = first.a[h], b = first.b[h], c = second.a[h], d = second.b[h];
            fft_.re[h] = 0.5f * (a + d);
            fft_.im[h] = 0.5f * (c - b);
            fft_.re[n - h] = 0.5f * (a - d);
            fft_.im[n - h] = 0.5f * (b + c);
        }
        fft_.run(n, true);
    }

    // A frame's floats as int16: the largest value over all its levels maps to 32767.
    static void quantise(const float *src, int16_t *dst, float &scale) {
        float pk = 0.0f;
        for (int s = 0; s < kWaveFrameStride; s++) pk = maxf(pk, absf(src[s]));
        scale = pk / 32767.0f;
        if (pk <= 0.0f) {
            for (int s = 0; s < kWaveFrameStride; s++) dst[s] = 0;
            return;
        }
        const float inv = 32767.0f / pk;
        for (int s = 0; s < kWaveFrameStride; s++) {
            const float v = roundf_(src[s] * inv);
            dst[s] = int16_t(v > 32767.0f ? 32767.0f : v < -32767.0f ? -32767.0f : v);
        }
    }

    // One gain for the whole table, peak 1 over every frame's level 0.
    static void normalise_whole(const int16_t *data, float *scales) {
        float pk = 0.0f;
        for (int f = 0; f < kWaveFrames; f++) {
            const int16_t *m0 = data + f * kWaveFrameStride;
            int top = 0;
            for (int s = 0; s < kWaveTopLength; s++) {
                const int v = m0[s] < 0 ? -m0[s] : m0[s];
                top = v > top ? v : top;
            }
            pk = maxf(pk, float(top) * scales[f]);
        }
        if (pk < 1e-9f) return;
        for (int f = 0; f < kWaveFrames; f++) scales[f] /= pk;
    }

    Fft fft_;
    Spectrum *specs_ = nullptr;
    Spectrum *keys_ = nullptr;
    float *f32_ = nullptr;
};

// --- the recipes, in TBL order -----------------------------------------------------------------

// n keys morphed evenly across the frames: key k sits on frame k * 15 / (keys - 1).
void morph(Builder &b, const Spectrum *keys, int count) {
    const int last = count - 1;
    for (int f = 0; f < kWaveFrames; f++) {
        const float x = float(f) * float(last) / float(kWaveFrames - 1);
        int i = enaga_floor_int(x);
        i = i > last - 1 ? last - 1 : i;
        mix_into(b.spec(f), keys[i], keys[i + 1], x - float(i));
    }
}

// Classic: sine -> triangle -> saw -> square.
bool classic(Builder &b) {
    Spectrum *keys = &b.key(0);
    make_sine(keys[0]);
    make_triangle(keys[1]);
    make_saw(keys[2]);
    make_square(keys[3]);
    morph(b, keys, 4);
    return true;
}

// PWM: duty 50% -> 4%.
bool pwm(Builder &b) {
    for (int f = 0; f < kWaveFrames; f++) make_pulse(b.spec(f), 0.5f - 0.46f * at(f, kWaveFrames));
    return true;
}

// Sync: a saw hard-synced to the fundamental, ratio 1 -> 8 (exponential).
bool sync(Builder &b) {
    b.sample([](float x, float t) {
        const float r = powf_(8.0f, x);
        return 2.0f * fracf(t * r) - 1.0f;
    });
    return true;
}

// Formant: a resonant peak sweeping harmonics 1 -> 48.
bool formant(Builder &b) {
    for (int f = 0; f < kWaveFrames; f++) make_formant(b.spec(f), powf_(48.0f, at(f, kWaveFrames)));
    return true;
}

// Reso Saw: a saw through a resonant 24 dB low-pass, cutoff harmonic 1.5 -> 96. An envelope to
// position is a filter sweep with no filter.
bool reso_saw(Builder &b) {
    Spectrum &src = b.key(0);
    make_saw(src);
    for (int f = 0; f < kWaveFrames; f++) {
        const float c = 1.5f * powf_(64.0f, at(f, kWaveFrames));
        Spectrum &s = b.spec(f);
        s.clear();
        for (int h = 1; h <= kTop; h++) {
            // Two second-order low-passes, Q 0.707 and 4: H = 1 / (1 - x^2 + i x / q) each.
            const float x = float(h) / c;
            float hr = 1.0f, hi = 0.0f;
            const float qs[2] = {0.707f, 4.0f};
            for (float q : qs) {
                const float dr = 1.0f - x * x, di = x / q;
                const float inv = 1.0f / (dr * dr + di * di);
                const float rr = dr * inv, ri = -di * inv;     // 1 / (dr + i di)
                const float nr = hr * rr - hi * ri, ni = hr * ri + hi * rr;
                hr = nr;
                hi = ni;
            }
            // a cos + b sin = Re((a - i b) e^(i w t)); times H, back to a and b.
            const float cr = src.a[h], ci = -src.b[h];
            const float yr = cr * hr - ci * hi, yi = cr * hi + ci * hr;
            s.a[h] = yr;
            s.b[h] = -yi;
        }
    }
    return true;
}

// Harmonics: additive, 1 -> 32 harmonics at 1/h^0.7, a saw's phases.
bool harmonics(Builder &b) {
    for (int f = 0; f < kWaveFrames; f++) {
        const int count = 1 + int(roundf_(31.0f * at(f, kWaveFrames)));
        Spectrum &s = b.spec(f);
        s.clear();
        for (int h = 1; h <= count; h++) s.b[h] = powf_(float(h), -0.7f) * ((h & 1) ? 1.0f : -1.0f);
    }
    return true;
}

// Fold: a sine through a triangle wavefolder, gain 1 -> 8, a touch of bias for even harmonics.
// asin(sin(pi v / 2)) * 2 / pi is a triangle of period 4 in v, written directly.
bool fold(Builder &b) {
    b.sample([](float x, float t) {
        const float gain = 1.0f + 7.0f * powf_(maxf(x, 1e-6f), 1.5f), bias = 0.15f * x;
        const float v = gain * (sinf_turns(t) + bias);
        const float u = v + 1.0f;
        const float m = u - 4.0f * floorf_(u * 0.25f);   // [0, 4)
        return 1.0f - absf(m - 2.0f);
    });
    return true;
}

// Phase Dist: a cosine through a bent phase (Casio CZ), sine -> a sharp saw.
bool phase_dist(Builder &b) {
    b.sample([](float x, float t) {
        const float d = 0.5f - 0.48f * x;
        const float ph = t < d ? 0.5f * t / d : 0.5f + 0.5f * (t - d) / (1.0f - d);
        return -cos_turns(ph);
    });
    return true;
}

// FM Ratio 1: a sine phase-modulated at 1x, index 0 -> 6.
bool fm_ratio(Builder &b) {
    b.sample([](float x, float t) { return sin_turns(t + 6.0f * x * sin_turns(t) * (1.0f / (2.0f * kPi))); });
    return true;
}

// FM Tine: 1:1 and 14:1 modulators rising, an FM electric piano's tine. Frame 0 is a pure sine,
// so an envelope to position decays the tine away.
bool fm_tine(Builder &b) {
    b.sample([](float x, float t) {
        const float k = 1.0f / (2.0f * kPi);
        return sin_turns(t + (0.6f * x * sin_turns(t) + 2.2f * x * sin_turns(14.0f * t)) * k);
    });
    return true;
}

// Digital: eight seeded random spectra morphed in turn, the stepping timbres of early digital
// wavetables. PolyWerk's seed and generator, so the same eight spectra.
bool digital(Builder &b) {
    Spectrum *keys = &b.key(0);
    uint32_t r = 0x9e3779b9u;
    auto rnd = [&r] {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return float(r >> 8) * (1.0f / 16777216.0f);
    };
    for (int k = 0; k < 8; k++) {
        const float centre = exp2(1.0f + 5.0f * rnd());   // harmonic 2..64
        const float width = 0.3f + 0.7f * rnd();          // octaves
        Spectrum &s = keys[k];
        s.clear();
        for (int h = 1; h <= 96; h++) {
            const float z = log2(float(h) / centre) / width;
            const float a = (0.3f + 0.7f * rnd()) * (0.25f + 1.5f * exp(-z * z)) / float(h);
            s.b[h] = rnd() < 0.5f ? a : -a;
        }
    }
    morph(b, keys, 8);
    return true;
}

void vowels(Builder &b, const Vowel *keys, int count, const Singer &c) {
    const int last = count - 1;
    for (int f = 0; f < kWaveFrames; f++) {
        const float x = float(f) * float(last) / float(kWaveFrames - 1);
        int i = enaga_floor_int(x);
        i = i > last - 1 ? last - 1 : i;
        sing(b.spec(f), keys[i], keys[i + 1], x - float(i), c);
    }
}

// Vowels: a tenor singing A -> E -> I -> O -> U, truest near C3.
bool vowels_tenor(Builder &b) {
    static constexpr Vowel keys[5] = {kTenorA, kTenorE, kTenorI, kTenorO, kTenorU};
    vowels(b, keys, 5, {130.8f, 1.0f, 0.03f, 1.6f});
    return true;
}

// Growl: a bass voice U -> O -> A -> E -> I, saturated, truest near C2.
bool growl(Builder &b) {
    static constexpr Vowel keys[5] = {kBassU, kBassO, kBassA, kBassE, kBassI};
    vowels(b, keys, 5, {65.4f, 0.55f, 0.12f, 1.3f});
    b.shape([](int, float v) { return tanhf_(2.2f * v); });
    return true;
}

// E-Piano: a soft 1:1 FM tone into an asymmetric saturator, both growing: mellow -> barking.
bool epiano(Builder &b) {
    b.sample([](float x, float t) {
        const float index = 0.15f + 1.6f * x, d = 1.0f + 2.5f * x, bias = 0.3f;
        const float v = sin_turns(t + index * sin_turns(t) * (1.0f / (2.0f * kPi)));
        return tanhf_(d * (v + bias)) - tanhf_(d * bias);
    });
    return true;
}

// Pluck: a string plucked near its end, bright -> dull as its high harmonics die away. One gain
// for the whole table, so the dull frames are as much quieter as a ringing string gets.
bool pluck(Builder &b) {
    for (int f = 0; f < kWaveFrames; f++) {
        const float k = 0.004f * powf_(150.0f, at(f, kWaveFrames));
        Spectrum &s = b.spec(f);
        s.clear();
        for (int h = 1; h <= kTop; h++)
            s.b[h] = sin_turns(0.5f * float(h) * 0.15f) / powf_(float(h), 1.3f) * exp(-float(h) * k);
    }
    return false;   // one gain for the table
}

// Mallet: struck bars, soft mallet -> marimba -> vibraphone -> xylophone -> glockenspiel ->
// kalimba, their partials rounded to harmonics, each at a golden-ratio phase so the peaks never
// all line up.
struct Partial {
    int h;
    float amp;
};
void sparse(Spectrum &s, const Partial *ps, int count) {
    s.clear();
    for (int i = 0; i < count; i++) {
        const float ph = fracf(0.6180339887f * float(i));
        s.a[ps[i].h] += ps[i].amp * sin_turns(ph);
        s.b[ps[i].h] += ps[i].amp * cos_turns(ph);
    }
}
bool mallet(Builder &b) {
    Spectrum *keys = &b.key(0);
    static constexpr Partial k0[] = {{1, 1.0f}, {4, 0.12f}};
    static constexpr Partial k1[] = {{1, 1.0f}, {4, 0.35f}, {10, 0.12f}};
    static constexpr Partial k2[] = {{1, 1.0f}, {4, 0.55f}, {10, 0.3f}};
    static constexpr Partial k3[] = {{1, 1.0f}, {3, 0.55f}, {6, 0.35f}, {10, 0.2f}};
    static constexpr Partial k4[] = {{1, 1.0f}, {3, 0.35f}, {5, 0.45f}, {9, 0.3f}, {14, 0.12f}};
    static constexpr Partial k5[] = {{1, 1.0f}, {2, 0.08f}, {6, 0.35f}, {13, 0.25f}};
    sparse(keys[0], k0, 2);
    sparse(keys[1], k1, 3);
    sparse(keys[2], k2, 3);
    sparse(keys[3], k3, 4);
    sparse(keys[4], k4, 5);
    sparse(keys[5], k5, 4);
    morph(b, keys, 6);
    return true;
}

// The recipe returns whether each frame is normalised on its own (false: one gain for the
// table).
struct Recipe {
    const char *short_name;
    const char *long_name;
    bool (*make)(Builder &);
};
constexpr Recipe kRecipes[kWaveTables] = {
    {"CLS", "CLASSIC", classic},     {"PWM", "PWM", pwm},
    {"SYN", "SYNC", sync},           {"FMT", "FORMANT", formant},
    {"RSW", "RESO SAW", reso_saw},   {"HRM", "HARMONICS", harmonics},
    {"FLD", "FOLD", fold},           {"PHD", "PHASE DIST", phase_dist},
    {"FM1", "FM RATIO", fm_ratio},   {"TIN", "FM TINE", fm_tine},
    {"DGT", "DIGITAL", digital},     {"VOW", "VOWELS", vowels_tenor},
    {"GRL", "GROWL", growl},         {"EPN", "E-PIANO", epiano},
    {"PLK", "PLUCK", pluck},         {"MLT", "MALLET", mallet},
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

const WaveLibrary *g_library = nullptr;
Builder g_builder;   // its scratch is taken by the one init() that builds

}  // namespace

bool WaveLibrary::init(void *scratch, uint32_t scratch_bytes) {
    if (data_ != nullptr) return true;
    int16_t *data = new (std::nothrow) int16_t[kWaveLibraryBytes / sizeof(int16_t)];
    if (data == nullptr) return false;
    // Borrowed memory when it is big enough (Engine::init lends the capture buffer, idle until
    // the first take); else the heap, where it stays: Enaga's heap is never freed into.
    Arena arena;
    if (scratch != nullptr && scratch_bytes >= wave_build_scratch_bytes() + 64u) {
        arena.at = static_cast<uint8_t *>(scratch);
        arena.left = scratch_bytes;
    }
    if (!g_builder.init(arena)) return false;
    Builder &builder = g_builder;
    for (int t = 0; t < kWaveTables; t++) {
        const bool normalise = kRecipes[t].make(builder);
        builder.store(data + t * kWaveFrames * kWaveFrameStride, scale_[t], normalise);
    }
    // Lent memory goes back as it came: silent (the capture buffer is zeroed at its init).
    if (scratch != nullptr && arena.at != nullptr) {
        uint8_t *used = static_cast<uint8_t *>(scratch);
        for (uint8_t *p = used; p < arena.at; p++) *p = 0;
    }
    data_ = data;
    return true;
}

const int16_t *WaveLibrary::frame(int table, int frame, int mip) const {
    table = clampi(table, 0, kWaveTables - 1);
    frame = clampi(frame, 0, kWaveFrames - 1);
    mip = clampi(mip, 0, kWaveMipLevels - 1);
    return data_ + (table * kWaveFrames + frame) * kWaveFrameStride + wave_mip_offset(mip);
}

float WaveLibrary::scale(int table, int frame) const {
    return scale_[clampi(table, 0, kWaveTables - 1)][clampi(frame, 0, kWaveFrames - 1)];
}

const char *WaveLibrary::short_name(int table) {
    return table >= 0 && table < kWaveTables ? kRecipes[table].short_name : "---";
}
const char *WaveLibrary::long_name(int table) {
    return table >= 0 && table < kWaveTables ? kRecipes[table].long_name : "---";
}

uint32_t wave_build_scratch_bytes() {
    // Fft: re, im and the two twiddle halves. Builder: the frames' and the keys' spectra and the
    // float copy of two frames. Borrowed from the capture buffer, which is idle at init
    // (Engine::init); only taken from the heap when nothing is lent.
    return uint32_t(sizeof(float)) * (2u * kFftMax + kFftMax) +
           uint32_t(sizeof(Spectrum)) * (kWaveFrames + Builder::kKeys) +
           uint32_t(sizeof(float)) * 2u * kWaveFrameStride;
}

const WaveLibrary *wave_library() { return g_library; }
void set_wave_library(const WaveLibrary *library) { g_library = library; }

}  // namespace strata
