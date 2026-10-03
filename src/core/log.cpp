#include <ore/core/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if ORE_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace ore {
namespace {

std::atomic<u8> g_level{static_cast<u8>(ORE_DEBUG_BUILD ? LogLevel::Trace : LogLevel::Info)};
std::mutex g_mutex;
std::FILE* g_file = nullptr;
int g_tty = -1;

constexpr std::string_view kLevelNames[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
constexpr std::string_view kLevelColors[] = {"\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[97;41m"};

[[nodiscard]] bool stderr_is_tty() {
    if (g_tty < 0) {
#if ORE_PLATFORM_LINUX
        g_tty = ::isatty(fileno(stderr)) ? 1 : 0;
#else
        g_tty = 0;
#endif
    }
    return g_tty == 1;
}

[[nodiscard]] std::string_view basename_of(std::string_view path) {
    const usize pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

[[nodiscard]] std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto ms = duration_cast<milliseconds>(now - secs).count();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if ORE_PLATFORM_WINDOWS
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms));
    return buffer;
}

} // namespace

bool log_enabled(LogLevel level) noexcept {
    return static_cast<u8>(level) >= g_level.load(std::memory_order_relaxed);
}

void set_log_level(LogLevel level) { g_level.store(static_cast<u8>(level), std::memory_order_relaxed); }

LogLevel log_level() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

bool set_log_file(std::string_view path) {
    std::scoped_lock lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    if (path.empty()) return true;
    const std::string owned(path);
    g_file = std::fopen(owned.c_str(), "a");
    return g_file != nullptr;
}

void flush_log() {
    std::scoped_lock lock(g_mutex);
    std::fflush(stderr);
    if (g_file != nullptr) std::fflush(g_file);
}

void log_write(LogLevel level, std::string_view message, const std::source_location& loc) {
    if (!log_enabled(level)) return;
    const auto index = static_cast<usize>(level);
    const bool color = stderr_is_tty();
    const std::string_view name = index < std::size(kLevelNames) ? kLevelNames[index] : "?????";
    const std::string_view tint = index < std::size(kLevelColors) ? kLevelColors[index] : "";

    std::string line;
    line.reserve(message.size() + 96);
    std::format_to(std::back_inserter(line), "{}[{}] [{}] ", color ? tint : "", timestamp(), name);
    if (level >= LogLevel::Warn) {
        std::format_to(std::back_inserter(line), "{}:{}: ", basename_of(loc.file_name()), loc.line());
    }
    line.append(message);
    if (color) line.append("\x1b[0m");

    std::scoped_lock lock(g_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    if (g_file != nullptr) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
}

void log_panic(std::string_view message, const std::source_location& loc) {
    log_write(LogLevel::Fatal, message, loc);
    flush_log();
    std::abort();
}

void assert_failed(std::string_view expression, std::string_view message, const std::source_location& loc) {
    std::string text;
    std::format_to(std::back_inserter(text), "assertion failed: {}", expression);
    if (!message.empty()) {
        std::format_to(std::back_inserter(text), " ({})", message);
    }
    log_panic(text, loc);
}

} // namespace ore
