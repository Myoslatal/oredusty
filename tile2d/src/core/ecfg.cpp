#include <t2d/core/ecfg.h>

#include <t2d/core/cli.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>

namespace t2d {
namespace {

constexpr u32 kTabWidth = 4;

[[nodiscard]] bool is_key_character(char character) {
    const bool alpha = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    const bool digit = character >= '0' && character <= '9';
    return alpha || digit || character == '_' || character == '-' || character == '.';
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto blank = [](char character) { return character == ' ' || character == '\t' || character == '\r'; };
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    return text;
}

/// Columns are counted with tabs expanded, so an error points where the editor's caret would be.
[[nodiscard]] u32 indent_of(std::string_view line, u32& column_out) {
    u32 column = 0;
    for (const char character : line) {
        if (character == ' ') ++column;
        else if (character == '\t') column += kTabWidth;
        else break;
    }
    column_out = column + 1;
    return column;
}

/// Removes a trailing comment. A '#' inside a quoted string is data, not a comment.
[[nodiscard]] std::string_view strip_comment(std::string_view line) {
    bool in_string = false;
    bool escaped = false;
    for (usize index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
            continue;
        }
        if (character == '"') in_string = true;
        else if (character == '#') return line.substr(0, index);
    }
    return line;
}

} // namespace

namespace ecfg_detail {

/// One entry line, already stripped of comments and trailing whitespace.
struct Line {
    u32 number = 0; ///< 1-based
    u32 column = 1; ///< 1-based, tabs expanded
    u32 indent = 0; ///< in columns
    std::string text;
    bool has_block = false; ///< the line opened a "<< ... >>" block
    std::string block;      ///< that block's raw content, verbatim
};

/// Recursive descent over the scanned lines. Indentation decides nesting, so the parser keeps a single
/// cursor into the line list and every routine advances it.
class Parser {
public:
    Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

    void run(EcfgValue& root) {
        root.type_ = EcfgType::Table;
        if (lines_.empty()) return;
        // The first entry decides the root indentation: a file that is indented as a whole still reads
        // the way it looks.
        parse_table(lines_.front().indent, root);
        if (index_ < lines_.size()) {
            fail(lines_[index_].number, lines_[index_].column, "unexpected line");
        }
    }

    [[noreturn]] void fail(u32 line, u32 column, std::string message) {
        throw EcfgError{line, column, std::move(message)};
    }

private:
    void parse_table(u32 indent, EcfgValue& table) {
        while (index_ < lines_.size()) {
            const Line& line = lines_[index_];
            if (line.indent < indent) return; // back to the enclosing table
            if (line.indent > indent) {
                fail(line.number, line.column,
                     std::format("unexpected indentation ({} columns, expected {})", line.indent, indent));
            }

            const usize colon = line.text.find(':');
            if (colon == std::string_view::npos) fail(line.number, line.column, "expected 'key:value'");

            const std::string_view key = trim(std::string_view(line.text).substr(0, colon));
            if (key.empty()) fail(line.number, line.column, "the key is empty");
            for (const char character : key) {
                if (is_key_character(character)) continue;
                fail(line.number, line.column,
                     std::format("'{}' is not allowed in a key (letters, digits, '_', '-' and '.' only)", character));
            }
            if (const EcfgValue* previous = table.find(key); previous != nullptr) {
                fail(line.number, line.column, std::format("'{}' is already defined on line {}", key, previous->line()));
            }

            EcfgValue value;
            const std::string_view rest = trim(std::string_view(line.text).substr(colon + 1));
            const u32 value_column = line.column + static_cast<u32>(colon) + 1;

            if (line.has_block) {
                value.type_ = EcfgType::String;
                value.text_ = line.block;
                ++index_;
            } else if (rest.starts_with(":")) {
                if (!trim(rest.substr(1)).empty()) {
                    fail(line.number, line.column, "a table opener cannot have anything after '::'");
                }
                value.type_ = EcfgType::Table;
                ++index_;
                if (index_ < lines_.size() && lines_[index_].indent > indent) {
                    parse_table(lines_[index_].indent, value);
                }
            } else {
                value = parse_value(rest, line.number, value_column);
                ++index_;
            }

            value.key_ = std::string(key);
            value.line_ = line.number;
            table.children_.push_back(std::move(value));
        }
    }

