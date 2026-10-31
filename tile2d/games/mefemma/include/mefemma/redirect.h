// Mefemma - redirect.ecfg: where the game keeps its saves, and where that file lives.
//
// The first time the game runs it asks the player for a save DIRECTORY and records it in a small file
// beside the executable:
//
//     save_path:"/absolute/path/to/saves"
//
// This header is that file's reader/writer and the path rules around it. It is deliberately free of any
// UI, startup order or settings-file code: the first-run screen, the command line and the moment the file
// is consulted are the shell's (they attach in a later step), and this module stays testable without a
// window.
//
// A relative value is resolved against the redirect file's own directory, `~` expands from HOME, and what
// is written back is always absolute and lexically normal. A missing or invalid file means "not configured
// yet"; a valid one always means "configured", so deleting redirect.ecfg is how a player asks to choose
// again.
#pragma once

#include <t2d/core/ecfg.h>

#include <optional>
#include <string>
#include <string_view>

namespace mefemma {

/// The one field redirect.ecfg holds.
struct RedirectConfig {
    /// The absolute save directory. load_redirect() returns the stored value as written (a file written by
    /// save_redirect() is already absolute and normal); use normalize_save_path() for freshly typed text.
    std::string save_path;
};

/// Reads redirect.ecfg at \p path.
///
/// nullopt with a reason in \p error for: a missing or unreadable file, a parse error, a missing
/// `save_path`, a value that is not a quoted string, and an empty string. Unknown fields are ignored - a
/// newer game may have put more in the file, and an older one must not lose its save directory over it.
[[nodiscard]] std::optional<RedirectConfig> load_redirect(const std::string& path,
                                                          t2d::EcfgError* error = nullptr);

/// Writes redirect.ecfg at \p path with the quoted `save_path` field and a short comment.
///
/// Creates the file's parent directory when needed, writes a temporary file in that same directory and
/// renames it over the target, so a crash mid-write leaves the old file (or no file) rather than half of a
/// new one. Backslashes, quotes, newlines, tabs, carriage returns and NUL are escaped for the .ecfg reader.
/// The value is not normalised here: a caller that accepted typed text runs normalize_save_path() first.
[[nodiscard]] bool save_redirect(const std::string& path, std::string_view save_path,
                                 std::string* error = nullptr);

/// The redirect file's default place: `<directory of the running executable>/redirect.ecfg`. When the
/// platform will not name the executable this falls back to a relative `redirect.ecfg`, the best answer
/// available; a caller that needs certainty checks the result before writing it.
[[nodiscard]] std::string default_redirect_path();

/// The default save directory for \p redirect_path: `<its directory>/saves`. The first-run prompt offers
/// it, so accepting the default needs no typing and lands next to the game.
[[nodiscard]] std::string default_save_directory(std::string_view redirect_path);

/// Turns text a player typed into an absolute, lexically normal save directory (or nullopt + a reason):
///
///   * surrounding ASCII whitespace is trimmed;
///   * `~` and `~/...` expand from HOME - nullopt when HOME is unset, rather than a literal tilde folder;
///   * a relative path is resolved against \p base_directory;
///   * empty input, control characters (tabs and newlines included) and NUL are refused.
///
/// The directory is not created and does not have to exist: ensure_save_directory() is that step, and a
/// save path is allowed to name a directory the game will create on first save.
[[nodiscard]] std::optional<std::string> normalize_save_path(std::string_view text,
                                                              std::string_view base_directory,
                                                              std::string* error = nullptr);

/// Creates \p directory and its parents when needed. An existing directory is success; an existing regular
/// file at the path is an error, as is a failure to create. Success iff the path is a directory afterwards,
/// with a readable reason in \p error otherwise.
[[nodiscard]] bool ensure_save_directory(const std::string& directory, std::string* error = nullptr);

} // namespace mefemma
