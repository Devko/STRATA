// The factory project (core/factory.hpp): what a first boot and a NEW project hold.
//
// Content, not behaviour, so these do not pin which note is on which step. They pin that the
// content is VALID -- every byte inside the domain its loader would clamp it to, so nothing in it
// changes across a save -- that it survives the round trip through all four file types, that it
// is playable (levels up, every track has something to play), that it keeps the promises the
// header makes (chain off, macros and scenes neutral at rest, melodic notes in the key),
// and that NEW and the card-less boot both get it.
#include "check.hpp"
#include "apprig.hpp"
#include "fakehost.hpp"

#include "chord.hpp"
#include "factory.hpp"
#include "perform.hpp"
#include "project.hpp"
#include "scale.hpp"
#include "vapoly.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace strata;
using strata::test::FakeHost;

namespace {

// File scope: a Project is a megabyte and a stack frame is capped at 16 KiB.
Project g_made, g_back;
SceneSet g_scenes, g_scenes_back;
MacroSet g_macros, g_macros_back;
uint8_t g_buffer[kBankMaxBytes];

void make() { factory_project(g_made, g_scenes, g_macros); }

constexpr int kLvl = kFactoryAmp + 3;
constexpr int kChordTrack = 6;
constexpr int kAtkParam = kFactoryAsr + 0, kSusParam = kFactoryAsr + 1, kRelParam = kFactoryAsr + 2;

bool steps_equal(const Step &a, const Step &b) {
    return a.flags == b.flags && a.note == b.note && a.velocity == b.velocity &&
           a.length == b.length && a.micro == b.micro && a.probability == b.probability &&
           a.condition == b.condition && a.ratchet == b.ratchet && a.lock_count == b.lock_count &&
           a.chord == b.chord;
}

int trigs_on(const Pattern &p, int track) {
    int n = 0;
    for (int s = 0; s < p.tracks[track].length; s++) n += (p.tracks[track].steps[s].flags & kStepTrig) != 0;
    return n;
}

}  // namespace

TEST(factory_the_kit_is_the_spec_kit_and_every_track_can_be_heard) {
    make();
    const MachineId spec[kTracks] = {MachineId::kick,  MachineId::snare,   MachineId::hat,
                                     MachineId::hat,   MachineId::perc,    MachineId::va_mono,
                                     MachineId::va_poly, MachineId::sample};
    for (int t = 0; t < kTracks; t++) {
        const KitTrack &k = g_made.kit.tracks[t];
        CHECK_EQ(int(k.machine), int(spec[t]));
        // A blank kit has LVL 0: that was the silent NEW project this module exists to fix.
        CHECK(k.params[kLvl] >= 64);
        CHECK(k.level > 0.5f && k.level <= 1.5f);           // 1.5: the MIXER fader ceiling
        for (int i = 0; i < kKitParams; i++) CHECK(k.params[i] <= 127);
        CHECK_EQ(int(k.sample[0]), 0);
    }
}

TEST(factory_bank_a_has_five_patterns_that_play_and_the_rest_are_empty) {
    make();
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        const Pattern &p = g_made.patterns[0][slot];
        int total = 0;
        for (int t = 0; t < kTracks; t++) total += trigs_on(p, t);
        if (slot < kFactoryPatterns) {
            CHECK(total > 16);
        } else {
            CHECK_EQ(total, 0);
        }
    }
    for (int bank = 1; bank < kBanks; bank++) {
        for (int t = 0; t < kTracks; t++) CHECK_EQ(trigs_on(g_made.patterns[bank][0], t), 0);
    }
    // The first pattern -- what PLAY gives on a fresh project -- uses all eight tracks.
    for (int t = 0; t < kTracks; t++) CHECK(trigs_on(g_made.patterns[0][0], t) > 0);
    CHECK_EQ(int(g_made.bank), 0);
    CHECK_EQ(int(g_made.slot), 0);
}

