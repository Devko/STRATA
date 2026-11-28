// ARR REC: see core/arrrec.hpp.
#include "arrrec.hpp"

#include "perform.hpp"   // kMorphScale
#include "queue.hpp"

namespace strata {

namespace {

bool same_fields(const SectionExtra &a, const SectionExtra &b) {
    return a.cap == b.cap && a.mask == b.mask && a.scene == b.scene && a.morph == b.morph &&
           a.energy == b.energy && a.bpm == b.bpm && a.transpose == b.transpose;
}

// Copy the chain into or out of a backup, whole (all 64, so the zeroed tail goes with it).
void store(const Project &project, ArrBackup &backup) {
    for (int i = 0; i < kChainEntries; i++) {
        backup.chain[i] = project.chain[i];
        backup.extra[i] = project.chain_extra[i];
    }
    backup.length = project.settings.chain_length;
}

void restore(Project &project, const ArrBackup &backup) {
    for (int i = 0; i < kChainEntries; i++) {
        project.chain[i] = backup.chain[i];
        project.chain_extra[i] = backup.extra[i];
    }
    project.settings.chain_length = backup.length;
}

bool same_mix(const ArrState &a, const ArrState &b) {
    if (a.bank != b.bank || a.slot != b.slot) return false;
    for (int t = 0; t < kTracks; t++) {
        if (a.lanes[t] != b.lanes[t]) return false;
    }
    return true;
}

bool borrows(const ArrState &state) {
    for (int t = 0; t < kTracks; t++) {
        if (state.lanes[t] != kLaneOwn) return true;
    }
    return false;
}

// CAPTURE what `pass` played into a free slot of its bank, avoiding what it played, what plays
// now (core/launch.hpp) and whatever a queue is about to land on (`queued`, ArrRec::queued). The
// slot, or -1 when none is free or the locks do not fit.
void captured_add(ArrCaptured &captured, int index) {
    for (int i = 0; i < captured.count; i++) {
        if (captured.index[i] == index) return;
    }
    if (captured.count < kProjectPatterns) captured.index[captured.count++] = uint8_t(index);
}

void captured_remove(ArrCaptured &captured, int index) {
    int keep = 0;
    for (int i = 0; i < captured.count; i++) {
        if (captured.index[i] != index) captured.index[keep++] = captured.index[i];
    }
    captured.count = uint8_t(keep);
}

int capture_pass(Project &project, const ArrState &pass, const uint8_t *queued,
                 ArrCaptured &captured) {
    constexpr int kAvoid = 2 * (kTracks + 1);
    uint8_t avoid[kAvoid];
    avoid[0] = lane_source(pass.bank, pass.slot);
    for (int t = 0; t < kTracks; t++) avoid[t + 1] = pass.lanes[t];
    for (int i = 0; i <= kTracks; i++) avoid[kTracks + 1 + i] = queued[i];
    const int slot = launch_capture_slot_avoiding(project, pass.bank, avoid, kAvoid);
    if (slot < 0) return -1;
    if (!launch_compose_lanes(project, avoid[0], pass.lanes, project.patterns[pass.bank][slot])) {
        return -1;
    }
    project.bank_dirty[pass.bank] = true;
    captured_add(captured, int(pass.bank) * kPatternsPerBank + slot);
    return slot;
}

}  // namespace

void arr_state_lanes(ArrState &state, const Project &project) {
    for (int t = 0; t < kTracks; t++) {
        state.lanes[t] = lane_borrowed(project, t) ? project.settings.lane_source[t] : kLaneOwn;
    }
}

ArrState arr_state(uint8_t bank, uint8_t slot, uint8_t mutes, uint8_t scene_b, uint16_t morph,
                   int energy, int transpose, uint16_t tempo_centi, bool external) {
    ArrState state;
    state.bank = bank;
    state.slot = slot;
    SectionExtra &f = state.fields;
    f.cap = 1;
    f.mask = mutes;
    f.scene = uint8_t((scene_b < kSectionScenesMax ? scene_b : kSectionScenesMax - 1) + 1);
    // Where the morph sits, to its nearer side, as the quick fill does (choice 3: SNAP, never a
    // guessed RIDE). Half way exactly is side A.
    f.morph = uint32_t(morph) * 2u > kMorphScale ? kSectionMorphSnap : kSectionMorphSnapA;
    f.energy = section_energy_byte(energy);
    f.transpose = section_transpose_byte(transpose);
    if (external) {
        f.bpm = 0;
    } else {
        f.bpm = tempo_centi < kTempoCentiMin
                    ? kTempoCentiMin
                    : (tempo_centi > kTempoCentiMax ? kTempoCentiMax : tempo_centi);
    }
    return state;
}

bool arr_start(Project &project, ArrRec &rec, ArrBackup &backup) {
    if (rec.on) return false;
    store(project, backup);
    backup.valid = true;
    backup.captured = ArrCaptured{};
    for (int i = 0; i < kChainEntries; i++) {
        project.chain[i] = ChainEntry{};
        project.chain_extra[i] = SectionExtra{};
    }
    project.settings.chain_length = 0;
    rec = ArrRec{};
    rec.on = true;
    return true;
}

void arr_pass_begin(ArrRec &rec, const ArrState &state, uint32_t tick, uint32_t loop) {
    if (!rec.on) return;
    rec.pass_open = true;
    rec.pass_from = tick;
    rec.pass_loop = loop < 1u ? 1u : loop;
    rec.pass = state;
}

ArrPass arr_pass_end(Project &project, ArrRec &rec, uint32_t tick) {
    rec.launch = ArrLaunch::none;
    if (!rec.on || !rec.pass_open) return ArrPass::none;
    rec.pass_open = false;
    // A transport that went backwards has recorded nothing of this pass.
    const uint32_t played = tick >= rec.pass_from ? tick - rec.pass_from : 0u;
    // At least half of the pass, compared without a divide (this chip has none for integers).
    if (uint64_t(played) * 2u < uint64_t(rec.pass_loop)) return ArrPass::dropped;

    // The pattern this pass goes into the chain as: the one the same mix went into last time,
    // the playing pattern when no lane is borrowed, or (below) a capture.
    bool known = false;
    uint8_t bank = rec.pass.bank;
    uint8_t slot = rec.pass.slot;
    if (rec.last_known && same_mix(rec.last, rec.pass)) {
        known = true;
        bank = rec.target_bank;
        slot = rec.target_slot;
    } else if (!borrows(rec.pass)) {
        known = true;
    }

    const int length = chain_length(project);
    if (known && rec.extending && length > 0) {
        ChainEntry &last = project.chain[length - 1];
        if (last.bank == bank && last.slot == slot && last.mutes == 0 &&
            same_fields(project.chain_extra[length - 1], rec.pass.fields) &&
            int(last.repeats) < kChainRepeatsMax) {
            last.repeats = uint8_t(last.repeats + 1);
            return ArrPass::extended;
        }
    }
    // Full before a capture, so a take that is over does not fill a slot for nothing.
    if (length >= kChainEntries) return ArrPass::full;
    ArrLaunch launch = ArrLaunch::none;
    if (!known) {
        const int captured = capture_pass(project, rec.pass, rec.queued, rec.captured);
        if (captured >= 0) {
            slot = uint8_t(captured);
            launch = ArrLaunch::kept;
        } else {
            launch = ArrLaunch::lost;
        }
    }
    // A pattern that cannot exist (damage) is not written; nothing else is refused.
    if (!chain_insert(project, length, bank, slot, 1, 0, &rec.pass.fields)) {
        return ArrPass::dropped;
    }
    rec.extending = true;
    rec.written = uint8_t(rec.written + 1);
    rec.last_known = true;
    rec.last = rec.pass;
    rec.target_bank = bank;
    rec.target_slot = slot;
    rec.launch = launch;
    rec.launch_bank = bank;
    rec.launch_slot = slot;
    return ArrPass::added;
}

void arr_chain_edited(ArrRec &rec) { rec.extending = false; }

int arr_finish(Project &project, ArrRec &rec, ArrBackup &backup, uint32_t tick) {
    if (!rec.on) return 0;
    arr_pass_end(project, rec, tick);
    const int written = int(rec.written);
    const ArrLaunch launch = rec.launch;
    const uint8_t launch_bank = rec.launch_bank;
    const uint8_t launch_slot = rec.launch_slot;
    backup.captured = rec.captured;
    rec = ArrRec{};
    rec.launch = launch;
    rec.launch_bank = launch_bank;
    rec.launch_slot = launch_slot;
    if (written == 0 && backup.valid) {
        restore(project, backup);
        backup.valid = false;
    }
    return written;
}

bool arr_undo(Project &project, const ArrRec &rec, ArrBackup &backup, uint8_t *emptied_banks) {
    if (emptied_banks != nullptr) *emptied_banks = 0;
    if (rec.on || !backup.valid) return false;
    // A swap, entry by entry: the take goes into the backup, so ARR UNDO again brings it back.
    for (int i = 0; i < kChainEntries; i++) {
        const ChainEntry entry = project.chain[i];
        project.chain[i] = backup.chain[i];
        backup.chain[i] = entry;
        const SectionExtra extra = project.chain_extra[i];
        project.chain_extra[i] = backup.extra[i];
        backup.extra[i] = extra;
    }
    const uint8_t length = project.settings.chain_length;
    project.settings.chain_length = backup.length;
    backup.length = length;
    if (backup.captured.count == 0) return true;
    // The take captured: empty what it filled, keep what is playing, and drop the take.
    for (int i = 0; i < backup.captured.count; i++) {
        const int index = backup.captured.index[i];
        const int bank = index / kPatternsPerBank;
        const int slot = index % kPatternsPerBank;
        const uint8_t here = lane_source(bank, slot);
        bool playing = here == lane_live_source(project);
        for (int t = 0; t < kTracks; t++) playing = playing || lane_playing_source(project, t) == here;
        if (playing) continue;
        pattern_init(project.patterns[bank][slot]);
        project.bank_dirty[bank] = true;
        if (emptied_banks != nullptr) *emptied_banks = uint8_t(*emptied_banks | (1u << bank));
    }
    backup.captured = ArrCaptured{};
    backup.valid = false;
    return true;
}

void arr_touched(ArrRec &rec, ArrBackup &backup, int bank, int slot) {
    if (bank < 0 || bank >= kBanks || slot < 0 || slot >= kPatternsPerBank) return;
    const int index = bank * kPatternsPerBank + slot;
    captured_remove(rec.captured, index);
    captured_remove(backup.captured, index);
}

}  // namespace strata
