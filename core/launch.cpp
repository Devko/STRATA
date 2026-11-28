// Track Launch. launch.hpp has the design and the decisions; this file has the bookkeeping.
#include "launch.hpp"

namespace strata {

uint8_t lane_live_source(const Project &project) {
    return lane_source(int(project.bank), int(project.slot));
}

bool lane_borrowed(const Project &project, int track) {
    if (track < 0 || track >= kTracks) return false;
    const uint8_t source = project.settings.lane_source[track];
    return source != kLaneOwn && lane_source_valid(source) && source != lane_live_source(project);
}

uint8_t lane_borrowed_mask(const Project &project) {
    uint8_t mask = 0;
    for (int t = 0; t < kTracks; t++) {
        if (lane_borrowed(project, t)) mask = uint8_t(mask | (1u << t));
    }
    return mask;
}

const Pattern &lane_pattern(const Project &project, int track) {
    if (!lane_borrowed(project, track)) return project.live();
    const uint8_t source = project.settings.lane_source[track];
    return project.patterns[lane_source_bank(source)][lane_source_slot(source)];
}

uint8_t lane_playing_source(const Project &project, int track) {
    return lane_borrowed(project, track) ? project.settings.lane_source[track]
                                         : lane_live_source(project);
}

bool launch_set(Project &project, int track, uint8_t source) {
    if (track < 0 || track >= kTracks || !lane_source_valid(source)) return false;
    project.settings.lane_source[track] = source == lane_live_source(project) ? kLaneOwn : source;
    return true;
}

bool launch_toggle_pin(Project &project, int track) {
    if (track < 0 || track >= kTracks) return false;
    project.settings.lane_pinned = uint8_t(project.settings.lane_pinned ^ (1u << track));
    return true;
}

uint8_t launch_pattern_changed(Project &project, int old_bank, int old_slot) {
    const uint8_t old_live = lane_source(old_bank, old_slot);
    const uint8_t new_live = lane_live_source(project);
    uint8_t restarted = 0;
    for (int t = 0; t < kTracks; t++) {
        uint8_t &source = project.settings.lane_source[t];
        if (!lane_source_valid(source)) source = kLaneOwn;
        const bool pinned = (project.settings.lane_pinned >> t & 1u) != 0;
        // What the lane played before the change, as a pattern.
        const uint8_t was = source == kLaneOwn ? old_live : source;
        if (!pinned) {
            source = kLaneOwn;
            // It plays the new pattern's part, and restarts with it unless it already played that
            // very part (a lane borrowing the slot just switched to keeps its place, NOW-style).
            if (was != new_live) restarted = uint8_t(restarted | (1u << t));
        } else {
            // Pinned: it keeps what it plays. On its own part, that is the pattern being left.
            source = was == new_live ? kLaneOwn : was;
        }
    }
    return restarted;
}

void launch_all_own(Project &project) {
    for (int t = 0; t < kTracks; t++) project.settings.lane_source[t] = kLaneOwn;
}

// --- the per-lane queue -----------------------------------------------------

bool lane_queue_arm(LaneQueue &lane, uint8_t source, QueueMode mode, uint32_t armed_tick,
                    int pattern_ticks, uint32_t origin) {
    if (!lane_source_valid(source)) return false;
    // The timing is the pattern queue's own; bank and slot are placeholders, never read.
    if (!queue_arm(lane.queue, 0, 0, mode, armed_tick, pattern_ticks, origin)) return false;
    lane.source = source;
    return true;
}

bool lane_queue_due(const LaneQueue &lane, uint32_t tick) { return queue_due(lane.queue, tick); }

void lane_queue_clear(LaneQueue &lane) {
    queue_clear(lane.queue);
    lane.source = kLaneOwn;
}

// --- CAPTURE ------------------------------------------------------------------

bool pattern_has_steps(const Pattern &pattern) {
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kStepsPerTrack; s++) {
            if ((pattern.tracks[t].steps[s].flags & (kStepTrig | kStepTrigless)) != 0) return true;
        }
    }
    return false;
}

