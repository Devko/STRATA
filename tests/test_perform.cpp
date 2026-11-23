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
constexpr uint32_t kPerformFileBytes = 4456;   // 2424, then the scenes' LFO (12 + 640), EQ (12 + 64), arp (12 + 640) and ASR (12 + 640)

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

// The file as the build before its LAST section existed wrote it: one table entry fewer, every
// section 12 bytes earlier, the last section's bytes gone. Returns the new size. The last section
// is the one at the end of the payload, which is how every writer here lays them out.
uint32_t drop_last_section(const uint8_t *file, uint8_t *out) {
    const uint32_t count = peek_u16(file, 6);
    const uint32_t last = kHeaderBytes + (count - 1) * kSectionEntryBytes;
    const uint32_t cut = peek_u32(file, last + 4);          // where the last section starts
    for (uint32_t i = 0; i < kHeaderBytes; i++) out[i] = file[i];
    for (uint32_t i = 0; i + 1 < count; i++) {
        const uint32_t from = kHeaderBytes + i * kSectionEntryBytes;
        poke_u16(out, from, peek_u16(file, from));
        poke_u16(out, from + 2, 0);
        poke_u32(out, from + 4, peek_u32(file, from + 4) - kSectionEntryBytes);
        poke_u32(out, from + 8, peek_u32(file, from + 8));
    }
    const uint32_t data_from = kHeaderBytes + count * kSectionEntryBytes;
    const uint32_t data_to = kHeaderBytes + cut;
    uint32_t at = kHeaderBytes + (count - 1) * kSectionEntryBytes;
    for (uint32_t i = data_from; i < data_to; i++) out[at++] = file[i];
    poke_u16(out, 6, uint16_t(count - 1));
    poke_u32(out, 8, at - kHeaderBytes);
    refresh_crc(out);
    return at;
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
    // 8 tracks x 20 parameters (16 sound, 4 LFO) in three bytes per track. A mask that aliased would make one
    // parameter's presence imply another's, which would show up as a scene dragging a sound it
    // never stored.
    perform_init(g_scenes, g_macros);
    Scene &scene = g_scenes.scenes[0];
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            scene_set(scene, t, p, uint8_t((t * kKitParams + p) & 127));
            CHECK_EQ(scene_count(scene), t * kKitParams + p + 1);
        }
    }
    CHECK_EQ(scene_count(scene), kTracks * kKitParams);

    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(scene.values[t][p]), (t * kKitParams + p) & 127);
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

TEST(perform_morph_at_rest_is_the_live_kit_and_at_the_top_is_scene_b_exactly) {
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

    // At rest the knobs own the sound: the morph writes nothing, so `out` keeps the live kit
    // it came in with. It used to write scene A here, which pinned every parameter a captured
    // scene stored -- turning that knob in EDIT moved the screen and not the sound (Roland,
    // 2026-11-23). Scene A is still the bottom of the sweep from the first click up.
    fill_out_from_kit(g_kit);
    scene_morph(a, b, 0, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }

    // One click up and scene A has it, to within the rounding of one step in 1024.
    fill_out_from_kit(g_kit);
    scene_morph(a, b, 1, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            const int delta = int(g_out[t][p]) - int(a.values[t][p]);
            CHECK(delta >= -1 && delta <= 1);
        }
    }

    fill_out_from_kit(g_kit);
    scene_morph(a, b, kMorphScale, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(g_out[t][p]), int(b.values[t][p]));
    }
}

TEST(perform_the_end_a_scene_does_not_store_is_the_live_value_in_out) {
    // The engine passed the kit as last SAVED, so a morph towards an empty scene pulled a knob
    // turned since the save back to its saved value. The fallback is now whatever `out` holds
    // on the way in, which the engine fills from the live kit.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[0], 2, 5, 100);      // A stores one value, B stores nothing

    fill_out_from_kit(g_kit);
    g_out[2][5] = 20;                               // the live knob, moved since the save
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, g_out);
    CHECK_EQ(int(g_out[2][5]), 20);                // fully at B: the live value, not the kit's
}

TEST(perform_a_macro_range_through_the_current_value_does_not_jump) {
    // For every value, every far end and every knob position: the range the assignment gets puts
    // the parameter within one step of where it was, and it is a real range (the knob moves it).
    int worst = 0;
    bool every_range_moves = true;
    for (int now = 0; now <= 127; now++) {
        for (int far : {0, 127}) {
            for (int position = 0; position <= 127; position++) {
                uint8_t low = 0, high = 0;
                macro_range_through(uint8_t(now), uint8_t(far), uint8_t(position), low, high);
                MacroTarget target;
                target.enabled = 1;
                target.low = low;
                target.high = high;
                const int at = int(macro_value(target, uint8_t(position)));
                const int miss = at > now ? at - now : now - at;
                // The documented exception: an end value heading for the other end.
                const bool exception = (now == 0 && far == 127) || (now == 127 && far == 0);
                if (!exception) worst = miss > worst ? miss : worst;
                if (low == high && now != far) every_range_moves = false;
            }
        }
    }
    CHECK(worst <= 1);
    CHECK(every_range_moves);

    // At position 0 it is the plain range, as before.
    uint8_t low = 0, high = 0;
    macro_range_through(40, 127, 0, low, high);
    CHECK_EQ(int(low), 40);
    CHECK_EQ(int(high), 127);
}

