// Ore framework - micro test harness (no external dependency).
//
//   #include <support/test_support.h>
//
//   ORE_TEST(block_allocator_coalesces) {
//       ORE_CHECK(allocator.validate());
//       ORE_CHECK_EQ(allocator.used(), 64u);
//   }
//
//   ORE_TEST_MAIN
#pragma once

#include <ore/core/types.h>

#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>

namespace ore::test {

using TestFn = void (*)();

struct Registrar {
    Registrar(std::string_view name, TestFn fn);
};

struct SkipException {
    std::string reason;
};

/// Thrown by ORE_SKIP / ORE_REQUIRE to unwind the current test.
[[noreturn]] void skip(std::string_view reason);

void report(bool passed, std::string_view expression, std::string_view file, i32 line, std::string_view detail);
int run_all(int argc, char** argv);

/// Same definition as std::formattable, but usable in C++20 mode.
template <class T, class CharT = char>
concept Formattable = requires(T&& value, std::basic_format_context<std::back_insert_iterator<std::string>, CharT>& ctx) {
    std::formatter<std::remove_cvref_t<T>, CharT>{}.format(value, ctx);
};

template <class T>
[[nodiscard]] std::string debug_string(const T& value) {
    if constexpr (Formattable<T>) {
        return std::format("{}", value);
    } else {
        return "<unprintable>";
    }
}

template <class A, class B>
void check_eq(const A& a, const B& b, std::string_view expr_a, std::string_view expr_b, std::string_view file,
              i32 line) {
    if (a == b) {
        report(true, expr_a, file, line, {});
        return;
    }
    report(false, expr_a, file, line,
           std::format("{} == {}  ({} vs {})", expr_a, expr_b, debug_string(a), debug_string(b)));
}

template <class A, class B>
void check_ne(const A& a, const B& b, std::string_view expr_a, std::string_view expr_b, std::string_view file,
              i32 line) {
    if (!(a == b)) {
        report(true, expr_a, file, line, {});
        return;
    }
    report(false, expr_a, file, line, std::format("{} != {}  (both {})", expr_a, expr_b, debug_string(a)));
}

template <class T>
void check_near(const T& a, const T& b, T epsilon, std::string_view expr, std::string_view file, i32 line) {
    const T diff = a > b ? a - b : b - a;
    if (diff <= epsilon) {
        report(true, expr, file, line, {});
        return;
    }
    report(false, expr, file, line,
           std::format("{}  ({} vs {} , tolerance {})", expr, debug_string(a), debug_string(b), debug_string(epsilon)));
}

} // namespace ore::test

#define ORE_TEST(name)                                                                               \
    static void ore_test_##name();                                                                   \
    static const ::ore::test::Registrar ore_test_registrar_##name(#name, &ore_test_##name);           \
    static void ore_test_##name()

#define ORE_CHECK(expr) ::ore::test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, {})
#define ORE_CHECK_MSG(expr, ...)                                                                     \
    ::ore::test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, ::std::format(__VA_ARGS__))
#define ORE_CHECK_EQ(a, b) ::ore::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define ORE_CHECK_NE(a, b) ::ore::test::check_ne((a), (b), #a, #b, __FILE__, __LINE__)
#define ORE_CHECK_NEAR(a, b, eps) ::ore::test::check_near((a), (b), (eps), #a " ~= " #b, __FILE__, __LINE__)
#define ORE_CHECK_FALSE(expr) ::ore::test::report(!static_cast<bool>(expr), "!(" #expr ")", __FILE__, __LINE__, {})

/// Marks the test as skipped; reported as SKIP and counted as a non-failure.
#define ORE_SKIP(reason) ::ore::test::skip(reason)

/// Aborts the current test on failure (useful before dereferencing a value).
#define ORE_REQUIRE(expr)                                                                            \
    do {                                                                                             \
        if (!(expr)) {                                                                               \
            ::ore::test::report(false, #expr " (required)", __FILE__, __LINE__, {});                  \
            throw ::ore::test::SkipException{"requirement failed: " #expr};                           \
        }                                                                                            \
    } while (false)

#define ORE_TEST_MAIN                                                                                \
    int main(int argc, char** argv) { return ::ore::test::run_all(argc, argv); }
