// The performance mixer's slots. core/perffx.hpp has the design.
#include "perffx.hpp"

namespace strata {
namespace {

constexpr float kInvAmount = 1.0f / float(kPerfAmountMax);

uint8_t byte_of(float x) {
    const float scaled = x * 127.0f + 0.5f;
    return uint8_t(scaled < 0.0f ? 0.0f : (scaled > 127.0f ? 127.0f : scaled));
}

}  // namespace

const char *perf_fx_name(PerfFx fx) {
    switch (fx) {
        case PerfFx::lp_sweep:    return "LP SWEEP";
        case PerfFx::hp_sweep:    return "HP SWEEP";
        case PerfFx::hp_riser:    return "HP RISER";
        case PerfFx::echo_riser:  return "ECHO RISER";
        case PerfFx::echo_freeze: return "ECHO FREEZE";
        case PerfFx::reverb_wash: return "REVERB WASH";
        case PerfFx::mute_bus:    return "MUTE BUS";
        case PerfFx::tape_stop:   return "TAPE STOP";
        case PerfFx::bass_kill:   return "BASS KILL";
        case PerfFx::pump:        return "PUMP";
        case PerfFx::crush:       return "CRUSH";
        case PerfFx::noise_riser: return "NOISE RISER";
        case PerfFx::morph:       return "MORPH";
        case PerfFx::gate:        return "GATE";
        case PerfFx::build:       return "BUILD";
        case PerfFx::looper:      return "LOOPER";
        default:                  return "OFF";
    }
}

const char *perf_fx_short(PerfFx fx) {
    switch (fx) {
        case PerfFx::lp_sweep:    return "LP";
        case PerfFx::hp_sweep:    return "HP";
        case PerfFx::hp_riser:    return "HPRIS";
        case PerfFx::echo_riser:  return "ECHO";
        case PerfFx::echo_freeze: return "FREEZ";
        case PerfFx::reverb_wash: return "WASH";
        case PerfFx::mute_bus:    return "MUTE";
        case PerfFx::tape_stop:   return "TAPE";
        case PerfFx::bass_kill:   return "BASS";
        case PerfFx::pump:        return "PUMP";
        case PerfFx::crush:       return "CRUSH";
        case PerfFx::noise_riser: return "NOISE";
        case PerfFx::morph:       return "MORPH";
        case PerfFx::gate:        return "GATE";
        case PerfFx::build:       return "BUILD";
        case PerfFx::looper:      return "LOOP";
        default:                  return "OFF";
    }
}

PerfPlan perf_plan(PerfFx fx, uint8_t amount) {
    PerfPlan plan;
    if (amount == 0) return plan;                 // at rest it lets go
    const uint8_t clamped = amount > kPerfAmountMax ? kPerfAmountMax : amount;
    const float a = float(clamped) * kInvAmount;
    switch (fx) {
        case PerfFx::lp_sweep:
            plan.dj_on = true;
            plan.dj = -a;
            break;
        case PerfFx::hp_sweep:
            plan.dj_on = true;
            plan.dj = a;
            break;
        case PerfFx::hp_riser:
            // The DJ filter's resonance already climbs with its amount (dsp/buses.cpp), so this
            // is the HP side stopping short of the top, where the riser is still audible, with
            // the whole mix going into the reverb behind it.
            plan.dj_on = true;
            plan.dj = 0.85f * a;
            plan.send_reverb = byte_of(a);
            plan.reverb_decay_s = 2.0f + 4.0f * a;
            break;
        case PerfFx::echo_riser:
            plan.send_delay = byte_of(a);
            plan.delay_feedback = 0.35f + 0.55f * a;
            break;
        case PerfFx::echo_freeze:
            plan.send_delay = byte_of(a);
            plan.delay_feedback = 0.35f + 0.35f * a;
            plan.delay_freeze = clamped >= kPerfFreezeThreshold;
            break;
        case PerfFx::reverb_wash:
            plan.send_reverb = byte_of(a);
            plan.reverb_decay_s = 2.0f + 8.0f * a;
            break;
        case PerfFx::mute_bus:
            // Squared, so the fade sounds even rather than all at the bottom of the knob, and
            // 127 is silence exactly.
            plan.tracks = (1.0f - a) * (1.0f - a);
            break;
        case PerfFx::tape_stop:
            plan.tape_stop = clamped >= kPerfHoldThreshold;
            break;
        case PerfFx::bass_kill:
            plan.eq_low = 1.0f - a;
            break;
        case PerfFx::pump:
            plan.pump = a;
            break;
        case PerfFx::crush:
            plan.crush = a;
            break;
        case PerfFx::noise_riser:
            plan.noise = a;
            break;
        case PerfFx::gate:
            plan.gate = a;
            break;
        case PerfFx::build:
            plan.dj_on = true;
            plan.dj = 0.7f * a;
            plan.send_delay = byte_of(a);
            plan.delay_feedback = 0.35f + 0.4f * a;
            plan.noise = 0.8f * a;
            break;
        default:
            break;
    }
    return plan;
}

}  // namespace strata