TEST(factory_every_step_is_already_valid) {
    // step_sanitize is what a load applies; a step it would change would come back different
    // after the first save.
    make();
    for (int slot = 0; slot < kFactoryPatterns; slot++) {
        const Pattern &p = g_made.patterns[0][slot];
        CHECK(p.swing >= kSwingMin && p.swing <= kSwingMax);
        CHECK(p.master_length >= 1 && p.master_length <= kStepsPerTrack);
        CHECK(p.root < 12);
        CHECK(p.seed != 0);
        for (int t = 0; t < kTracks; t++) {
            CHECK(p.tracks[t].length >= 1 && p.tracks[t].length <= kStepsPerTrack);
            for (int s = 0; s < kStepsPerTrack; s++) {
                Step copy = p.tracks[t].steps[s];
                step_sanitize(copy);
                CHECK(steps_equal(copy, p.tracks[t].steps[s]));
                CHECK(condition_valid(p.tracks[t].steps[s].condition));
                CHECK(chord_valid(p.tracks[t].steps[s].chord));
            }
        }
        // No lock leaked: every slot taken is reachable, so the pool is already compact.
        CHECK_EQ(int(pattern_locks_reachable(p)), int(p.locks_used));
    }
}

TEST(factory_melodic_notes_are_in_the_key_and_slices_skip_the_one_that_is_not) {
    make();
    const uint8_t key = g_made.settings.root;
    const int scale = int(g_made.settings.scale);
    for (int slot = 0; slot < kFactoryPatterns; slot++) {
        const Pattern &p = g_made.patterns[0][slot];
        CHECK_EQ(int(p.root), int(key % 12));
        CHECK_EQ(int(p.scale), scale);
        for (int t = 0; t < kTracks; t++) {
            const uint8_t machine = g_made.kit.tracks[t].machine;
            for (int s = 0; s < p.tracks[t].length; s++) {
                const Step &step = p.tracks[t].steps[s];
                if (!(step.flags & kStepTrig)) continue;
                if (machine == uint8_t(MachineId::va_mono) || machine == uint8_t(MachineId::va_poly)) {
                    // As written, not only as folded: a part that relies on the fold sounds
                    // different the moment someone changes the scale.
                    CHECK(scale_contains(step.note, key, scale));
                }
                if (machine == uint8_t(MachineId::sample)) {
                    // Note 60 + k plays slice k of the placeholder: k = 1..7 only.
                    CHECK(step.note >= 61 && step.note <= 67);
                }
            }
        }
    }
}

TEST(factory_the_chain_names_factory_patterns_and_is_off) {
    make();
    CHECK(!g_made.settings.chain_enabled);
    CHECK(g_made.settings.chain_length > 0);
    for (int i = 0; i < kChainEntries; i++) {
        const ChainEntry &e = g_made.chain[i];
        if (i < g_made.settings.chain_length) {
            CHECK_EQ(int(e.bank), 0);
            CHECK(e.slot < kFactoryPatterns);
            CHECK(e.repeats >= 1 && e.repeats <= kChainRepeatsMax);
        } else {
            CHECK_EQ(int(e.repeats), 0);
        }
    }
}

TEST(factory_macros_start_at_the_kit_and_scene_a_is_the_kit) {
    make();
    // Three macros, every target starting at the kit's own value (so the first detent is a nudge,
    // not a jump) and going somewhere (so the knob does something).
    int targets = 0;
    // Macro A stays empty: pad pressure bends it on whichever track is selected.
    CHECK_EQ(macro_count(g_macros.macros[0]), 0);
    for (int m = 1; m < kMacros; m++) {
        CHECK(macro_count(g_macros.macros[m]) > 0);
        for (const MacroTarget &t : g_macros.macros[m].targets) {
            if (!t.enabled) continue;
            targets++;
            CHECK(t.track < kTracks && t.param < kKitParams);
            CHECK_EQ(int(t.low), int(g_made.kit.tracks[t.track].params[t.param]));
            CHECK(t.high != t.low && t.high <= 127);
        }
    }
    CHECK(targets >= 8);

    // Slots A and B are scenes 1 and 2; A stores nothing so the morph starts from the kit.
    CHECK_EQ(int(g_made.settings.scene_a), 0);
    CHECK_EQ(int(g_made.settings.scene_b), 1);
    CHECK_EQ(scene_count(g_scenes.scenes[0]), 0);
    CHECK(scene_count(g_scenes.scenes[1]) > 0);
    CHECK(scene_count(g_scenes.scenes[2]) > 0);
    CHECK(scene_count(g_scenes.scenes[3]) > 0);

    // At morph position 0 the result is the kit, so shipping scenes moves nothing at rest; at the
    // far end every stored value arrives.
    uint8_t out[kTracks][kKitParams];
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) out[t][i] = g_made.kit.tracks[t].params[i];
    }
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], 0, out);
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) CHECK_EQ(int(out[t][i]), int(g_made.kit.tracks[t].params[i]));
    }
    scene_morph(g_scenes.scenes[0], g_scenes.scenes[1], kMorphScale, out);
    int moved = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) {
            if (!scene_has(g_scenes.scenes[1], t, i)) continue;
            CHECK_EQ(int(out[t][i]), int(g_scenes.scenes[1].values[t][i]));
            moved += out[t][i] != g_made.kit.tracks[t].params[i];
        }
    }
    CHECK_EQ(moved, scene_count(g_scenes.scenes[1]));      // every stored value is a change
}

