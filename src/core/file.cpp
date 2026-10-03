#include <ore/core/file.h>

#include <ore/core/log.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ore {
namespace {

[[nodiscard]] fs::path to_path(std::string_view view) { return fs::path(std::string(view)); }

} // namespace

std::optional<std::vector<u8>> read_binary_file(std::string_view path) {
    std::ifstream stream(to_path(path), std::ios::binary | std::ios::ate);
    if (!stream) return std::nullopt;
    const std::streamoff size = stream.tellg();
    if (size < 0) return std::nullopt;
    std::vector<u8> data(static_cast<usize>(size));
    stream.seekg(0, std::ios::beg);
    if (size > 0 && !stream.read(reinterpret_cast<char*>(data.data()), size)) return std::nullopt;
    return data;
}

std::optional<std::string> read_text_file(std::string_view path) {
    auto bytes = read_binary_file(path);
    if (!bytes) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

bool write_binary_file(std::string_view path, ConstSpan<u8> data) {
    const fs::path target = to_path(path);
    if (target.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
    }
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        ORE_ERROR("cannot open '{}' for writing", target.string());
        return false;
    }
    if (!data.empty()) stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return stream.good();
}

bool write_text_file(std::string_view path, std::string_view text) {
    return write_binary_file(path, ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

bool path_exists(std::string_view path) {
    std::error_code ec;
    return fs::exists(to_path(path), ec);
}

bool is_directory(std::string_view path) {
    std::error_code ec;
    return fs::is_directory(to_path(path), ec);
}

std::optional<u64> file_mtime_nanos(std::string_view path) {
    std::error_code ec;
    const auto time = fs::last_write_time(to_path(path), ec);
    if (ec) return std::nullopt;
    return static_cast<u64>(time.time_since_epoch().count());
}

std::string path_join(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    std::string result(a);
    if (result.back() != '/' && result.back() != '\\') result.push_back('/');
    result.append(b);
    return result;
}

std::string parent_path(std::string_view path) { return to_path(path).parent_path().string(); }
std::string file_name(std::string_view path) { return to_path(path).filename().string(); }
std::string file_stem(std::string_view path) { return to_path(path).stem().string(); }
std::string file_extension(std::string_view path) { return to_path(path).extension().string(); }
std::string normalize_path(std::string_view path) { return to_path(path).lexically_normal().string(); }

bool create_directories(std::string_view path) {
    std::error_code ec;
    fs::create_directories(to_path(path), ec);
    return !ec;
}

std::string find_data_file(std::string_view relative) {
    const std::string exe = executable_dir();
    const std::string candidates[] = {
        std::string(relative),
        path_join(exe, relative),
        path_join(parent_path(exe), relative),
        path_join(parent_path(parent_path(exe)), relative),
        path_join(current_dir(), relative),
        path_join(ORE_SOURCE_DIR, relative),
    };
    for (const std::string& candidate : candidates) {
        if (path_exists(candidate)) return candidate;
    }
    return std::string(relative);
}

std::string executable_dir() {
#if ORE_PLATFORM_LINUX
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return exe.parent_path().string();
#endif
    return current_dir();
}

std::string current_dir() {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    return ec ? std::string(".") : cwd.string();
}

std::string source_dir() { return ORE_SOURCE_DIR; }

std::string builtin_shader_dir() { return ORE_SOURCE_SHADER_DIR; }

} // namespace ore
