// Ore framework - very small command line parser used by the applications and tests.
//
//   --headless --frames 240 --size 1280x720 --screenshot out.png --vsync=0 -v
#pragma once

#include <ore/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ore {

struct CliOption {
    std::string_view name;       ///< long name without leading dashes, e.g. "frames"
    std::string_view value_hint; ///< e.g. "<count>", empty for flags
    std::string_view description;
};

class CommandLine {
public:
    static CommandLine parse(int argc, char** argv);

    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> value(std::string_view name) const;
    /// All values given for a repeated option.
    [[nodiscard]] std::vector<std::string> values(std::string_view name) const;

    [[nodiscard]] std::optional<i64> int_value(std::string_view name) const;
    [[nodiscard]] std::optional<u32> uint_value(std::string_view name) const;
    [[nodiscard]] std::optional<f64> float_value(std::string_view name) const;
    /// Accepts true/false, 1/0, yes/no, on/off (case insensitive).
    [[nodiscard]] std::optional<bool> bool_value(std::string_view name) const;

    [[nodiscard]] ConstSpan<std::string> positional() const { return positional_; }
    [[nodiscard]] const std::string& program_name() const { return program_; }

    [[nodiscard]] std::string help(std::string_view usage_line, ConstSpan<CliOption> options) const;

private:
    std::vector<std::pair<std::string, std::string>> options_;
    std::vector<std::string> positional_;
    std::string program_;
};

} // namespace ore