TEST(perform_morph_halfway_is_halfway) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[0], 0, 0, 0);
    scene_set(g_scenes.scenes[1], 0, 0, 100);
    scene_set(g_scenes.scenes[0], 0, 1, 40);
    scene_set(g_scenes.scenes[1], 0, 1, 41);        // one apart: the rounding shows

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_out);
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
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_out);

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

    // From the first click up, not from 0: at rest the morph lets go and `out` keeps the live
    // kit, which sits between the two ends and would read as a step backwards here.
    int previous_up = -1, previous_down = 999;
    for (uint32_t at = 8; at <= kMorphScale; at += 8) {
        fill_out_from_kit(g_kit);
        scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], uint16_t(at), g_out);
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
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            if (t == 5) continue;
            CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }
}

TEST(perform_the_end_a_scene_does_not_store_is_the_kit_value) {
    // The rule worth pinning. A stores a parameter, B says nothing about it: just off rest the
    // result is A's value, and by the far end it has faded back to the KIT value, not stayed at
    // A's. Held instead, two scenes touching different tracks would never cross over.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    g_kit.tracks[0].params[0] = 60;
    scene_set(g_scenes.scenes[0], 0, 0, 20);          // A stores it; B does not

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 0, g_out);
    CHECK_EQ(int(g_out[0][0]), 60);                   // at rest: the live kit, not A

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 1, g_out);
    CHECK_EQ(int(g_out[0][0]), 20);                   // one click up: A's value

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, g_out);
    CHECK_EQ(int(g_out[0][0]), 60);                   // the kit, not 20

    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, g_out);
    CHECK_EQ(int(g_out[0][0]), 40);

    // And the other way round: B stores it, A does not.
    perform_init(g_scenes, g_macros);
    scene_set(g_scenes.scenes[1], 0, 0, 100);
    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 0, g_out);
    CHECK_EQ(int(g_out[0][0]), 60);
    fill_out_from_kit(g_kit);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, g_out);
    CHECK_EQ(int(g_out[0][0]), 100);
}

TEST(perform_an_empty_scene_morphs_as_the_kit) {
    // What an unassigned slot should do.
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    for (uint32_t at : {0u, uint32_t(kMorphScale) / 3, uint32_t(kMorphScale)}) {
        fill_out_from_kit(g_kit);
        scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], uint16_t(at), g_out);
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
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 65535, g_out);
    CHECK_EQ(int(g_out[0][0]), 90);
}