TEST(factory_mute_states_are_distinct_and_start_unmuted) {
    make();
    CHECK_EQ(int(g_made.settings.live_mutes), 0);
    CHECK_EQ(int(g_made.settings.mute_states[0]), 0);
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) CHECK(g_made.settings.mute_states[i] != g_made.settings.mute_states[j]);
        CHECK(g_made.settings.mute_states[i] != 0xFF);            // never everything off
    }
}

TEST(factory_survives_every_file_it_is_saved_in) {
    make();
    uint32_t n = 0;

    project_init(g_back);
    CHECK(project_write(g_made, g_buffer, sizeof g_buffer, &n) == SerializeResult::ok);
    CHECK(project_read(g_back, g_buffer, n) == SerializeResult::ok);
    CHECK_EQ(int(g_back.settings.tempo_centi_bpm), int(g_made.settings.tempo_centi_bpm));
    CHECK_EQ(int(g_back.settings.root), int(g_made.settings.root));
    CHECK_EQ(int(g_back.settings.scale), int(g_made.settings.scale));
    CHECK_EQ(int(g_back.settings.chain_length), int(g_made.settings.chain_length));
    CHECK(g_back.settings.chain_enabled == g_made.settings.chain_enabled);
    CHECK(std::memcmp(g_back.settings.sends, g_made.settings.sends, sizeof g_made.settings.sends) == 0);
    CHECK(std::memcmp(g_back.settings.master, g_made.settings.master, sizeof g_made.settings.master) == 0);
    CHECK(std::memcmp(g_back.settings.mute_states, g_made.settings.mute_states,
                      sizeof g_made.settings.mute_states) == 0);
    for (int i = 0; i < g_made.settings.chain_length; i++) {
        CHECK_EQ(int(g_back.chain[i].slot), int(g_made.chain[i].slot));
        CHECK_EQ(int(g_back.chain[i].repeats), int(g_made.chain[i].repeats));
    }

    CHECK(kit_write(g_made.kit, g_buffer, sizeof g_buffer, &n) == SerializeResult::ok);
    CHECK(kit_read(g_back.kit, g_buffer, n) == SerializeResult::ok);
    for (int t = 0; t < kTracks; t++) {
        CHECK(std::memcmp(g_back.kit.tracks[t].params, g_made.kit.tracks[t].params, kKitParams) == 0);
        CHECK_EQ(int(g_back.kit.tracks[t].machine), int(g_made.kit.tracks[t].machine));
    }

    int refused = -1;
    CHECK(bank_write(g_made, 0, g_buffer, sizeof g_buffer, &n) == SerializeResult::ok);
    CHECK(bank_read(g_back, 0, g_buffer, n, &refused) == SerializeResult::ok);
    CHECK_EQ(refused, 0);
    for (int slot = 0; slot < kFactoryPatterns; slot++) {
        const Pattern &a = g_made.patterns[0][slot];
        const Pattern &b = g_back.patterns[0][slot];
        CHECK_EQ(int(b.swing), int(a.swing));
        for (int t = 0; t < kTracks; t++) {
            CHECK_EQ(int(b.tracks[t].length), int(a.tracks[t].length));
            for (int s = 0; s < kStepsPerTrack; s++) {
                CHECK(steps_equal(a.tracks[t].steps[s], b.tracks[t].steps[s]));
                // Locks by value: the writer compacts, so indices may differ but values may not.
                for (int param = 0; param < kKitParams; param++) {
                    uint8_t va = 0, vb = 0;
                    const bool ha = pattern_get_lock(a, t, s, uint8_t(param), va);
                    const bool hb = pattern_get_lock(b, t, s, uint8_t(param), vb);
                    CHECK(ha == hb);
                    if (ha && hb) CHECK_EQ(int(va), int(vb));
                }
            }
        }
    }

    CHECK(perform_write(g_scenes, g_macros, g_buffer, sizeof g_buffer, &n) == SerializeResult::ok);
    CHECK(perform_read(g_scenes_back, g_macros_back, g_buffer, n) == SerializeResult::ok);
    for (int s = 0; s < kScenes; s++) {
        CHECK(std::memcmp(&g_scenes_back.scenes[s], &g_scenes.scenes[s], sizeof(Scene)) == 0);
    }
    for (int m = 0; m < kMacros; m++) {
        for (int i = 0; i < kMacroTargets; i++) {
            const MacroTarget &a = g_macros.macros[m].targets[i];
            const MacroTarget &b = g_macros_back.macros[m].targets[i];
            CHECK_EQ(int(b.enabled), int(a.enabled));
            if (a.enabled) {
                CHECK_EQ(int(b.track), int(a.track));
                CHECK_EQ(int(b.param), int(a.param));
                CHECK_EQ(int(b.low), int(a.low));
                CHECK_EQ(int(b.high), int(a.high));
            }
        }
    }
}

