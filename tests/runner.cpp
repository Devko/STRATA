// The host test runner: owns the registry's storage and main().
#include "check.hpp"

#include <cstring>

namespace strata::test {

Registry::Entry Registry::entries[Registry::kMax];
int Registry::count = 0;
int Registry::checks = 0;
int Registry::failures = 0;
const char *Registry::current = "";

void Registry::add(const char *name, void (*fn)()) {
    if (count >= kMax) {
        // A failure, not just a message. Dropping a registration means a test that never runs
        // and therefore never fails, and a suite that is quietly getting smaller while reporting
        // success is worse than no suite at all.
        std::printf("test registry full (%d), raise Registry::kMax: dropped %s\n", kMax, name);
        failures++;
        return;
    }
    entries[count].name = name;
    entries[count].fn = fn;
    count++;
}

void fail_header(const char *file, int line) {
    Registry::failures++;
    std::printf("  FAIL %s\n    %s:%d\n", Registry::current, file, line);
}

void check_true(bool ok, const char *expression, const char *file, int line) {
    Registry::checks++;
    if (ok) return;
    fail_header(file, line);
    std::printf("    %s\n", expression);
}

int Registry::run_all() {
    std::printf("running %d test%s\n", count, count == 1 ? "" : "s");
    for (int i = 0; i < count; i++) {
        current = entries[i].name;
        const int before = failures;
        entries[i].fn();
        if (failures == before) std::printf("  ok   %s\n", entries[i].name);
    }
    std::printf("\n%d check%s, %d failure%s\n", checks, checks == 1 ? "" : "s",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

}  // namespace strata::test

int main(int argc, char **argv) {
    // --list prints the test names, which the emulator scenarios and CI can use to shard.
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < strata::test::Registry::count; i++) {
            std::printf("%s\n", strata::test::Registry::entries[i].name);
        }
        return 0;
    }
    return strata::test::Registry::run_all();
}
