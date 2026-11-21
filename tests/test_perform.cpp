// Scenes, the morph and the macros.
//
// The morph's rule for a parameter only ONE scene stores is the thing most worth pinning: the
// other end is the KIT value, not the stored value held constant, so sweeping from A to B fades
// A's changes away where B says nothing about them. Held instead, two scenes that each touched
// different tracks would never fully cross over, and where the sweep ended would depend on which
// parameters happened to be stored where. That is the kind of behaviour nobody reads out of the
// code later, so it is tested by value.
//
// The rest is the round trip (PERFORM.BIN, the fourth file type) under the same rules as the
// other three: value-tolerant, structure-strict, and the refusals in the same order.
#include "check.hpp"

#include "perform.hpp"
#include "project.hpp"
#include "serialize.hpp"

#include <initializer_list>

namespace {

using namespace strata;

constexpr uint32_t kSlack = 64;
constexpr uint32_t kPerformFileBytes = 2424;

uint8_t g_buffer[kPerformFileBytes + kSlack];
uint8_t g_alternate[kPerformFileBytes + kSlack];
constexpr uint8_t kSentinel = 0xAB;

SceneSet g_scenes;
MacroSet g_macros;
SceneSet g_loaded_scenes;
MacroSet g_loaded_macros;
Kit g_kit;
uint8_t g_out[kTracks][kKitParams];

uint32_t peek_u32(const uint8_t *buffer, uint32_t at) {
    return uint32_t(buffer[at]) | uint32_t(buffer[at + 1]) << 8 |
           uint32_t(buffer[at + 2]) << 16 | uint32_t(buffer[at + 3]) << 24;
}

uint16_t peek_u16(const uint8_t *buffer, uint32_t at) {
    return uint16_t(uint16_t(buffer[at]) | uint16_t(uint16_t(buffer[at + 1]) << 8));
}

void poke_u32(uint8_t *buffer, uint32_t at, uint32_t value) {
    for (int i = 0; i < 4; i++) buffer[at + uint32_t(i)] = uint8_t((value >> (8 * i)) & 0xFFu);
}

void poke_u16(uint8_t *buffer, uint32_t at, uint16_t value) {
    buffer[at + 0] = uint8_t(value & 0xFFu);
    buffer[at + 1] = uint8_t(value >> 8);
}

void refresh_crc(uint8_t *buffer) {
    poke_u32(buffer, kCrcOffset, crc32(buffer + kHeaderBytes, peek_u32(buffer, 8)));
}

uint32_t section_at(const uint8_t *buffer, uint16_t id) {
    const uint32_t sections = peek_u16(buffer, 6);
    for (uint32_t i = 0; i < sections; i++) {
        const uint32_t entry = kHeaderBytes + i * kSectionEntryBytes;
        if (peek_u16(buffer, entry) == id) return kHeaderBytes + peek_u32(buffer, entry + 4);
    }
    return 0;
}

uint32_t section_entry_at(const uint8_t *buffer, uint16_t id) {
    const uint32_t sections = peek_u16(buffer, 6);
    for (uint32_t i = 0; i < sections; i++) {
        const uint32_t entry = kHeaderBytes + i * kSectionEntryBytes;
        if (peek_u16(buffer, entry) == id) return entry;
    }
    return 0;
}

// A kit whose every parameter is different, so a morph that crossed two of them over shows up.
void fill_kit(Kit &kit) {
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            kit.tracks[t].params[p] = uint8_t((t * 16 + p * 7 + 3) % 128);
        }
    }
}

void fill_out_from_kit(const Kit &kit) {
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) g_out[t][p] = kit.tracks[t].params[p];
    }
}

}  // namespace

// --- scenes ---------------------------------------------------------------

TEST(perform_a_fresh_set_stores_nothing_and_drives_nothing) {
    perform_init(g_scenes, g_macros);
    for (int s = 0; s < kScenes; s++) {
        CHECK_EQ(scene_count(g_scenes.scenes[s]), 0);
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitParams; p++) {
                CHECK(!scene_has(g_scenes.scenes[s], t, p));
            }
        }
    }
    for (int m = 0; m < kMacros; m++) CHECK_EQ(macro_count(g_macros.macros[m]), 0);
}