TEST(factory_is_what_a_new_project_holds_and_it_plays) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());

    make();
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kKitParams; i++) CHECK_EQ(int(kit_value(t, i)), int(g_made.kit.tracks[t].params[i]));
    }
    CHECK(std::memcmp(e.pattern().tracks, g_made.patterns[0][0].tracks, sizeof g_made.patterns[0][0].tracks) == 0);
    CHECK_EQ(scene_count(e.scenes().scenes[1]), scene_count(g_scenes.scenes[1]));

    // Two bars of ticks: something sounds, nothing is non-finite, and the limiter holds it in.
    FakeHost &host = FakeHost::the();
    host.set_running(true);
    float loudest = 0.0f;
    bool finite = true;
    for (uint32_t tick = 0; tick < 2 * kTicksPerBar; tick++) {
        host.deliver_tick(tick, 0);
        loudest = std::fmax(loudest, pump(1));
        for (int i = 0; i < kRigStereo; i++) finite = finite && std::isfinite(last_block()[i]);
    }
    host.set_running(false);
    CHECK(finite);
    CHECK(loudest > 0.1f);
    CHECK(loudest <= 1.0f);
}

TEST(factory_new_project_is_empty_with_the_kit_and_centred_pans) {
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new(true);
    CHECK(settle());
    make();
    for (int t = 0; t < kTracks; t++) {
        CHECK_EQ(int(e.project_for_load().kit.tracks[t].machine), int(g_made.kit.tracks[t].machine));
        CHECK_EQ(int(kit_value(t, kFactoryFx + 1)), 64);        // PAN, centred on every track
        // The sound is the factory's, so a pad plays it.
        for (int i = 0; i < kFactoryFx; i++) CHECK_EQ(int(kit_value(t, i)), int(g_made.kit.tracks[t].params[i]));
        for (int s = 0; s < kStepsPerTrack; s++) CHECK((e.pattern().tracks[t].steps[s].flags & kStepTrig) == 0);
    }
    for (int i = 0; i < kScenes; i++) CHECK_EQ(scene_count(e.scenes().scenes[i]), 0);
    CHECK_EQ(int(e.project_for_load().settings.chain_length), 0);
    // And LOAD DEMO (request_new's default) is still the factory beat.
    storage().request_new();
    CHECK(settle());
    CHECK(std::memcmp(e.pattern().tracks, g_made.patterns[0][0].tracks, sizeof g_made.patterns[0][0].tracks) == 0);
}

