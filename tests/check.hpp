// A tiny host test framework for STRATA's portable code.
//
// It is deliberately small and dependency-free. The device build has no exceptions, no RTTI
// and no iostreams, and core/ must keep compiling under exactly those rules, so the tests are
// built with the same flags (see host/Makefile) and this header obeys them too: a failed check
// records and carries on rather than throwing.
//
//     TEST(swing_shifts_even_sixteenths) {
//         CHECK_EQ(shift(1, 60), 0);
//         CHECK(shift(1, 75) > 0);
//     }
//
// Only tests/ and host/ use this; it never reaches the device.
#pragma once

#include <cstdint>
#include <cstdio>

namespace strata::test {

struct Registry {
    // Headroom, not a budget: the suite passed 400 at M3 and the cap is a silent cliff if it is
    // ever reached (a skipped registration is a test that never runs and never fails). The
    // overflow in Registry::add also counts a failure now, so the suite goes red rather than
    // quietly getting smaller.
    //
    // Raised from 1024 at 963 tests, when app/ joined the suite and four test files arrived in
    // one evening. 61 slots of headroom is not headroom.
    static constexpr int kMax = 4096;
    struct Entry {
        const char *name;
        void (*fn)();
        // What the test costs against a typical one, for --shard's cuts (TEST_WEIGHTED). Most
        // tests are 1; only one that takes seconds needs more, or its shard holds up the run.
        unsigned weight;
    };

    static Entry entries[kMax];
    static int count;
    static int checks;
    static int failures;
    static const char *current;

    static void add(const char *name, void (*fn)(), unsigned weight = 1);
    static int run_all(int shard = 0, int shards = 1, unsigned timeout_s = 0);
    static int run_names(const char *path, unsigned timeout_s);
};

struct Register {
    Register(const char *name, void (*fn)(), unsigned weight = 1) {
        Registry::add(name, fn, weight);
    }
};

// Value printing, chosen by overload so the header needs no RTTI and no iostreams.
inline void print_value(long long v) { std::printf("%lld", v); }
inline void print_value(unsigned long long v) { std::printf("%llu", v); }
inline void print_value(int v) { std::printf("%d", v); }
inline void print_value(unsigned v) { std::printf("%u", v); }
inline void print_value(double v) { std::printf("%.9g", v); }
inline void print_value(float v) { std::printf("%.9g", double(v)); }
inline void print_value(bool v) { std::printf("%s", v ? "true" : "false"); }
inline void print_value(const char *v) { std::printf("%s", v ? v : "(null)"); }
inline void print_value(const void *v) { std::printf("%p", v); }

void fail_header(const char *file, int line);

template <class A, class B>
void check_eq(const A &a, const B &b, const char *ea, const char *eb, const char *file, int line) {
    Registry::checks++;
    if (a == b) return;
    fail_header(file, line);
    std::printf("    %s == %s\n      left:  ", ea, eb);
    print_value(a);
    std::printf("\n      right: ");
    print_value(b);
    std::printf("\n");
}

// C strings compare by CONTENT. The template above would compare ADDRESSES -- array-to-pointer
// decay, then `==` on two pointers -- so `CHECK_EQ(chord_name(t), "MIN7")` passes only when the
// compiler happens to pool the two literals into one object. At -O2 it usually does, which is
// how five tests in this suite (chord, scale, perform and repeat names) came to pass for a reason
// that had nothing to do with their strings: an AddressSanitizer build at -O1 stopped pooling
// them and all five failed at once, printing two identical strings as "left" and "right".
//
// A non-template overload wins over the template when both arguments are C strings -- the
// array-to-pointer conversion is an lvalue transformation, so the two candidates rank equal and
// the non-template is preferred. So this repairs every CHECK_EQ on a name without editing a test.
// A null on either side falls back to a pointer compare, because strcmp on null is undefined.
void check_eq(const char *a, const char *b, const char *ea, const char *eb, const char *file,
              int line);

template <class A, class B>
void check_near(const A &a, const B &b, double tolerance, const char *ea, const char *eb,
                const char *file, int line) {
    Registry::checks++;
    const double d = double(a) - double(b);
    if ((d < 0 ? -d : d) <= tolerance) return;
    fail_header(file, line);
    std::printf("    %s ~= %s (tolerance %.9g)\n      left:  ", ea, eb, tolerance);
    print_value(a);
    std::printf("\n      right: ");
    print_value(b);
    std::printf("\n");
}

void check_true(bool ok, const char *expression, const char *file, int line);

}  // namespace strata::test

#define TEST(name)                                                        \
    static void name();                                                   \
    static ::strata::test::Register strata_register_##name(#name, name);  \
    static void name()

// A test that costs about `weight` typical tests (a typical test is about 15 ms under ASan).
// tools/test_shards.sh cuts the suite into blocks of equal weight, not of equal count, so a test
// of several seconds gets a shard of its own instead of holding up the whole run.
#define TEST_WEIGHTED(name, weight)                                                \
    static void name();                                                            \
    static ::strata::test::Register strata_register_##name(#name, name, weight);   \
    static void name()

#define CHECK(cond) ::strata::test::check_true((cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(a, b) ::strata::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tolerance) \
    ::strata::test::check_near((a), (b), (tolerance), #a, #b, __FILE__, __LINE__)
