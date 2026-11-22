// Scenes, the morph and the macros. core/perform.hpp has the design.
#include "perform.hpp"

namespace strata {
namespace {

// A kit parameter's domain (dsp/track.hpp: "Values are 0-127"), stated here as core/project.cpp
// states it, and for the same reason: a parameter reaches dsp/ as a table index or an x(1/127)
// scale, so a 255 out of a damaged file is an out-of-range read down in a machine.
constexpr uint8_t kParamMax = 127;

uint8_t clamp_u8(uint8_t value, uint8_t low, uint8_t high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

int clamp_int(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// --- the container --------------------------------------------------------
//
// The THIRD copy of these fifty lines: core/serialize.cpp has them for the pattern,
// core/project.cpp for the project, kit and bank files, and this is the fourth file type.
// core/project.cpp's header records why they are duplicated rather than shared -- so that one
// format can be read and changed without reading another -- and the rules they enforce are
// restated and checked in the same ORDER, which is what makes all five file types give the same
// reason for the same damage.
//
// Three copies is where that decision starts to cost more than it pays. It is kept here because
// it is a decision the project recorded and not one to change in passing (CLAUDE.md), but a
// FIFTH file type should extract these into core/serialize.cpp first.

struct Section {
    uint16_t id;
    uint32_t offset;
    uint32_t size;
};

void write_container(Writer &w, uint16_t version, const Section *sections, uint32_t count,
                     uint32_t payload) {
    w.u8(kMagic0);
    w.u8(kMagic1);
    w.u8(kMagic2);
    w.u8(kMagic3);
    w.u16(version);
    w.u16(uint16_t(count));
    w.u32(payload);
    w.u32(0);
    for (uint32_t i = 0; i < count; i++) {
        w.u16(sections[i].id);
        w.u16(0);
        w.u32(sections[i].offset);
        w.u32(sections[i].size);
    }
}

SerializeResult finish_container(Writer &w, uint8_t *buffer, uint32_t total, uint32_t *written) {
    if (!w.ok()) return SerializeResult::buffer_too_small;
    w.patch_u32(kCrcOffset, crc32(buffer + kHeaderBytes, total - kHeaderBytes));
    if (!w.ok()) return SerializeResult::buffer_too_small;
    if (written != nullptr) *written = total;
    return SerializeResult::ok;
}

// The checks in pattern_read's order: the declared payload against the buffer, then the CRC,
// then the version, then the table.
// The last `optional` of `wanted` may be missing, and come back as offset 0 (core/project.cpp's
// copy says why a format grows this way).
SerializeResult find_sections(const uint8_t *buffer, uint32_t size, uint16_t version,
                              const uint16_t *wanted, uint32_t count, uint32_t *offset,
                              uint32_t *length, uint32_t optional = 0) {
    for (uint32_t k = 0; k < count; k++) {
        offset[k] = 0;
        length[k] = 0;
    }
    if (buffer == nullptr || size < kHeaderBytes) return SerializeResult::truncated;

    Reader r(buffer, size);
    const bool magic =
        r.u8() == kMagic0 && r.u8() == kMagic1 && r.u8() == kMagic2 && r.u8() == kMagic3;
    if (!magic) return SerializeResult::bad_magic;

    const uint16_t have = r.u16();
    const uint32_t sections = r.u16();
    const uint32_t payload = r.u32();
    const uint32_t want_crc = r.u32();
    if (!r.ok()) return SerializeResult::truncated;

    if (payload > size - kHeaderBytes) return SerializeResult::truncated;
    if (crc32(buffer + kHeaderBytes, payload) != want_crc) return SerializeResult::bad_crc;
    if (have != version) return SerializeResult::bad_version;

    if (sections == 0 || sections > kMaxSections) return SerializeResult::malformed;
    const uint32_t table = sections * kSectionEntryBytes;
    if (table > payload) return SerializeResult::malformed;

    for (uint32_t i = 0; i < sections; i++) {
        if (!r.seek(kHeaderBytes + i * kSectionEntryBytes)) return SerializeResult::malformed;
        const uint16_t id = r.u16();
        r.skip(2);
        const uint32_t at = r.u32();
        const uint32_t len = r.u32();
        if (!r.ok()) return SerializeResult::truncated;
        if (at < table || at > payload || len > payload - at) return SerializeResult::malformed;
        for (uint32_t k = 0; k < count; k++) {
            if (id != wanted[k]) continue;
            if (offset[k] != 0) return SerializeResult::malformed;
            offset[k] = at;
            length[k] = len;
        }
    }
    const uint32_t required = optional >= count ? 0 : count - optional;
    for (uint32_t k = 0; k < required; k++) {
        if (offset[k] == 0) return SerializeResult::malformed;
    }
    return SerializeResult::ok;
}

// --- PERFORM.BIN sizes ----------------------------------------------------

constexpr uint32_t kPerformSections = 5;
// The last three, the scenes' LFO bytes, their master EQ and their arp bytes, may each be absent
// (an older file).
constexpr uint32_t kPerformOptionalSections = 3;
constexpr uint32_t kSceneTrackBytes = uint32_t(kKitSoundParams) + 2; // values, then the bitmask
constexpr uint32_t kSceneBytes = kSceneTrackBytes * uint32_t(kTracks);
constexpr uint32_t kScenesBytes = kSceneBytes * uint32_t(kScenes);
constexpr uint32_t kMacroTargetBytes = 5;                            // enabled, track, param, low, high
constexpr uint32_t kMacrosBytes =
    kMacroTargetBytes * uint32_t(kMacroTargets) * uint32_t(kMacros);
constexpr uint32_t kPerformTableBytes = kPerformSections * kSectionEntryBytes;
constexpr uint32_t kScenesOffset = kPerformTableBytes;
constexpr uint32_t kMacrosOffset = kScenesOffset + kScenesBytes;
// The LFO's four values and its mask byte, per track per scene.
constexpr uint32_t kSceneLfoTrackBytes = uint32_t(kKitLfoParams) + 1;
constexpr uint32_t kSceneLfoBytes =
    kSceneLfoTrackBytes * uint32_t(kTracks) * uint32_t(kScenes);
constexpr uint32_t kSceneLfoOffset = kMacrosOffset + kMacrosBytes;
constexpr uint32_t kSceneMasterBytes = uint32_t(kMasterBands) + 1;
constexpr uint32_t kScenesMasterBytes = kSceneMasterBytes * uint32_t(kScenes);
constexpr uint32_t kScenesMasterOffset = kSceneLfoOffset + kSceneLfoBytes;
// The arp's four values and its four stored bits (the high half of the third mask byte), per
// track per scene.
constexpr uint32_t kSceneArpTrackBytes = uint32_t(kKitArpParams) + 1;
constexpr uint32_t kSceneArpBytes =
    kSceneArpTrackBytes * uint32_t(kTracks) * uint32_t(kScenes);
constexpr uint32_t kSceneArpOffset = kScenesMasterOffset + kScenesMasterBytes;
constexpr uint32_t kPerformTotalBytes = kHeaderBytes + kSceneArpOffset + kSceneArpBytes;
constexpr uint8_t kLfoStoredMask = uint8_t((1u << kKitLfoParams) - 1u);

static_assert(kKitSoundParams == 16, "section 1's stored bitmask is two bytes on the strength of "
                                     "16 sound parameters per track");
static_assert(kKitLfoParams + kKitArpParams == 8 && kSceneStoredBytes == 3,
              "the LFO's stored bits are the low half of the third mask byte, the arp's the high");

}  // namespace

// --- scenes ---------------------------------------------------------------

void scene_set(Scene &scene, int track, int param, uint8_t value) {
    if (track < 0 || track >= kTracks || param < 0 || param >= kKitParams) return;
    scene.values[track][param] = clamp_u8(value, 0, kParamMax);
    scene.stored[track][param >> 3] =
        uint8_t(scene.stored[track][param >> 3] | uint8_t(1u << (param & 7)));
}

void scene_clear(Scene &scene, int track, int param) {
    if (track < 0 || track >= kTracks || param < 0 || param >= kKitParams) return;
    scene.stored[track][param >> 3] =
        uint8_t(scene.stored[track][param >> 3] & ~uint8_t(1u << (param & 7)));
    // The value is left alone. It costs nothing, and it means clearing and storing again gets
    // the value back rather than a zero -- which on a filter cutoff is the difference between
    // "not in this scene" and "closed".
}

void scene_capture_track(Scene &scene, const Kit &kit, int track) {
    if (track < 0 || track >= kTracks) return;
    for (int p = 0; p < kKitParams; p++) {
        scene_set(scene, track, p, kit.tracks[track].params[p]);
    }
}

void scene_capture(Scene &scene, const Kit &kit) {
    for (int t = 0; t < kTracks; t++) scene_capture_track(scene, kit, t);
}

void scene_capture_master(Scene &scene, const uint8_t bands[kMasterBands]) {
    for (int b = 0; b < kMasterBands; b++) scene.master[b] = clamp_u8(bands[b], 0, kParamMax);
    scene.master_stored = uint8_t((1u << kMasterBands) - 1u);
}

void scene_erase(Scene &scene) {
    for (int t = 0; t < kTracks; t++) {
        for (int i = 0; i < kSceneStoredBytes; i++) scene.stored[t][i] = 0;
        for (int p = 0; p < kKitParams; p++) scene.values[t][p] = 0;
    }
    for (int b = 0; b < kMasterBands; b++) scene.master[b] = 0;
    scene.master_stored = 0;
}

int scene_count(const Scene &scene) {
    int n = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int p = 0; p < kKitParams; p++) {
            if (scene_has(scene, t, p)) n++;
        }
    }
    for (int b = 0; b < kMasterBands; b++) n += int(scene.master_stored >> b & 1u);
    return n;
}

// --- the morph ------------------------------------------------------------

void scene_morph(const Scene &a, const Scene &b, uint16_t position,
                 uint8_t out[kTracks][kKitParams]) {
    const int at = clamp_int(int(position), 0, int(kMorphScale));

    for (int t = 0; t < kTracks; t++) {
        // Both bitmasks for this track, read once. A track neither scene touches is the common
        // case in a performance -- a scene for the bass line says nothing about the drums -- and
        // skipping it whole is what keeps this cheap enough for control rate.
        const uint32_t mask_a = uint32_t(a.stored[t][0]) | uint32_t(a.stored[t][1]) << 8 |
                                uint32_t(a.stored[t][2]) << 16;
        const uint32_t mask_b = uint32_t(b.stored[t][0]) | uint32_t(b.stored[t][1]) << 8 |
                                uint32_t(b.stored[t][2]) << 16;
        const uint32_t touched = mask_a | mask_b;
        if (touched == 0) continue;

        for (int p = 0; p < kKitParams; p++) {
            if ((touched >> p & 1u) == 0) continue;     // neither scene stores it: leave it

            // The end a scene does not store is the KIT value, not the other scene's held
            // constant. core/perform.hpp says why: it makes the sweep fade A's changes away
            // where B says nothing about them, which is what a crossfade between two sounds
            // means. The kit value is what `out` holds on the way in -- the live kit the caller
            // copied there -- and not a stored Kit, which in the engine is only refreshed by a
            // save and went stale with every knob turned since.
            const int fallback = int(out[t][p]);
            const int from = (mask_a >> p & 1u) != 0 ? int(a.values[t][p]) : fallback;
            const int to = (mask_b >> p & 1u) != 0 ? int(b.values[t][p]) : fallback;

            // A shift, not a divide: kMorphScale is a power of two and this runs 128 times per
            // move. In C++20 a right shift of a negative value is an arithmetic shift, so
            // (num + half) >> shift rounds half upwards for negative deltas as well as positive
            // ones -- which is why the rounding is written as an add and not as a conditional.
            const int delta = to - from;
            const int num = delta * at;
            const int value = from + ((num + int(kMorphScale) / 2) >> kMorphShift);
            out[t][p] = uint8_t(clamp_int(value, 0, kParamMax));
        }
    }
}

void scene_morph_master(const Scene &a, const Scene &b, uint16_t position,
                        uint8_t out[kMasterBands]) {
    const int at = clamp_int(int(position), 0, int(kMorphScale));
    const unsigned touched = unsigned(a.master_stored | b.master_stored);
    if (touched == 0) return;
    for (int band = 0; band < kMasterBands; band++) {
        if ((touched >> band & 1u) == 0) continue;
        // scene_morph's rule and arithmetic, for three bytes instead of 128.
        const int fallback = int(out[band]);
        const int from = (a.master_stored >> band & 1u) != 0 ? int(a.master[band]) : fallback;
        const int to = (b.master_stored >> band & 1u) != 0 ? int(b.master[band]) : fallback;
        const int num = (to - from) * at;
        out[band] = uint8_t(clamp_int(from + ((num + int(kMorphScale) / 2) >> kMorphShift), 0,
                                      kParamMax));
    }
}

// --- macros ---------------------------------------------------------------

uint8_t macro_value(const MacroTarget &target, uint8_t position) {
    const int low = int(clamp_u8(target.low, 0, kParamMax));
    const int high = int(clamp_u8(target.high, 0, kParamMax));
    const int at = clamp_int(int(position), 0, 127);

    // A divide by 127, which is not a power of two. Affordable here and not in scene_morph: a
    // macro has at most kMacroTargets targets and there are kMacros of them, so this runs at
    // most 16 times when a knob moves, against the morph's 128.
    //
    // Rounded to nearest, which is NOT about reaching the ends -- those are exact either way,
    // since delta * 127 / 127 is delta. It is about the middle: truncating biases every
    // intermediate value towards `low`, so a knob swept up lags by a value for most of its
    // travel and two macros driving the same parameter from different ranges disagree.
    const int delta = high - low;
    const int num = delta * at;
    const int rounded = num >= 0 ? (num + 63) / 127 : -((-num + 63) / 127);
    return uint8_t(clamp_int(low + rounded, 0, kParamMax));
}

void macro_range_through(uint8_t now, uint8_t far, uint8_t position, uint8_t &low,
                         uint8_t &high) {
    const int n = int(clamp_u8(now, 0, kParamMax));
    const int f = int(clamp_u8(far, 0, kParamMax));
    const int p = clamp_int(int(position), 0, 127);
    if (p == 0) {                       // the old behaviour, which was already right here
        low = uint8_t(n);
        high = uint8_t(f);
        return;
    }
    if (p == 127) {                     // the knob is at the top: sweep down towards `far`
        low = uint8_t(f);
        high = uint8_t(n);
        return;
    }
    // value(p) = low + (high - low) * p / 127 = n. With high = f: low = (127 n - f p) / (127 - p).
    // Control rate (an assignment is a button press), so the divisions are fine here.
    const int den = 127 - p;
    int l = (127 * n - f * p);
    l = l >= 0 ? (l + den / 2) / den : -((-l + den / 2) / den);
    if (l >= 0 && l <= kParamMax) {
        low = uint8_t(l);
        high = uint8_t(f);
        return;
    }
    // Out of range: pin low at its end and solve high = (127 n - low (127 - p)) / p instead.
    const int pinned = l < 0 ? 0 : kParamMax;
    int h = 127 * n - pinned * den;
    h = h >= 0 ? (h + p / 2) / p : -((-h + p / 2) / p);
    low = uint8_t(pinned);
    high = uint8_t(clamp_int(h, 0, kParamMax));
    // The one case no line can serve: the parameter at one end (0, say) with the knob above the
    // bottom and `far` the other end. A range through (position, 0) heading up would have to be
    // negative below the knob, so it comes out flat -- a macro that does nothing. The plain range
    // is better there: one jump on assignment, and then the knob works.
    if (low == high && n != f) {
        low = uint8_t(n);
        high = uint8_t(f);
    }
}

uint8_t macro_value_from(uint8_t live, const MacroTarget &target, uint8_t position) {
    // macro_value's arithmetic with `live` in place of `low`, for the same reasons: a divide by
    // 127 that at most 16 targets pay when a knob moves, rounded to nearest so a swept knob does
    // not lag. The ends are exact.
    const int from = int(clamp_u8(live, 0, kParamMax));
    const int high = int(clamp_u8(target.high, 0, kParamMax));
    const int at = clamp_int(int(position), 0, 127);
    const int num = (high - from) * at;
    const int rounded = num >= 0 ? (num + 63) / 127 : -((-num + 63) / 127);
    return uint8_t(clamp_int(from + rounded, 0, kParamMax));
}

void macro_apply(const Macro &macro, uint8_t position, uint8_t out[kTracks][kKitParams]) {
    // At rest the knob owns the parameter (core/perform.hpp); the sweep below gives `live` at 0
    // anyway, and this keeps the resting case free of arithmetic.
    if (position == 0) return;
    for (int i = 0; i < kMacroTargets; i++) {
        const MacroTarget &target = macro.targets[i];
        if (target.enabled == 0) continue;
        // A macro set up by a build with more tracks or parameters must not write past the
        // array. Skipped rather than clamped: clamping would drive someone else's parameter.
        if (target.track >= kTracks || target.param >= kKitParams) continue;
        uint8_t &value = out[target.track][target.param];
        value = macro_value_from(value, target, position);
    }
}

void macro_apply_master(const Macro &macro, uint8_t position, uint8_t out[kMasterBands]) {
    if (position == 0) return;
    for (int i = 0; i < kMacroTargets; i++) {
        const MacroTarget &target = macro.targets[i];
        if (target.enabled == 0 || target.track != kMacroMasterTrack) continue;
        if (target.param >= kMasterBands) continue;
        out[target.param] = macro_value_from(out[target.param], target, position);
    }
}

int macro_count(const Macro &macro) {
    int n = 0;
    for (int i = 0; i < kMacroTargets; i++) {
        if (macro_target_usable(macro.targets[i])) n++;
    }
    return n;
}

// --- mutes ----------------------------------------------------------------

void mute_state_apply(uint8_t state, bool muted[kTracks]) {
    for (int t = 0; t < kTracks; t++) muted[t] = (state >> t & 1u) != 0;
}

uint8_t mute_state_capture(const bool muted[kTracks]) {
    uint8_t state = 0;
    for (int t = 0; t < kTracks; t++) {
        if (muted[t]) state = uint8_t(state | uint8_t(1u << t));
    }
    return state;
}

void mute_latch_arm(MuteLatch &latch, uint8_t state) {
    // Replaces rather than queues: a performer pressing three pads before the bar means the
    // third one. A queue would play back their hesitation.
    latch.pending = true;
    latch.state = state;
}

bool mute_latch_release(MuteLatch &latch, uint8_t &state) {
    if (!latch.pending) return false;
    state = latch.state;
    latch.pending = false;
    return true;
}

void mute_latch_clear(MuteLatch &latch) {
    latch.pending = false;
    latch.state = 0;
}

// --- auto-fill ------------------------------------------------------------

const char *auto_fill_name(AutoFill period) {
    switch (period) {
        case AutoFill::off:      return "OFF";
        case AutoFill::every_4:  return "4";
        case AutoFill::every_8:  return "8";
        case AutoFill::every_16: return "16";
        default:                 return "?";
    }
}

void perform_init(SceneSet &scenes, MacroSet &macros) {
    // Scene by scene rather than `scenes = SceneSet{}`: the set is over 2 KiB and the habit one
    // structure up would be a megabyte (core/project.cpp says the same about Project).
    for (int s = 0; s < kScenes; s++) scene_erase(scenes.scenes[s]);
    for (int m = 0; m < kMacros; m++) {
        for (int i = 0; i < kMacroTargets; i++) macros.macros[m].targets[i] = MacroTarget{};
    }
}

// --- PERFORM.BIN ----------------------------------------------------------

uint32_t perform_bytes() { return kPerformTotalBytes; }

SerializeResult perform_write(const SceneSet &scenes, const MacroSet &macros, uint8_t *buffer,
                              uint32_t capacity, uint32_t *written) {
    if (written != nullptr) *written = 0;
    if (buffer == nullptr || capacity < kPerformTotalBytes) {
        return SerializeResult::buffer_too_small;
    }

    const Section sections[kPerformSections] = {
        {kSectionPerformScenes, kScenesOffset, kScenesBytes},
        {kSectionPerformMacros, kMacrosOffset, kMacrosBytes},
        {kSectionPerformSceneLfo, kSceneLfoOffset, kSceneLfoBytes},
        {kSectionPerformSceneMaster, kScenesMasterOffset, kScenesMasterBytes},
        {kSectionPerformSceneArp, kSceneArpOffset, kSceneArpBytes},
    };

    Writer w(buffer, capacity);
    write_container(w, kPerformVersion, sections, kPerformSections,
                    kPerformTotalBytes - kHeaderBytes);

    // Values go down as they stand; clamping belongs to the reader, which is the side that
    // cannot trust its bytes (core/project.cpp's rule).
    for (int s = 0; s < kScenes; s++) {
        const Scene &scene = scenes.scenes[s];
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitSoundParams; p++) w.u8(scene.values[t][p]);
            w.u8(scene.stored[t][0]);
            w.u8(scene.stored[t][1]);
        }
    }

    for (int m = 0; m < kMacros; m++) {
        for (int i = 0; i < kMacroTargets; i++) {
            const MacroTarget &target = macros.macros[m].targets[i];
            w.u8(target.enabled);
            w.u8(target.track);
            w.u8(target.param);
            w.u8(target.low);
            w.u8(target.high);
        }
    }

    for (int s = 0; s < kScenes; s++) {
        const Scene &scene = scenes.scenes[s];
        for (int t = 0; t < kTracks; t++) {
            for (int p = kKitSoundParams; p < kKitLfoEnd; p++) w.u8(scene.values[t][p]);
            w.u8(uint8_t(scene.stored[t][2] & kLfoStoredMask));
        }
    }

    for (int s = 0; s < kScenes; s++) {
        const Scene &scene = scenes.scenes[s];
        for (int b = 0; b < kMasterBands; b++) w.u8(scene.master[b]);
        w.u8(scene.master_stored);
    }

    for (int s = 0; s < kScenes; s++) {
        const Scene &scene = scenes.scenes[s];
        for (int t = 0; t < kTracks; t++) {
            for (int p = kKitLfoEnd; p < kKitParams; p++) w.u8(scene.values[t][p]);
            w.u8(uint8_t(scene.stored[t][2] >> kKitLfoParams));
        }
    }

    if (!w.ok() || w.size() != kPerformTotalBytes) return SerializeResult::buffer_too_small;
    return finish_container(w, buffer, kPerformTotalBytes, written);
}