TEST(perform_storing_and_forgetting_one_parameter) {
    perform_init(g_scenes, g_macros);
    Scene &scene = g_scenes.scenes[0];

    scene_set(scene, 3, 9, 42);
    CHECK(scene_has(scene, 3, 9));
    CHECK_EQ(int(scene.values[3][9]), 42);
    CHECK_EQ(scene_count(scene), 1);
    CHECK(!scene_has(scene, 3, 8));
    CHECK(!scene_has(scene, 2, 9));

    // Forgetting keeps the value: storing again gets it back rather than a zero, which on a
    // filter cutoff is the difference between "not in this scene" and "closed".
    scene_clear(scene, 3, 9);
    CHECK(!scene_has(scene, 3, 9));
    CHECK_EQ(scene_count(scene), 0);
    CHECK_EQ(int(scene.values[3][9]), 42);
}

TEST(perform_every_parameter_has_its_own_bit) {
    // 8 tracks x 16 parameters in two bytes per track. A mask that aliased would make one
    // parameter's presence imply another's, which would show up as a scene dragging a sound it
    // never stored.
    perform_init(g_scenes, g_macros);
    Scene &scene = g_scenes.scenes[0];
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            scene_set(scene, t, p, uint8_t(t * 16 + p));
            CHECK_EQ(scene_count(scene), t * kKitParams + p + 1);
        }
    }
    CHECK_EQ(scene_count(scene), kTracks * kKitParams);

    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(scene.values[t][p]), t * 16 + p);
            scene_clear(scene, t, p);
            CHECK(!scene_has(scene, t, p));
        }
    }
    CHECK_EQ(scene_count(scene), 0);
}

TEST(perform_an_index_that_is_not_a_parameter_is_ignored) {
    // Ignored, not clamped: clamping would write someone else's parameter.
    perform_init(g_scenes, g_macros);
    Scene &scene = g_scenes.scenes[0];
    scene_set(scene, -1, 0, 5);
    scene_set(scene, kTracks, 0, 5);
    scene_set(scene, 0, -1, 5);
    scene_set(scene, 0, kKitParams, 5);
    CHECK_EQ(scene_count(scene), 0);
    CHECK(!scene_has(scene, -1, 0));
    CHECK(!scene_has(scene, 0, kKitParams));

    scene_clear(scene, -1, 0);        // and these do not crash or corrupt
    scene_clear(scene, 0, kKitParams);
    CHECK_EQ(scene_count(scene), 0);
}

TEST(perform_a_scene_value_out_of_range_is_clamped_on_the_way_in) {
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[0], 0, 0, 255);
    CHECK_EQ(int(g_scenes.scenes[0].values[0][0]), 127);
}

TEST(perform_capture_takes_the_kit) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);

    scene_capture(g_scenes.scenes[1], g_kit);
    CHECK_EQ(scene_count(g_scenes.scenes[1]), kTracks * kKitParams);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(g_scenes.scenes[1].values[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }

    // One track only, which is what capturing with a track selected does.
    scene_erase(g_scenes.scenes[2]);
    scene_capture_track(g_scenes.scenes[2], g_kit, 5);
    CHECK_EQ(scene_count(g_scenes.scenes[2]), kKitParams);
    for (int p = 0; p < kKitParams; p++) {
        CHECK(scene_has(g_scenes.scenes[2], 5, p));
        CHECK(!scene_has(g_scenes.scenes[2], 4, p));
    }
    scene_capture_track(g_scenes.scenes[2], g_kit, -1);   // ignored
    scene_capture_track(g_scenes.scenes[2], g_kit, kTracks);
    CHECK_EQ(scene_count(g_scenes.scenes[2]), kKitParams);
}

// --- the morph ------------------------------------------------------------

