#include <mefemma/data_paths.h>

#include <t2d/core/executable.h>

#include <filesystem>

namespace mefemma {
namespace {

namespace fs = std::filesystem;

/// The directory of a path, or an empty string when it has none.
[[nodiscard]] std::string directory_of(std::string_view path) {
    const fs::path parent = fs::path(std::string(path)).parent_path();
    return parent.empty() ? std::string{} : parent.string();
}

} // namespace

DataPaths::DataPaths(std::string root) : root_(std::move(root)) {}

DataPaths DataPaths::resolve(std::string_view executable_path, std::string_view source_root) {
    std::error_code code;
    const std::string executable_directory = directory_of(executable_path);

    // Beside the executable: what a package and a build tree with its assets copied both look like.
    if (!executable_directory.empty()) {
        const fs::path beside = fs::path(executable_directory) / "assets";
        if (fs::is_directory(beside, code)) return DataPaths(beside.string());
    }
    // The source tree: a build tree that was never packaged. This is a developer's fallback, not a
    // promise - a package that lost its assets is a package that must not silently read someone's
    // checkout, so the fallback is taken only when the assets really are there.
    if (!source_root.empty()) {
        const fs::path source = fs::path(std::string(source_root)) / "assets";
        if (fs::is_directory(source, code)) return DataPaths(source.string());
    }
    // Neither: report the place a package would have put them, so the error names a real candidate.
    if (!executable_directory.empty()) {
        return DataPaths((fs::path(executable_directory) / "assets").string());
    }
    return DataPaths((fs::path(std::string(source_root)) / "assets").string());
}

std::string DataPaths::file(std::string_view relative) const {
    return (fs::path(root_) / std::string(relative)).string();
}

std::string DataPaths::texture(std::string_view name) const {
    return file((fs::path("textures") / (std::string(name) + ".png")).string());
}

std::string DataPaths::text(std::string_view name) const {
    return file((fs::path("text") / (std::string(name) + ".ecfg")).string());
}

std::string DataPaths::settings() const { return file((fs::path("config") / "game.ecfg").string()); }

std::string DataPaths::font(std::string_view name) const {
    return file((fs::path("fonts") / std::string(name)).string());
}

bool DataPaths::has_file(std::string_view relative) const {
    std::error_code code;
    return fs::is_regular_file(file(relative), code);
}

} // namespace mefemma
