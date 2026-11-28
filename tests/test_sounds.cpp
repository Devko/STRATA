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
#include "kick.hpp"
#include "perc.hpp"
#include "snare.hpp"
#include "chain.hpp"

#include <cmath>
#include <cstdio>
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
                         id == MachineId::wave_poly || id == MachineId::fm ||
                         id == MachineId::grain;
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
            // The other drums have no ASR page; a preset for one says so with zeroes. (KICK's is
            // PNCH, dsp/kick.hpp.)
            if (!machine_has_asr_page(uint8_t(m))) {
                for (int k = 0; k < 4; k++) CHECK_EQ(int(s->asr[k]), 0);
            }
            // A held sound is loud: five held voices are far more than a 200 ms stab, so a VA
            // preset with SUS up keeps LVL down (core/sounds.cpp).
            if ((m == int(MachineId::va_poly) || m == int(MachineId::wave_poly) ||
                 m == int(MachineId::fm)) && s->asr[1] > 0)
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
            const uint64_t mask = sound_preset_writes(uint8_t(m));
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
    // The synths read ASR; KICK reads its slots as PNCH, the other drums not at all.
    CHECK(((sound_preset_writes(uint8_t(MachineId::kick)) >> kSoundAsrFirst) & 1u) != 0);   // PNCH
    CHECK(((sound_preset_writes(uint8_t(MachineId::snare)) >> kSoundAsrFirst) & 1u) == 0);
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

// --- switching machines: the new machine's default sound (Roland, 2026-11-23) -----------------

namespace {

// The KitTrack parameters `machine` reads for its sound: SRC, FLTR and AMP, plus ASR and OSC on
// the machines that read them. Everything machine_defaults_apply must leave no stale byte in.
bool reads_for_sound(uint8_t machine, int p) {
    if (p < 12) return true;
    if (p >= kSoundAsrFirst && p < kSoundAsrFirst + 4) return machine_has_asr_page(machine);
    if (p >= kSoundScnFirst && p < kSoundScnFirst + 4) return machine_reads_scn(machine);
    return machine_has_osc_page(machine) && p >= kSoundOscFirst && p < kSoundOscFirst + 4;
}

}  // namespace

TEST(sounds_every_machine_but_mid_has_defaults_for_a_switch) {
    uint8_t params[kKitParams] = {};
    for (int m = 0; m < int(MachineId::count); m++) {
        const bool has = machine_defaults_apply(uint8_t(m), params);
        // MID's knobs are CCs for someone else's instrument. GRAIN's defaults are CLOUD, preset 0
        // of its SOUNDS list.
        const bool exempt = m == int(MachineId::midi);
        if (!exempt && !has) std::printf("    machine %d has no defaults\n", m);
        CHECK(exempt || has);
        if (m == int(MachineId::midi)) CHECK(!has);
    }
}

TEST(sounds_a_switch_leaves_no_parameter_of_the_new_machine_stale) {
    // Whatever the old machine left -- all 0s or all 127s here -- every byte the new machine reads
    // for its sound comes out the same, and the mix, LFO and ARP bytes are untouched.
    for (int m = 0; m < int(MachineId::count); m++) {
        uint8_t low[kKitParams], high[kKitParams];
        std::memset(low, 0, sizeof low);
        std::memset(high, 127, sizeof high);
        const bool a = machine_defaults_apply(uint8_t(m), low);
        const bool b = machine_defaults_apply(uint8_t(m), high);
        CHECK_EQ(a, b);
        if (!a) continue;
        for (int p = 0; p < kKitParams; p++) {
            if (reads_for_sound(uint8_t(m), p)) {
                if (low[p] != high[p]) std::printf("    machine %d param %d stale\n", m, p);
                CHECK_EQ(int(low[p]), int(high[p]));
                CHECK(low[p] <= 127);
            } else {
                CHECK_EQ(int(low[p]), 0);
                CHECK_EQ(int(high[p]), 127);
            }
        }
    }
}

