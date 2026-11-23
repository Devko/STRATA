// The factory sounds (core/sounds.hpp) and Cmd::sound_preset, which loads one into a track.
//
// Content, not behaviour, for the tables -- which knob holds which number is a sound choice, not
// a contract -- so these pin that every preset is valid and drawable, that each list starts
// with the factory kit's own sound, and that a preset writes the pages it says it writes and no
// others. Then the command: it changes what plays, touches one track, refuses a preset meant for
// another machine, and AS WAS puts back exactly what was there.
#include "check.hpp"
#include "apprig.hpp"

#include "factory.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "sounds.hpp"

#include <cstring>

using namespace strata;

namespace {

Project g_factory;
SceneSet g_scenes;
MacroSet g_macros;

constexpr int kFx = kFactoryFx;
constexpr int kLfoFirst = kKitSoundParams;

bool drawable(const char *name) {
    // The 5x7 font: upper case, digits and a few symbols (tests/test_ui.cpp, kFontChars).
    for (const char *p = name; *p; p++) {
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

}  // namespace

TEST(sounds_every_machine_that_makes_sound_from_knobs_has_a_list_and_the_rest_none) {
    for (int m = 0; m < int(MachineId::count); m++) {
        const MachineId id = MachineId(m);
        const bool has = id == MachineId::kick || id == MachineId::snare || id == MachineId::hat ||
                         id == MachineId::perc || id == MachineId::va_mono ||
                         id == MachineId::va_poly || id == MachineId::wave_mono ||
                         id == MachineId::wave_poly;
        CHECK_EQ(sound_preset_count(uint8_t(m)) > 0, has);
        CHECK_EQ(sound_preset_writes(uint8_t(m)) != 0, has);
        // AS WAS and at most two pages of the SOUNDS list, which scrolls (the VAs have more).
        CHECK(sound_preset_count(uint8_t(m)) <= 15);
        if (has) CHECK(sound_preset_count(uint8_t(m)) >= 5);
    }
    CHECK(sound_preset(uint8_t(MachineId::kick), -1) == nullptr);
    CHECK(sound_preset(uint8_t(MachineId::kick), sound_preset_count(uint8_t(MachineId::kick))) ==
          nullptr);
    CHECK(sound_preset(uint8_t(MachineId::sample), 0) == nullptr);
    CHECK(sound_preset(200, 0) == nullptr);
}

TEST(sounds_every_preset_is_named_drawably_uniquely_and_holds_knob_bytes) {
    for (int m = 0; m < int(MachineId::count); m++) {
        const int n = sound_preset_count(uint8_t(m));
        for (int i = 0; i < n; i++) {
            const SoundPreset *s = sound_preset(uint8_t(m), i);
            CHECK(s != nullptr && s->name != nullptr);
            const int length = int(std::strlen(s->name));
            CHECK(length >= 1 && length <= kSoundNameChars);
            CHECK(drawable(s->name));
            for (int j = 0; j < i; j++) CHECK(std::strcmp(sound_preset(uint8_t(m), j)->name, s->name) != 0);
            for (int k = 0; k < 4; k++) {
                CHECK(s->src[k] <= 127 && s->fltr[k] <= 127 && s->amp[k] <= 127 && s->asr[k] <= 127);
            }
            // The drums have no ASR page; a preset for one says so with zeroes.
            if (!machine_reads_asr(uint8_t(m))) {
                for (int k = 0; k < 4; k++) CHECK_EQ(int(s->asr[k]), 0);
            }
            // A held sound is loud: five held voices are far more than a 200 ms stab, so a VA
            // preset with SUS up keeps LVL down (core/sounds.cpp).
            if ((m == int(MachineId::va_poly) || m == int(MachineId::wave_poly)) && s->asr[1] > 0)
                CHECK(s->amp[3] <= 72);
        }
    }
}

TEST(sounds_each_list_starts_with_the_factory_kits_own_sound) {
    factory_project(g_factory, g_scenes, g_macros);
    const Kit &kit = g_factory.kit;
    // Track by track: KICK, CLAP (SNARE), the two hats, PERC, BASS (VA MONO), CHORD (VA POLY).
    CHECK(sound_preset_matches(uint8_t(MachineId::kick), 0, kit.tracks[0].params));
    CHECK(sound_preset_matches(uint8_t(MachineId::snare), 0, kit.tracks[1].params));
    CHECK(sound_preset_matches(uint8_t(MachineId::hat), 0, kit.tracks[2].params));
    CHECK(sound_preset_matches(uint8_t(MachineId::hat), 1, kit.tracks[3].params));
    CHECK(sound_preset_matches(uint8_t(MachineId::perc), 0, kit.tracks[4].params));
    CHECK(sound_preset_matches(uint8_t(MachineId::va_mono), 0, kit.tracks[5].params));
    CHECK(sound_preset_matches(uint8_t(MachineId::va_poly), 0, kit.tracks[6].params));
    // And no other preset is mistaken for it.
    CHECK(!sound_preset_matches(uint8_t(MachineId::kick), 1, kit.tracks[0].params));
}

TEST(sounds_poly_pad_is_a05s_pad) {
    // The PAD preset is A05's locked sound, so loading it on the chord track and clearing A05's
    // locks gives the same pad.
    factory_project(g_factory, g_scenes, g_macros);
    const Pattern &a05 = g_factory.patterns[0][kFactoryPadSlot];
    int pad = -1;
    for (int i = 0; i < sound_preset_count(uint8_t(MachineId::va_poly)); i++) {
        if (std::strcmp(sound_preset(uint8_t(MachineId::va_poly), i)->name, "PAD") == 0) pad = i;
    }
    CHECK(pad >= 0);
    uint8_t params[kKitParams];
    std::memcpy(params, g_factory.kit.tracks[6].params, sizeof params);
    sound_preset_apply(uint8_t(MachineId::va_poly), pad, params);
    int checked = 0;
    for (int p = 0; p < kKitParams; p++) {
        uint8_t locked = 0;
        if (!pattern_get_lock(a05, 6, 0, uint8_t(p), locked)) continue;
        if (p == kFactoryFx + 3) continue;           // SNB: the mix, which a preset leaves alone
        CHECK_EQ(int(params[p]), int(locked));
        checked++;
    }
    CHECK(checked >= 6);
}

TEST(sounds_a_preset_writes_its_pages_and_leaves_the_mix_the_lfo_and_the_arp) {
    for (int m = 0; m < int(MachineId::count); m++) {
        for (int i = 0; i < sound_preset_count(uint8_t(m)); i++) {
            uint8_t params[kKitParams];
            std::memset(params, 0xAA, sizeof params);         // not a knob byte: shows any write
            CHECK(sound_preset_apply(uint8_t(m), i, params));
            CHECK(sound_preset_matches(uint8_t(m), i, params));
            const uint32_t mask = sound_preset_writes(uint8_t(m));
            for (int p = 0; p < kKitParams; p++) {
                const bool written = params[p] != 0xAA;
                CHECK_EQ(written, ((mask >> p) & 1u) != 0);
            }
            // The FX page, the LFO and the arp: never.
            for (int p = kFx; p < kFx + 4; p++) CHECK_EQ(int(params[p]), 0xAA);
            for (int p = kLfoFirst; p < kSoundAsrFirst; p++) CHECK_EQ(int(params[p]), 0xAA);
        }
    }
    // HAT's choke group is wiring, not sound.
    CHECK(((sound_preset_writes(uint8_t(MachineId::hat)) >> (kFactoryAmp + 2)) & 1u) == 0);
    // Only the VAs read ASR.
    CHECK(((sound_preset_writes(uint8_t(MachineId::kick)) >> kSoundAsrFirst) & 1u) == 0);
    CHECK(((sound_preset_writes(uint8_t(MachineId::va_mono)) >> kSoundAsrFirst) & 1u) != 0);
    uint8_t params[kKitParams] = {};
    CHECK(!sound_preset_apply(uint8_t(MachineId::sample), 0, params));
    CHECK(!sound_preset_apply(uint8_t(MachineId::kick), 99, params));
    CHECK(!sound_preset_matches(uint8_t(MachineId::kick), 99, params));
}

// --- Cmd::sound_preset ------------------------------------------------------

TEST(sounds_the_command_changes_what_plays_on_that_track_only) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    TrackParams before[kTracks];
    for (int t = 0; t < kTracks; t++) before[t] = e.params(t);

    // KICK's 808 into track 1.
    post(Cmd::sound_preset, 0, 0, uint8_t(MachineId::kick), 1);
    pump(2);
    CHECK(sound_preset_matches(uint8_t(MachineId::kick), 1, e.params(0).v));
    for (int p = 0; p < kTrackParams; p++) CHECK_EQ(int(e.performed(0, p)), int(e.params(0).v[p]));
    for (int t = 1; t < kTracks; t++) CHECK(std::memcmp(e.params(t).v, before[t].v, kTrackParams) == 0);
    // The mix stayed: pan and sends as they were.
    for (int p = kFx; p < kFx + 4; p++) CHECK_EQ(int(e.params(0).v[p]), int(before[0].v[p]));
}

TEST(sounds_the_command_refuses_a_preset_for_a_machine_the_track_does_not_host) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    const TrackParams before = e.params(0);
    const uint32_t refused = e.status().refused.load(std::memory_order_relaxed);
    post(Cmd::sound_preset, 0, 0, uint8_t(MachineId::va_mono), 1);     // track 1 is KICK
    post(Cmd::sound_preset, 0, 0, uint8_t(MachineId::kick), 50);       // no such preset
    post(Cmd::sound_preset, 2, 0, uint8_t(MachineId::hat), kSoundPresetBack);   // nothing kept
    pump(2);
    CHECK(std::memcmp(e.params(0).v, before.v, kTrackParams) == 0);
    CHECK_EQ(int(e.status().refused.load(std::memory_order_relaxed) - refused), 3);
}