TEST(perform_morphing_a_scene_onto_itself_changes_nothing) {
    fill_kit(g_kit);
    perform_init(g_scenes, g_macros);
    scene_capture(g_scenes.scenes[0], g_kit);
    for (uint32_t at = 0; at <= kMorphScale; at += 64) {
        fill_out_from_kit(g_kit);
        scene_morph(g_scenes.scenes[0], g_scenes.scenes[0], uint16_t(at), g_out);
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
    macro.targets[0].track = uint8_t(kTracks + 1);    // no such track (kTracks is the master EQ)
    macro.targets[0].param = 0;
    macro.targets[1].enabled = 1;
    macro.targets[1].track = 0;
    macro.targets[1].param = uint8_t(kKitParams);     // no such parameter
    macro.targets[2].enabled = 1;
    macro.targets[2].track = kMacroMasterTrack;
    macro.targets[2].param = uint8_t(kMasterBands);   // no such EQ band
    CHECK_EQ(macro_count(macro), 0);

    fill_out_from_kit(g_kit);
    macro_apply(macro, 127, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }
}

// --- mutes ----------------------------------------------------------------

TEST(perform_a_mute_state_is_one_bit_per_track) {
    bool muted[kTracks] = {};

    mute_state_apply(0, muted);
    for (int t = 0; t < kTracks; t++) CHECK(!muted[t]);
    CHECK_EQ(int(mute_state_capture(muted)), 0);

    mute_state_apply(0xFF, muted);
    for (int t = 0; t < kTracks; t++) CHECK(muted[t]);
    CHECK_EQ(int(mute_state_capture(muted)), 0xFF);

    // Bit 0 is track 1, which is the bit order the file and the UI both assume.
    mute_state_apply(0b00000101, muted);
    CHECK(muted[0]);
    CHECK(!muted[1]);
    CHECK(muted[2]);
    for (int t = 3; t < kTracks; t++) CHECK(!muted[t]);
}

TEST(perform_a_mute_state_round_trips_through_every_combination) {
    // All 256 of them, since a state is a byte and a project stores eight: an aliased bit would
    // recall the wrong track and look like a broken pad.
    bool muted[kTracks] = {};
    for (int state = 0; state <= 255; state++) {
        mute_state_apply(uint8_t(state), muted);
        CHECK_EQ(int(mute_state_capture(muted)), state);
    }
}

TEST(perform_the_mute_latch_holds_one_change_not_a_queue) {
    MuteLatch latch;
    uint8_t state = 0xAA;

    // Nothing armed: nothing to release, and the caller's value is untouched.
    CHECK(!mute_latch_release(latch, state));
    CHECK_EQ(int(state), 0xAA);

    mute_latch_arm(latch, 0x01);
    mute_latch_arm(latch, 0x02);
    mute_latch_arm(latch, 0x04);
    CHECK(latch.pending);
    // The third press, not all three: a queue would play back the performer's hesitation.
    CHECK(mute_latch_release(latch, state));
    CHECK_EQ(int(state), 0x04);
    CHECK(!latch.pending);

    // And it fires exactly once.
    CHECK(!mute_latch_release(latch, state));
    CHECK_EQ(int(state), 0x04);

    mute_latch_arm(latch, 0x10);
    mute_latch_clear(latch);
    CHECK(!mute_latch_release(latch, state));
    CHECK_EQ(int(state), 0x04);
}

TEST(perform_arming_an_unmuted_state_still_counts_as_a_change) {
    // 0 is a legitimate state -- "unmute everything" -- so the latch must not treat it as empty.
    MuteLatch latch;
    uint8_t state = 0xFF;
    mute_latch_arm(latch, 0);
    CHECK(mute_latch_release(latch, state));
    CHECK_EQ(int(state), 0);
}

// --- auto-fill ------------------------------------------------------------

TEST(perform_auto_fill_fires_on_the_last_bar_of_its_period) {
    // A fill is the bar that leads back into the loop, so at every 4 it is bars 3, 7, 11...
    CHECK(!auto_fill_active(AutoFill::every_4, 0));
    CHECK(!auto_fill_active(AutoFill::every_4, 1));
    CHECK(!auto_fill_active(AutoFill::every_4, 2));
    CHECK(auto_fill_active(AutoFill::every_4, 3));
    CHECK(!auto_fill_active(AutoFill::every_4, 4));
    CHECK(auto_fill_active(AutoFill::every_4, 7));
    CHECK(auto_fill_active(AutoFill::every_4, 11));

    CHECK(auto_fill_active(AutoFill::every_8, 7));
    CHECK(!auto_fill_active(AutoFill::every_8, 3));
    CHECK(auto_fill_active(AutoFill::every_16, 15));
    CHECK(!auto_fill_active(AutoFill::every_16, 7));
}

TEST(perform_auto_fill_off_never_fires) {
    // And zeroed memory is off, which is why `off` is 0.
    static_assert(int(AutoFill::off) == 0, "zeroed memory must mean auto-fill off");
    for (uint32_t bar = 0; bar < 64; bar++) CHECK(!auto_fill_active(AutoFill::off, bar));
    for (uint32_t bar = 0; bar < 64; bar++) {
        CHECK(!auto_fill_active(AutoFill(int(AutoFill::count) + 3), bar));
    }
    CHECK_EQ(auto_fill_bars(AutoFill::off), 0);
}

TEST(perform_auto_fill_fires_once_per_period_for_ever) {
    // Counted over many periods, because the bar number is a count from the start of the
    // transport and a modulo that drifted would make fills wander.
    for (AutoFill period : {AutoFill::every_4, AutoFill::every_8, AutoFill::every_16}) {
        const int bars = auto_fill_bars(period);
        int fills = 0;
        for (uint32_t bar = 0; bar < uint32_t(bars) * 50; bar++) {
            if (auto_fill_active(period, bar)) fills++;
        }
        CHECK_EQ(fills, 50);
    }
    // Including across a wrap of the bar counter, which at 130 BPM is years away but costs
    // nothing to be right about.
    CHECK(auto_fill_active(AutoFill::every_4, 0xFFFFFFFFu));
}

TEST(perform_auto_fill_names_fit_the_screen) {
    CHECK_EQ(auto_fill_name(AutoFill::off), "OFF");
    CHECK_EQ(auto_fill_name(AutoFill::every_4), "4");
    CHECK_EQ(auto_fill_name(AutoFill::every_16), "16");
    CHECK_EQ(auto_fill_name(AutoFill(99)), "?");
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
    CHECK_EQ(int(peek_u16(g_buffer, 6)), 6);          // scenes, macros, the scenes' LFO, EQ, arp, ASR
    CHECK_EQ(peek_u32(g_buffer, 8), written - kHeaderBytes);
    CHECK_EQ(peek_u32(g_buffer, kCrcOffset),
             crc32(g_buffer + kHeaderBytes, written - kHeaderBytes));
    CHECK_EQ(section_at(g_buffer, kSectionPerformScenes), kHeaderBytes + 6 * kSectionEntryBytes);
    CHECK(section_at(g_buffer, kSectionPerformMacros) >
          section_at(g_buffer, kSectionPerformScenes));
    CHECK(section_at(g_buffer, kSectionPerformSceneLfo) >
          section_at(g_buffer, kSectionPerformMacros));
    CHECK(section_at(g_buffer, kSectionPerformSceneMaster) >
          section_at(g_buffer, kSectionPerformSceneLfo));
    CHECK(section_at(g_buffer, kSectionPerformSceneArp) >
          section_at(g_buffer, kSectionPerformSceneMaster));
    CHECK(section_at(g_buffer, kSectionPerformSceneAsr) >
          section_at(g_buffer, kSectionPerformSceneArp));
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

// --- what a line-by-line mutation run found (docs/MUTATION.md) -------------------------------

TEST(perform_a_scene_index_one_past_the_table_touches_nothing) {
    // One past the end in either direction writes into the NEXT track's row, or past the scene:
    // so the neighbours are what is checked.
    SceneSet &set = g_scenes;
    perform_init(set, g_macros);
    fill_kit(g_kit);
    scene_capture(set.scenes[0], g_kit);
    const int full = scene_count(set.scenes[0]);
    scene_set(set.scenes[0], kTracks, 0, 5);
    scene_set(set.scenes[0], 0, kKitParams, 5);
    scene_set(set.scenes[0], -1, 0, 5);
    scene_clear(set.scenes[0], kTracks, 0);
    scene_clear(set.scenes[0], 0, kKitParams);
    scene_clear(set.scenes[0], -1, 3);
    scene_clear(set.scenes[0], 2, -1);
    CHECK_EQ(scene_count(set.scenes[0]), full);
    CHECK_EQ(scene_count(set.scenes[1]), 0);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            CHECK_EQ(int(set.scenes[0].values[t][p]), int(g_kit.tracks[t].params[p] & 127));
        }
    }
}