SerializeResult perform_read(SceneSet &scenes, MacroSet &macros, const uint8_t *buffer,
                             uint32_t size) {
    // Every refusal goes through here, so a refused load leaves an empty set rather than half a
    // file. A missing PERFORM.BIN is not this function's business: `app/` treats a file that is
    // not there as "no scenes" and never calls this.
    const auto refuse = [&scenes, &macros](SerializeResult why) {
        perform_init(scenes, macros);
        return why;
    };

    const uint16_t wanted[kPerformSections] = {kSectionPerformScenes, kSectionPerformMacros,
                                               kSectionPerformSceneLfo, kSectionPerformSceneMaster,
                                               kSectionPerformSceneArp};
    uint32_t offset[kPerformSections] = {};
    uint32_t length[kPerformSections] = {};
    const SerializeResult found = find_sections(buffer, size, kPerformVersion, wanted,
                                                kPerformSections, offset, length,
                                                kPerformOptionalSections);
    if (found != SerializeResult::ok) return refuse(found);
    if (length[0] < kScenesBytes) return refuse(SerializeResult::malformed);
    if (length[1] < kMacrosBytes) return refuse(SerializeResult::malformed);
    // Present but short is damage; absent is a file from before the LFO or the kill EQ.
    if (offset[2] != 0 && length[2] < kSceneLfoBytes) return refuse(SerializeResult::malformed);
    if (offset[3] != 0 && length[3] < kScenesMasterBytes) return refuse(SerializeResult::malformed);
    if (offset[4] != 0 && length[4] < kSceneArpBytes) return refuse(SerializeResult::malformed);

    Reader r(buffer, size);
    if (!r.seek(kHeaderBytes + offset[0])) return refuse(SerializeResult::malformed);

    for (int s = 0; s < kScenes; s++) {
        Scene &scene = scenes.scenes[s];
        for (int t = 0; t < kTracks; t++) {
            for (int p = 0; p < kKitSoundParams; p++) {
                // Clamped, for core/project.cpp's reason about a kit parameter: dsp/ maps these
                // as table indices, so a 255 that survived the CRC is an out-of-range read in a
                // machine rather than an odd sound.
                scene.values[t][p] = clamp_u8(r.u8(), 0, kParamMax);
            }
            // Every bit of both bytes is a parameter this build has, so these are taken as they
            // come -- there is no spare bit that could mean anything else.
            scene.stored[t][0] = r.u8();
            scene.stored[t][1] = r.u8();
        }
    }
    if (!r.ok()) return refuse(SerializeResult::truncated);

    if (!r.seek(kHeaderBytes + offset[1])) return refuse(SerializeResult::malformed);
    for (int m = 0; m < kMacros; m++) {
        for (int i = 0; i < kMacroTargets; i++) {
            const uint8_t enabled = r.u8();
            const uint8_t track = r.u8();
            const uint8_t param = r.u8();
            const uint8_t low = r.u8();
            const uint8_t high = r.u8();

            // A target naming a track or parameter that cannot exist is dropped WHOLE rather
            // than clamped, the same way core/project.cpp drops a chain entry naming no
            // pattern: clamping would quietly drive a parameter nobody assigned.
            MacroTarget probe;
            probe.enabled = enabled;
            probe.track = track;
            probe.param = param;
            const bool usable = macro_target_usable(probe);
            MacroTarget &target = macros.macros[m].targets[i];
            target.enabled = usable ? 1 : 0;
            target.track = usable ? track : 0;
            target.param = usable ? param : 0;
            target.low = usable ? clamp_u8(low, 0, kParamMax) : 0;
            target.high = usable ? clamp_u8(high, 0, kParamMax) : kParamMax;
        }
    }
    if (!r.ok()) return refuse(SerializeResult::truncated);

    // The scenes' LFO bytes. Absent: a file from before the LFO, so no scene stores one --
    // set explicitly, since `scenes` is the engine's set and may hold the last project's.
    if (offset[2] == 0) {
        for (int s = 0; s < kScenes; s++) {
            for (int t = 0; t < kTracks; t++) {
                Scene &scene = scenes.scenes[s];
                for (int p = kKitSoundParams; p < kKitLfoEnd; p++) scene.values[t][p] = 0;
                scene.stored[t][2] = 0;
            }
        }
    } else {
        if (!r.seek(kHeaderBytes + offset[2])) return refuse(SerializeResult::malformed);
        for (int s = 0; s < kScenes; s++) {
            Scene &scene = scenes.scenes[s];
            for (int t = 0; t < kTracks; t++) {
                for (int p = kKitSoundParams; p < kKitLfoEnd; p++) {
                    scene.values[t][p] = clamp_u8(r.u8(), 0, kParamMax);
                }
                // Only the low four bits name an LFO parameter; the rest are dropped, so a
                // damaged byte cannot mark a parameter as stored that this section did not carry.
                scene.stored[t][2] = uint8_t(r.u8() & kLfoStoredMask);
            }
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The scenes' master EQ bytes. Absent means no scene stores the EQ -- set explicitly, for the
    // same reason.
    if (offset[3] == 0) {
        for (int s = 0; s < kScenes; s++) {
            Scene &scene = scenes.scenes[s];
            for (int b = 0; b < kMasterBands; b++) scene.master[b] = 0;
            scene.master_stored = 0;
        }
    } else {
        if (!r.seek(kHeaderBytes + offset[3])) return refuse(SerializeResult::malformed);
        for (int s = 0; s < kScenes; s++) {
            Scene &scene = scenes.scenes[s];
            for (int b = 0; b < kMasterBands; b++) scene.master[b] = clamp_u8(r.u8(), 0, kParamMax);
            scene.master_stored = uint8_t(r.u8() & ((1u << kMasterBands) - 1u));
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    // The scenes' arp bytes, into the high half of the third mask byte (the LFO's section has
    // already set the low half). Absent: no scene stores an arp byte.
    if (offset[4] == 0) {
        for (int s = 0; s < kScenes; s++) {
            Scene &scene = scenes.scenes[s];
            for (int t = 0; t < kTracks; t++) {
                for (int p = kKitLfoEnd; p < kKitParams; p++) scene.values[t][p] = 0;
                scene.stored[t][2] = uint8_t(scene.stored[t][2] & kLfoStoredMask);
            }
        }
    } else {
        if (!r.seek(kHeaderBytes + offset[4])) return refuse(SerializeResult::malformed);
        for (int s = 0; s < kScenes; s++) {
            Scene &scene = scenes.scenes[s];
            for (int t = 0; t < kTracks; t++) {
                for (int p = kKitLfoEnd; p < kKitParams; p++) {
                    scene.values[t][p] = clamp_u8(r.u8(), 0, kParamMax);
                }
                const uint8_t bits = uint8_t(r.u8() & ((1u << kKitArpParams) - 1u));
                scene.stored[t][2] =
                    uint8_t((scene.stored[t][2] & kLfoStoredMask) | (bits << kKitLfoParams));
            }
        }
        if (!r.ok()) return refuse(SerializeResult::truncated);
    }

    return SerializeResult::ok;
}

}  // namespace strata
