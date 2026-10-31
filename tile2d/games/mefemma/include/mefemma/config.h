// Mefemma - the settings one save directory remembers: <save directory>/config.ecfg.
//
// The Settings menu's language choice is a property of the save directory, not of the packaged game: two
// save directories can use two languages, and a package's assets directory may be read-only. The file lives
// next to the saves it configures:
//
//     # Mefemma's settings for this save directory. Written by the Settings menu.
//     language:"zh-Hans"
//
// This header is only the reader/writer and the path rule. Loading it at startup and writing it from the
// Settings menu are the shell's job (a later step); keeping it here makes the format testable without a
// window and keeps the app from growing a second .ecfg reader.
#pragma once

#include <t2d/core/ecfg.h>
#include <t2d/text/locale.h>

#include <optional>
#include <string>
#include <string_view>

namespace mefemma {

/// What a save directory's config.ecfg holds. A missing field is a valid default, so a file written by a
/// newer game remains readable by an older one.
struct GameConfig {
    /// The interface language chosen for this save directory; empty means "not chosen here" and the shell
    /// falls back to the startup language.
    std::optional<t2d::Language> language;
};

/// <save_directory>/config.ecfg. An empty save directory yields the relative "config.ecfg" (the platform's
/// working directory), not "/config.ecfg"; normal callers pass the absolute save directory from redirect.ecfg
/// or --save-path.
[[nodiscard]] std::string config_path(std::string_view save_directory);

/// Reads config.ecfg at \p path.
///
/// nullopt with a reason for a missing/unreadable file, a parse error, a `language` that is not a quoted
/// string, and an unknown language code. A valid file with no `language` field is a valid empty GameConfig
/// (language == nullopt), and unknown fields are ignored: the file may grow.
[[nodiscard]] std::optional<GameConfig> load_game_config(const std::string& path,
                                                         t2d::EcfgError* error = nullptr);

/// Writes config.ecfg at \p path with the quoted language code. Creates the parent directory when needed,
/// writes a temporary file in that directory and renames it over the target (atomic on POSIX; the temp is
/// removed on failure). The language code is t2d::language_code(language).
[[nodiscard]] bool save_game_config(const std::string& path, t2d::Language language,
                                    std::string* error = nullptr);

} // namespace mefemma
