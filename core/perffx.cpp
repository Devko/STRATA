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
        case PerfFx::stutter:     return "STUTTER";
        case PerfFx::tape_stop:   return "TAPE STOP";
        case PerfFx::bass_kill:   return "BASS KILL";
        case PerfFx::pump:        return "PUMP";
        case PerfFx::crush:       return "CRUSH";
        case PerfFx::noise_riser: return "NOISE RISER";
        case PerfFx::flanger:     return "FLANGER";
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
        case PerfFx::stutter:     return "STUTT";
        case PerfFx::tape_stop:   return "TAPE";
        case PerfFx::bass_kill:   return "BASS";
        case PerfFx::pump:        return "PUMP";
        case PerfFx::crush:       return "CRUSH";
        case PerfFx::noise_riser: return "NOISE";
        case PerfFx::flanger:     return "FLANG";
        case PerfFx::gate:        return "GATE";
        case PerfFx::build:       return "BUILD";
        case PerfFx::looper:      return "LOOP";
        default:                  return "OFF";
    }
}

int perf_stutter_band(uint8_t amount) {
    if (amount < kPerfHoldThreshold) return -1;
    // Four equal bands over the rest of the knob, longest first: the build gets faster as it
    // turns, which is what a stutter riser is.
    const int span = int(kPerfAmountMax) + 1 - int(kPerfHoldThreshold);   // 112
    const int band = (int(amount) - int(kPerfHoldThreshold)) * 4 / span;
    return band > 3 ? 3 : band;
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
        case PerfFx::stutter:
            plan.repeat = int8_t(perf_stutter_band(clamped));
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
        case PerfFx::flanger:
            plan.flanger = a;
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