    EcfgValue parse_value(std::string_view text, u32 line, u32 column) {
        EcfgValue value;
        value.line_ = line;
        if (text.empty()) fail(line, column, "expected a value after ':'");

        if (text.front() == '"') {
            value.type_ = EcfgType::String;
            value.text_ = parse_string(text, line, column);
            return value;
        }
        if (text.front() == '[') {
            // An array may continue on the following lines; they are consumed here, so the cursor moves.
            std::string joined(text);
            while (!brackets_balanced(joined)) {
                if (index_ + 1 >= lines_.size()) fail(line, column, "the array is never closed");
                ++index_;
                joined.push_back(' ');
                joined.append(lines_[index_].text);
            }
            value.type_ = EcfgType::Array;
            parse_array(joined, line, column, value);
            return value;
        }
        if (text == "true" || text == "false") {
            value.type_ = EcfgType::Bool;
            value.bool_ = text == "true";
            return value;
        }
        if (const auto integer = parse_integer(text); integer.has_value()) {
            value.type_ = EcfgType::Int;
            value.int_ = *integer;
            return value;
        }
        if (const auto real = parse_float(text); real.has_value()) {
            value.type_ = EcfgType::Float;
            value.float_ = *real;
            return value;
        }
        fail(line, column,
             std::format("'{}' is not a number, true/false or a quoted string (strings need \"quotes\")", text));
    }

    [[nodiscard]] std::string parse_string(std::string_view text, u32 line, u32 column) {
        std::string result;
        usize index = 1;
        bool closed = false;
        for (; index < text.size(); ++index) {
            const char character = text[index];
            if (character == '"') {
                closed = true;
                break;
            }
            if (character != '\\') {
                result.push_back(character);
                continue;
            }
            ++index;
            if (index >= text.size()) break;
            switch (text[index]) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case 'n': result.push_back('\n'); break;
                case 't': result.push_back('\t'); break;
                case 'r': result.push_back('\r'); break;
                case '0': result.push_back('\0'); break;
                default:
                    fail(line, column, std::format("unknown escape '\\{}' in a string", text[index]));
            }
        }
        if (!closed) fail(line, column, "the string is never closed");
        if (!trim(text.substr(index + 1)).empty()) {
            fail(line, column, "unexpected text after the closing quote");
        }
        return result;
    }

    [[nodiscard]] static bool brackets_balanced(std::string_view text) {
        i32 depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (const char character : text) {
            if (in_string) {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') in_string = false;
                continue;
            }
            if (character == '"') in_string = true;
            else if (character == '[') ++depth;
            else if (character == ']') --depth;
        }
        return depth == 0;
    }

    void parse_array(std::string_view text, u32 line, u32 column, EcfgValue& out) {
        if (text.back() != ']') fail(line, column, "the array is never closed");
        const std::string_view inner = trim(text.substr(1, text.size() - 2));
        if (inner.empty()) return; // an empty array is valid
        usize start = 0;
        i32 depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (usize index = 0; index <= inner.size(); ++index) {
            const bool at_end = index == inner.size();
            const char character = at_end ? ',' : inner[index];
            if (!at_end) {
                if (in_string) {
                    if (escaped) escaped = false;
                    else if (character == '\\') escaped = true;
                    else if (character == '"') in_string = false;
                } else if (character == '"') {
                    in_string = true;
                } else if (character == '[') {
                    ++depth;
                } else if (character == ']') {
                    --depth;
                }
            }
            if (character != ',' || depth != 0 || in_string) continue;

            const std::string_view element = trim(inner.substr(start, index - start));
            const u32 element_column = column + 1 + static_cast<u32>(start);
            if (element.empty()) fail(line, element_column, "empty array element (a trailing comma is not allowed)");
            EcfgValue item = parse_value(element, line, element_column);
            item.key_.clear();
            out.children_.push_back(std::move(item));
            start = index + 1;
        }
    }

