// MID's CC map (dsp/extmidi.cpp). The slot names say the CC number ("C74"), and the engine sends
// whatever `cc_for` returns, so the two tables in extmidi.cpp must agree: a slot that reads C74
// on screen and sends CC 71 would be wrong in a way no other test sees.
#include "check.hpp"

#include "extmidi.hpp"

using namespace strata;

TEST(mid_slot_names_say_the_cc_each_slot_sends) {
    MidiMachine m;
    for (int i = 0; i < kParams; i++) {
        const char *name = m.param_name(i);
        const uint8_t cc = MidiMachine::cc_for(i);
        if (cc == 0) {
            CHECK_EQ(name[0], '-');
            continue;
        }
        CHECK_EQ(name[0], 'C');
        CHECK_EQ(int(name[1] - '0') * 10 + int(name[2] - '0'), int(cc));
    }
}

TEST(mid_sends_the_gm2_sound_controllers_on_src_and_amp_only) {
    const uint8_t expected[kParams] = {74, 71, 73, 72, 0, 0, 0, 0, 70, 75, 76, 77, 0, 0, 0, 0};
    for (int i = 0; i < kParams; i++) CHECK_EQ(int(MidiMachine::cc_for(i)), int(expected[i]));
    CHECK_EQ(int(MidiMachine::cc_for(-1)), 0);
    CHECK_EQ(int(MidiMachine::cc_for(kParams)), 0);
}
