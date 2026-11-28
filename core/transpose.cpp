#include "transpose.hpp"

#include "project.hpp"   // machine_folds_note
#include "scale.hpp"     // note_name

namespace strata {

namespace {

void put(char *&out, char *end, char c) {
    if (out < end) *out++ = c;
}

void put_text(char *&out, char *end, const char *text) {
    while (*text != 0) put(out, end, *text++);
}

void put_signed(char *&out, char *end, int value) {
    if (value == 0) {
        put(out, end, '0');
        return;
    }
    put(out, end, value < 0 ? '-' : '+');
    const int magnitude = value < 0 ? -value : value;
    if (magnitude >= 10) put(out, end, char('0' + magnitude / 10));
    put(out, end, char('0' + magnitude % 10));
}

}  // namespace

int8_t transpose_turn(int8_t current, int clicks) {
    int next = int(current) + clicks;
    if (next < -kTransposeRange) next = -kTransposeRange;
    if (next > kTransposeRange) next = kTransposeRange;
    return int8_t(next);
}

bool transpose_moves(uint8_t machine, int slices) { return machine_folds_note(machine, slices); }

uint8_t transpose_note(uint8_t note, int semis) {
    const int moved = int(note & 0x7Fu) + semis;
    return uint8_t(moved < 0 ? 0 : (moved > 127 ? 127 : moved));
}

uint8_t transpose_root(uint8_t root, int semis) {
    // Two octaves of headroom keep the sum positive for any root and any semis in the range;
    // a damaged root (over 11) is taken as its pitch class first.
    return uint8_t((int(root % 12) + semis + 24) % 12);
}

void transpose_text(int8_t semis, uint8_t root, char *out, int max) {
    if (max <= 0) return;
    char *end = out + max - 1;
    put_text(out, end, "TRANSPOSE ");
    const int8_t held = transpose_turn(semis, 0);
    put_signed(out, end, held);
    if (held != 0) {
        put(out, end, ' ');
        put_text(out, end, note_name(transpose_root(root, held)));
    }
    *out = 0;
}

void transpose_short(int8_t semis, char *out, int max) {
    if (max <= 0) return;
    char *end = out + max - 1;
    const int8_t held = transpose_turn(semis, 0);
    if (held != 0) {
        put_text(out, end, "TR");
        put_signed(out, end, held);
    }
    *out = 0;
}

}  // namespace strata
