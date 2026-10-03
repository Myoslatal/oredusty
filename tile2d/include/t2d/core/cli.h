// Tile2D - tiny command line parser for the graphics-free binaries (server, bot, tests).
//
// The playable client uses the richer parser of the rendering framework instead; these two tools
// must build and run without linking any of it.
#pragma once

#include <t2d/core/types.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace t2d {

class Args {
public:
    Args(int argc, char** argv) {
        if (argc > 0 && argv[0] != nullptr) program_ = argv[0];
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
            if (arg.size() < 2 || arg[0] != '-') {
                positional_.emplace_back(arg);
                continue;
            }
            const std::string_view body = arg.substr(arg[1] == '-' ? 2 : 1);
            const usize equals = body.find('=');
            if (equals != std::string_view::npos) {
                options_.emplace_back(std::string(body.substr(0, equals)), std::string(body.substr(equals + 1)));
            } else if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-') {
                options_.emplace_back(std::string(body), std::string(argv[++i]));
            } else {
                options_.emplace_back(std::string(body), std::string{});
            }
        }
        if (program_.find('/') != std::string::npos) program_ = program_.substr(program_.find_last_of('/') + 1);
    }

    [[nodiscard]] bool has(std::string_view name) const { return find(name) != nullptr; }
    [[nodiscard]] std::string value(std::string_view name, std::string fallback = {}) const {
        const auto* found = find(name);
        return found != nullptr && !found->second.empty() ? found->second : std::move(fallback);
    }
    [[nodiscard]] u32 uint_value(std::string_view name, u32 fallback = 0) const {
        const auto* found = find(name);
        if (found == nullptr || found->second.empty()) return fallback;
        return static_cast<u32>(std::strtoul(found->second.c_str(), nullptr, 10));
    }
    [[nodiscard]] const std::string& program() const { return program_; }
    [[nodiscard]] ConstSpan<std::string> positional() const { return positional_; }

private:
    [[nodiscard]] const std::pair<std::string, std::string>* find(std::string_view name) const {
        for (const auto& option : options_) {
            if (option.first == name) return &option;
        }
        return nullptr;
    }

    std::vector<std::pair<std::string, std::string>> options_;
    std::vector<std::string> positional_;
    std::string program_ = "tile2d";
};

/// Reads a whole text file without pulling in any framework code.
[[nodiscard]] inline std::optional<std::string> read_text(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return std::nullopt;
    std::string content;
    char buffer[4096];
    usize read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) content.append(buffer, read);
    std::fclose(file);
    return content;
}

} // namespace t2d
