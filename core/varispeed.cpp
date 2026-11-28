#include "varispeed.hpp"

namespace strata {

namespace {

// 2^(s / 12), s = 0 .. 12, and 2^(t / 120), t = 0 .. 9.
constexpr float kSemitone[13] = {
    1.000000000f, 1.059463094f, 1.122462048f, 1.189207115f, 1.259921050f,
    1.334839854f, 1.414213562f, 1.498307077f, 1.587401052f, 1.681792831f,
    1.781797436f, 1.887748625f, 2.000000000f,
};
constexpr float kTenth[10] = {
    1.000000000f, 1.005792941f, 1.011619440f, 1.017479692f, 1.023373892f,
    1.029302237f, 1.035264924f, 1.041262153f, 1.047294123f, 1.053361036f,
};

int held(int tenths) {
    return tenths < -kVariRange ? -kVariRange : (tenths > kVariRange ? kVariRange : tenths);
}

}  // namespace

int8_t vari_turn(int8_t current, int clicks) {
    const int from = held(current);
    int to = from + clicks;
    // The detent: crossing 0 in either direction lands on it.
    if ((from > 0 && to < 0) || (from < 0 && to > 0)) to = 0;
    return int8_t(held(to));
}

float vari_ratio(int8_t tenths) {
    const int t = held(tenths);
    // t = 10 s + f with f in 0..9, s in -12..12: floor, written for a negative t without `/`.
    const int s = t >= 0 ? t / 10 : -((-t + 9) / 10);
    const int f = t - 10 * s;
    // A negative s is the octave below times 2^((s + 12) / 12).
    const float semis = s >= 0 ? kSemitone[s] : 0.5f * kSemitone[s + 12];
    return semis * kTenth[f];
}

float vari_semis(int8_t tenths) { return float(held(tenths)) * 0.1f; }

int8_t vari_fit(int8_t tenths, uint32_t base_centi, uint32_t min_centi, uint32_t max_centi) {
    int t = held(tenths);
    const float base = float(base_centi);
    // At most kVariRange steps, on a knob click: control rate.
    while (t != 0) {
        const float moved = base * vari_ratio(int8_t(t));
        if (moved >= float(min_centi) && moved <= float(max_centi)) break;
        t += t > 0 ? -1 : 1;
    }
    return int8_t(t);
}

uint32_t vari_tempo_tenths(uint32_t base_tenths, int8_t tenths) {
    if (held(tenths) == 0) return base_tenths;
    return uint32_t(float(base_tenths) * vari_ratio(tenths) + 0.5f);
}

void vari_text(int8_t tenths, char *out, int max) {
    if (out == nullptr || max <= 0) return;
    char text[kVariTextMax];
    char *p = text;
    const int t = held(tenths);
    if (t == 0) {
        *p++ = '0';
    } else {
        *p++ = t > 0 ? '+' : '-';
        const int a = t > 0 ? t : -t;
        if (a >= 100) *p++ = char('0' + a / 100);
        *p++ = char('0' + a / 10 % 10);
        *p++ = '.';
        *p++ = char('0' + a % 10);
    }
    *p = 0;
    int i = 0;
    for (; i < max - 1 && text[i] != 0; i++) out[i] = text[i];
    out[i] = 0;
}

}  // namespace strata