TEST(sounds_a_switch_uses_the_first_preset_and_sample_plays_whole) {
    for (int m = 0; m < int(MachineId::count); m++) {
        if (sound_preset_count(uint8_t(m)) == 0) continue;
        uint8_t params[kKitParams] = {};
        CHECK(machine_defaults_apply(uint8_t(m), params));
        CHECK(sound_preset_matches(uint8_t(m), 0, params));
    }
    // SAMPLE agrees with the browser's pick on where in the file it plays.
    uint8_t params[kKitParams];
    std::memset(params, 33, sizeof params);
    CHECK(machine_defaults_apply(uint8_t(MachineId::sample), params));
    for (const ParamReset &r : kSamplePickResets) CHECK_EQ(int(params[r.slot]), int(r.value));
    CHECK_EQ(int(params[0]), 64);       // PIT unity
    CHECK_EQ(int(params[4]), 0);        // DRV off
    CHECK_EQ(int(params[5]), 127);      // CUT open
    CHECK_EQ(int(params[7]), 0);        // MOD off
}

TEST(sounds_switching_a_track_loads_the_new_machines_defaults) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    // Track 6 is VA MONO: leave odd values everywhere, then switch it to SAMPLE.
    for (int p = 0; p < kTrackParams; p++) {
        if (track_param_real(p)) post(Cmd::set_param, 5, 0, uint8_t(p), 37);
    }
    pump(2);
    post(Cmd::set_machine, 5, 0, uint8_t(MachineId::sample));
    pump(2);
    uint8_t want[kKitParams];
    // 37 where the parameter goes that high: WAV and DST stop at their last number.
    for (int p = 0; p < kKitParams; p++) want[p] = kit_param_max(p) < 37 ? kit_param_max(p) : 37;
    for (int p = kKitOscEnd; p < kKitScnFirst; p++) want[p] = 0;   // the hole takes no value
    CHECK(machine_defaults_apply(uint8_t(MachineId::sample), want));
    for (int p = 0; p < kTrackParams; p++) {
        CHECK_EQ(int(e.params(5).v[p]), int(want[p]));
        CHECK_EQ(int(e.performed(5, p)), int(want[p]));
    }
    CHECK_EQ(int(e.params(5).v[1]), 0);           // STA
    CHECK_EQ(int(e.params(5).v[kFx + 1]), 37);    // PAN stayed

    // And to MID: nothing is written.
    post(Cmd::set_machine, 5, 0, uint8_t(MachineId::midi));
    pump(2);
    for (int p = 0; p < kTrackParams; p++) CHECK_EQ(int(e.params(5).v[p]), int(want[p]));
}

TEST(sounds_a_switch_to_grain_loads_cloud_from_the_start_of_the_sample_at_its_pitch) {
    // The SOUNDS list never moves POS or PIT; a switch to GRN sets them too, so a cloud starts at
    // the sample's first frame and its own pitch rather than wherever the old machine left them.
    uint8_t params[kKitParams];
    for (int p = 0; p < kKitParams; p++) params[p] = 99;
    CHECK(machine_defaults_apply(uint8_t(MachineId::grain), params));
    CHECK_EQ(int(params[0]), 0);
    CHECK_EQ(int(params[8]), 64);
    CHECK(sound_preset_matches(uint8_t(MachineId::grain), 0, params));
    CHECK_EQ(std::strcmp(sound_preset(uint8_t(MachineId::grain), 0)->name, "CLOUD"), 0);
}

// --- levels ---------------------------------------------------------------