TEST(perform_morph_at_the_ends_is_each_scene_exactly) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    Scene &a = g_scenes.scenes[0];
    Scene &b = g_scenes.scenes[1];
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            scene_set(a, t, p, uint8_t((t + p) % 128));
            scene_set(b, t, p, uint8_t((100 + t * 3 + p) % 128));
        }
    }

    fill_out_from_kit(g_kit);
    scene_morph(a, b, 0, g_kit, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(g_out[t][p]), int(a.values[t][p]));
    }

    fill_out_from_kit(g_kit);
    scene_morph(a, b, kMorphScale, g_kit, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(g_out[t][p]), int(b.values[t][p]));
    }
}

TEST(perform_morph_halfway_is_halfway) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[0], 0, 0, 0);
    scene_set(g_scenes.scenes[1], 0, 0, 100);
    scene_set(g_scenes.scenes[0], 0, 1, 40);
    scene_set(g_scenes.scenes[1], 0, 1, 41);        // one apart: the rounding shows

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 50);
    // 40.5 rounds up, which is what adding half the scale before the shift does.
    CHECK_EQ(int(g_out[0][1]), 41);
}

TEST(perform_morph_does_not_reset_what_it_was_handed) {
    // core/perform.hpp: "A parameter neither stores is left exactly as `out` had it." That is
    // load-bearing rather than cosmetic, and only visible when `out` holds something other than
    // the kit -- which is exactly the case once a macro has already written into it. Writing
    // the kit value back would be indistinguishable from skipping if `out` were always a fresh
    // copy of the kit, and it would silently wipe the macro.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[0], 0, 0, 10);
    scene_set(g_scenes.scenes[1], 0, 0, 90);

    // Everything in `out` deliberately different from the kit, as if a macro had just run.
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            g_out[t][p] = uint8_t((g_kit.tracks[t].params[p] + 64) % 128);
        }
    }
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_kit, g_out);

    // The one parameter a scene stores was morphed...
    CHECK_EQ(int(g_out[0][0]), 50);
    // ...and every other one still holds what the caller put there, not the kit value.
    int reset = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            if (t == 0 && p == 0) continue;
            if (g_out[t][p] != uint8_t((g_kit.tracks[t].params[p] + 64) % 128)) reset++;
        }
    }
    CHECK_EQ(reset, 0);
}

TEST(perform_morph_is_monotonic_and_in_range) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    // Both directions: one parameter rising and one falling, so a sign error shows.
    scene_set(g_scenes.scenes[0], 2, 3, 10);
    scene_set(g_scenes.scenes[1], 2, 3, 120);
    scene_set(g_scenes.scenes[0], 2, 4, 120);
    scene_set(g_scenes.scenes[1], 2, 4, 10);

    int previous_up = -1, previous_down = 999;
    for (uint32_t at = 0; at <= kMorphScale; at += 8) {
        fill_out_from_kit(g_kit);
        scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], uint16_t(at), g_kit, g_out);
        const int up = int(g_out[2][3]);
        const int down = int(g_out[2][4]);
        CHECK(up >= previous_up);
        CHECK(down <= previous_down);
        CHECK(up >= 10);
        CHECK(up <= 120);
        CHECK(down >= 10);
        CHECK(down <= 120);
        previous_up = up;
        previous_down = down;
    }
    CHECK_EQ(previous_up, 120);
    CHECK_EQ(previous_down, 10);
}

TEST(perform_morph_leaves_a_parameter_neither_scene_stores_alone) {
    // The common case in a performance: a scene for the bass line says nothing about the drums,
    // and must not drag them with it.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_capture_track(g_scenes.scenes[0], g_kit, 5);
    scene_capture_track(g_scenes.scenes[1], g_kit, 5);
    for (int p = 0; p < kKitParams; p++) {
        scene_set(g_scenes.scenes[1], 5, p, uint8_t((g_kit.tracks[5].params[p] + 30) % 128));
    }

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_kit, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            if (t == 5) continue;
            CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }
}

