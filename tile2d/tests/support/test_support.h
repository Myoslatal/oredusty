// Tile2D - micro test harness (same shape as the one the Ore framework uses).
//
//   T2D_TEST(name) { T2D_CHECK(condition); T2D_CHECK_EQ(a, b); }
//   T2D_TEST_MAIN
#pragma once

#include <t2d/core/types.h>

#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>

namespace t2d::test {

using TestFn = void (*)();

struct Registrar {
    Registrar(std::string_view name, TestFn fn);
};

struct SkipException {
    std::string reason;
};

[[noreturn]] void skip(std::string_view reason);
void report(bool passed, std::string_view expression, std::string_view file, i32 line, std::string_view detail);
int run_all(int argc, char** argv);

/// Same definition as std::formattable, usable in C++20 mode.
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
void check_eq(const A& a, const B& b, std::string_view expr_a, std::string_view expr_b, std::string_view file, i32 line) {
    if (a == b) {
        report(true, expr_a, file, line, {});
        return;
    }
    report(false, expr_a, file, line,
           std::format("{} == {}  ({} vs {})", expr_a, expr_b, debug_string(a), debug_string(b)));
}

template <class A, class B>
void check_ne(const A& a, const B& b, std::string_view expr_a, std::string_view expr_b, std::string_view file, i32 line) {
    if (!(a == b)) {
        report(true, expr_a, file, line, {});
        return;
    }
    report(false, expr_a, file, line, std::format("{} != {}  (both {})", expr_a, expr_b, debug_string(a)));
}

template <class T>
void check_near(const T& a, const T& b, T epsilon, std::string_view expr, std::string_view file, i32 line) {
    const T difference = a > b ? a - b : b - a;
    if (difference <= epsilon) {
        report(true, expr, file, line, {});
        return;
    }
    report(false, expr, file, line,
           std::format("{}  ({} vs {}, tolerance {})", expr, debug_string(a), debug_string(b), debug_string(epsilon)));
}

} // namespace t2d::test

#define T2D_TEST(name)                                                                               \
    static void t2d_test_##name();                                                                   \
    static const ::t2d::test::Registrar t2d_test_registrar_##name(#name, &t2d_test_##name);           \
    static void t2d_test_##name()

#define T2D_CHECK(expr) ::t2d::test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, {})
#define T2D_CHECK_MSG(expr, ...)                                                                     \
    ::t2d::test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, ::std::format(__VA_ARGS__))
#define T2D_CHECK_EQ(a, b) ::t2d::test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
#define T2D_CHECK_NE(a, b) ::t2d::test::check_ne((a), (b), #a, #b, __FILE__, __LINE__)
#define T2D_CHECK_NEAR(a, b, eps) ::t2d::test::check_near((a), (b), (eps), #a " ~= " #b, __FILE__, __LINE__)
#define T2D_CHECK_GT(a, b) ::t2d::test::report((a) > (b), #a " > " #b, __FILE__, __LINE__, {})
#define T2D_CHECK_GE(a, b) ::t2d::test::report((a) >= (b), #a " >= " #b, __FILE__, __LINE__, {})
#define T2D_CHECK_LT(a, b) ::t2d::test::report((a) < (b), #a " < " #b, __FILE__, __LINE__, {})
#define T2D_CHECK_FALSE(expr) ::t2d::test::report(!static_cast<bool>(expr), "!(" #expr ")", __FILE__, __LINE__, {})
#define T2D_SKIP(reason) ::t2d::test::skip(reason)
#define T2D_REQUIRE(expr)                                                                            \
    do {                                                                                             \
        if (!(expr)) {                                                                               \
            ::t2d::test::report(false, #expr " (required)", __FILE__, __LINE__, {});                  \
            throw ::t2d::test::SkipException{"requirement failed: " #expr};                           \
        }                                                                                            \
    } while (false)

#define T2D_TEST_MAIN                                                                                \
    int main(int argc, char** argv) { return ::t2d::test::run_all(argc, argv); }
