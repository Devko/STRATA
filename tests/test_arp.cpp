// The arpeggiator's note logic (core/arp.hpp): the bytes, the cycles, the hits.
#include "check.hpp"

#include "arp.hpp"
#include "arppresets.hpp"

#include <cstring>

using namespace strata;

namespace {

ArpSettings settings(ArpMode mode, int octaves) {
    ArpSettings s;
    s.mode = mode;
    s.octaves = uint8_t(octaves);
    return s;
}

}  // namespace

TEST(arp_zeroed_bytes_are_off_and_sensible) {
    const uint8_t zero[kArpBytes] = {};
    const ArpSettings s = ArpSettings::make(zero);
    CHECK(!s.on());
    CHECK_EQ(int(s.rate_ticks), 96);
    CHECK_EQ(int(s.octaves), 1);
    CHECK_EQ(int(s.gate_percent), 50);
    uint8_t out[kArpMaxCycle];
    const uint8_t notes[3] = {60, 64, 67};
    CHECK_EQ(arp_cycle(notes, 3, s, out), 0);
}

TEST(arp_bytes_cover_every_zone_and_end_at_the_top) {
    // Every mode, rate and octave is reachable, in order, and 127 is the last of each.
    int last_mode = -1, last_rate = -1, last_oct = 0;
    for (int b = 0; b <= 127; b++) {
        const int m = arp_mode_index(uint8_t(b));
        const int r = arp_rate_index(uint8_t(b));
        const int o = arp_octaves(uint8_t(b));
        CHECK(m == last_mode || m == last_mode + 1);
        CHECK(r == last_rate || r == last_rate + 1);
        CHECK(o == last_oct || o == last_oct + 1);
        last_mode = m;
        last_rate = r;
        last_oct = o;
    }
    CHECK_EQ(last_mode, int(ArpMode::count) - 1);
    CHECK_EQ(last_rate, kArpRates - 1);
    CHECK_EQ(last_oct, kArpMaxOctaves);
    CHECK_EQ(arp_gate_percent(127), 99);
    // Slow to fast.
    for (int i = 1; i < kArpRates; i++) CHECK(arp_rate_ticks(i) < arp_rate_ticks(i - 1));
    CHECK(std::strcmp(arp_rate_name(0), "1/4") == 0);
    CHECK(std::strcmp(arp_rate_name(127), "1/32T") == 0);
    CHECK(std::strcmp(arp_mode_name(0), "OFF") == 0);
    CHECK(std::strcmp(arp_mode_name(127), "RND") == 0);
    // Out-of-range bytes clamp rather than read past a table.
    CHECK_EQ(arp_mode_index(255), int(ArpMode::count) - 1);
}

TEST(arp_cycles_up_down_updown_order_over_octaves) {
    const uint8_t played[3] = {67, 60, 64};     // played out of order
    uint8_t out[kArpMaxCycle];

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::up, 1), out), 3);
    CHECK_EQ(int(out[0]), 60);
    CHECK_EQ(int(out[1]), 64);
    CHECK_EQ(int(out[2]), 67);

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::up, 2), out), 6);
    CHECK_EQ(int(out[3]), 72);
    CHECK_EQ(int(out[5]), 79);

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::down, 1), out), 3);
    CHECK_EQ(int(out[0]), 67);
    CHECK_EQ(int(out[2]), 60);

    // Up and down without repeating either end: 60 64 67 64, then 60 again.
    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::up_down, 1), out), 4);
    CHECK_EQ(int(out[3]), 64);

    CHECK_EQ(arp_cycle(played, 3, settings(ArpMode::order, 1), out), 3);
    CHECK_EQ(int(out[0]), 67);
    CHECK_EQ(int(out[1]), 60);

    // One note over four octaves: a classic octave arp.
    const uint8_t one[1] = {48};
    CHECK_EQ(arp_cycle(one, 1, settings(ArpMode::up_down, 4), out), 6);
    CHECK_EQ(int(out[0]), 48);
    CHECK_EQ(int(out[3]), 84);
    CHECK_EQ(int(out[4]), 72);
    CHECK_EQ(int(out[5]), 60);
    // A single note, one octave, up/down: just the note.
    CHECK_EQ(arp_cycle(one, 1, settings(ArpMode::up_down, 1), out), 1);

    // Notes past 127 are dropped, never wrapped.
    const uint8_t high[1] = {120};
    CHECK_EQ(arp_cycle(high, 1, settings(ArpMode::up, 4), out), 1);

    // More than eight inputs are capped, and the cycle fits.
    uint8_t many[12];
    for (int i = 0; i < 12; i++) many[i] = uint8_t(40 + i);
    CHECK_EQ(arp_cycle(many, 12, settings(ArpMode::up_down, 4), out), 2 * 32 - 2);
}