    [[nodiscard]] static std::optional<i64> parse_integer(std::string_view text) {
        if (text.empty()) return std::nullopt;
        i64 sign = 1;
        if (text.front() == '+' || text.front() == '-') {
            sign = text.front() == '-' ? -1 : 1;
            text.remove_prefix(1);
        }
        if (text.empty()) return std::nullopt;
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
            text.remove_prefix(2);
            if (text.empty()) return std::nullopt;
        }
        for (const char character : text) {
            const bool digit = character >= '0' && character <= '9';
            const bool hex_digit =
                base == 16 && ((character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F'));
            if (!digit && !hex_digit) return std::nullopt;
        }
        i64 magnitude = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), magnitude, base);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
        return sign * magnitude;
    }

    [[nodiscard]] static std::optional<f64> parse_float(std::string_view text) {
        if (text.empty()) return std::nullopt;
        bool has_digit = false;
        bool has_marker = false;
        for (usize index = 0; index < text.size(); ++index) {
            const char character = text[index];
            if (character >= '0' && character <= '9') {
                has_digit = true;
                continue;
            }
            if ((character == '+' || character == '-') &&
                (index == 0 || text[index - 1] == 'e' || text[index - 1] == 'E')) {
                continue;
            }
            if (character == '.' || character == 'e' || character == 'E') {
                has_marker = true;
                continue;
            }
            return std::nullopt;
        }
        if (!has_digit || !has_marker) return std::nullopt;
        // strtod rather than from_chars: floating point from_chars is not available everywhere yet.
        const std::string owned(text);
        char* end = nullptr;
        const f64 parsed = std::strtod(owned.c_str(), &end);
        if (end != owned.c_str() + owned.size() || !std::isfinite(parsed)) return std::nullopt;
        return parsed;
    }

    std::vector<Line> lines_;
    usize index_ = 0;
};

} // namespace ecfg_detail

namespace {

/// Splits the text into entries, resolving text blocks while scanning (their bodies are raw: comments
/// and blank lines inside them are content).
[[nodiscard]] std::vector<ecfg_detail::Line> scan_lines(std::string_view text, EcfgError& error) {
    using ecfg_detail::Line;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3); // UTF-8 BOM
    std::vector<Line> lines;
    std::string pending;
    Line owner;
    bool in_block = false;
    u32 line_number = 0;
    usize cursor = 0;

    const auto opens_block = [](std::string_view content) {
        const usize colon = content.find(':');
        if (colon == std::string_view::npos) return false;
        return trim(content.substr(colon + 1)) == "<<";
    };

    while (cursor < text.size()) {
        const usize newline = text.find('\n', cursor);
        const std::string_view raw =
            text.substr(cursor, newline == std::string_view::npos ? std::string_view::npos : newline - cursor);
        cursor = newline == std::string_view::npos ? text.size() : newline + 1;
        ++line_number;

        if (in_block) {
            if (trim(raw) == ">>") {
                in_block = false;
                owner.block = std::move(pending);
                pending.clear();
                lines.push_back(std::move(owner));
                continue;
            }
            pending.append(raw);
            pending.push_back('\n');
            continue;
        }

        u32 column = 1;
        const u32 indent = indent_of(raw, column);
        const std::string_view content = trim(strip_comment(raw));
        if (content.empty()) continue; // blank line or comment

        if (opens_block(content)) {
            owner = Line{line_number, column, indent, std::string(content), true, {}};
            in_block = true;
            continue;
        }
        lines.push_back(Line{line_number, column, indent, std::string(content), false, {}});
    }

    if (in_block) {
        error = EcfgError{owner.number, owner.column,
                          "the '<<' text block is never closed (expected a line with only '>>')"};
    }
    return lines;
}

} // namespace