TEST(perform_erasing_a_scene_zeroes_its_values_as_well_as_its_bits) {
    fill_kit(g_kit);
    Scene &scene = g_scenes.scenes[2];
    scene_capture(scene, g_kit);
    scene_erase(scene);
    CHECK_EQ(scene_count(scene), 0);
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(scene.stored[t][0]), 0);
        CHECK_EQ(int(scene.stored[t][1]), 0);
        for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(scene.values[t][p]), 0);
    }
}

TEST(perform_a_macro_naming_the_track_or_parameter_one_past_the_end_drives_nothing) {
    struct {
        uint8_t out[kTracks][kKitParams];
        uint8_t after[kKitParams];             // where out[kTracks] would land
    } guard;
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) guard.out[t][p] = 64;
    }
    for (int p = 0; p < kKitParams; p++) guard.after[p] = 64;
    Macro macro;
    macro.targets[0] = MacroTarget{1, uint8_t(kTracks + 1), 0, 0, 0};
    macro.targets[1] = MacroTarget{1, 0, uint8_t(kKitParams), 0, 0};
    // A master EQ target is not a track target: macro_apply leaves it to macro_apply_master.
    macro.targets[2] = MacroTarget{1, kMacroMasterTrack, 0, 0, 0};
    CHECK_EQ(macro_count(macro), 1);
    macro_apply(macro, 127, guard.out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(guard.out[t][p]), 64);
    }
    for (int p = 0; p < kKitParams; p++) CHECK_EQ(int(guard.after[p]), 64);
}

TEST(perform_a_range_through_the_knob_passes_through_the_value_it_was_assigned_at) {
    // The contract of macro_range_through: the returned range, at the knob's position, gives the
    // parameter's present value back (to within the rounding of one step), so assigning a macro
    // does not jump the sound. The single exception is the flat case, which falls back to the
    // plain range from `now` to `far` -- a macro must never be assigned flat.
    const uint8_t values[] = {0, 1, 13, 64, 100, 126, 127};
    for (uint8_t now : values) {
        for (uint8_t far : values) {
            if (now == far) continue;
            for (int p = 0; p <= 127; p++) {
                uint8_t low = 0, high = 0;
                macro_range_through(now, far, uint8_t(p), low, high);
                CHECK(low != high);
                const bool plain = low == now && high == far;
                if (plain && p != 0) continue;
                MacroTarget target{1, 0, 0, low, high};
                const int got = int(macro_value(target, uint8_t(p)));
                CHECK(got - int(now) <= 1 && int(now) - got <= 1);
                if (p > 0 && p < 127 && !plain) {
                    // Only the end the knob is not at moves; the far end is kept when it can be.
                    CHECK(high == far || low == 0 || low == 127);
                }
            }
        }
    }
    // One step above the bottom is the general case, not the bottom's own: 64 heading to 0.
    uint8_t low = 0, high = 0;
    macro_range_through(64, 0, 1, low, high);
    CHECK_EQ(int(high), 0);
    CHECK_EQ(int(low), 65);
}