TEST(arp_hits_walk_the_cycle_and_random_repeats_per_seed) {
    const uint8_t cycle[4] = {60, 64, 67, 72};
    for (uint32_t h = 0; h < 12; h++) CHECK_EQ(int(arp_note(cycle, 4, ArpMode::up, h, 0)), int(cycle[h % 4]));

    // Random: always a note of the cycle, the same for the same seed and hit, and not one note.
    bool seen[4] = {};
    for (uint32_t h = 0; h < 64; h++) {
        const uint8_t a = arp_note(cycle, 4, ArpMode::random, h, 1234);
        CHECK_EQ(int(a), int(arp_note(cycle, 4, ArpMode::random, h, 1234)));
        for (int i = 0; i < 4; i++) seen[i] = seen[i] || a == cycle[i];
    }
    for (int i = 0; i < 4; i++) CHECK(seen[i]);
    int differ = 0;
    for (uint32_t h = 0; h < 32; h++) {
        differ += arp_note(cycle, 4, ArpMode::random, h, 1) != arp_note(cycle, 4, ArpMode::random, h, 2);
    }
    CHECK(differ > 8);
}

// --- the kit settings: VAR, RHYTHM, ACCENT, RATCHET ---------------------------------------------

TEST(arp_var_off_is_the_walk_as_it_was) {
    const uint8_t played[3] = {67, 60, 64};
    uint8_t a[kArpMaxCycle], b[kArpMaxCycle];
    for (int m = 1; m < int(ArpMode::count); m++) {
        ArpSettings s = settings(ArpMode(m), 2);
        const int n = arp_cycle(played, 3, s, a);
        s.var = ArpVar::off;
        CHECK_EQ(arp_cycle(played, 3, s, b), n);
        CHECK(std::memcmp(a, b, size_t(n)) == 0);
    }
}

TEST(arp_var_converge_and_diverge_close_in_and_open_out) {
    const uint8_t chord[4] = {60, 64, 67, 71};
    uint8_t out[kArpMaxCycle];
    ArpSettings s = settings(ArpMode::up, 1);
    s.var = ArpVar::converge;
    CHECK_EQ(arp_cycle(chord, 4, s, out), 4);
    CHECK_EQ(int(out[0]), 60);
    CHECK_EQ(int(out[1]), 71);
    CHECK_EQ(int(out[2]), 64);
    CHECK_EQ(int(out[3]), 67);
    // An odd count ends on the middle note.
    CHECK_EQ(arp_cycle(chord, 3, s, out), 3);
    CHECK_EQ(int(out[2]), 64);
    s.var = ArpVar::diverge;
    CHECK_EQ(arp_cycle(chord, 4, s, out), 4);
    CHECK_EQ(int(out[0]), 67);
    CHECK_EQ(int(out[1]), 64);
    CHECK_EQ(int(out[2]), 71);
    CHECK_EQ(int(out[3]), 60);
    // On DOWN the walk starts high, so converge does too.
    s = settings(ArpMode::down, 1);
    s.var = ArpVar::converge;
    arp_cycle(chord, 4, s, out);
    CHECK_EQ(int(out[0]), 71);
    CHECK_EQ(int(out[1]), 60);
}

