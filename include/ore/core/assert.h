// Ore framework - assertions and panic helpers.
#pragma once

#include <ore/core/log.h>
#include <ore/core/types.h>

#include <format>
#include <source_location>

namespace ore {

/// Called by ORE_ASSERT when the condition fails. Never returns.
[[noreturn]] void assert_failed(std::string_view expression, std::string_view message,
                                const std::source_location& loc);

inline void assert_check(bool condition, std::string_view expression, std::string_view message,
                         const std::source_location& loc) {
    if (!condition) [[unlikely]] {
        assert_failed(expression, message, loc);
    }
}

} // namespace ore

/// Always-on contract check: the expression is evaluated in every build configuration.
#define ORE_ASSERT(expr) ::ore::assert_check(static_cast<bool>(expr), #expr, {}, std::source_location::current())
#define ORE_ASSERT_MSG(expr, ...)                                                                    \
    ::ore::assert_check(static_cast<bool>(expr), #expr, ::std::format(__VA_ARGS__),                  \
                        std::source_location::current())

/// Debug-only check: compiled out when NDEBUG is defined.
#if defined(NDEBUG)
#define ORE_DEBUG_ASSERT(expr) ((void)0)
#define ORE_DEBUG_ASSERT_MSG(expr, ...) ((void)0)
#else
#define ORE_DEBUG_ASSERT(expr) ORE_ASSERT(expr)
#define ORE_DEBUG_ASSERT_MSG(expr, ...) ORE_ASSERT_MSG(expr, __VA_ARGS__)
#endif

/// Evaluates p expr and aborts on failure - for API calls that must not fail.
#define ORE_VERIFY(expr)                                                                             \
    do {                                                                                             \
        if (!(expr)) [[unlikely]] {                                                                  \
            ::ore::assert_failed(#expr, "ORE_VERIFY failed", std::source_location::current());        \
        }                                                                                            \
    } while (false)

#define ORE_NOT_IMPLEMENTED()                                                                        \
    ::ore::log_panic("not implemented", std::source_location::current())

#if defined(_MSC_VER)
#define ORE_UNREACHABLE() __assume(false)
#else
#define ORE_UNREACHABLE() __builtin_unreachable()
#endif