TEST(factory_macros_at_rest_leave_every_knob_heard) {
    // The reason the factory once shipped no macros: a resting macro held its parameters. Now
    // a macro at 0 writes nothing (core/perform.hpp), so on a NEW project every parameter a
    // factory macro drives still answers its EDIT knob -- and moving the macro takes over.
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    make();
    const MacroTarget &cut = g_macros.macros[1].targets[0];           // ACID: the bass cutoff
    CHECK_EQ(int(e.macro_position(1)), 0);
    post(Cmd::set_param, cut.track, 0, cut.param, 20);
    pump(2);
    CHECK_EQ(int(e.performed(cut.track, cut.param)), 20);
    post(Cmd::set_macro, 0, 0, 1, 1);                                 // one detent: no jump
    pump(2);
    CHECK(std::abs(int(e.performed(cut.track, cut.param)) - 20) <= 1);
    post(Cmd::set_macro, 0, 0, 1, 127);
    pump(2);
    CHECK_EQ(int(e.performed(cut.track, cut.param)), int(cut.high));
    post(Cmd::set_macro, 0, 0, 1, 0);
    pump(2);
    CHECK_EQ(int(e.performed(cut.track, cut.param)), 20);
}

TEST(factory_macros_move_on_from_the_morph_not_back_to_the_kit) {
    // Morphed fully to DUB, the chord's delay send is the scene's 112. The DUB macro shares that
    // parameter; its first detent must nudge from 112, not drop to the kit's 72.
    using namespace strata::test;
    Engine &e = fresh();
    storage().request_new();
    CHECK(settle());
    make();
    const MacroTarget &send = g_macros.macros[2].targets[0];          // DUB: the chord's SNA
    CHECK(scene_has(g_scenes.scenes[1], send.track, send.param));
    const int scene = g_scenes.scenes[1].values[send.track][send.param];
    CHECK(scene != int(send.low));
    Command morph;
    morph.cmd = Cmd::set_morph;
    morph.u = kMorphScale;
    e.commands().post(morph);
    pump(2);
    CHECK_EQ(int(e.performed(send.track, send.param)), scene);
    post(Cmd::set_macro, 0, 0, 2, 1);
    pump(2);
    const int nudged = int(e.performed(send.track, send.param));
    CHECK(nudged >= scene && nudged - scene <= 1);
    post(Cmd::set_macro, 0, 0, 2, 0);
    post(Cmd::set_morph);
    pump(2);
}


TEST(factory_new_keeps_the_loop_track_playing_the_placeholder) {
    // The factory LOOP track is SAMPLE with no file: it plays the placeholder every Sampler is
    // given at init, which is what a first boot without a card plays. NEW must give the same.
    using namespace strata::test;
    fresh();
    storage().request_new();
    CHECK(settle());
    constexpr int kLoopTrack = 7;
    CHECK_EQ(int(engine().machine_id(kLoopTrack)), int(MachineId::sample));
    const Sampler *loop = static_cast<const Sampler *>(engine().machine(kLoopTrack));
    CHECK(loop->has_sample());
}

// --- A05 PAD ----------------------------------------------------------------

TEST(factory_the_pad_is_in_a05s_locks_and_the_kit_chord_is_still_a_stab) {
    make();
    // The kit's chord keeps SUS 0 and the rest of its ASR page at 0: A01-A04's stab.
    for (int i = 0; i < 4; i++) CHECK_EQ(int(g_made.kit.tracks[kChordTrack].params[kFactoryAsr + i]), 0);
    // And none of A01-A04 locks the ASR page anywhere on the chord track.
    for (int slot = 0; slot < kFactoryPatterns; slot++) {
        if (slot == kFactoryPadSlot) continue;
        const Pattern &p = g_made.patterns[0][slot];
        for (int s = 0; s < kStepsPerTrack; s++) {
            uint8_t v = 0;
            CHECK(!pattern_get_lock(p, kChordTrack, s, uint8_t(kSusParam), v));
        }
    }

    // A05: four chords a bar apart, each held for its bar, each carrying the pad's ASR.
    const Pattern &pad = g_made.patterns[0][kFactoryPadSlot];
    CHECK_EQ(int(pad.master_length), 64);
    CHECK_EQ(int(pad.tracks[kChordTrack].length), 64);
    CHECK_EQ(trigs_on(pad, kChordTrack), 4);
    for (int s = 0; s < kStepsPerTrack; s++) {
        const Step &step = pad.tracks[kChordTrack].steps[s];
        const bool trig = (step.flags & kStepTrig) != 0;
        CHECK(trig == ((s & 15) == 0));
        if (!trig) continue;
        CHECK(chord_type(step.chord) != ChordType::none);
        CHECK_EQ(int(length_sixteenths(step.length)), 16 * 16);     // a bar
        uint8_t atk = 0, sus = 0, rel = 0;
        CHECK(pattern_get_lock(pad, kChordTrack, s, uint8_t(kAtkParam), atk));
        CHECK(pattern_get_lock(pad, kChordTrack, s, uint8_t(kSusParam), sus));
        CHECK(pattern_get_lock(pad, kChordTrack, s, uint8_t(kRelParam), rel));
        CHECK(atk > 0 && sus > 0 && rel > 0);
    }
}