TEST(arp_var_thumb_and_pinky_put_a_pedal_between_the_notes) {
    const uint8_t chord[3] = {64, 60, 67};
    uint8_t out[kArpMaxCycle];
    ArpSettings s = settings(ArpMode::up, 1);
    s.var = ArpVar::thumb;
    CHECK_EQ(arp_cycle(chord, 3, s, out), 4);         // C E C G
    CHECK_EQ(int(out[0]), 60);
    CHECK_EQ(int(out[1]), 64);
    CHECK_EQ(int(out[2]), 60);
    CHECK_EQ(int(out[3]), 67);
    s.var = ArpVar::pinky;
    CHECK_EQ(arp_cycle(chord, 3, s, out), 4);         // C G E G
    CHECK_EQ(int(out[0]), 60);
    CHECK_EQ(int(out[1]), 67);
    CHECK_EQ(int(out[2]), 64);
    CHECK_EQ(int(out[3]), 67);
    // One note on one octave is all pedal: it plays as it is, not as nothing.
    const uint8_t one[1] = {48};
    CHECK_EQ(arp_cycle(one, 1, s, out), 1);
    CHECK_EQ(int(out[0]), 48);
    // The largest walk, doubled, still fits the cycle.
    uint8_t many[8];
    for (int i = 0; i < 8; i++) many[i] = uint8_t(30 + i);
    s = settings(ArpMode::up_down, 4);
    s.var = ArpVar::thumb;
    const int n = arp_cycle(many, 8, s, out);
    CHECK(n > 2 * 32 && n <= kArpMaxCycle);
}

TEST(arp_rhythms_rest_on_their_slots_and_all_plays_every_one) {
    for (uint32_t slot = 0; slot < 64; slot++) CHECK(arp_rhythm_plays(0, slot));
    // X- : every other.
    CHECK(arp_rhythm_plays(1, 0));
    CHECK(!arp_rhythm_plays(1, 1));
    CHECK(arp_rhythm_plays(1, 2));
    // XX- : two and a rest, round again.
    CHECK(arp_rhythm_plays(2, 3) && arp_rhythm_plays(2, 4) && !arp_rhythm_plays(2, 5));
    // Every rhythm but ALL rests somewhere and plays somewhere, inside 16 slots.
    for (int r = 1; r < kArpRhythms; r++) {
        int plays = 0;
        for (uint32_t slot = 0; slot < 48; slot++) plays += arp_rhythm_plays(uint8_t(r), slot);
        CHECK(plays > 0 && plays < 48);
    }
    // The tresillo, 332: hits on 0 3 6 of eight.
    int hits = 0;
    for (uint32_t slot = 0; slot < 8; slot++) hits += arp_rhythm_plays(5, slot) ? 1 << slot : 0;
    CHECK_EQ(hits, 0x49);
    // Damage reads as ALL.
    CHECK(arp_rhythm_plays(200, 1));
}

TEST(arp_accents_fall_on_their_slots_and_cyc_on_each_cycles_first_note) {
    for (uint32_t slot = 0; slot < 32; slot++) CHECK(!arp_accent_on(0, slot, slot, 4));
    CHECK(arp_accent_on(2, 0, 0, 4) && !arp_accent_on(2, 1, 1, 4) && arp_accent_on(2, 3, 3, 4));
    // CYC: the note count, not the slot.
    const uint8_t cyc = kArpAccents - 1;
    CHECK(std::strcmp(arp_ext_name(2, cyc), "CYC") == 0);
    CHECK(arp_accent_on(cyc, 7, 0, 3));
    CHECK(!arp_accent_on(cyc, 0, 1, 3));
    CHECK(arp_accent_on(cyc, 1, 3, 3));
    CHECK(!arp_accent_on(cyc, 0, 0, 0));
    CHECK(!arp_accent_on(99, 0, 0, 4));
}

TEST(arp_ratchets_land_on_their_slots_and_chance_repeats_per_seed) {
    for (uint32_t slot = 0; slot < 32; slot++) CHECK_EQ(arp_ratchet_count(0, slot, 7), 1);
    // 2:4 -- the last of every four.
    CHECK_EQ(arp_ratchet_count(1, 0, 0), 1);
    CHECK_EQ(arp_ratchet_count(1, 3, 0), 2);
    CHECK_EQ(arp_ratchet_count(1, 7, 0), 2);
    CHECK_EQ(arp_ratchet_count(3, 7, 0), 3);
    CHECK_EQ(arp_ratchet_count(4, 15, 0), 4);
    CHECK_EQ(arp_ratchet_count(4, 14, 0), 1);
    CHECK_EQ(arp_ratchet_count(5, 9, 0), 2);
    // Chance: about a quarter of the slots, the same for the same seed, 2 on RND2, 2 or 3 on RND3.
    int ratcheted = 0, threes = 0;
    for (uint32_t slot = 0; slot < 400; slot++) {
        const int a = arp_ratchet_count(6, slot, 99);
        CHECK_EQ(a, arp_ratchet_count(6, slot, 99));
        CHECK(a == 1 || a == 2);
        ratcheted += a > 1;
        const int b = arp_ratchet_count(7, slot, 99);
        CHECK(b >= 1 && b <= 3);
        threes += b == 3;
    }
    CHECK(ratcheted > 60 && ratcheted < 140);
    CHECK(threes > 10);
    for (int r = 0; r < kArpRatchets; r++) {
        for (uint32_t slot = 0; slot < 32; slot++) CHECK(arp_ratchet_count(uint8_t(r), slot, slot) <= kArpMaxRatchet);
    }
}

