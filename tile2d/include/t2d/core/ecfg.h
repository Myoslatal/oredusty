// Tile2D - the .ecfg configuration reader.
//
// The format is defined by example.ecfg in the repository root; this is the grammar it uses:
//
//     # comment                              whole line comments; a trailing "# ..." works too
//     number1:0                              integers, floats, true/false
//     string1:"aaa"                          strings are always quoted
//     table1::                               "::" opens a table; its entries are the following,
//         subtable::                         more indented lines, so nesting is indentation
//             num1:1
//         array1:[1,1,1]                     "[...]" is an array; it may span several lines
//     text1:<<                               "<< ... >>" is a raw text block: everything between the
//     title                                  two markers is kept verbatim, blank lines and "#"
//                                            included, and the closing marker is a line with only ">>"
//     content
//     >>
//
// Rules the reader enforces, because a config that is half understood is worse than one that is
// rejected:
//   * keys are [A-Za-z0-9_.-]+ and must be unique inside their table
//   * a value is a number, true/false, a quoted string, an array or a text block - a bare word such
//     as aaa is an error, not a string (quote it)
//   * indentation decides nesting; tabs count as four columns; a line indented deeper than the entry
//     before it is an error unless that entry opened a table
//   * every failure reports line and column
#pragma once

#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace t2d {

/// Implementation detail: the recursive descent parser needs to fill values in place.
namespace ecfg_detail {
class Parser;
} // namespace ecfg_detail

enum class EcfgType : u8 { None, Bool, Int, Float, String, Array, Table };

[[nodiscard]] const char* ecfg_type_name(EcfgType type);

struct EcfgError {
    u32 line = 0;   ///< 1-based
    u32 column = 0; ///< 1-based, counted after tabs are expanded
    std::string message;

    [[nodiscard]] std::string describe(std::string_view source_name = {}) const;
};

/// One value of a document. Tables and arrays own their children, so a value is a small tree.
class EcfgValue {
public:
    EcfgValue() = default;

    [[nodiscard]] EcfgType type() const { return type_; }
    [[nodiscard]] bool is_none() const { return type_ == EcfgType::None; }
    [[nodiscard]] bool is_table() const { return type_ == EcfgType::Table; }
    [[nodiscard]] bool is_array() const { return type_ == EcfgType::Array; }
    [[nodiscard]] bool is_string() const { return type_ == EcfgType::String; }

    /// Conversions never throw: a missing or mistyped value yields the fallback.
    [[nodiscard]] bool as_bool(bool fallback = false) const;
    [[nodiscard]] i64 as_int(i64 fallback = 0) const;
    [[nodiscard]] f64 as_float(f64 fallback = 0.0) const;
    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const;

    /// Table entry or array element, keyed by name. nullptr when there is no such entry.
    [[nodiscard]] const EcfgValue* find(std::string_view key) const;
    /// Walks a dotted path ("table1.subtable.num1"). A key that itself contains a dot can only be
    /// reached by calling find() step by step.
    [[nodiscard]] const EcfgValue* find_path(std::string_view path) const;
    /// Table entries / array elements, in file order.
    [[nodiscard]] ConstSpan<EcfgValue> children() const;
    [[nodiscard]] usize size() const { return children_.size(); }
    /// Key this entry has inside its parent table (empty for array elements and the root).
    [[nodiscard]] std::string_view key() const { return key_; }
    /// 1-based line the value starts on.
    [[nodiscard]] u32 line() const { return line_; }

    /// Renders the subtree; used by logs and by tests that want to see what was parsed.
    [[nodiscard]] std::string describe(usize indent = 0) const;

private:
    friend class EcfgDocument;
    friend class ecfg_detail::Parser;

    EcfgType type_ = EcfgType::None;
    bool bool_ = false;
    i64 int_ = 0;
    f64 float_ = 0.0;
    std::string text_;
    std::string key_;
    u32 line_ = 0;
    std::vector<EcfgValue> children_;
};

/// A parsed .ecfg file. The document owns the tree; values stay valid for its lifetime.
class EcfgDocument {
public:
    /// Parses text. On failure returns nullopt and fills error (when given).
    [[nodiscard]] static std::optional<EcfgDocument> parse(std::string_view text, EcfgError* error = nullptr);
    /// Reads and parses a file. A file that cannot be read reports line 0.
    [[nodiscard]] static std::optional<EcfgDocument> load(const std::string& path, EcfgError* error = nullptr);

    [[nodiscard]] const EcfgValue& root() const { return root_; }
    [[nodiscard]] const EcfgValue* find(std::string_view key) const { return root_.find(key); }
    [[nodiscard]] const EcfgValue* find_path(std::string_view path) const { return root_.find_path(path); }
    [[nodiscard]] const std::string& source_name() const { return source_name_; }

private:
    EcfgValue root_;
    std::string source_name_;
};

} // namespace t2d