TEST(factory_the_pads_chord_changes_never_need_more_than_six_voices) {
    // VA POLY retriggers a voice already sounding a note (dsp/vapoly.hpp, rule 1), so a change
    // needs the union of the two chords. Six or fewer and nothing is stolen -- the loop back to
    // the first chord included, since REL rings the last one on into it.
    make();
    const Pattern &pad = g_made.patterns[0][kFactoryPadSlot];
    uint8_t notes[4][kChordMaxNotes];
    int counts[4] = {};
    for (int b = 0; b < 4; b++) {
        const Step &step = pad.tracks[kChordTrack].steps[b * 16];
        counts[b] = chord_expand(step.note, step.chord, pad.root, pad.scale, notes[b]);
        CHECK(counts[b] >= 4);
    }
    for (int b = 0; b < 4; b++) {
        const int next = (b + 1) & 3;
        int both = counts[next];
        for (int i = 0; i < counts[b]; i++) {
            bool shared = false;
            for (int j = 0; j < counts[next]; j++) shared = shared || notes[b][i] == notes[next][j];
            both += shared ? 0 : 1;
        }
        CHECK(both <= VaPoly::kVoices);
        CHECK(both < counts[b] + counts[next]);             // something carries on
    }
}

namespace {

// Plays the transport at 130 BPM for `blocks` blocks, delivering each tick on its own frame as
// the clock would. Returns the tick reached.
uint32_t play_130(uint32_t tick, double &frame, int blocks) {
    using namespace strata::test;
    const double frames_per_tick = 48000.0 * 60.0 / (130.0 * 96.0);
    for (int b = 0; b < blocks; b++) {
        while (double(tick) * frames_per_tick < frame + kRigBlock) {
            FakeHost::the().deliver_tick(tick, uint32_t(double(tick) * frames_per_tick - frame));
            tick++;
        }
        pump(1);
        frame += kRigBlock;
    }
    return tick;
}

}  // namespace

TEST(factory_a05_holds_each_chord_through_its_bar_and_a01_still_stabs) {
    using namespace strata::test;
    constexpr int kBlocksPerStep = int(48000.0 * 60.0 / 130.0 / 4.0) / kRigBlock;   // ~1.8

    for (int slot : {0, kFactoryPadSlot}) {
        Engine &e = fresh();
        storage().request_new();
        CHECK(settle());
        e.project_for_load().slot = uint8_t(slot);
        post(Cmd::project_loaded);
        CHECK(settle());
        const VaPoly *poly = static_cast<const VaPoly *>(e.machine(kChordTrack));
        CHECK_EQ(int(e.machine_id(kChordTrack)), int(MachineId::va_poly));

        FakeHost::the().set_running(true);
        uint32_t tick = 0;
        double frame = 0.0;
        // Into the last quarter of bar 1: A01's stabs (steps 3 and 10, DEC 210 ms) have died away
        // by step 14, A05's Cm9 is still holding at SUS.
        tick = play_130(tick, frame, 14 * kBlocksPerStep);
        if (slot == kFactoryPadSlot) {
            CHECK_EQ(poly->sounding_voices(), 5);
            CHECK(poly->decay_seconds() > 0.6f);        // DEC's lock (0.69 s) has lasted the bar
        } else {
            CHECK_EQ(poly->sounding_voices(), 0);
            CHECK(poly->decay_seconds() < 0.3f);        // the kit's stab, 0.21 s
        }
        // Through all four bars and back into the first: never more than six voices, so the
        // chord track never steals from itself.
        int most = 0;
        for (int i = 0; i < 4 * 16 * 2; i++) {
            tick = play_130(tick, frame, 1);
            most = poly->busy_voices() > most ? poly->busy_voices() : most;
        }
        CHECK(most <= VaPoly::kVoices);
        FakeHost::the().set_running(false);
    }
}
