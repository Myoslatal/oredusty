#include <support/test_support.h>

#include <ore/core/log.h>
#include <ore/core/time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace ore::test {
namespace {

struct TestCase {
    std::string name;
    TestFn fn;
};

struct State {
    std::vector<TestCase>& tests;
    std::string current;
    int checks = 0;
    int failures = 0;
    std::vector<std::string> failure_messages;
    bool verbose = false;
};

State* g_state = nullptr;

[[nodiscard]] std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

} // namespace

Registrar::Registrar(std::string_view name, TestFn fn) { registry().push_back(TestCase{std::string(name), fn}); }

void skip(std::string_view reason) { throw SkipException{std::string(reason)}; }

void report(bool passed, std::string_view expression, std::string_view file, i32 line, std::string_view detail) {
    State* state = g_state;
    if (state == nullptr) return;
    ++state->checks;
    if (passed) {
        if (state->verbose) {
            std::fprintf(stdout, "    ok   %s\n", std::string(expression).c_str());
        }
        return;
    }
    ++state->failures;
    std::string message = std::format("{}:{}: {} failed", file, line, expression);
    if (!detail.empty()) message.append("  ->  ").append(detail);
    state->failure_messages.push_back(message);
    std::fprintf(stdout, "    FAIL %s\n", message.c_str());
}

int run_all(int argc, char** argv) {
    std::string filter;
    bool list_only = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
        if (arg.starts_with("--filter=")) filter = std::string(arg.substr(9));
        else if (arg == "--list") list_only = true;
        else if (arg == "-v" || arg == "--verbose") verbose = true;
    }

    auto& tests = registry();
    std::stable_sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) { return a.name < b.name; });

    if (list_only) {
        for (const TestCase& test : tests) std::fprintf(stdout, "%s\n", test.name.c_str());
        return 0;
    }

    set_log_level(LogLevel::Warn);

    int executed = 0;
    int failed_tests = 0;
    int skipped_tests = 0;
    int total_checks = 0;
    const f64 start = Clock::now_seconds();

    for (const TestCase& test : tests) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        State state{tests, test.name, 0, 0, {}, verbose};
        g_state = &state;
        const f64 test_start = Clock::now_seconds();
        std::string status = "PASS";
        std::string note;
        try {
            test.fn();
        } catch (const SkipException& skipped) {
            status = "SKIP";
            note = skipped.reason;
            ++skipped_tests;
        } catch (const std::exception& error) {
            status = "FAIL";
            note = std::format("uncaught exception: {}", error.what());
            ++state.failures;
        } catch (...) {
            status = "FAIL";
            note = "uncaught unknown exception";
            ++state.failures;
        }
        g_state = nullptr;
        // A test fails when a check failed, even if the test body itself returned normally.
        if (state.failures > 0) {
            status = "FAIL";
        }
        if (status == "FAIL") ++failed_tests;
        ++executed;
        total_checks += state.checks;

        const f64 ms = (Clock::now_seconds() - test_start) * 1000.0;
        std::fprintf(stdout, "[%-4s] %-52s %7.2f ms  %2d checks%s%s\n", status.c_str(), test.name.c_str(), ms,
                     state.checks, note.empty() ? "" : "  ", note.c_str());
        std::fflush(stdout);
    }

    const f64 total_ms = (Clock::now_seconds() - start) * 1000.0;
    std::fprintf(stdout, "\n%d test(s), %d check(s), %d failure(s), %d skipped in %.1f ms\n", executed, total_checks,
                 failed_tests, skipped_tests, total_ms);
    std::fflush(stdout);
    return failed_tests == 0 ? 0 : 1;
}

} // namespace ore::test
