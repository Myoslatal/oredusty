// Ore framework - small filesystem helpers used by the runtime and the tools.
#pragma once

#include <ore/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ore {

[[nodiscard]] std::optional<std::vector<u8>> read_binary_file(std::string_view path);
[[nodiscard]] std::optional<std::string> read_text_file(std::string_view path);
bool write_binary_file(std::string_view path, ConstSpan<u8> data);
bool write_text_file(std::string_view path, std::string_view text);

[[nodiscard]] bool path_exists(std::string_view path);
[[nodiscard]] bool is_directory(std::string_view path);
/// Modification time in nanoseconds since the epoch, or nullopt when the file does not exist.
[[nodiscard]] std::optional<u64> file_mtime_nanos(std::string_view path);

[[nodiscard]] std::string path_join(std::string_view a, std::string_view b);
[[nodiscard]] std::string parent_path(std::string_view path);
[[nodiscard]] std::string file_name(std::string_view path);
[[nodiscard]] std::string file_stem(std::string_view path);
[[nodiscard]] std::string file_extension(std::string_view path);
[[nodiscard]] std::string normalize_path(std::string_view path);
/// Creates every missing directory in p path. Returns true when the directory exists afterwards.
bool create_directories(std::string_view path);

/// Resolves a data file (textures, models, configs) relative to the current directory, the
/// executable directory, its parent directories and finally the source tree. Returns \p relative
/// unchanged when nothing matches, so the caller's error message stays meaningful.
[[nodiscard]] std::string find_data_file(std::string_view relative);

/// Directory containing the running executable (no trailing separator).
[[nodiscard]] std::string executable_dir();
[[nodiscard]] std::string current_dir();
/// Directory of the source tree this build was configured from (compile time constant).
[[nodiscard]] std::string source_dir();
/// Directory of the repository-level shaders/ folder of this build.
[[nodiscard]] std::string builtin_shader_dir();

} // namespace ore