TEST(perform_clearing_a_mute_latch_forgets_its_state) {
    MuteLatch latch;
    mute_latch_arm(latch, 0x5A);
    mute_latch_clear(latch);
    CHECK(!latch.pending);
    CHECK_EQ(int(latch.state), 0);
}

TEST(perform_a_payload_declared_one_byte_past_the_file_is_truncated) {
    fill_performance();
    const uint32_t size = write_performance();
    poke_u32(g_buffer, 8, size - kHeaderBytes + 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::truncated));
}

TEST(perform_a_section_one_byte_longer_than_the_payload_or_a_short_macro_table_is_malformed) {
    fill_performance();
    const uint32_t size = write_performance();
    // The LAST section, the scenes' ASR, so one byte more runs past the payload.
    const uint32_t last = section_entry_at(g_buffer, kSectionPerformSceneAsr);
    const uint32_t last_len = peek_u32(g_buffer, last + 8);
    poke_u32(g_buffer, last + 8, last_len + 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::malformed));

    // A macro table one byte short, a scenes' LFO section one byte short, and a scenes' master
    // section one byte short: all damage, not an old file.
    write_performance();
    const uint32_t macros = section_entry_at(g_buffer, kSectionPerformMacros);
    poke_u32(g_buffer, macros + 8, peek_u32(g_buffer, macros + 8) - 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::malformed));

    const uint32_t entry = section_entry_at(g_buffer, kSectionPerformSceneLfo);
    const uint32_t len = peek_u32(g_buffer, entry + 8);
    write_performance();
    poke_u32(g_buffer, entry + 8, len - 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::malformed));

    write_performance();
    poke_u32(g_buffer, last + 8, last_len - 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::malformed));

    write_performance();
    const uint32_t master = section_entry_at(g_buffer, kSectionPerformSceneMaster);
    poke_u32(g_buffer, master + 8, peek_u32(g_buffer, master + 8) - 1);
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::malformed));
}

TEST(perform_the_section_table_reserved_bytes_are_written_as_zero) {
    fill_performance();
    write_performance();
    for (uint32_t i = 0; i < peek_u16(g_buffer, 6); i++) {
        CHECK_EQ(int(peek_u16(g_buffer, kHeaderBytes + i * kSectionEntryBytes + 2)), 0);
    }
}

TEST(perform_a_macro_target_naming_track_or_param_one_past_the_end_is_dropped_whole) {
    // One past the last track (kTracks is the master EQ now), one past the last parameter, and
    // one past the master EQ's last band.
    for (int which = 0; which < 3; which++) {
        fill_performance();
        const uint32_t size = write_performance();
        const uint32_t at = section_at(g_buffer, kSectionPerformMacros);
        g_buffer[at + 0] = 1;
        g_buffer[at + 1] = which == 0 ? uint8_t(kTracks + 1) : (which == 1 ? 0 : kMacroMasterTrack);
        g_buffer[at + 2] = which == 0 ? 0 : (which == 1 ? uint8_t(kKitParams) : uint8_t(kMasterBands));
        g_buffer[at + 3] = 30;
        g_buffer[at + 4] = 90;
        refresh_crc(g_buffer);
        CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
                 int(SerializeResult::ok));
        const MacroTarget &target = g_loaded_macros.macros[0].targets[0];
        CHECK_EQ(int(target.enabled), 0);
        CHECK_EQ(int(target.low), 0);
        CHECK_EQ(int(target.high), 127);
    }
}

