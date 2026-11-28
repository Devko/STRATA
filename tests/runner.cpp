// The host test runner: owns the registry's storage and main().
#include "check.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace strata::test {

Registry::Entry Registry::entries[Registry::kMax];
int Registry::count = 0;
int Registry::checks = 0;
int Registry::failures = 0;
const char *Registry::current = "";

void Registry::add(const char *name, void (*fn)(), unsigned weight) {
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
    entries[count].weight = weight > 0 ? weight : 1;
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

// --timeout: a test that never returns (a key loop waiting for a screen that a remap took away)
// would otherwise hold a CI job until GitHub's six-hour cap. SIGALRM names the test and exits.
// Only async-signal-safe calls in the handler: write() and _exit().
static void on_alarm(int) {
    static const char head[] = "\n  TIMEOUT ";
    static const char tail[] = " (did not return within --timeout)\n";
    (void)!write(1, head, sizeof head - 1);
    (void)!write(1, Registry::current, std::strlen(Registry::current));
    (void)!write(1, tail, sizeof tail - 1);
    _exit(2);
}

int Registry::run_all(int shard, int shards, unsigned timeout_s) {
    if (timeout_s > 0) std::signal(SIGALRM, on_alarm);
    // CONTIGUOUS blocks in registration order, not round robin. Every test then runs after the
    // same predecessors it has in the serial run, or as the first test in its process, which
    // `--only` already has to survive. Round robin (tried first) gives each test a predecessor it
    // never had: it passes today only after the three tests/apprig.cpp fixes of the same change,
    // and a random order still fails dozens of tests (docs/HANDOFF.md, "Test order").
    //
    // The blocks are of equal WEIGHT (TEST_WEIGHTED), not of equal count: test i goes to the shard
    // its starting weight falls in, so a test of several seconds is spread over its own shards
    // instead of making one shard as long as the rest of the suite together.
    int64_t total = 0;
    for (int i = 0; i < count; i++) total += entries[i].weight;
    int first = count, end = count;
    int64_t before = 0;
    for (int i = 0; i < count; i++) {
        const int64_t k = total > 0 ? before * shards / total : 0;
        if (k >= shard && first == count) first = i;
        if (k > shard) {
            end = i;
            break;
        }
        before += entries[i].weight;
    }
    if (first > end) first = end;
    const int mine = end - first;
    if (shards > 1) {
        std::printf("running %d of %d tests (shard %d/%d)\n", mine, count, shard + 1, shards);
    } else {
        std::printf("running %d test%s\n", count, count == 1 ? "" : "s");
    }
    for (int i = first; i < end; i++) {
        current = entries[i].name;
        const int before = failures;
        if (timeout_s > 0) alarm(timeout_s);
        entries[i].fn();
        if (failures == before) std::printf("  ok   %s\n", entries[i].name);
    }
    if (timeout_s > 0) alarm(0);
    std::printf("\n%d check%s, %d failure%s\n", checks, checks == 1 ? "" : "s",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

// --names FILE runs the tests named in FILE, one per line, in that order: the way to replay a
// shard's exact sequence when a test passes alone and fails in a shard (an order dependence).
int Registry::run_names(const char *path, unsigned timeout_s) {
    std::FILE *f = std::fopen(path, "r");
    if (f == nullptr) {
        std::printf("cannot open %s\n", path);
        return 1;
    }
    if (timeout_s > 0) std::signal(SIGALRM, on_alarm);
    char line[512];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        line[std::strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        int i = 0;
        while (i < count && std::strcmp(entries[i].name, line) != 0) i++;
        if (i == count) {
            std::printf("  FAIL no test named %s\n", line);
            failures++;
            continue;
        }
        current = entries[i].name;
        const int before = failures;
        if (timeout_s > 0) alarm(timeout_s);
        entries[i].fn();
        if (failures == before) std::printf("  ok   %s\n", entries[i].name);
    }
    if (timeout_s > 0) alarm(0);
    std::fclose(f);
    std::printf("\n%d check%s, %d failure%s\n", checks, checks == 1 ? "" : "s",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

}  // namespace strata::test

int main(int argc, char **argv) {
    // Line buffered even into a pipe or a file (CI, tools/test_shards.sh), so the lines before a
    // --timeout or a sanitizer abort are not lost in the buffer when the process dies.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    // --list prints the test names, which the emulator scenarios and CI can use to shard.
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < strata::test::Registry::count; i++) {
            std::printf("%s\n", strata::test::Registry::entries[i].name);
        }
        return 0;
    }
    // --only NAME runs one test. tools/screenshots.sh uses it: the screenshot test must start
    // from the app's own first init, with the default beat in memory, which a full run would
    // already have replaced with a blank project.
    if (argc > 2 && std::strcmp(argv[1], "--only") == 0) {
        for (int i = 0; i < strata::test::Registry::count; i++) {
            if (std::strcmp(strata::test::Registry::entries[i].name, argv[2]) != 0) continue;
            strata::test::Registry::current = argv[2];
            strata::test::Registry::entries[i].fn();
            std::printf("%d check%s, %d failure%s\n", strata::test::Registry::checks,
                        strata::test::Registry::checks == 1 ? "" : "s",
                        strata::test::Registry::failures,
                        strata::test::Registry::failures == 1 ? "" : "s");
            return strata::test::Registry::failures == 0 ? 0 : 1;
        }
        std::printf("no test named %s\n", argv[2]);
        return 1;
    }
    // --shard K/N runs every Nth test starting at K (1-based), so tools/ci_local.sh and CI can
    // run N processes side by side. --timeout S fails a test that runs longer than S seconds.
    int shard = 0, shards = 1;
    unsigned timeout_s = 0;
    const char *names = nullptr;
    for (int a = 1; a < argc; a++) {
        if (std::strcmp(argv[a], "--shard") == 0 && a + 1 < argc) {
            const char *spec = argv[++a];
            const char *slash = std::strchr(spec, '/');
            const int k = std::atoi(spec);
            const int n = slash ? std::atoi(slash + 1) : 0;
            if (k < 1 || n < 1 || k > n) {
                std::printf("--shard wants K/N with 1 <= K <= N, got %s\n", spec);
                return 1;
            }
            shard = k - 1;
            shards = n;
        } else if (std::strcmp(argv[a], "--names") == 0 && a + 1 < argc) {
            names = argv[++a];
        } else if (std::strcmp(argv[a], "--timeout") == 0 && a + 1 < argc) {
            timeout_s = static_cast<unsigned>(std::atoi(argv[++a]));
        } else {
            std::printf("unknown argument %s (try --list, --only NAME, --names FILE, --shard K/N, --timeout S)\n",
                        argv[a]);
            return 1;
        }
    }
    if (names != nullptr) return strata::test::Registry::run_names(names, timeout_s);
    return strata::test::Registry::run_all(shard, shards, timeout_s);
}