namespace {

// One hit of a PERC preset through the machine alone, its energy over the first 400 ms in dB:
// the window a listener judges a hit's loudness by. Note 60, velocity 100, the preset's own
// SRC and AMP bytes; the insert chain is not involved, and PERC's presets leave it flat.
float perc_hit_db(const SoundPreset &s) {
    static float buffer[19200];
    TrackParams p{};
    for (int i = 0; i < 4; i++) {
        p.set(i, s.src[i]);
        p.set(8 + i, s.amp[i]);
    }
    Perc m;
    m.init(48000.0f);
    m.prepare(p);
    Trig t;
    t.note = 60;
    t.velocity = 100;
    m.trigger(t);
    for (float &x : buffer) x = 0.0f;
    for (int b = 0; b < 19200 / 64; b++) {
        m.prepare(p);
        m.render_add(buffer + b * 64, 64);
    }
    double sum = 0.0;
    for (float x : buffer) sum += double(x) * double(x);
    return float(10.0 * std::log10(sum + 1e-20));
}

}  // namespace

TEST(sounds_ringing_and_short_perc_presets_land_within_a_few_db) {
    // Before the rebalance (2026-11-27) the ringing presets (BELL, CHIME, METAL) were about 10 dB
    // louder over a hit than the short ones (CLAVE, KNOCK, BLIP, ZAP): flipping through the list
    // jumped in level more than it changed in sound. LVL now evens them out. Toms and congas are
    // left out: plain energy counts their low end far louder than an ear does.
    const uint8_t perc = uint8_t(MachineId::perc);
    static const char *const kCompared[] = {"BELL", "CHIME", "METAL", "CLAVE", "KNOCK", "BLIP", "ZAP"};
    float lo = 1e9f, hi = -1e9f;
    int found = 0;
    for (int i = 0; i < sound_preset_count(perc); i++) {
        const SoundPreset &s = *sound_preset(perc, i);
        bool compared = false;
        for (const char *name : kCompared) compared = compared || std::strcmp(s.name, name) == 0;
        if (!compared) continue;
        found++;
        const float db = perc_hit_db(s);
        lo = db < lo ? db : lo;
        hi = db > hi ? db : hi;
    }
    CHECK_EQ(found, 7);
    CHECK(hi - lo < 4.5f);
}

// Each kit byte comes from its own page and place, at every page's first byte; a preset matches
// only when every byte it writes is its own, the first too; GRAIN's write mask spares POS and PIT.
TEST(sounds_bytes_come_from_their_own_page_and_matching_checks_them_all) {
    const uint8_t fm = uint8_t(MachineId::fm);
    for (int i = 0; i < sound_preset_count(fm); i++) {
        const SoundPreset *s = sound_preset(fm, i);
        uint8_t params[kKitParams];
        std::memset(params, 0xEE, sizeof params);
        CHECK(sound_preset_apply(fm, i, params));
        for (int p = 0; p < 4; p++) {
            CHECK_EQ(int(params[p]), int(s->src[p]));
            CHECK_EQ(int(params[4 + p]), int(s->fltr[p]));
            CHECK_EQ(int(params[8 + p]), int(s->amp[p]));
            CHECK_EQ(int(params[kSoundAsrFirst + p]), int(s->asr[p]));
            CHECK_EQ(int(params[kSoundOscFirst + p]), int(s->osc[p]));
        }
        CHECK(sound_preset_matches(fm, i, params));
        for (int p = 0; p < kKitParams; p++) {
            if (((sound_preset_writes(fm) >> p) & 1u) == 0) continue;
            params[p] ^= 1;
            CHECK(!sound_preset_matches(fm, i, params));
            params[p] ^= 1;
        }
        CHECK(!sound_preset_matches(fm, i, nullptr));
    }
    const uint32_t grain = sound_preset_writes(uint8_t(MachineId::grain));
    CHECK_EQ(grain & 1u, 0u);            // POS
    CHECK((grain >> 1) & 1u);
    CHECK_EQ((grain >> 8) & 1u, 0u);     // PIT
    // SAMPLE's defaults: no resonance, filter OFF, no attack, forward one-shot.
    uint8_t sample[kKitParams] = {};
    std::memset(sample, 0xEE, sizeof sample);
    CHECK(machine_defaults_apply(uint8_t(MachineId::sample), sample));
    CHECK_EQ(int(sample[6]), 0);
    CHECK_EQ(int(sample[7]), 0);
    CHECK_EQ(int(sample[8]), 0);
    CHECK_EQ(int(sample[10]), 0);
}