const char* ecfg_type_name(EcfgType type) {
    switch (type) {
        case EcfgType::None: return "none";
        case EcfgType::Bool: return "bool";
        case EcfgType::Int: return "int";
        case EcfgType::Float: return "float";
        case EcfgType::String: return "string";
        case EcfgType::Array: return "array";
        case EcfgType::Table: return "table";
    }
    return "?";
}

std::string EcfgError::describe(std::string_view source_name) const {
    const std::string_view name = source_name.empty() ? std::string_view("ecfg") : source_name;
    if (line == 0) return std::format("{}: {}", name, message);
    return std::format("{}:{}:{}: {}", name, line, column, message);
}

bool EcfgValue::as_bool(bool fallback) const { return type_ == EcfgType::Bool ? bool_ : fallback; }

i64 EcfgValue::as_int(i64 fallback) const {
    if (type_ == EcfgType::Int) return int_;
    if (type_ == EcfgType::Float) return static_cast<i64>(float_);
    return fallback;
}

f64 EcfgValue::as_float(f64 fallback) const {
    if (type_ == EcfgType::Float) return float_;
    if (type_ == EcfgType::Int) return static_cast<f64>(int_);
    return fallback;
}

std::string_view EcfgValue::as_string(std::string_view fallback) const {
    return type_ == EcfgType::String ? std::string_view(text_) : fallback;
}

const EcfgValue* EcfgValue::find(std::string_view key) const {
    for (const EcfgValue& child : children_) {
        if (child.key_ == key) return &child;
    }
    return nullptr;
}

const EcfgValue* EcfgValue::find_path(std::string_view path) const {
    const EcfgValue* current = this;
    usize start = 0;
    for (;;) {
        const usize dot = path.find('.', start);
        const std::string_view key = path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        current = current->find(key);
        if (current == nullptr || dot == std::string_view::npos) return current;
        start = dot + 1;
    }
}

ConstSpan<EcfgValue> EcfgValue::children() const { return ConstSpan<EcfgValue>(children_.data(), children_.size()); }

std::string EcfgValue::describe(usize indent) const {
    const std::string pad(indent * 2, ' ');
    switch (type_) {
        case EcfgType::None: return pad + "<none>";
        case EcfgType::Bool: return pad + (bool_ ? "true" : "false");
        case EcfgType::Int: return pad + std::format("{}", int_);
        case EcfgType::Float: return pad + std::format("{}", float_);
        case EcfgType::String: return pad + std::format("\"{}\"", text_);
        case EcfgType::Array: {
            std::string out = pad + "[";
            for (usize index = 0; index < children_.size(); ++index) {
                if (index > 0) out += ", ";
                out += trim(children_[index].describe(0));
            }
            return out + "]";
        }
        case EcfgType::Table: {
            std::string out = pad + "{\n";
            for (const EcfgValue& child : children_) {
                out += std::format("{}{}: {}\n", std::string((indent + 1) * 2, ' '), child.key_,
                                   trim(child.describe(0)));
            }
            return out + pad + "}";
        }
    }
    return pad + "?";
}

// --- __include -------------------------------------------------------------------------------------

/// One line of the source the parser ends up reading, and where it really came from.
struct LineOrigin {
    std::string file;   ///< empty for text the caller passed rather than a file
    u32 line = 0;
};

/// The expanded source: what the parser reads, and the file and line each of its lines came from.
struct ExpandedSource {
    std::string text;
    std::vector<LineOrigin> origin;
};

/// How deeply `__include` may nest: a file that includes a file that includes ... is a stack the reader
/// would otherwise walk until the real one runs out.
constexpr u32 kMaxIncludeDepth = 16;

/// The leading whitespace of a line, as written (tabs stay tabs).
[[nodiscard]] std::string_view leading_whitespace(std::string_view line) {
    usize count = 0;
    while (count < line.size() && (line[count] == ' ' || line[count] == '\t')) ++count;
    return line.substr(0, count);
}

/// Whether a trimmed line is the `__not_main` marker: the word, then spaces, then a colon.
[[nodiscard]] bool is_not_main_line(std::string_view content) {
    constexpr std::string_view kWanted = "__not_main";
    if (!content.starts_with(kWanted)) return false;
    content = trim(content.substr(kWanted.size()));
    return !content.empty() && content.front() == ':';
}

