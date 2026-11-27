// Factory arps. core/arppresets.hpp says what a preset is and what it writes.
//
// The bytes sit in the middle of their zone (core/arp.hpp):
//   MOD  32 UP, 53 DN, 75 U/D, 96 ORD, 117 RND
//   RAT  9 1/4, 28 1/8, 46 1/8T, 64 1/16, 82 1/16T, 100 1/32, 119 1/32T
//   OCT  16 one octave, 48 two, 80 three, 112 four
//   GAT  0 50%, 26 60%, 51 70%, 64 75%, 103 90%, 127 99%
// and the kit settings are core/arp.cpp's table indices:
//   VAR      0 OFF, 1 CONV, 2 DIVG, 3 THUMB, 4 PINKY
//   RHYTHM   0 ALL, 1 X-, 2 XX-, 3 XXX-, 4 X-XX, 5 332, 6 E5/8, 7 E5/16, 8 E7/16, 9 E9/16,
//            10 -X, 11 X--
//   ACCENT   0 OFF, 1 X-, 2 X--, 3 X---, 4 X----, 5 332, 6 --X-, 7 CYC
//   RATCHET  0 OFF, 1 2:4, 2 2:8, 3 3:8, 4 4:16, 5 2ALL, 6 RND2, 7 RND3
// tests/test_arp.cpp checks each preset's bytes land in the zone its comment names.
#include "arppresets.hpp"

namespace strata {

namespace {

constexpr ArpPreset kPresets[] = {
    // The classic: up through a chord over two octaves at 1/16, an accent on every beat. A pad
    // chord on VA POLY becomes a trance arp.
    {"TRANCE", {32, 64, 48, 26}, {0, 0, 3, 0}},
    // Rolling techno: up and down, two hits and a rest, accented in threes. The rest holds the
    // note, so the melody turns against the bar every pass.
    {"ROLLER", {75, 64, 16, 64}, {0, 2, 2, 0}},
    // An acid line from one held chord: the notes in the order played, long gates that let a
    // VA MONO glide, a skipped 16th and the 3-3-2 accent the filter bites on.
    {"ACID", {96, 64, 16, 103}, {0, 4, 5, 0}},
    // Bleep techno: random notes over three octaves, short, five hits in eight, and now and
    // then a double. Best on PERC's BLIP or an FM bell.
    {"BLEEP", {117, 64, 80, 0}, {0, 6, 0, 6}},
    // Dub techno: a chord walked at 1/8 with held gates on the tresillo, the first note of each
    // pass accented. Put the delay send up.
    {"DUB", {32, 28, 16, 127}, {0, 5, 7, 0}},
    // The offbeat: 1/8 on the "and" only. One note on a bass track is the classic offbeat bass.
    {"OFFBT", {96, 28, 16, 90}, {0, 10, 0, 0}},
    // A thumb pedal: the lowest note between every two others, up over two octaves.
    {"THUMB", {32, 64, 48, 26}, {3, 0, 7, 0}},
    // A pinky pedal: down, with the top note ringing after each.
    {"PINKY", {53, 64, 16, 26}, {4, 0, 0, 0}},
    // Converging triplets: the outside notes first, closing in, over two octaves at 1/16T.
    {"CONVRG", {75, 82, 48, 51}, {1, 0, 7, 0}},
    // A ratchet on the last 16th of every beat, the beat accented: a stuttering drive.
    {"RATCHT", {32, 64, 16, 0}, {0, 0, 3, 1}},
    // A fill: an up arp with a four-hit roll on the last 16th of each bar.
    {"FILL", {32, 64, 48, 0}, {0, 0, 0, 4}},
    // Euclidean: the played order over two octaves, seven hits in sixteen, accented in fives.
    {"EUCLID", {96, 64, 48, 64}, {0, 8, 4, 0}},
    // Ambient: quarter notes from the middle out over four octaves, every note held to the next.
    // Slow bells on an FM or WAVE POLY pad, under a long reverb.
    {"AMBNT", {32, 9, 112, 127}, {2, 0, 0, 0}},
    // A drift: random 1/8 triplets over three octaves, five in sixteen, held. Sparse and never
    // the same twice in a bar, the same every pass.
    {"DRIFT", {117, 46, 80, 127}, {0, 7, 0, 0}},
};

constexpr int kCount = int(sizeof kPresets / sizeof kPresets[0]);

}  // namespace

int arp_preset_count() { return kCount; }

const ArpPreset *arp_preset(int index) {
    return index >= 0 && index < kCount ? &kPresets[index] : nullptr;
}

bool arp_preset_apply(int index, uint8_t *arp_bytes, KitArp &ext) {
    const ArpPreset *p = arp_preset(index);
    if (p == nullptr || arp_bytes == nullptr) return false;
    for (int i = 0; i < kArpBytes; i++) arp_bytes[i] = p->bytes[i];
    ext = kit_arp_sanitized(p->ext);
    return true;
}

bool arp_preset_matches(int index, const uint8_t *arp_bytes, const KitArp &ext) {
    const ArpPreset *p = arp_preset(index);
    if (p == nullptr || arp_bytes == nullptr) return false;
    for (int i = 0; i < kArpBytes; i++) {
        if (arp_bytes[i] != p->bytes[i]) return false;
    }
    for (int f = 0; f < kArpExtFields; f++) {
        if (arp_ext_get(ext, f) != arp_ext_get(p->ext, f)) return false;
    }
    return true;
}

}  // namespace strata