TEST(macro_apply_sweeps_from_the_live_value_and_writes_nothing_at_rest) {
    // The knob owns the parameter while the macro rests, and the sweep starts from wherever the
    // parameter is -- not from the stored `low` (core/perform.hpp).
    Macro macro{};
    macro.targets[0] = MacroTarget{1, 2, 3, 100, 20};
    uint8_t out[kTracks][kKitParams] = {};
    out[2][3] = 77;
    macro_apply(macro, 0, out);
    CHECK_EQ(int(out[2][3]), 77);
    for (int at = 0; at <= 127; at++) {
        out[2][3] = 77;
        macro_apply(macro, uint8_t(at), out);
        CHECK_EQ(int(out[2][3]), int(macro_value_from(77, macro.targets[0], uint8_t(at))));
    }
    // No jump off 0: one detent moves at most one value (here the 57-value span rounds to 0).
    out[2][3] = 77;
    macro_apply(macro, 1, out);
    CHECK_EQ(int(out[2][3]), 77);
    out[2][3] = 77;
    macro_apply(macro, 64, out);
    CHECK_EQ(int(out[2][3]), 48);
    out[2][3] = 77;
    macro_apply(macro, 127, out);
    CHECK_EQ(int(out[2][3]), 20);
    // Monotone from live to high, with no step larger than the span allows.
    int last = 77;
    for (int at = 1; at <= 127; at++) {
        const int v = int(macro_value_from(77, macro.targets[0], uint8_t(at)));
        CHECK(v <= last && last - v <= 1);
        last = v;
    }
}


// --- the master kill EQ in scenes and macros --------------------------------------------------

TEST(perform_scene_master_bytes_round_trip_and_an_older_file_stores_none) {
    fill_performance();
    const uint8_t bands[kMasterBands] = {10, 64, 120};
    scene_capture_master(g_scenes.scenes[3], bands);
    g_scenes.scenes[7].master[1] = 33;
    g_scenes.scenes[7].master_stored = 0x2;
    const uint32_t size = write_performance();

    perform_init(g_loaded_scenes, g_loaded_macros);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    for (int s = 0; s < kScenes; s++) {
        for (int b = 0; b < kMasterBands; b++) {
            CHECK_EQ(int(g_loaded_scenes.scenes[s].master[b]), int(g_scenes.scenes[s].master[b]));
        }
        CHECK_EQ(int(g_loaded_scenes.scenes[s].master_stored),
                 int(g_scenes.scenes[s].master_stored));
    }
    CHECK_EQ(int(g_loaded_scenes.scenes[3].master_stored), 0x7);
    CHECK_EQ(int(g_loaded_scenes.scenes[3].master[2]), 120);

    // A file from before the section: drop the fourth table entry by declaring three sections, as
    // the LFO's writer did (the table is a lookup by id, so the extra bytes are never asked for).
    poke_u16(g_buffer, 6, 3);
    refresh_crc(g_buffer);
    g_loaded_scenes.scenes[3].master_stored = 0x7;      // left over from the last project
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    for (int s = 0; s < kScenes; s++) CHECK_EQ(int(g_loaded_scenes.scenes[s].master_stored), 0);
    // And the rest of the file still arrived.
    CHECK_EQ(int(g_loaded_scenes.scenes[0].stored[0][0]), int(g_scenes.scenes[0].stored[0][0]));
}

TEST(perform_a_scene_master_byte_and_its_stored_bits_are_clamped) {
    fill_performance();
    const uint32_t size = write_performance();
    const uint32_t at = section_at(g_buffer, kSectionPerformSceneMaster);
    g_buffer[at + 0] = 200;
    g_buffer[at + 3] = 0xFF;
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded_scenes.scenes[0].master[0]), 127);
    CHECK_EQ(int(g_loaded_scenes.scenes[0].master_stored), 0x7);
}

TEST(perform_the_master_morph_follows_scene_morph_s_rule) {
    Scene a, b;
    uint8_t out[kMasterBands] = {64, 64, 64};
    // Neither stores anything: untouched.
    scene_morph_master(a, b, kMorphScale / 2, out);
    CHECK_EQ(int(out[0]), 64);

    // B stores LOW at 0 (a kill): A's end is the live value, so half way is half way down.
    const uint8_t killed[kMasterBands] = {0, 64, 64};
    scene_capture_master(b, killed);
    scene_morph_master(a, b, kMorphScale / 2, out);
    CHECK_EQ(int(out[0]), 32);
    for (int i = 0; i < kMasterBands; i++) out[i] = 64;
    scene_morph_master(a, b, kMorphScale, out);
    CHECK_EQ(int(out[0]), 0);
    CHECK_EQ(int(out[1]), 64);
    for (int i = 0; i < kMasterBands; i++) out[i] = 64;
    scene_morph_master(a, b, 0, out);
    CHECK_EQ(int(out[0]), 64);

    // Counted with the scene, and erased with it.
    CHECK_EQ(scene_count(b), 3);
    scene_erase(b);
    CHECK_EQ(scene_count(b), 0);
    CHECK_EQ(int(b.master_stored), 0);
}

