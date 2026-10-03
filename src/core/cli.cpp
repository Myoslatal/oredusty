#include <ore/core/cli.h>

#include <ore/core/log.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <unordered_map>

namespace ore {
namespace {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] std::optional<i64> to_int(std::string_view text) {
    i64 value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<f64> to_float(std::string_view text) {
    try {
        usize consumed = 0;
        const f64 value = std::stod(std::string(text), &consumed);
        if (consumed != text.size()) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

CommandLine CommandLine::parse(int argc, char** argv) {
    CommandLine cli;
    if (argc > 0 && argv[0] != nullptr) {
        cli.program_ = std::string(argv[0]);
        const usize slash = cli.program_.find_last_of("/\\");
        if (slash != std::string::npos) cli.program_ = cli.program_.substr(slash + 1);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
        if (arg.empty()) continue;
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::string_view body = arg.substr(2);
            std::string_view name = body;
            std::string_view value;
            bool has_value = false;
            if (const usize eq = body.find('='); eq != std::string_view::npos) {
                name = body.substr(0, eq);
                value = body.substr(eq + 1);
                has_value = true;
            } else if (i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-') {
                value = argv[i + 1];
                has_value = true;
                ++i;
            }
            cli.options_.emplace_back(std::string(name), has_value ? std::string(value) : std::string());
        } else if (arg.size() >= 1 && arg[0] == '-' && arg.size() > 1) {
            // Short flag, possibly with an attached value: -v, -o file
            std::string_view body = arg.substr(1);
            std::string_view name = body.substr(0, 1);
            std::string_view value = body.substr(1);
            bool has_value = !value.empty();
            if (!has_value && i + 1 < argc && argv[i + 1] != nullptr && argv[i + 1][0] != '-' &&
                body.size() == 1 && body != "v" && body != "h") {
                value = argv[i + 1];
                has_value = true;
                ++i;
            }
            cli.options_.emplace_back(std::string(name), has_value ? std::string(value) : std::string());
        } else {
            cli.positional_.emplace_back(arg);
        }
    }
    return cli;
}

bool CommandLine::has(std::string_view name) const {
    return std::any_of(options_.begin(), options_.end(),
                       [name](const auto& entry) { return entry.first == name; });
}

std::optional<std::string> CommandLine::value(std::string_view name) const {
    for (auto it = options_.rbegin(); it != options_.rend(); ++it) {
        if (it->first == name && !it->second.empty()) return it->second;
    }
    return std::nullopt;
}

std::vector<std::string> CommandLine::values(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& entry : options_) {
        if (entry.first == name && !entry.second.empty()) out.push_back(entry.second);
    }
    return out;
}

std::optional<i64> CommandLine::int_value(std::string_view name) const {
    const auto text = value(name);
    if (!text) return std::nullopt;
    const auto parsed = to_int(*text);
    if (!parsed) ORE_WARN("command line: '{}' is not an integer", *text);
    return parsed;
}

std::optional<u32> CommandLine::uint_value(std::string_view name) const {
    const auto parsed = int_value(name);
    if (!parsed || *parsed < 0) return std::nullopt;
    return static_cast<u32>(*parsed);
}

std::optional<f64> CommandLine::float_value(std::string_view name) const {
    const auto text = value(name);
    if (!text) return std::nullopt;
    const auto parsed = to_float(*text);
    if (!parsed) ORE_WARN("command line: '{}' is not a number", *text);
    return parsed;
}

std::optional<bool> CommandLine::bool_value(std::string_view name) const {
    if (!has(name)) return std::nullopt;
    const auto text = value(name);
    if (!text) return true; // bare flag
    const std::string normalized = lower(*text);
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") return true;
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") return false;
    ORE_WARN("command line: '{}' is not a boolean", *text);
    return std::nullopt;
}

std::string CommandLine::help(std::string_view usage_line, ConstSpan<CliOption> options) const {
    std::string out;
    std::format_to(std::back_inserter(out), "usage: {} {}\n\noptions:\n", program_, usage_line);
    for (const CliOption& option : options) {
        std::string left;
        std::format_to(std::back_inserter(left), "  --{}", option.name);
        if (!option.value_hint.empty()) std::format_to(std::back_inserter(left), " {}", option.value_hint);
        std::format_to(std::back_inserter(out), "{:<28} {}\n", left, option.description);
    }
    return out;
}

} // namespace ore
