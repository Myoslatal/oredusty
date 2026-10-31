#include <mefemma/config.h>

#include <t2d/text/locale.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
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

} // namespace

std::string config_path(std::string_view save_directory) {
    if (save_directory.empty()) return "config.ecfg";
    return (fs::path(std::string(save_directory)) / "config.ecfg").string();
}

std::optional<GameConfig> load_game_config(const std::string& path, t2d::EcfgError* error) {
    t2d::EcfgError parse_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &parse_error);
    if (!document.has_value()) {
        if (error != nullptr) *error = parse_error;
        return std::nullopt;
    }

    GameConfig config;
    const t2d::EcfgValue* language = document->root().find("language");
    if (language == nullptr) return config;   // a valid file that predates this field, or has no settings yet

    if (!language->is_string()) {
        fail_ecfg(error, "config: language must be a quoted language code (en, zh-Hans or zh-Hant)");
        return std::nullopt;
    }
    const std::string_view code = language->as_string();
    const std::optional<t2d::Language> parsed = t2d::parse_language(code);
    if (!parsed.has_value()) {
        fail_ecfg(error, std::format("config: '{}' is not a language (try en, zh-Hans or zh-Hant)", code));
        return std::nullopt;
    }
    config.language = *parsed;
    return config;
}

bool save_game_config(const std::string& path, t2d::Language language, std::string* error) {
    const fs::path target(path);
    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code code;
        fs::create_directories(parent, code);
        if (code) {
            fail(error, std::format("config: cannot create '{}': {}", parent.string(), code.message()));
            return false;
        }
    }

    const fs::path temporary = target.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail(error, std::format("config: cannot write '{}'", temporary.string()));
            return false;
        }
        out << "# Mefemma's settings for this save directory. Written by the Settings menu.\n";
        out << "language:\"" << t2d::language_code(language) << "\"\n";
        out.flush();
        if (!out) {
            out.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(error, std::format("config: writing '{}' failed", temporary.string()));
            return false;
        }
    }

    std::error_code code;
    fs::rename(temporary, target, code);
    if (code) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        fail(error, std::format("config: cannot replace '{}': {}", target.string(), code.message()));
        return false;
    }
    return true;
}

} // namespace mefemma