TEST(arp_kit_settings_sanitize_name_and_round_trip_by_field) {
    KitArp a;
    a.var = 200;
    a.rhythm = kArpRhythms;
    a.accent = 3;
    a.ratchet = 255;
    const KitArp s = kit_arp_sanitized(a);
    CHECK_EQ(int(s.var), 0);
    CHECK_EQ(int(s.rhythm), 0);
    CHECK_EQ(int(s.accent), 3);
    CHECK_EQ(int(s.ratchet), 0);
    KitArp b;
    for (int f = 0; f < kArpExtFields; f++) {
        CHECK(arp_ext_count(f) >= 5);
        CHECK(std::strlen(arp_ext_label(f)) == 3);
        for (int v = 0; v < arp_ext_count(f); v++) {
            arp_ext_set(b, f, uint8_t(v));
            CHECK_EQ(int(arp_ext_get(b, f)), v);
            const char *name = arp_ext_name(f, uint8_t(v));
            CHECK(name != nullptr && std::strlen(name) >= 1 && std::strlen(name) <= 5);
            // Unique within the field.
            for (int w = 0; w < v; w++) CHECK(std::strcmp(arp_ext_name(f, uint8_t(w)), name) != 0);
        }
        arp_ext_set(b, f, 250);
        CHECK_EQ(int(arp_ext_get(b, f)), 0);
        CHECK(std::strcmp(arp_ext_name(f, 250), arp_ext_name(f, 0)) == 0);
    }
}

// RND's notes and RND2/RND3's ratchets are drawn from the seed: a saved project plays the same
// draws after an update, so they are pinned here, hit by hit.
TEST(arp_random_draws_are_the_same_in_every_build) {
    const char *rnd3_seed5 = "1111131113111121213111111112231112113121111111111113311111121111";
    const char *rnd3_seed99 = "1111311212131111311112211121111111121131113321113111111321122111";
    for (uint32_t slot = 0; slot < 64; slot++) {
        CHECK_EQ(arp_ratchet_count(7, slot, 5), rnd3_seed5[slot] - '0');
        CHECK_EQ(arp_ratchet_count(7, slot, 99), rnd3_seed99[slot] - '0');
        // RND2 ratchets the same slots, always into two.
        const int two = arp_ratchet_count(6, slot, 99);
        CHECK_EQ(two, rnd3_seed99[slot] == '1' ? 1 : 2);
    }
    const uint8_t cycle[5] = {60, 62, 64, 65, 67};
    const int drawn[24] = {0, 4, 2, 7, 0, 4, 5, 7, 4, 0, 7, 0, 4, 7, 5, 0, 0, 4, 4, 7, 7, 7, 7, 2};
    for (uint32_t h = 0; h < 24; h++) CHECK_EQ(int(arp_note(cycle, 5, ArpMode::random, h, 3)) - 60, drawn[h]);
}