TEST(perform_a_master_macro_target_drives_the_eq_and_no_track) {
    Macro macro;
    macro.targets[0] = MacroTarget{1, kMacroMasterTrack, 0, 64, 0};   // LOW towards a kill
    macro.targets[1] = MacroTarget{1, 2, 5, 0, 127};                   // a track parameter
    CHECK_EQ(macro_count(macro), 2);

    uint8_t eq[kMasterBands] = {64, 64, 64};
    macro_apply_master(macro, 0, eq);
    CHECK_EQ(int(eq[0]), 64);                       // at rest it lets go
    macro_apply_master(macro, 127, eq);
    CHECK_EQ(int(eq[0]), 0);
    CHECK_EQ(int(eq[1]), 64);
    CHECK_EQ(int(eq[2]), 64);

    fill_kit(g_kit);
    fill_out_from_kit(g_kit);
    macro_apply(macro, 127, g_out);
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            if (t == 2 && p == 5) continue;
            CHECK_EQ(int(g_out[t][p]), int(g_kit.tracks[t].params[p]));
        }
    }
    CHECK_EQ(int(g_out[2][5]), 127);
}

TEST(perform_scenes_keep_their_lfo_bytes_through_a_save) {
    fill_performance();
    Scene &scene = g_scenes.scenes[5];
    scene_set(scene, 2, kKitSoundParams + 0, 44);
    scene_set(scene, 2, kKitSoundParams + 3, 90);
    scene_clear(scene, 2, kKitSoundParams + 1);
    scene_clear(scene, 2, kKitSoundParams + 2);
    const uint32_t size = write_performance();
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    const Scene &loaded = g_loaded_scenes.scenes[5];
    CHECK(scene_has(loaded, 2, kKitSoundParams + 0));
    CHECK(!scene_has(loaded, 2, kKitSoundParams + 1));
    CHECK(!scene_has(loaded, 2, kKitSoundParams + 2));
    CHECK(scene_has(loaded, 2, kKitSoundParams + 3));
    CHECK_EQ(int(loaded.values[2][kKitSoundParams + 0]), 44);
    CHECK_EQ(int(loaded.values[2][kKitSoundParams + 3]), 90);
    for (int s = 0; s < kScenes; s++) {
        CHECK_EQ(scene_count(g_loaded_scenes.scenes[s]), scene_count(g_scenes.scenes[s]));
    }

    // The mask byte's high bits name nothing: a damaged one stores no phantom parameter.
    const uint32_t lfo = section_at(g_buffer, kSectionPerformSceneLfo);
    g_buffer[lfo + kKitLfoParams] = 0xFF;          // scene 0, track 0's mask byte
    refresh_crc(g_buffer);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    CHECK_EQ(int(g_loaded_scenes.scenes[0].stored[0][2] & ((1 << kKitLfoParams) - 1)),
             (1 << kKitLfoParams) - 1);
    // And the arp's bits come from the arp's own section, not from this byte.
    CHECK_EQ(int(g_loaded_scenes.scenes[0].stored[0][2] >> kKitLfoParams),
             int(g_scenes.scenes[0].stored[0][2] >> kKitLfoParams));
}

TEST(perform_a_file_from_before_the_lfo_loads_with_no_scene_storing_one) {
    fill_performance();
    for (int t = 0; t < kTracks; t++) scene_set(g_scenes.scenes[0], t, kKitSoundParams, 77);
    const uint32_t size = write_performance();
    // Before the LFO there was no scenes' EQ, arp or ASR section either: drop all four, last
    // first.
    const uint32_t sizes[4] = {3804u, 3152u, 3076u, 2424u};
    uint32_t old_size = size;
    for (int k = 0; k < 4; k++) {
        old_size = drop_last_section(g_buffer, g_alternate);
        CHECK_EQ(old_size, sizes[k]);
        for (uint32_t i = 0; i < old_size; i++) g_buffer[i] = g_alternate[i];
    }
    CHECK(old_size < size);
    // Whatever the set held before, the old file says no scene stores an LFO byte.
    for (int s = 0; s < kScenes; s++) {
        for (int t = 0; t < kTracks; t++) scene_set(g_loaded_scenes.scenes[s], t, kKitSoundParams, 5);
    }
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_alternate, old_size)),
             int(SerializeResult::ok));
    for (int s = 0; s < kScenes; s++) {
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitSoundParams; p++) {
                CHECK_EQ(scene_has(g_loaded_scenes.scenes[s], t, p), scene_has(g_scenes.scenes[s], t, p));
            }
            for (int p = kKitSoundParams; p < kKitParams; p++) {
                CHECK(!scene_has(g_loaded_scenes.scenes[s], t, p));
                CHECK_EQ(int(g_loaded_scenes.scenes[s].values[t][p]), 0);
            }
        }
    }
}