TEST(sounds_as_was_puts_back_exactly_what_the_track_had_and_nothing_else) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    // The user's own bass: a tweak on a preset-written knob and one on the mix.
    post(Cmd::set_param, 5, 0, 2, 99);
    pump(2);
    const TrackParams mine = e.params(5);
    post(Cmd::sound_preset, 5, 0, 0, kSoundPresetKeep);
    post(Cmd::sound_preset, 5, 0, uint8_t(MachineId::va_mono), 1);      // ACID
    post(Cmd::sound_preset, 5, 0, uint8_t(MachineId::va_mono), 6);      // DRONE
    pump(2);
    CHECK(sound_preset_matches(uint8_t(MachineId::va_mono), 6, e.params(5).v));
    // A pan move made while trying sounds is the user's and survives AS WAS.
    post(Cmd::set_param, 5, 0, uint8_t(kFx + 1), 30);
    post(Cmd::sound_preset, 5, 0, uint8_t(MachineId::va_mono), kSoundPresetBack);
    pump(2);
    for (int p = 0; p < kTrackParams; p++) {
        const int want = p == kFx + 1 ? 30 : int(mine.v[p]);
        CHECK_EQ(int(e.params(5).v[p]), want);
    }
    // Remembering is per track: track 6's keep is not track 5's.
    post(Cmd::sound_preset, 6, 0, 0, kSoundPresetKeep);
    const uint32_t refused = e.status().refused.load(std::memory_order_relaxed);
    post(Cmd::sound_preset, 5, 0, uint8_t(MachineId::va_mono), kSoundPresetBack);
    pump(2);
    CHECK_EQ(int(e.status().refused.load(std::memory_order_relaxed) - refused), 1);
}
