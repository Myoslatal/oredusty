// Ore framework - logging.
//
//   ore::log::info("device: {} ({})", name, api_version);
//   ORE_WARN("swapchain recreate: {}", reason);
#pragma once

#include <ore/core/types.h>

#include <format>
#include <source_location>
#include <string_view>

namespace ore {

enum class LogLevel : u8 { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

/// Minimum level that is actually emitted. Defaults to Info in release, Trace in debug builds.
void set_log_level(LogLevel level);
[[nodiscard]] LogLevel log_level();
[[nodiscard]] bool log_enabled(LogLevel level) noexcept;

/// Mirrors every emitted line into p path (appending). Pass an empty view to stop mirroring.
bool set_log_file(std::string_view path);
void flush_log();

/// Writes a pre-formatted line. Prefer the typed helpers below.
void log_write(LogLevel level, std::string_view message, const std::source_location& loc);

/// Aborts the process after emitting a fatal line.
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

/// Logs and aborts. Use for unrecoverable engine errors.
template <class... Args>
[[noreturn]] void fatal(std::format_string<Args...> fmt, Args&&... args) {
    log_panic(std::format(fmt, std::forward<Args>(args)...), std::source_location::current());
}

} // namespace log
} // namespace ore

#define ORE_TRACE(...) ::ore::log::trace(__VA_ARGS__)
#define ORE_DEBUG(...) ::ore::log::debug(__VA_ARGS__)
#define ORE_INFO(...) ::ore::log::info(__VA_ARGS__)
#define ORE_WARN(...) ::ore::log::warn(__VA_ARGS__)
#define ORE_ERROR(...) ::ore::log::error(__VA_ARGS__)
#define ORE_FATAL(...) ::ore::log::fatal(__VA_ARGS__)