/// Whether a trimmed line is an `__include` directive: the word, then spaces, then a colon. A line that
/// reads like one but names nothing is an error rather than an entry, so this stays loose and the expander
/// says what is missing.
[[nodiscard]] bool is_include_line(std::string_view content) {
    constexpr std::string_view kDirective = "__include";
    if (!content.starts_with(kDirective)) return false;
    content = trim(content.substr(kDirective.size()));
    return !content.empty() && content.front() == ':';
}

/// Expands the `__include: #<path>` lines of \p text, which is what \p file holds (empty for text the
/// caller passed), prefixing every line of the result with \p prefix so that a file written at the top level
/// can land inside a table. The body of a `<< ... >>` block is copied exactly: it is the value.
class IncludeExpander {
public:
    IncludeExpander(std::string base_directory, EcfgError* error)
        : base_(std::move(base_directory)), error_(error) {}

    [[nodiscard]] bool expand(std::string_view text, const std::string& file, std::string_view prefix,
                              u32 depth) {
        u32 number = 0;
        bool block = false;   // inside a "<< ... >>" body, which is copied verbatim
        usize start = 0;
        while (start < text.size()) {
            const usize end = text.find('\n', start);
            const std::string_view raw =
                text.substr(start, (end == std::string_view::npos ? text.size() : end) - start);
            start = end == std::string_view::npos ? text.size() : end + 1;
            ++number;
            const std::string_view content = trim(raw);
            if (block && content != ">>") {
                // The body of a text block is the value: it is copied exactly, prefix and all.
                source.text += raw;
                source.text += '\n';
                source.origin.push_back(LineOrigin{file, number});
                continue;
            }
            if (is_include_line(content)) {
                if (!include(content, raw, file, prefix, number, depth)) return false;
                continue;
            }
            // `__not_main` belongs to the **file**, not to the lines it contributes: a part keeps saying it
            // where it is loaded - and is refused there - while the file that includes it is a main file and
            // stays loadable. So the marker is not copied into the document it lands in.
            if (!file.empty() && leading_whitespace(raw).empty() && is_not_main_line(content)) continue;
            source.text += prefix;
            source.text += raw;
            source.text += '\n';
            source.origin.push_back(LineOrigin{file, number});
            if (content.size() >= 2 && content.substr(content.size() - 2) == "<<") block = true;
            else if (content == ">>") block = false;
        }
        return true;
    }

    ExpandedSource source;

private:
    [[nodiscard]] bool fail(u32 line, std::string message) {
        if (error_ != nullptr) *error_ = EcfgError{line, 1, std::move(message)};
        return false;
    }

    /// One `__include` line: the path is checked, the file is read, and its text lands where the line was.
    [[nodiscard]] bool include(std::string_view content, std::string_view raw, const std::string& file,
                               std::string_view prefix, u32 number, u32 depth) {
        constexpr std::string_view kWanted = "an __include wants a file: __include: #path/to/file.ecfg";
        std::string_view rest = trim(content.substr(std::string_view("__include").size()));
        rest = trim(rest.substr(1));   // after the ':'
        if (rest.size() < 2 || rest.front() != '#') return fail(number, std::string(kWanted));
        rest.remove_prefix(1);
        // Like every other line, the rest of it may be a comment: the path ends at the next '#'.
        if (const usize comment = rest.find('#'); comment != std::string_view::npos) rest = rest.substr(0, comment);
        const std::string path(trim(rest));
        if (path.empty()) return fail(number, std::string(kWanted));
        if (base_.empty()) {
            return fail(number,
                        std::format("__include '{}' has no file to be relative to: this text was not read "
                                    "from one",
                                    path));
        }
        const std::filesystem::path target = std::filesystem::path(path).is_absolute()
                                                 ? std::filesystem::path(path)
                                                 : std::filesystem::path(base_) / path;
        const std::string key = target.lexically_normal().string();
        if (std::find(chain_.begin(), chain_.end(), key) != chain_.end()) {
            return fail(number, std::format("__include '{}' is already being read: {} (a loop)", path,
                                            describe_chain(file, number)));
        }
        if (depth >= kMaxIncludeDepth) {
            return fail(number,
                        std::format("__include '{}' is nested deeper than {} levels", path, kMaxIncludeDepth));
        }
        const std::optional<std::string> included = read_text(key);
        if (!included.has_value()) {
            return fail(number, std::format("__include '{}' cannot be read (from {})", key,
                                            describe_chain(file, number)));
        }
        chain_.push_back(key);
        const bool ok = expand(*included, key, std::string(prefix) + std::string(leading_whitespace(raw)),
                               depth + 1);
        chain_.pop_back();
        return ok;
    }