TEST(sounds_the_wave_pair_has_scanning_presets_and_the_rest_scan_nothing) {
    // A preset with SCN bytes writes them; one without writes the kit's default (MOD OFF), so a
    // pick on a track that was scanning stops the scan. Each WAVE machine has at least two that
    // scan, and the first preset -- the one a fresh track plays -- is not one of them.
    const MachineId pair[] = {MachineId::wave_mono, MachineId::wave_poly};
    for (const MachineId machine : pair) {
        const uint8_t id = uint8_t(machine);
        int scanning = 0;
        for (int i = 0; i < sound_preset_count(id); i++) {
            uint8_t params[kKitParams] = {};
            for (int p = kSoundScnFirst; p < kKitParams; p++) params[p] = 77;
            CHECK(sound_preset_apply(id, i, params));
            const SoundPreset *preset = sound_preset(id, i);
            const bool scans = preset->scn[0] != 0;
            for (int k = 0; k < kKitScnParams; k++) {
                const int want = scans ? preset->scn[k] : kKitScnDefault[k];
                CHECK_EQ(int(params[kSoundScnFirst + k]), want);
                CHECK(params[kSoundScnFirst + k] <= kit_param_max(kSoundScnFirst + k));
            }
            CHECK(sound_preset_matches(id, i, params));
            if (scans) {
                scanning++;
                CHECK(preset->scn[2] != preset->src[1]);     // END apart from POS: it travels
            }
        }
        CHECK(scanning >= 2);
        CHECK_EQ(int(sound_preset(id, 0)->scn[0]), 0);
    }
    // Only the WAVE pair reads the page.
    for (int m = 0; m < kMachines; m++) {
        const MachineId machine = MachineId(m);
        CHECK_EQ(machine_reads_scn(uint8_t(m)),
                 machine == MachineId::wave_mono || machine == MachineId::wave_poly);
    }
}

namespace {

const SoundPreset &named(MachineId machine, const char *name) {
    const uint8_t m = uint8_t(machine);
    for (int i = 0; i < sound_preset_count(m); i++)
        if (std::strcmp(sound_preset(m, i)->name, name) == 0) return *sound_preset(m, i);
    CHECK(false);
    return *sound_preset(m, 0);
}

// One hit of a drum preset through its machine and the insert chain's drive and filter (the
// chain's tap: before the fader, pan and sends), `frames` frames, a multiple of 64.
template <class M>
void drum_hit(const SoundPreset &s, float *out, int frames) {
    TrackParams p{};
    for (int i = 0; i < 4; i++) {
        p.set(i, s.src[i]);
        p.set(4 + i, s.fltr[i]);
        p.set(8 + i, s.amp[i]);
    }
    p.set(kFx + 1, 64);    // centred
    M m;
    m.init(48000.0f);
    m.prepare(p);
    InsertChain chain;
    chain.init(48000.0f);
    Trig t;
    t.note = 60;
    t.velocity = 100;
    m.trigger(t);
    static float block[64], dry_l[64], dry_r[64];
    ChainOut to;
    to.dry_l = dry_l;
    to.dry_r = dry_r;
    for (int i = 0; i < frames; i++) out[i] = 0.0f;
    for (int b = 0; b < frames / 64; b++) {
        for (float &x : block) x = 0.0f;
        m.prepare(p);
        m.render_add(block, 64);
        chain.prepare(p, ChainMix{});
        to.tap = out + b * 64;
        chain.process(block, 64, to);
    }
}

// The energy of x[from, from + n) above (high = true) or below `hz`, through three one-poles in
// a row (18 dB an octave), so a kick's 50 Hz body does not leak into a band two octaves up.
float band_energy(const float *x, int from, int n, float hz, bool high) {
    const float a = 1.0f - std::exp(-6.2831853f * hz / 48000.0f);
    float lp[3] = {0.0f, 0.0f, 0.0f};
    double sum = 0.0;
    for (int i = 0; i < from + n; i++) {
        float y = x[i];
        for (float &state : lp) {
            state += a * (y - state);
            y = high ? y - state : state;
        }
        if (i >= from) sum += double(y) * double(y);
    }
    return float(sum);
}

// The energy of x[0, n) between `low_hz` and `high_hz`: three one-pole high-passes, then three
// low-passes.
float band_pass_energy(const float *x, int n, float low_hz, float high_hz) {
    const float a = 1.0f - std::exp(-6.2831853f * low_hz / 48000.0f);
    const float b = 1.0f - std::exp(-6.2831853f * high_hz / 48000.0f);
    float hp[3] = {0.0f, 0.0f, 0.0f}, lp[3] = {0.0f, 0.0f, 0.0f};
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        float y = x[i];
        for (float &state : hp) {
            state += a * (y - state);
            y -= state;
        }
        for (float &state : lp) {
            state += b * (y - state);
            y = state;
        }
        sum += double(y) * double(y);
    }
    return float(sum);
}

}  // namespace

