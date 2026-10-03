#include <t2d/core/log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

#if T2D_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace t2d {
namespace {

std::atomic<u8> g_level{static_cast<u8>(T2D_DEBUG_BUILD ? LogLevel::Debug : LogLevel::Info)};
std::mutex g_mutex;
std::FILE* g_file = nullptr;
int g_tty = -1;
thread_local char g_thread_name[24] = {};

constexpr std::string_view kNames[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
constexpr std::string_view kColors[] = {"\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[97;41m"};

[[nodiscard]] bool stderr_is_tty() {
    if (g_tty < 0) {
#if T2D_PLATFORM_LINUX
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

} // namespace

bool log_enabled(LogLevel level) noexcept { return static_cast<u8>(level) >= g_level.load(std::memory_order_relaxed); }
void set_log_level(LogLevel level) { g_level.store(static_cast<u8>(level), std::memory_order_relaxed); }
LogLevel log_level() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

void set_thread_name(std::string_view name) {
    const usize count = name.size() < sizeof(g_thread_name) - 1 ? name.size() : sizeof(g_thread_name) - 1;
    for (usize i = 0; i < count; ++i) g_thread_name[i] = name[i];
    g_thread_name[count] = '\0';
}

std::string_view thread_name() { return std::string_view(g_thread_name); }

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
    const std::string_view name = index < std::size(kNames) ? kNames[index] : "?????";
    const std::string_view tint = index < std::size(kColors) ? kColors[index] : "";

    std::string line;
    line.reserve(message.size() + 96);
    std::format_to(std::back_inserter(line), "{}[{}][{}] ", color ? tint : "", thread_name(), name);
    if (level >= LogLevel::Warn) std::format_to(std::back_inserter(line), "{}:{}: ", basename_of(loc.file_name()), loc.line());
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

} // namespace t2d
