// ARR REC: see core/arrrec.hpp.
#include "arrrec.hpp"

#include "perform.hpp"   // kMorphScale
#include "queue.hpp"

namespace strata {

namespace {

bool same_fields(const SectionExtra &a, const SectionExtra &b) {
    return a.cap == b.cap && a.mask == b.mask && a.scene == b.scene && a.morph == b.morph &&
           a.energy == b.energy && a.bpm == b.bpm;
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

}  // namespace

ArrState arr_state(uint8_t bank, uint8_t slot, uint8_t mutes, uint8_t scene_b, uint16_t morph,
                   int energy, uint16_t tempo_centi, bool external) {
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
    if (!rec.on || !rec.pass_open) return ArrPass::none;
    rec.pass_open = false;
    // A transport that went backwards has recorded nothing of this pass.
    const uint32_t played = tick >= rec.pass_from ? tick - rec.pass_from : 0u;
    // At least half of the pass, compared without a divide (this chip has none for integers).
    if (uint64_t(played) * 2u < uint64_t(rec.pass_loop)) return ArrPass::dropped;

    const int length = chain_length(project);
    if (rec.extending && length > 0) {
        ChainEntry &last = project.chain[length - 1];
        if (last.bank == rec.pass.bank && last.slot == rec.pass.slot && last.mutes == 0 &&
            same_fields(project.chain_extra[length - 1], rec.pass.fields) &&
            int(last.repeats) < kChainRepeatsMax) {
            last.repeats = uint8_t(last.repeats + 1);
            return ArrPass::extended;
        }
    }
    if (length >= kChainEntries) return ArrPass::full;
    // A pattern that cannot exist (damage) is not written; nothing else is refused.
    if (!chain_insert(project, length, rec.pass.bank, rec.pass.slot, 1, 0, &rec.pass.fields)) {
        return ArrPass::dropped;
    }
    rec.extending = true;
    rec.written = uint8_t(rec.written + 1);
    return ArrPass::added;
}

void arr_chain_edited(ArrRec &rec) { rec.extending = false; }

int arr_finish(Project &project, ArrRec &rec, ArrBackup &backup, uint32_t tick) {
    if (!rec.on) return 0;
    arr_pass_end(project, rec, tick);
    const int written = int(rec.written);
    rec = ArrRec{};
    if (written == 0 && backup.valid) {
        restore(project, backup);
        backup.valid = false;
    }
    return written;
}

bool arr_undo(Project &project, const ArrRec &rec, ArrBackup &backup) {
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
    return true;
}

}  // namespace strata