namespace strata {

const char *perf_glide_name(PerfGlide glide) {
    switch (glide) {
        case PerfGlide::off:       return "OFF";
        case PerfGlide::sixteenth: return "1/16";
        case PerfGlide::beat:      return "1 BEAT";
        case PerfGlide::bar:       return "1 BAR";
        default:                   return "?";
    }
}

uint32_t perf_glide_ms(PerfGlide glide, float bpm) {
    const float tempo = bpm < 20.0f ? 20.0f : (bpm > 300.0f ? 300.0f : bpm);
    float beats = 0.0f;
    switch (glide) {
        case PerfGlide::sixteenth: beats = 0.25f; break;
        case PerfGlide::beat:      beats = 1.0f; break;
        case PerfGlide::bar:       beats = 4.0f; break;
        default:                   return 0;
    }
    // Control rate, once per button press: the one divide is fine here.
    return uint32_t(60000.0f * beats / tempo + 0.5f);
}

uint32_t perf_release_ms(PerfGlide glide, float bpm) {
    const uint32_t full = perf_glide_ms(glide, bpm);
    const uint32_t sixteenth = perf_glide_ms(PerfGlide::sixteenth, bpm);
    return full < sixteenth ? full : sixteenth;
}

namespace {

uint8_t lerp_amount(uint8_t from, uint8_t to, uint32_t at, uint32_t length) {
    const int span = int(to) - int(from);
    return uint8_t(int(from) + span * int(at) / int(length));
}

}  // namespace

void perf_ride_at(const PerfRide &ride, uint32_t elapsed_ms, PerfFx &fx, uint8_t &amount) {
    const uint32_t length = ride.length_ms;
    if (elapsed_ms >= length) {
        fx = ride.to_fx;
        amount = ride.to_amount;
        return;
    }
    if (ride.to_amount == 0 || ride.to_fx == PerfFx::none) {
        fx = ride.from_fx;
        amount = lerp_amount(ride.from_amount, 0, elapsed_ms, length);
        return;
    }
    if (ride.from_amount == 0 || ride.from_fx == PerfFx::none || ride.from_fx == ride.to_fx) {
        fx = ride.to_fx;
        const uint8_t from = ride.from_fx == ride.to_fx ? ride.from_amount : 0;
        amount = lerp_amount(from, ride.to_amount, elapsed_ms, length);
        return;
    }
    const uint32_t half = length / 2u;
    if (elapsed_ms < half) {
        fx = ride.from_fx;
        amount = lerp_amount(ride.from_amount, 0, elapsed_ms, half);
    } else {
        fx = ride.to_fx;
        amount = lerp_amount(0, ride.to_amount, elapsed_ms - half, length - half);
    }
}

}  // namespace strata