TEST(perform_the_end_a_scene_does_not_store_is_the_kit_value) {
    // The rule worth pinning. A stores a parameter, B says nothing about it: at position 0 the
    // result is A's value, and by the far end it has faded back to the KIT value, not stayed at
    // A's. Held instead, two scenes touching different tracks would never cross over.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    g_kit.tracks[0].params[0] = 60;
    scene_set(g_scenes.scenes[0], 0, 0, 20);          // A stores it; B does not

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 0, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 20);

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 60);                   // the kit, not 20

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 40);

    // And the other way round: B stores it, A does not.
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[1], 0, 0, 100);
    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 0, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 60);
    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 100);
}

TEST(perform_an_empty_scene_morphs_as_the_kit) {
    // What an unassigned slot should do.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    for (uint32_t at : {0u, uint32_t(kMorphScale) / 3, uint32_t(kMorphScale)}) {
        fill_out_from_kit(g_kit);
        scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], uint16_t(at), g_kit, g_out);
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitParams; p++) {
                CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
            }
        }
    }
}

TEST(perform_a_morph_position_past_the_end_is_clamped) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[0], 0, 0, 10);
    scene_set(g_scenes.scenes[1], 0, 0, 90);
    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 65535, g_kit, g_out);
    CHECK_EQ(int(g_out[0][0]), 90);
}

TEST(perform_morphing_a_scene_onto_itself_changes_nothing) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_capture(g_scenes.scenes[0], g_kit);
    for (uint32_t at = 0; at <= kMorphScale; at += 64) {
        fill_out_from_kit(g_kit);
        scene_morph(g_scenes.scenes[0], g_scenes.scenes[0], uint16_t(at), g_kit, g_out);
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitParams; p++) {
                CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
            }
        }
    }
}

// --- macros ---------------------------------------------------------------

TEST(perform_a_macro_spans_its_whole_range) {
    MacroTarget target;
    target.enabled = 1;
    target.low = 0;
    target.high = 127;
    CHECK_EQ(int(macro_value(target, 0)), 0);
    CHECK_EQ(int(macro_value(target, 127)), 127);
    CHECK_EQ(int(macro_value(target, 64)), 64);

    // A narrower range, and its ends must be reachable -- which is what rounding to nearest
    // rather than truncating buys.
    target.low = 40;
    target.high = 60;
    CHECK_EQ(int(macro_value(target, 0)), 40);
    CHECK_EQ(int(macro_value(target, 127)), 60);
    CHECK_EQ(int(macro_value(target, 64)), 50);
}

TEST(perform_a_macro_rounds_to_nearest_across_its_travel) {
    // The ends of a range are exact whether the divide rounds or truncates, because delta * 127
    // over 127 is delta. What rounding buys is the middle: truncation biases every intermediate
    // value towards `low`. Checked at a position where the two disagree, and then as a bound on
    // the whole travel.
    MacroTarget target;
    target.enabled = 1;
    target.low = 40;
    target.high = 60;
    // 20 * 100 / 127 is 15.75: nearest is 16, truncation gives 15.
    CHECK_EQ(int(macro_value(target, 100)), 56);

    // Never more than half a step from the exact value, anywhere, for either direction.
    int far = 0;
    for (int low = 0; low <= 127; low += 17) {
        for (int high = 0; high <= 127; high += 19) {
            target.low = uint8_t(low);
            target.high = uint8_t(high);
            for (int at = 0; at <= 127; at++) {
                const int got = int(macro_value(target, uint8_t(at)));
                // The exact value, scaled up by 254 so half a step is an integer.
                const int exact2 = 2 * low * 127 + 2 * (high - low) * at;
                const int got2 = got * 254;
                const int error = got2 - exact2;
                if (error > 127 || error < -127) far++;
            }
        }
    }
    CHECK_EQ(far, 0);
}

TEST(perform_an_inverted_macro_target_counts_down) {
    // One knob opening a filter while it closes a send is most of what makes a macro worth
    // having, so low above high is legal and has to be exact at both ends.
    MacroTarget target;
    target.enabled = 1;
    target.low = 127;
    target.high = 0;
    CHECK_EQ(int(macro_value(target, 0)), 127);
    CHECK_EQ(int(macro_value(target, 127)), 0);
    CHECK_EQ(int(macro_value(target, 64)), 63);

    int previous = 128;
    for (int at = 0; at <= 127; at++) {
        const int value = int(macro_value(target, uint8_t(at)));
        CHECK(value <= previous);
        previous = value;
    }
}