// Every table's edge: one past the last entry is off (or plays, for a rhythm), and so is an
// unknown field.
TEST(arp_kit_settings_one_past_each_table_is_off) {
    KitArp a;
    a.var = kArpVars;
    a.rhythm = kArpRhythms;
    a.accent = kArpAccents;
    a.ratchet = kArpRatchets;
    const KitArp s = kit_arp_sanitized(a);
    CHECK_EQ(int(s.var), 0);
    CHECK_EQ(int(s.rhythm), 0);
    CHECK_EQ(int(s.accent), 0);
    CHECK_EQ(int(s.ratchet), 0);
    a.var = kArpVars - 1;
    a.accent = kArpAccents - 1;
    a.ratchet = kArpRatchets - 1;
    const KitArp t = kit_arp_sanitized(a);
    CHECK_EQ(int(t.var), kArpVars - 1);
    CHECK_EQ(int(t.accent), kArpAccents - 1);
    CHECK_EQ(int(t.ratchet), kArpRatchets - 1);
    for (int f = 0; f < kArpExtFields; f++) {
        KitArp b;
        arp_ext_set(b, f, uint8_t(arp_ext_count(f) - 1));
        CHECK_EQ(int(arp_ext_get(b, f)), arp_ext_count(f) - 1);
        arp_ext_set(b, f, uint8_t(arp_ext_count(f)));
        CHECK_EQ(int(arp_ext_get(b, f)), 0);
        CHECK(std::strcmp(arp_ext_name(f, uint8_t(arp_ext_count(f))), arp_ext_name(f, 0)) == 0);
    }
    KitArp c;
    c.var = 3;
    CHECK_EQ(arp_ext_count(kArpExtFields), 1);
    CHECK_EQ(int(arp_ext_get(c, kArpExtFields)), 0);
    for (uint32_t slot = 0; slot < 16; slot++) {
        CHECK(arp_rhythm_plays(kArpRhythms, slot));
        CHECK(!arp_accent_on(kArpAccents, slot, 0, 4));
        CHECK_EQ(arp_ratchet_count(kArpRatchets, slot, 0), 1);
        // 2:8 is a fixed slot, never drawn: the eighth of every eight, twice.
        CHECK_EQ(arp_ratchet_count(2, slot, slot * 7u + 1u), slot % 8 == 7 ? 2 : 1);
    }
    // RAT's ticks clamp at both ends.
    CHECK_EQ(int(arp_rate_ticks(-1)), 96);
    CHECK_EQ(int(arp_rate_ticks(kArpRates)), 8);
    CHECK_EQ(int(arp_rate_ticks(kArpRates + 5)), 8);
    CHECK_EQ(int(arp_rate_ticks(kArpRates - 2)), 12);
    // GATE from its byte.
    const uint8_t bytes[kArpBytes] = {30, 0, 0, 127};
    CHECK_EQ(int(ArpSettings::make(bytes).gate_percent), 99);
}

TEST(arp_cycle_edges_octaves_top_note_and_short_cycles) {
    uint8_t out[kArpMaxWalk];
    const uint8_t notes[3] = {60, 64, 67};
    uint8_t one[kArpMaxWalk];
    const int n1 = arp_cycle(notes, 3, settings(ArpMode::up, 1), one);
    // OCT below 1 plays one octave; above the most plays the most.
    CHECK_EQ(arp_cycle(notes, 3, settings(ArpMode::up, 0), out), n1);
    for (int i = 0; i < n1; i++) CHECK_EQ(int(out[i]), int(one[i]));
    uint8_t most[kArpMaxWalk];
    const int nm = arp_cycle(notes, 3, settings(ArpMode::up, kArpMaxOctaves), most);
    CHECK_EQ(arp_cycle(notes, 3, settings(ArpMode::up, kArpMaxOctaves + 3), out), nm);
    for (int i = 0; i < nm; i++) CHECK_EQ(int(out[i]), int(most[i]));
    // 127 is played; an octave above it is not.
    const uint8_t top[2] = {115, 127};
    CHECK_EQ(arp_cycle(top, 2, settings(ArpMode::up, 2), out), 3);
    CHECK_EQ(int(out[0]), 115);
    CHECK_EQ(int(out[1]), 127);
    CHECK_EQ(int(out[2]), 127);
    // THUMB finds the lowest note wherever ORD puts it.
    ArpSettings thumb = settings(ArpMode::order, 1);
    thumb.var = ArpVar::thumb;
    const uint8_t ord[3] = {64, 60, 67};
    CHECK_EQ(arp_cycle(ord, 3, thumb, out), 4);
    const int want[4] = {60, 64, 60, 67};
    for (int i = 0; i < 4; i++) CHECK_EQ(int(out[i]), want[i]);
    // A cycle of one note is that note; none (or no cycle) is middle C.
    const uint8_t c1[1] = {72};
    CHECK_EQ(int(arp_note(c1, 1, ArpMode::up, 5, 0)), 72);
    CHECK_EQ(int(arp_note(c1, 0, ArpMode::up, 5, 0)), 60);
    CHECK_EQ(int(arp_note(nullptr, 4, ArpMode::up, 5, 0)), 60);
}

// --- the factory arps (core/arppresets.hpp) ------------------------------------------------------