TEST(perform_the_morph_crosses_lfo_bytes_like_any_other) {
    // A scene can store the LFO's depth and rate, and the morph sweeps them.
    perform_init(g_scenes, g_macros);
    uint8_t out[kTracks][kKitParams] = {};
    scene_set(g_scenes.scenes[0], 1, kKitSoundParams + 2, 0);      // DEP 0
    scene_set(g_scenes.scenes[1], 1, kKitSoundParams + 2, 100);    // DEP 100
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, out);
    CHECK_EQ(int(out[1][kKitSoundParams + 2]), 50);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, out);
    CHECK_EQ(int(out[1][kKitSoundParams + 2]), 100);

    // And a macro drives one.
    MacroTarget target;
    target.enabled = 1;
    target.track = 1;
    target.param = uint8_t(kKitSoundParams + 2);
    target.high = 127;
    g_macros.macros[0].targets[0] = target;
    out[1][kKitSoundParams + 2] = 0;
    macro_apply(g_macros.macros[0], 127, out);
    CHECK_EQ(int(out[1][kKitSoundParams + 2]), 127);
}

TEST(perform_scenes_keep_their_arp_bytes_and_an_older_file_stores_none) {
    fill_performance();
    Scene &scene = g_scenes.scenes[6];
    scene_set(scene, 4, kKitLfoEnd + 0, 30);
    scene_set(scene, 4, kKitLfoEnd + 2, 100);
    scene_clear(scene, 4, kKitLfoEnd + 1);
    scene_clear(scene, 4, kKitLfoEnd + 3);
    scene_set(scene, 4, kKitSoundParams + 1, 12);       // an LFO byte beside them
    const uint32_t size = write_performance();
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    const Scene &loaded = g_loaded_scenes.scenes[6];
    CHECK(scene_has(loaded, 4, kKitLfoEnd + 0));
    CHECK(!scene_has(loaded, 4, kKitLfoEnd + 1));
    CHECK(scene_has(loaded, 4, kKitLfoEnd + 2));
    CHECK(!scene_has(loaded, 4, kKitLfoEnd + 3));
    CHECK(scene_has(loaded, 4, kKitSoundParams + 1));
    CHECK_EQ(int(loaded.values[4][kKitLfoEnd + 2]), 100);
    for (int s = 0; s < kScenes; s++) {
        CHECK_EQ(scene_count(g_loaded_scenes.scenes[s]), scene_count(g_scenes.scenes[s]));
    }

    // The file the LFO and kill EQ builds wrote: everything but the arp (and the ASR page after
    // it) comes back.
    drop_last_section(g_buffer, g_alternate);
    for (uint32_t i = 0; i < 3804u; i++) g_buffer[i] = g_alternate[i];
    const uint32_t old_size = drop_last_section(g_buffer, g_alternate);
    CHECK_EQ(old_size, 3152u);
    for (int s = 0; s < kScenes; s++) scene_set(g_loaded_scenes.scenes[s], 1, kKitLfoEnd, 9);
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_alternate, old_size)),
             int(SerializeResult::ok));
    CHECK(scene_has(g_loaded_scenes.scenes[6], 4, kKitSoundParams + 1));
    for (int s = 0; s < kScenes; s++) {
        for (int t = 0; t < kTracks; t++) {
            for (int p = kKitLfoEnd; p < kKitParams; p++) {
                CHECK(!scene_has(g_loaded_scenes.scenes[s], t, p));
                CHECK_EQ(int(g_loaded_scenes.scenes[s].values[t][p]), 0);
            }
        }
    }
}

TEST(perform_scenes_keep_their_asr_bytes_and_morph_them) {
    // The ASR page (attack, sustain, release, filter sustain) is stored per scene in its own
    // section, in the fourth mask byte, and the morph sweeps it like any other byte -- the fourth
    // byte of the mask must be read, or a scene storing only SUS would morph nothing.
    fill_performance();
    Scene &scene = g_scenes.scenes[3];
    scene_set(scene, 6, kKitArpEnd + 1, 110);
    scene_clear(scene, 6, kKitArpEnd + 2);
    const uint32_t size = write_performance();
    CHECK_EQ(int(perform_read(g_loaded_scenes, g_loaded_macros, g_buffer, size)),
             int(SerializeResult::ok));
    const Scene &loaded = g_loaded_scenes.scenes[3];
    CHECK(scene_has(loaded, 6, kKitArpEnd + 1));
    CHECK(!scene_has(loaded, 6, kKitArpEnd + 2));
    CHECK_EQ(int(loaded.values[6][kKitArpEnd + 1]), 110);
    for (int s = 0; s < kScenes; s++) {
        CHECK_EQ(scene_count(g_loaded_scenes.scenes[s]), scene_count(g_scenes.scenes[s]));
    }

    perform_init(g_scenes, g_macros);
    uint8_t out[kTracks][kKitParams] = {};
    scene_set(g_scenes.scenes[0], 6, kKitArpEnd + 1, 0);
    scene_set(g_scenes.scenes[1], 6, kKitArpEnd + 1, 100);
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale / 2, out);
    CHECK_EQ(int(out[6][kKitArpEnd + 1]), 50);
}