TEST(perform_a_macro_value_is_always_in_range) {
    int outside = 0;
    for (int low = 0; low <= 127; low += 11) {
        for (int high = 0; high <= 127; high += 13) {
            MacroTarget target;
            target.enabled = 1;
            target.low = uint8_t(low);
            target.high = uint8_t(high);
            for (int at = 0; at <= 127; at += 7) {
                const int value = int(macro_value(target, uint8_t(at)));
                const int lo = low < high ? low : high;
                const int hi = low < high ? high : low;
                if (value < lo || value > hi) outside++;
            }
        }
    }
    CHECK_EQ(outside, 0);

    // And a target whose stored bytes are out of range is clamped rather than wrapping.
    MacroTarget wild;
    wild.enabled = 1;
    wild.low = 255;
    wild.high = 200;
    CHECK_EQ(int(macro_value(wild, 0)), 127);
    CHECK_EQ(int(macro_value(wild, 127)), 127);
}

TEST(perform_apply_drives_only_the_enabled_targets) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    Macro &macro = g_macros.macros[0];
    macro.targets[0].enabled = 1;
    macro.targets[0].track = 1;
    macro.targets[0].param = 2;
    macro.targets[0].low = 0;
    macro.targets[0].high = 100;
    macro.targets[2].enabled = 1;
    macro.targets[2].track = 6;
    macro.targets[2].param = 15;
    macro.targets[2].low = 100;
    macro.targets[2].high = 0;
    CHECK_EQ(macro_count(macro), 2);

    fill_out_from_kit(g_kit);
    macro_apply(macro, 127, g_out);
    CHECK_EQ(int(g_out[1][2]), 100);
    CHECK_EQ(int(g_out[6][15]), 0);
    // Nothing else moved.
    int moved = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            if (t == 1 && p == 2) continue;
            if (t == 6 && p == 15) continue;
            if (g_out[t][p] != g_kit.tracks[t].params[p]) moved++;
        }
    }
    CHECK_EQ(moved, 0);
}

TEST(perform_a_macro_target_naming_nothing_is_skipped) {
    // A macro set up by a build with more tracks or parameters must not write past the array.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    Macro &macro = g_macros.macros[0];
    macro.targets[0].enabled = 1;
    macro.targets[0].track = uint8_t(kTracks);        // no such track
    macro.targets[0].param = 0;
    macro.targets[1].enabled = 1;
    macro.targets[1].track = 0;
    macro.targets[1].param = uint8_t(kKitParams);     // no such parameter
    CHECK_EQ(macro_count(macro), 0);

    fill_out_from_kit(g_kit);
    macro_apply(macro, 127, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }
}

// --- PERFORM.BIN ----------------------------------------------------------

namespace {

void fill_performance() {
    perform_init(g_scenes, g_macros);
    fill_kit(g_kit);
    for (int s = 0; s < kScenes; s++) {
        // Each scene stores a different subset, so the bitmask round trip is exercised rather
        // than only the values.
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitParams; p++) {
                if (((t + p + s) % 3) != 0) continue;
                scene_set(g_scenes.scenes[s], t, p, uint8_t((s * 7 + t * 5 + p * 3) % 128));
            }
        }
    }
    for (int m = 0; m < kMacros; m++) {
        for (int i = 0; i < kMacroTargets; i++) {
            MacroTarget &target = g_macros.macros[m].targets[i];
            if (((m + i) % 2) != 0) continue;         // some disabled, on purpose
            target.enabled = 1;
            target.track = uint8_t((m * 2 + i) % kTracks);
            target.param = uint8_t((m * 3 + i * 5) % kKitParams);
            target.low = uint8_t((m * 11 + i) % 128);
            target.high = uint8_t((120 - m * 9 - i) % 128);
        }
    }
}

uint32_t write_performance() {
    uint32_t written = 0;
    const SerializeResult res =
        perform_write(g_scenes, g_macros, g_buffer, uint32_t(sizeof(g_buffer)), &written);
    CHECK_EQ(int(res), int(SerializeResult::ok));
    return written;
}

}  // namespace

