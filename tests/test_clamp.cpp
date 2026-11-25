// core/clamp.hpp: the shared integer clamps that replaced nine private copies.
#include "check.hpp"

#include "clamp.hpp"

using namespace strata;

TEST(clamp_int_holds_both_ends_and_passes_the_inside) {
    CHECK_EQ(clamp_int(-5, 0, 10), 0);
    CHECK_EQ(clamp_int(15, 0, 10), 10);
    for (int v = 0; v <= 10; v++) CHECK_EQ(clamp_int(v, 0, 10), v);
    CHECK_EQ(clamp_int(-100, -64, 64), -64);
    CHECK_EQ(clamp_int(3, 3, 3), 3);
}

TEST(clamp_u8_and_u16_hold_both_ends) {
    CHECK_EQ(int(clamp_u8(0, 1, 127)), 1);
    CHECK_EQ(int(clamp_u8(200, 1, 127)), 127);
    CHECK_EQ(int(clamp_u8(64, 1, 127)), 64);
    CHECK_EQ(int(clamp_u16(10, 200, 3000)), 200);
    CHECK_EQ(int(clamp_u16(65535, 200, 3000)), 3000);
    CHECK_EQ(int(clamp_u16(1200, 200, 3000)), 1200);
    static_assert(clamp_int(9, 0, 4) == 4, "usable in constant expressions");
}