TEST(arp_presets_are_named_drawably_and_uniquely_and_each_is_an_arp) {
    CHECK(arp_preset_count() >= 10);
    CHECK(arp_preset_count() <= 15);                  // AS WAS and at most two screens of rows
    CHECK(arp_preset(-1) == nullptr);
    CHECK(arp_preset(arp_preset_count()) == nullptr);
    for (int i = 0; i < arp_preset_count(); i++) {
        const ArpPreset *p = arp_preset(i);
        CHECK(p != nullptr && p->name != nullptr);
        const int length = int(std::strlen(p->name));
        CHECK(length >= 1 && length <= kArpPresetNameChars);
        for (const char *c = p->name; *c; c++) CHECK((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'));
        for (int j = 0; j < i; j++) CHECK(std::strcmp(arp_preset(j)->name, p->name) != 0);
        // Every preset is ON, its bytes knob bytes, its settings in their tables.
        for (int b = 0; b < kArpBytes; b++) CHECK(p->bytes[b] <= 127);
        CHECK(ArpSettings::make(p->bytes).on());
        const KitArp clean = kit_arp_sanitized(p->ext);
        for (int f = 0; f < kArpExtFields; f++) CHECK_EQ(int(arp_ext_get(clean, f)), int(arp_ext_get(p->ext, f)));
        // In the middle of a zone: one click either way (arp_click) lands in the next one, not
        // in this one, so a byte at a zone's very edge would make the knob feel off by one.
        for (int b = 0; b < 3; b++) {
            const int v = p->bytes[b];
            const int lo = v - arp_click(b), hi = v + arp_click(b);
            const auto zone = [b](int x) {
                const uint8_t y = uint8_t(x < 0 ? 0 : (x > 127 ? 127 : x));
                return b == 0 ? arp_mode_index(y) : (b == 1 ? arp_rate_index(y) : arp_octaves(y));
            };
            if (lo >= 0) CHECK(zone(lo) != zone(v));
            if (hi <= 127) CHECK(zone(hi) != zone(v));
        }
    }
}

TEST(arp_presets_write_their_bytes_and_settings_and_match_after) {
    for (int i = 0; i < arp_preset_count(); i++) {
        uint8_t bytes[kArpBytes] = {1, 2, 3, 4};
        KitArp ext = {1, 1, 1, 1};
        CHECK(arp_preset_apply(i, bytes, ext));
        CHECK(arp_preset_matches(i, bytes, ext));
        for (int b = 0; b < kArpBytes; b++) CHECK_EQ(int(bytes[b]), int(arp_preset(i)->bytes[b]));
        // Every other preset differs from it somewhere.
        for (int j = 0; j < arp_preset_count(); j++) {
            if (j != i) CHECK(!arp_preset_matches(j, bytes, ext));
        }
        // One kit setting off is not a match.
        ext.ratchet = uint8_t((ext.ratchet + 1) % kArpRatchets);
        CHECK(!arp_preset_matches(i, bytes, ext));
    }
    uint8_t bytes[kArpBytes] = {9, 9, 9, 9};
    KitArp ext;
    CHECK(!arp_preset_apply(99, bytes, ext));
    CHECK_EQ(int(bytes[0]), 9);
    CHECK(!arp_preset_apply(0, nullptr, ext));
    CHECK(!arp_preset_matches(-1, bytes, ext));
}

// A preset matches only when all four bytes and all four kit fields are its own: any one of
// them changed, first or last, and it does not.
TEST(arp_presets_match_only_when_every_byte_and_field_is_theirs) {
    for (int p = 0; p < arp_preset_count(); p++) {
        uint8_t bytes[kArpBytes];
        KitArp ext;
        CHECK(arp_preset_apply(p, bytes, ext));
        CHECK(arp_preset_matches(p, bytes, ext));
        for (int i = 0; i < kArpBytes; i++) {
            uint8_t other[kArpBytes];
            std::memcpy(other, bytes, sizeof other);
            other[i] ^= 1;
            CHECK(!arp_preset_matches(p, other, ext));
        }
        for (int f = 0; f < kArpExtFields; f++) {
            KitArp changed = ext;
            arp_ext_set(changed, f, uint8_t((arp_ext_get(ext, f) + 1) % arp_ext_count(f)));
            CHECK(!arp_preset_matches(p, bytes, changed));
        }
    }
}
