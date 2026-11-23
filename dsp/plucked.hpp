// STRING: plucked strings and struck bars, for plucks, mallets, kalimbas and glass.
//
// Two physical models (dsp/pluckloops.h draws both):
//
//   STRING  Karplus-Strong (Karplus and Strong, 1983, with Jaffe and Smith's 1983 extensions):
//           a delay line one period long recirculating through a low-pass. The excitation is put
//           in the line when the note starts -- filtered noise for a pluck, a longer brighter
//           burst for NOISE, a half-sine for a MALLET -- and a pick-position comb notches it the
//           way plucking a string at that point does.
//   BAR     Four resonators at a struck bar's modes. INH moves them from a tuned bar (1, 4, 10,
//           20: marimba-like, almost harmonic) to a free bar (1, 2.756, 5.404, 8.933: glass and
//           metal). The higher modes decay faster, as they do on wood and metal alike.
//
// Pages (the machine maps them; the voice reads only PluckSettings):
//
//     SRC   MOD string or bar   EXC pluck/noise/mallet   BRT brightness   POS strike position
//     AMP   DEC decay           INH inharmonicity (bar)  VEL vel>bright   LVL level
//
// No filter of its own and no envelope: the model decays by itself, and the insert chain's
// filter on FLTR is there for the rest. A released note (a KEYS key up, a gate ending) is muted
// the way a hand on the string mutes it: its decay drops to kMuteS.
//
// Nothing here allocates in audio(): a voice's delay line is taken in init().
#pragma once

#include <cstdint>

#include <enaga/dsp.hpp>

#include "pluckloops.h"
#include "track.hpp"

namespace strata {

enum class PluckModel : uint8_t { string = 0, bar };
enum class PluckExciter : uint8_t { pluck = 0, noise, mallet };

PluckModel pluck_model(uint8_t value);
PluckExciter pluck_exciter(uint8_t value);
float pluck_decay_seconds(uint8_t value);   // DEC: the T60, 0.1 s to 20 s

struct PluckSettings {
    // Reads SRC 0-3 and AMP 8-11. Control rate, once per block.
    static PluckSettings make(const TrackParams &params, float rate, float inv_rate);

    PluckModel model = PluckModel::string;
    PluckExciter exciter = PluckExciter::pluck;
    float brightness = 0.5f;      // BRT, 0-1
    float position = 0.2f;        // POS, as a fraction of the string, 0.05-0.5
    float decay_s = 2.0f;         // DEC, the T60
    float inharmonicity = 0.0f;   // INH, 0-1
    float velocity_bright = 0.0f; // VEL, 0-1
    float out_gain = 0.25f;       // LVL
    float rate = 48000.0f;
    float inv_rate = 1.0f / 48000.0f;
};

class PluckVoice {
public:
    static constexpr int kControl = 16;
    static constexpr int kChokeFrames = 64;
    static constexpr float kSilent = 1e-4f;
    static constexpr float kMuteS = 0.12f;      // a released note's T60
    static constexpr int kBurst = 128;           // the bar's excitation, in samples
    static constexpr float kLowestHz = 24.0f;    // the delay line's length allows 23.4

    // Takes the delay line. init() only.
    bool init();

    void reset();
    void kill();
    void set_note(float note, float velocity) {
        note_ = note;
        velocity_ = velocity;
    }
    // Starts the note: puts the excitation in the string, or arms the bar's burst. `scratch` is a
    // STRATA_PLUCK_LINE-float buffer the caller owns and every voice shares.
    void gate_on(const PluckSettings &s, float *scratch);
    void gate_off() { muted_ = true; }
    bool released() const { return muted_; }
    bool sounding() const { return sounding_; }
    float amp_level() const { return peak_; }
    float pitch() const { return note_; }
    void choke();
    bool choking() const { return choke_left_ > 0; }

    // One control slice of n frames (1..kControl), ADDED into out. `work` and `env` are
    // kControl-float scratch buffers the caller owns.
    void run(const PluckSettings &s, float *out, int n, float *work, float *env);

    // For tests: the period the string was last tuned to, in samples.
    float period() const { return delay_; }

private:
    float frequency(const PluckSettings &s) const;

    strata_pluck_state string_{};
    strata_bar_state bar_{};
    strata_bar_coefs coefs_{};
    float burst_[kBurst] = {};
    int burst_left_ = 0;
    int burst_pos_ = 0;
    uint32_t rng_ = 0x2545F491u;

    PluckModel model_ = PluckModel::string;   // the model this note started on
    float note_ = 60.0f;
    float velocity_ = 1.0f;
    float delay_ = 100.0f;
    float peak_ = 0.0f;
    int quiet_slices_ = 0;
    bool sounding_ = false;
    bool muted_ = false;

    // The bar's coefficients depend on the note and the settings; worked out again only when
    // one of them moves.
    float coef_note_ = -1.0f;
    float coef_decay_ = -1.0f;
    float coef_inh_ = -1.0f;
    float coef_bright_ = -1.0f;
    float coef_pos_ = -1.0f;
    bool coef_muted_ = false;

    int choke_left_ = 0;
    float choke_step_ = 0.0f;
    float choke_gain_ = 1.0f;
};

}  // namespace strata
