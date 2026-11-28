#include "taptempo.hpp"

namespace strata {

void tap_reset(TapTempo &tap) {
    tap = TapTempo{};
}

uint32_t tap_tempo(TapTempo &tap, uint32_t now_ms) {
    if (!tap.started) {
        tap.started = true;
        tap.last_ms = now_ms;
        return 0;
    }
    const uint32_t interval = now_ms - tap.last_ms;   // unsigned: wraps with the counter
    tap.last_ms = now_ms;
    // Too long, or no time at all (two events in one main-loop pass): a new count from here.
    if (interval > kTapTimeoutMs || interval == 0) {
        tap.count = 0;
        tap.next = 0;
        return 0;
    }

    uint32_t sum = 0;
    for (int i = 0; i < tap.count; i++) sum += tap.intervals[i];
    if (tap.count > 0) {
        // Half or double the running average: the hand started again at another tempo.
        const uint32_t average = sum / uint32_t(tap.count);
        if (interval * 2u < average || interval > average * 2u) {
            tap.count = 0;
            tap.next = 0;
            sum = 0;
        }
    }

    if (tap.count < kTapIntervals) {
        tap.intervals[tap.count] = interval;
        tap.count++;
        tap.next = tap.count % kTapIntervals;
        sum += interval;
    } else {
        sum -= tap.intervals[tap.next];
        tap.intervals[tap.next] = interval;
        tap.next = (tap.next + 1) % kTapIntervals;
        sum += interval;
    }

    // Tenths of a BPM from the mean interval in ms: 600000 / mean, rounded, as
    // 600000 * count / sum. Main context, not a hot loop: the division is fine here.
    const uint32_t count = uint32_t(tap.count);
    uint32_t tenths = (600000u * count + sum / 2u) / sum;
    if (tenths < kTempoTenthsMin) tenths = kTempoTenthsMin;
    if (tenths > kTempoTenthsMax) tenths = kTempoTenthsMax;
    return tenths;
}

void nudge_turn(Nudge &nudge, int delta, uint32_t now_ms) {
    if (delta == 0) return;
    nudge.direction = delta > 0 ? 1 : -1;
    nudge.until_ms = now_ms + kNudgeMs;
}

bool nudge_due(Nudge &nudge, uint32_t now_ms) {
    if (nudge.direction == 0) return false;
    // Signed difference: still correct across the counter's wrap.
    if (int32_t(now_ms - nudge.until_ms) < 0) return false;
    nudge.direction = 0;
    return true;
}

float nudged_bpm(uint32_t tempo_tenths, int direction) {
    const int sign = direction > 0 ? 1 : (direction < 0 ? -1 : 0);
    const float bpm = float(tempo_tenths) * 0.1f * (1.0f + float(sign * kNudgePermille) * 0.001f);
    if (bpm < float(kTempoTenthsMin) * 0.1f) return float(kTempoTenthsMin) * 0.1f;
    if (bpm > float(kTempoTenthsMax) * 0.1f) return float(kTempoTenthsMax) * 0.1f;
    return bpm;
}

}  // namespace strata
