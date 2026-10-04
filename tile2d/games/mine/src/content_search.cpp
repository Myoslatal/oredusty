#include <mine/content_search.h>

#include <t2d/core/executable.h>

#include <algorithm>
#include <filesystem>

namespace mine {
namespace {

namespace fs = std::filesystem;

} // namespace

std::string packs_beside(const std::string& executable_path) {
    const std::string directory = t2d::parent_directory_of(executable_path);
    if (directory.empty()) return {};
    return (fs::path(directory) / kPacksDirectoryName).string();
}

std::string content_beside(const std::string& executable_path) {
    const std::string directory = t2d::parent_directory_of(executable_path);
    if (directory.empty()) return {};
    return (fs::path(directory) / kContentDirectoryName).string();
}

std::vector<std::string> default_content_directories(const std::string& executable_path,
                                                     const std::string& working_directory) {
    std::vector<std::string> directories;
    // What each candidate really is, so two spellings of one directory (a relative "packs" and the
    // absolute path the game computes) are looked at once. Kept beside the list rather than resolved
    // into it, because the readable spelling is what a content list should show.
    std::vector<std::string> real;
    const auto add = [&](std::string candidate) {
        if (candidate.empty()) return;
        std::error_code code;
        if (!fs::is_directory(candidate, code)) return;
        const fs::path resolved = fs::weakly_canonical(candidate, code);
        const std::string key = code ? candidate : resolved.string();
        if (std::find(real.begin(), real.end(), key) != real.end()) return;
        real.push_back(key);
        directories.push_back(std::move(candidate));
    };
    add(packs_beside(executable_path));
    if (!working_directory.empty()) add((fs::path(working_directory) / kPacksDirectoryName).string());
    return directories;
}

} // namespace mine