// The techno benchmark (2026-11-28) measured the presets against techno one-shots from sample
// libraries. These pin the two findings presets could fix, as properties of the sound rather
// than as bytes: a later re-voicing can move every number as long as these still hold.
TEST(sounds_techno_kicks_put_more_knock_in_the_attack_than_the_909) {
    // The 909's first 20 ms was 11-25 dB short of the references above 250 Hz. The techno kicks,
    // with the chain's drive and the PNCH page their presets turn up, must beat it there by more
    // than 6 dB (measured: 11-15).
    static float a[9600], b[9600];
    drum_hit<Kick>(named(MachineId::kick, "909"), a, 9600);
    // The first 20 ms above 300 Hz against the same 20 ms under 150 Hz: the knock against the body.
    const float base = band_energy(a, 0, 960, 300.0f, true) / band_energy(a, 0, 960, 150.0f, false);
    static const char *const kTechno[] = {"TECHNO", "WHOUSE", "HEAVY", "MINIM"};
    for (const char *name : kTechno) {
        drum_hit<Kick>(named(MachineId::kick, name), b, 9600);
        const float knock = band_energy(b, 0, 960, 300.0f, true) / band_energy(b, 0, 960, 150.0f, false);
        CHECK(knock > 4.0f * base);   // at least 6 dB more of the attack above 300 Hz
    }
}

TEST(sounds_techno_claps_reach_down_to_one_kilohertz) {
    // The old voicing at TUN 64 high-passed the noise at about 2 kHz (ten times TUN) and filled
    // 200 Hz with the drum body; the references peak at 1-2 kHz with 250 Hz 20 dB down. So: more
    // of each techno clap sits between 700 Hz and 2 kHz than the factory CLAP's, and less of it
    // under 400 Hz.
    static float x[9600];
    auto shares = [&](const SoundPreset &s, float *mid, float *low) {
        drum_hit<Snare>(s, x, 9600);
        const float total = band_energy(x, 0, 9600, 1.0f, true);
        *mid = band_pass_energy(x, 9600, 700.0f, 2000.0f) / total;
        *low = band_energy(x, 0, 9600, 400.0f, false) / total;
    };
    float clap_mid, clap_low;
    shares(named(MachineId::snare, "CLAP"), &clap_mid, &clap_low);
    static const char *const kClaps[] = {"TCLAP", "CLAP4", "DARK"};
    for (const char *name : kClaps) {
        float mid, low;
        shares(named(MachineId::snare, name), &mid, &low);
        CHECK(mid > clap_mid);
        CHECK(low < clap_low);
    }
}
