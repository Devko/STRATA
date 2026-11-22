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
    // A second test under a name already taken is a failure too. Both still run, but the suite
    // prints the name twice, so a failure no longer says which file it came from -- and --list,
    // which shards by name, would hand both to whichever shard asked for one.
    for (int i = 0; i < count; i++) {
        if (std::strcmp(entries[i].name, name) == 0) {
            std::printf("test name registered twice, rename one: %s\n", name);
            failures++;
            break;
        }
    }
    entries[count].name = name;
    entries[count].fn = fn;
    count++;
}

void fail_header(const char *file, int line) {
    Registry::failures++;
    std::printf("  FAIL %s\n    %s:%d\n", Registry::current, file, line);
}

void check_eq(const char *a, const char *b, const char *ea, const char *eb, const char *file,
              int line) {
    Registry::checks++;
    const bool same = (a == nullptr || b == nullptr) ? a == b : std::strcmp(a, b) == 0;
    if (same) return;
    fail_header(file, line);
    std::printf("    %s == %s\n      left:  %s\n      right: %s\n", ea, eb,
                a ? a : "(null)", b ? b : "(null)");
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
