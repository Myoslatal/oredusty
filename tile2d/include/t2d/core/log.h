// Tile2D - logging with per-thread tags (server thread / client thread / net).
#pragma once

#include <t2d/core/types.h>

#include <format>
#include <source_location>
#include <string_view>

namespace t2d {

enum class LogLevel : u8 { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

void set_log_level(LogLevel level);
[[nodiscard]] LogLevel log_level();
[[nodiscard]] bool log_enabled(LogLevel level) noexcept;

/// Names the calling thread; every line it logs is tagged with it ("server", "client", ...).
void set_thread_name(std::string_view name);
[[nodiscard]] std::string_view thread_name();

bool set_log_file(std::string_view path);
void flush_log();

void log_write(LogLevel level, std::string_view message, const std::source_location& loc);
[[noreturn]] void log_panic(std::string_view message, const std::source_location& loc);

namespace log {

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (!log_enabled(LogLevel::Trace)) return;
    log_write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (!log_enabled(LogLevel::Debug)) return;
    log_write(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (!log_enabled(LogLevel::Info)) return;
    log_write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (!log_enabled(LogLevel::Warn)) return;
    log_write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (!log_enabled(LogLevel::Error)) return;
    log_write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}
template <class... Args>
[[noreturn]] void fatal(std::format_string<Args...> fmt, Args&&... args) {
    log_panic(std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}

} // namespace log
} // namespace t2d

#define T2D_TRACE(...) ::t2d::log::trace(__VA_ARGS__)
#define T2D_DEBUG(...) ::t2d::log::debug(__VA_ARGS__)
#define T2D_INFO(...) ::t2d::log::info(__VA_ARGS__)
#define T2D_WARN(...) ::t2d::log::warn(__VA_ARGS__)
#define T2D_ERROR(...) ::t2d::log::error(__VA_ARGS__)
#define T2D_FATAL(...) ::t2d::log::fatal(__VA_ARGS__)

/// Always-on contract check.
#define T2D_ASSERT(expr)                                                                             \
    do {                                                                                             \
        if (!(expr)) [[unlikely]] {                                                                  \
            ::t2d::log_panic("assertion failed: " #expr, std::source_location::current());           \
        }                                                                                            \
    } while (false)

#define T2D_VERIFY(expr)                                                                             \
    do {                                                                                             \
        if (!(expr)) [[unlikely]] {                                                                  \
            ::t2d::log_panic("T2D_VERIFY failed: " #expr, std::source_location::current());           \
        }                                                                                            \
    } while (false)
