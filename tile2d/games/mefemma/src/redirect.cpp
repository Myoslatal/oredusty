#include <mefemma/redirect.h>

#include <t2d/core/executable.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mefemma {
namespace {

namespace fs = std::filesystem;

void fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

void fail_ecfg(t2d::EcfgError* error, std::string message) {
    if (error != nullptr) *error = t2d::EcfgError{0, 0, std::move(message)};
}

[[nodiscard]] bool is_ascii_space(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f' || character == '\v';
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) {
    while (!text.empty() && is_ascii_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_ascii_space(text.back())) text.remove_suffix(1);
    return text;
}

/// A control byte inside a path: a newline, a tab or NUL would make the value ambiguous in the .ecfg file
/// or in a shell, and no save DIRECTORY needs one. Multibyte UTF-8 bytes are all >= 0x80 and pass.
[[nodiscard]] bool has_control_character(std::string_view text) {
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20u || byte == 0x7Fu) return true;
    }
    return false;
}

} // namespace

std::optional<RedirectConfig> load_redirect(const std::string& path, t2d::EcfgError* error) {
    t2d::EcfgError parse_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &parse_error);
    if (!document.has_value()) {
        // A missing file is the "first run" answer as much as a parse failure is, and both carry a reason
        // for the caller that wants to log why it asked the player.
        if (error != nullptr) *error = parse_error;
        return std::nullopt;
    }

    const t2d::EcfgValue* value = document->root().find("save_path");
    if (value == nullptr) {
        fail_ecfg(error, "redirect: save_path is missing");
        return std::nullopt;
    }
    if (!value->is_string()) {
        fail_ecfg(error, "redirect: save_path must be a quoted string");
        return std::nullopt;
    }
    const std::string_view save_path = value->as_string();
    if (save_path.empty()) {
        fail_ecfg(error, "redirect: save_path is empty");
        return std::nullopt;
    }
    RedirectConfig config;
    config.save_path = std::string(save_path);
    return config;
}

bool save_redirect(const std::string& path, std::string_view save_path, std::string* error) {
    const fs::path target(path);
    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code code;
        fs::create_directories(parent, code);
        if (code) {
            fail(error, std::format("redirect: cannot create '{}': {}", parent.string(), code.message()));
            return false;
        }
    }

    // The temporary lives beside the target, so the rename below stays on one file system and is atomic on
    // POSIX: a crash leaves the old redirect.ecfg in place rather than a half-written new one.
    const fs::path temporary = target.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail(error, std::format("redirect: cannot write '{}'", temporary.string()));
            return false;
        }
        out << "# Mefemma's redirect file: the save directory chosen on the first run.\n";
        out << "# Deleting it makes the next run ask again.\n";
        out << "save_path:\"";
        for (const char character : save_path) {
            switch (character) {
                case '\\': out << "\\\\"; break;
                case '"': out << "\\\""; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                case '\0': out << "\\0"; break;
                default: out << character; break;
            }
        }
        out << "\"\n";
        out.flush();
        if (!out) {
            out.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(error, std::format("redirect: writing '{}' failed", temporary.string()));
            return false;
        }
    }

    std::error_code code;
    fs::rename(temporary, target, code);
    if (code) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        fail(error, std::format("redirect: cannot replace '{}': {}", target.string(), code.message()));
        return false;
    }
    return true;
}

std::string default_redirect_path() {
    const std::string directory = t2d::parent_directory_of(t2d::executable_path());
    if (directory.empty()) return "redirect.ecfg";
    return (fs::path(directory) / "redirect.ecfg").string();
}

std::string default_save_directory(std::string_view redirect_path) {
    const std::string directory = t2d::parent_directory_of(redirect_path);
    if (directory.empty()) return "saves";
    return (fs::path(directory) / "saves").string();
}

std::optional<std::string> normalize_save_path(std::string_view text, std::string_view base_directory,
                                               std::string* error) {
    const std::string_view trimmed = trim_ascii(text);
    if (trimmed.empty()) {
        fail(error, "save path is empty");
        return std::nullopt;
    }
    if (has_control_character(trimmed)) {
        fail(error, "save path contains a control character (tabs, newlines and NUL are not allowed)");
        return std::nullopt;
    }

    fs::path path;
    if (trimmed == "~" || trimmed.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') {
            fail(error, "save path starts with ~ but HOME is not set");
            return std::nullopt;
        }
        path = fs::path(home);
        if (trimmed.size() > 1) path /= std::string(trimmed.substr(2));
    } else {
        path = fs::path(std::string(trimmed));
    }

    if (!path.is_absolute()) {
        fs::path base{std::string(base_directory)};
        if (base.empty()) {
            std::error_code code;
            base = fs::current_path(code);
            if (code) {
                fail(error, std::format("save path: no base directory ('{}')", code.message()));
                return std::nullopt;
            }
        }
        path = base / path;
    }
    path = path.lexically_normal();
    if (!path.is_absolute()) {
        // A relative base_directory is a caller error, but answering with a relative "absolute" path would
        // be worse: resolve the result against the working directory and stay absolute.
        std::error_code code;
        const fs::path current = fs::current_path(code);
        if (code) {
            fail(error, std::format("save path: cannot make '{}' absolute ('{}')", trimmed, code.message()));
            return std::nullopt;
        }
        path = (current / path).lexically_normal();
    }

    std::string result = path.string();
    if (result.empty()) {
        fail(error, "save path is empty after normalisation");
        return std::nullopt;
    }
    return result;
}

bool ensure_save_directory(const std::string& directory, std::string* error) {
    if (directory.empty()) {
        fail(error, "save directory is empty");
        return false;
    }

    std::error_code code;
    if (fs::is_regular_file(directory, code)) {
        fail(error, std::format("save path '{}' is a file, not a directory", directory));
        return false;
    }
    code.clear();
    fs::create_directories(directory, code);
    if (code) {
        fail(error, std::format("cannot create save directory '{}': {}", directory, code.message()));
        return false;
    }
    std::error_code kind;
    if (!fs::is_directory(directory, kind)) {
        fail(error, std::format("save path '{}' is not a directory", directory));
        return false;
    }
    return true;
}

} // namespace mefemma