TEST(perform_the_file_size_is_what_the_format_says) {
    CHECK_EQ(perform_bytes(), kPerformFileBytes);
}

TEST(perform_round_trips_every_scene_and_every_macro) {
    fill_performance();
    for (uint32_t i = 0; i < sizeof(g_buffer); i++) g_buffer[i] = kSentinel;

    uint32_t written = 0;
    CHECK_EQ(int(perform_write(g_scenes, g_macros, g_buffer, kPerformFileBytes, &written)),
             int(SerializeResult::ok));
    CHECK_EQ(written, kPerformFileBytes);
    for (uint32_t i = kPerformFileBytes; i < sizeof(g_buffer); i++) {
        CHECK_EQ(int(g_buffer[i]), int(kSentinel));
    }

    perform_init(g_loaded_scenes, g_loaded_macros);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, written)),
             int(SerializeResult::ok));

    int stored_wrong = 0, values_wrong = 0;
    for (int s = 0; s < kScenes; s++) {
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitParams; p++) {
                if (scene_has(g_loaded_scenes.scenes[s], t, p) !=
                    scene_has(g_scenes.scenes[s], t, p)) {
                    stored_wrong++;
                } else if (scene_has(g_scenes.scenes[s], t, p) &&
                           g_loaded_scenes.scenes[s].values[t][p] !=
                               g_scenes.scenes[s].values[t][p]) {
                    values_wrong++;
                }
            }
        }
    }
    CHECK_EQ(stored_wrong, 0);
    CHECK_EQ(values_wrong, 0);

    int macros_wrong = 0;
    for (int m = 0; m < kMacros; m++) {
        for (int i = 0; i < kMacroTargets; i++) {
            const MacroTarget &a = g_macros.macros[m].targets[i];
            const MacroTarget &b = g_loaded_macros.macros[m].targets[i];
            if ((a.enabled != 0) != (b.enabled != 0)) macros_wrong++;
            if (a.enabled == 0) continue;
            if (a.track != b.track || a.param != b.param || a.low != b.low ||
                a.high != b.high) {
                macros_wrong++;
            }
        }
    }
    CHECK_EQ(macros_wrong, 0);
}

TEST(perform_the_header_says_strata_and_the_version) {
    fill_performance();
    const uint32_t written = write_performance();
    CHECK_EQ(int(g_buffer[0]), int(kMagic0));
    CHECK_EQ(int(peek_u16(g_buffer, kVersionOffset)), int(kPerformVersion));
    CHECK_EQ(int(peek_u16(g_buffer, 6)), 2);
    CHECK_EQ(peek_u32(g_buffer, 8), written - kHeaderBytes);
    CHECK_EQ(peek_u32(g_buffer, kCrcOffset),
             crc32(g_buffer + kHeaderBytes, written - kHeaderBytes));
    CHECK_EQ(section_at(g_buffer, kSectionPerformScenes), kHeaderBytes + 2 * kSectionEntryBytes);
    CHECK(section_at(g_buffer, kSectionPerformMacros) >
          section_at(g_buffer, kSectionPerformScenes));
}

TEST(perform_write_refuses_a_buffer_one_byte_short_and_touches_nothing) {
    fill_performance();
    for (uint32_t i = 0; i < sizeof(g_buffer); i++) g_buffer[i] = kSentinel;
    uint32_t written = 99;
    CHECK_EQ(int(perform_write(g_scenes, g_macros, g_buffer, kPerformFileBytes - 1, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);
    for (uint32_t i = 0; i < sizeof(g_buffer); i++) CHECK_EQ(int(g_buffer[i]), int(kSentinel));

    CHECK_EQ(int(perform_write(g_scenes, g_macros, nullptr, kPerformFileBytes, &written)),
             int(SerializeResult::buffer_too_small));
    CHECK_EQ(written, 0u);
}

TEST(perform_a_scene_value_out_of_range_in_a_file_is_clamped) {
    fill_performance();
    write_performance();
    const uint32_t at = section_at(g_buffer, kSectionPerformScenes);
    g_buffer[at + 0] = 255;
    g_buffer[at + 1] = 200;
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded_scenes.scenes[0].values[0][0]), 127);
    CHECK_EQ(int(g_loaded_scenes.scenes[0].values[0][1]), 127);
}

