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
    static constexpr int kMax = 512;
    struct Entry {
        const char *name;
        void (*fn)();
    };

    static Entry entries[kMax];
    static int count;
    static int checks;
    static int failures;
    static const char *current;

    static void add(const char *name, void (*fn)());
    static int run_all();
};

struct Register {
    Register(const char *name, void (*fn)()) { Registry::add(name, fn); }
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

#define CHECK(cond) ::strata::test::check_true((cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(a, b) ::strata::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, tolerance) \
    ::strata::test::check_near((a), (b), (tolerance), #a, #b, __FILE__, __LINE__)