int launch_capture_slot(const Project &project, int bank) {
    return launch_capture_slot_avoiding(project, bank, nullptr, 0);
}

int launch_capture_slot_avoiding(const Project &project, int bank, const uint8_t *avoid,
                                 int count) {
    if (bank < 0 || bank >= kBanks) return -1;
    for (int slot = 0; slot < kPatternsPerBank; slot++) {
        const uint8_t here = lane_source(bank, slot);
        if (here == lane_live_source(project)) continue;
        bool feeding = false;
        for (int t = 0; t < kTracks; t++) feeding = feeding || lane_playing_source(project, t) == here;
        for (int i = 0; i < count; i++) feeding = feeding || (avoid[i] != kLaneOwn && avoid[i] == here);
        if (feeding) continue;
        if (!pattern_has_steps(project.patterns[bank][slot])) return slot;
    }
    return -1;
}

namespace {

// The pattern a source names: `live` for kLaneOwn or a byte no pattern exists at.
const Pattern &source_pattern(const Project &project, uint8_t source, const Pattern &live) {
    if (source == kLaneOwn || !lane_source_valid(source)) return live;
    return project.patterns[lane_source_bank(source)][lane_source_slot(source)];
}

// Each lane's part from `from[t]`, the master settings from `live`, locks compacted into `out`.
bool compose(const Pattern &live, const Pattern *const from[kTracks], Pattern &out) {
    // Count first: nothing is written unless every part's locks fit in one pool.
    uint32_t needed = 0;
    for (int t = 0; t < kTracks; t++) {
        for (int s = 0; s < kStepsPerTrack; s++) needed += pattern_step_locks(*from[t], t, s).count;
    }
    if (needed > uint32_t(kLockPool)) return false;

    out.master_length = live.master_length;
    out.swing = live.swing;
    out.root = live.root;
    out.scale = live.scale;
    out.feel = live.feel;
    out.seed = live.seed;
    uint16_t used = 0;
    for (int t = 0; t < kTracks; t++) {
        out.tracks[t] = from[t]->tracks[t];
        for (int s = 0; s < kStepsPerTrack; s++) {
            Step &step = out.tracks[t].steps[s];
            const LockView view = pattern_step_locks(*from[t], t, s);
            step.lock_first = view.count != 0 ? used : kNoLock;
            step.lock_count = view.count;
            for (const Lock &lock : view) out.locks[used++] = lock;
        }
    }
    out.locks_used = used;
    return true;
}

}  // namespace

bool launch_compose(const Project &project, Pattern &out) {
    const Pattern *from[kTracks];
    for (int t = 0; t < kTracks; t++) from[t] = &lane_pattern(project, t);
    return compose(project.live(), from, out);
}

bool launch_compose_lanes(const Project &project, uint8_t live, const uint8_t lanes[kTracks],
                          Pattern &out) {
    if (live == kLaneOwn || !lane_source_valid(live)) return false;
    const Pattern &base = project.patterns[lane_source_bank(live)][lane_source_slot(live)];
    const Pattern *from[kTracks];
    for (int t = 0; t < kTracks; t++) from[t] = &source_pattern(project, lanes[t], base);
    return compose(base, from, out);
}

CaptureResult launch_capture(Project &project, int bank, int slot) {
    if (!project_index_valid(bank, slot)) return CaptureResult::no_slot;
    // The destination must not be read while it is written: not the playing pattern and not any
    // lane's source. And it must be empty, so nothing the user made is overwritten.
    const uint8_t here = lane_source(bank, slot);
    for (int t = 0; t < kTracks; t++) {
        if (lane_playing_source(project, t) == here) return CaptureResult::no_slot;
    }
    if (pattern_has_steps(project.patterns[bank][slot])) return CaptureResult::no_slot;

    if (!launch_compose(project, project.patterns[bank][slot])) return CaptureResult::locks_full;
    project.bank_dirty[bank] = true;
    return CaptureResult::ok;
}

}  // namespace strata
