#include <t2d/core/executable.h>

#include <cstdint>
#include <filesystem>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif

namespace t2d {
namespace {

#if !defined(_WIN32)
/// The separator this platform uses. Windows accepts both, so parent_directory_of() treats a
/// backslash as one there and as an ordinary character everywhere else.
constexpr char kSeparator = '/';
#else
constexpr bool is_separator(char character) { return character == '/' || character == '\\'; }
#endif

} // namespace

std::string parent_directory_of(std::string_view path) {
    if (path.empty()) return {};
    // A trailing separator belongs to the directory, not to a name after it: "a/b/" is the directory
    // "a/b", whose parent is "a".
    std::string::size_type end = path.size();
    while (end > 0) {
#if defined(_WIN32)
        if (!is_separator(path[end - 1])) break;
#else
        if (path[end - 1] != kSeparator) break;
#endif
        --end;
    }
    if (end == 0) return {};   // nothing but separators: no directory to speak of
    std::string::size_type cut = end;
    while (cut > 0) {
#if defined(_WIN32)
        const bool separator = is_separator(path[cut - 1]);
#else
        const bool separator = path[cut - 1] == kSeparator;
#endif
        if (separator) break;
        --cut;
    }
    if (cut == 0) return {};              // "name": a file in the working directory
    if (cut == 1) return std::string(1, path[0]);   // "/name": the root
    // The separator itself is not part of the answer, so "/a/b" gives "/a" and not "/a/".
    return std::string(path.substr(0, cut - 1));
}

std::string executable_path() {
#if defined(_WIN32)
    // GetModuleFileNameW reports the size it needed when the buffer was too small, so it is asked
    // again with twice the room until the answer fits.
    std::wstring buffer(260, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::error_code code;
    const std::filesystem::path path(buffer);
    const std::filesystem::path absolute = std::filesystem::absolute(path, code);
    return (code ? path : absolute).string();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // asks for the size it needs
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    const std::string::size_type end = buffer.find('\0');
    buffer.resize(end == std::string::npos ? buffer.size() : end);
    std::error_code code;
    const std::filesystem::path path(buffer);
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, code);
    return (code ? path : canonical).string();
#else
    // Linux: the kernel's own answer, a symlink to the file that was started. It is read with a
    // growing buffer because a path has no length limit the kernel promises.
    std::string buffer(1024, '\0');
    for (;;) {
        const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (written < 0) return {};
        if (static_cast<std::string::size_type>(written) < buffer.size()) {
            buffer.resize(static_cast<std::string::size_type>(written));
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return buffer;
#endif
}

std::string executable_directory() { return parent_directory_of(executable_path()); }

} // namespace t2d