TEST(perform_a_macro_target_naming_nothing_is_dropped_whole) {
    // Dropped, not clamped, the same way core/project.cpp drops a chain entry naming no
    // pattern: clamping would quietly drive a parameter nobody assigned.
    fill_performance();
    write_performance();
    const uint32_t at = section_at(g_buffer, kSectionPerformMacros);
    g_buffer[at + 0] = 1;                       // enabled
    g_buffer[at + 1] = uint8_t(kTracks + 2);    // no such track
    g_buffer[at + 2] = 3;
    g_buffer[at + 3] = 10;
    g_buffer[at + 4] = 90;
    refresh_crc(g_buffer);

    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::ok));
    const MacroTarget &target = g_loaded_macros.macros[0].targets[0];
    CHECK_EQ(int(target.enabled), 0);
    CHECK_EQ(int(target.track), 0);
    CHECK_EQ(int(target.param), 0);
    CHECK_EQ(macro_count(g_loaded_macros.macros[0]),
             macro_count(g_macros.macros[0]) - (g_macros.macros[0].targets[0].enabled ? 1 : 0));
}

TEST(perform_any_nonzero_enabled_byte_enables_a_target) {
    fill_performance();
    write_performance();
    const uint32_t at = section_at(g_buffer, kSectionPerformMacros);
    g_buffer[at + 0] = 7;                       // not 1, and still on
    g_buffer[at + 1] = 2;
    g_buffer[at + 2] = 4;
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded_macros.macros[0].targets[0].enabled), 1);
    CHECK_EQ(int(g_loaded_macros.macros[0].targets[0].track), 2);
    CHECK_EQ(int(g_loaded_macros.macros[0].targets[0].param), 4);
}

TEST(perform_read_refuses_a_damaged_container_and_empties_the_set) {
    fill_performance();
    write_performance();
    g_buffer[1] = 'X';
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::bad_magic));
    // A refused load leaves an empty set, never half a file.
    for (int s = 0; s < kScenes; s++) CHECK_EQ(scene_count(g_loaded_scenes.scenes[s]), 0);
    for (int m = 0; m < kMacros; m++) CHECK_EQ(macro_count(g_loaded_macros.macros[m]), 0);

    write_performance();
    poke_u16(g_buffer, kVersionOffset, uint16_t(kPerformVersion + 1));
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::bad_version));

    write_performance();
    g_buffer[kHeaderBytes + 40] = uint8_t(g_buffer[kHeaderBytes + 40] ^ 0x20);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::bad_crc));

    write_performance();
    poke_u32(g_buffer, 8, 0x0FFFFFFFu);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::truncated));

    write_performance();
    poke_u16(g_buffer, 6, 0);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::malformed));

    write_performance();
    poke_u32(g_buffer, section_entry_at(g_buffer, kSectionPerformScenes) + 8, 10);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, kPerformFileBytes)),
             int(SerializeResult::malformed));

    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, nullptr, 0)),
             int(SerializeResult::truncated));
}

TEST(perform_read_never_looks_past_the_bytes_it_was_given) {
    fill_performance();
    const uint32_t written = write_performance();
    for (uint32_t i = 0; i < written; i++) g_alternate[i] = g_buffer[i];
    for (uint32_t i = written; i < sizeof(g_buffer); i++) {
        g_buffer[i] = 0x00;
        g_alternate[i] = 0xFF;
    }
    for (uint32_t size = 0; size <= written; size += 7) {
        const int a = int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size));
        const int b = int(perform_read(g_loaded_scenes, g_loaded_macros, g_alternate, size));
        if (a != b) CHECK_EQ(a, b);
        const bool ok = a == int(SerializeResult::ok);
        if (ok != (size == written)) CHECK_EQ(ok, size == written);
    }
}