    [[nodiscard]] static std::string describe_chain(const std::string& file, u32 number) {
        return file.empty() ? std::format("line {}", number) : std::format("{}:{}", file, number);
    }

    std::string base_;
    EcfgError* error_ = nullptr;
    std::vector<std::string> chain_;   ///< the files being expanded, outermost first
};

std::optional<EcfgDocument> EcfgDocument::parse(std::string_view text, EcfgError* error) {
    // Text on its own is still assembled from parts as long as it names none: an include in it is refused
    // with a message that says why (there is no file to be relative to), not as a syntax error.
    return parse(text, std::string{}, error);
}

std::optional<EcfgDocument> EcfgDocument::parse(std::string_view text, const std::string& base_directory,
                                                EcfgError* error) {
    IncludeExpander expander(base_directory, error);
    if (!expander.expand(text, std::string{}, std::string_view{}, 0)) return std::nullopt;

    EcfgError scan_error;
    std::vector<ecfg_detail::Line> lines = scan_lines(expander.source.text, scan_error);
    if (scan_error.line != 0) {
        if (error != nullptr) *error = scan_error;
        return std::nullopt;
    }

    bool ok = true;
    EcfgDocument document;
    try {
        ecfg_detail::Parser parser(std::move(lines));
        parser.run(document.root_);
    } catch (const EcfgError& failure) {
        if (error != nullptr) *error = failure;
        ok = false;
    }
    if (!ok && error != nullptr && error->line > 0 && error->line <= expander.source.origin.size()) {
        // The parser counted lines of the **expanded** source; say which file and line that really is,
        // because the expanded text is not the file the designer is looking at.
        const LineOrigin& origin = expander.source.origin[error->line - 1];
        if (!origin.file.empty()) {
            error->message = std::format("{} ({}:{})", error->message, origin.file, origin.line);
            error->line = origin.line;
        }
    }
    if (!ok) return std::nullopt;
    return document;
}

std::optional<EcfgDocument> EcfgDocument::load(const std::string& path, EcfgError* error) {
    const auto text = read_text(path);
    if (!text.has_value()) {
        if (error != nullptr) *error = EcfgError{0, 0, std::format("cannot read '{}'", path)};
        return std::nullopt;
    }
    // Includes are resolved relative to the file that asks for them, the way a content file's images are
    // resolved relative to it (docs/MODS.md).
    std::optional<EcfgDocument> document = parse(*text, std::filesystem::path(path).parent_path().string(), error);
    if (!document.has_value()) return std::nullopt;
    document->source_name_ = path;
    // __not_main: true says this file is a part: it is read where another file includes it, and reading it
    // on its own is a mistake worth reporting rather than a load that half works. The include path never
    // comes through here - it reads the text itself - so a part is refused exactly when it is loaded
    // directly, and reloading one (F5 on a pack, say) is refused the same way.
    if (const EcfgValue* marker = document->find("__not_main"); marker != nullptr && marker->as_bool()) {
        if (error != nullptr) {
            *error = EcfgError{marker->line(), 1,
                               std::format("'{}' is a part (__not_main is true): include it from another file "
                                           "instead of loading it on its own",
                                           path)};
        }
        return std::nullopt;
    }
    return document;
}

} // namespace t2d
