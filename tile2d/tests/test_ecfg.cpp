// The .ecfg reader: the format is the one shown by example.ecfg in the repository root.
#include <t2d/core/ecfg.h>

#include <support/test_support.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace t2d;

namespace {

/// example.ecfg, byte for byte. The reader is written against this file, so the test keeps a copy of
/// it here as well: the shipped file is parsed by its own test below.
constexpr const char* kExample = R"(number1:0

string1:"aaa"

bool1:true

#注释

table1::
    subtable::
        num1:1
    array1:[1,1,1]

text1:<<
title

content

end
>>
)";

[[nodiscard]] std::optional<EcfgDocument> parse_ok(std::string_view text) {
    EcfgError error;
    std::optional<EcfgDocument> document = EcfgDocument::parse(text, &error);
    if (!document.has_value()) {
        T2D_CHECK_MSG(false, "parsing failed: {}", error.describe("test"));
    }
    return document;
}

/// Parses text that must be rejected and reports the line the error points at (0 when none).
[[nodiscard]] u32 parse_error_line(std::string_view text) {
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::parse(text, &error);
    if (document.has_value()) {
        T2D_CHECK_MSG(false, "this text was accepted but should not be: {}", text);
        return 0;
    }
    return error.line;
}

} // namespace

T2D_TEST(the_shipped_example_file_parses) {
    const std::string path = std::string(T2D_SOURCE_DIR) + "/../example.ecfg";
    EcfgError error;
    std::optional<EcfgDocument> document = EcfgDocument::load(path, &error);
    if (!document.has_value()) {
        if (error.line == 0) T2D_SKIP("example.ecfg is not next to the source tree");
        T2D_CHECK_MSG(false, "example.ecfg failed to parse: {}", error.describe(path));
        return;
    }
    T2D_CHECK_EQ(document->find("number1") != nullptr, true);
    T2D_CHECK_EQ(document->find("number1")->as_int(-1), 0);
    T2D_CHECK_EQ(document->find("string1")->as_string(), std::string_view("aaa"));
    T2D_CHECK_EQ(document->find("bool1")->as_bool(false), true);
    T2D_CHECK_EQ(document->find_path("table1.subtable.num1")->as_int(-1), 1);
    T2D_CHECK_EQ(document->find_path("table1.array1")->size(), 3u);
    T2D_CHECK_EQ(document->find("text1")->as_string(), std::string_view("title\n\ncontent\n\nend\n"));
}

T2D_TEST(the_example_format_parses_value_for_value) {
    const std::optional<EcfgDocument> document = parse_ok(kExample);
    T2D_REQUIRE(document.has_value());
    const EcfgValue& root = document->root();
    T2D_CHECK_EQ(static_cast<u32>(root.type()), static_cast<u32>(EcfgType::Table));
    T2D_CHECK_EQ(root.size(), 5u); // number1, string1, bool1, table1, text1

    const EcfgValue* number = root.find("number1");
    T2D_REQUIRE(number != nullptr);
    T2D_CHECK_EQ(static_cast<u32>(number->type()), static_cast<u32>(EcfgType::Int));
    T2D_CHECK_EQ(number->as_int(-1), 0);
    T2D_CHECK_EQ(number->line(), 1u);
    T2D_CHECK_EQ(number->key(), std::string_view("number1"));

    T2D_REQUIRE(root.find("string1") != nullptr);
    T2D_CHECK_EQ(root.find("string1")->as_string(), std::string_view("aaa"));
    T2D_REQUIRE(root.find("bool1") != nullptr);
    T2D_CHECK_EQ(static_cast<u32>(root.find("bool1")->type()), static_cast<u32>(EcfgType::Bool));
    T2D_CHECK_EQ(root.find("bool1")->as_bool(false), true);

    // A table holds its entries in file order, and nesting follows the indentation.
    const EcfgValue* table = root.find("table1");
    T2D_REQUIRE(table != nullptr);
    T2D_CHECK(table->is_table());
    T2D_REQUIRE(table->size() == 2u);
    T2D_CHECK_EQ(table->children()[0].key(), std::string_view("subtable"));
    T2D_CHECK_EQ(table->children()[1].key(), std::string_view("array1"));
    T2D_REQUIRE(table->find("subtable") != nullptr);
    T2D_REQUIRE(table->find("subtable")->find("num1") != nullptr);
    T2D_CHECK_EQ(table->find("subtable")->find("num1")->as_int(-1), 1);

    const EcfgValue* array = table->find("array1");
    T2D_REQUIRE(array != nullptr);
    T2D_CHECK(array->is_array());
    T2D_REQUIRE(array->size() == 3u);
    for (const EcfgValue& element : array->children()) {
        T2D_CHECK_EQ(static_cast<u32>(element.type()), static_cast<u32>(EcfgType::Int));
        T2D_CHECK_EQ(element.as_int(-1), 1);
    }

    // The text block is verbatim: its blank lines and the word "end" are content, not syntax.
    T2D_REQUIRE(root.find("text1") != nullptr);
    T2D_CHECK_EQ(root.find("text1")->as_string(), std::string_view("title\n\ncontent\n\nend\n"));
    T2D_CHECK_EQ(root.find("text1")->line(), 14u);
}

T2D_TEST(scalars_cover_the_types_the_format_promises) {
    const std::optional<EcfgDocument> document = parse_ok(R"(
zero:0
negative:-42
plus:+7
hex:0xFF
big:9007199254740993
fraction:1.5
negative_float:-0.25
exponent:1e3
yes:true
no:false
empty_string:""
escaped:"a\"b\\c\nd\te\rf"
quoted_hash:"# not a comment"
quoted_colon:"a:b"
)");
    T2D_REQUIRE(document.has_value());
    const EcfgValue& root = document->root();
    T2D_CHECK_EQ(root.find("zero")->as_int(-1), 0);
    T2D_CHECK_EQ(root.find("negative")->as_int(0), -42);
    T2D_CHECK_EQ(root.find("plus")->as_int(0), 7);
    T2D_CHECK_EQ(root.find("hex")->as_int(0), 255);
    T2D_CHECK_EQ(root.find("big")->as_int(0), 9007199254740993LL);
    T2D_CHECK_EQ(static_cast<u32>(root.find("fraction")->type()), static_cast<u32>(EcfgType::Float));
    T2D_CHECK_EQ(root.find("fraction")->as_float(0.0), 1.5);
    T2D_CHECK_EQ(root.find("negative_float")->as_float(0.0), -0.25);
    T2D_CHECK_EQ(root.find("exponent")->as_float(0.0), 1000.0);
    T2D_CHECK_EQ(root.find("yes")->as_bool(false), true);
    T2D_CHECK_EQ(root.find("no")->as_bool(true), false);
    T2D_CHECK_EQ(root.find("empty_string")->as_string("x"), std::string_view(""));
    T2D_CHECK_EQ(root.find("escaped")->as_string(), std::string_view("a\"b\\c\nd\te\rf"));
    T2D_CHECK_EQ(root.find("quoted_hash")->as_string(), std::string_view("# not a comment"));
    T2D_CHECK_EQ(root.find("quoted_colon")->as_string(), std::string_view("a:b"));

    // A mistyped lookup returns the fallback instead of throwing or reading garbage.
    T2D_CHECK_EQ(root.find("missing"), nullptr);
    T2D_CHECK_EQ(root.find("fraction")->as_int(-7), 1);
    T2D_CHECK_EQ(root.find("zero")->as_float(-1.0), 0.0);
    T2D_CHECK_EQ(root.find("zero")->as_string("fallback"), std::string_view("fallback"));
    T2D_CHECK_EQ(root.find_path("a.b.c"), nullptr);
}

T2D_TEST(comments_blank_lines_and_whitespace_are_forgiving) {
    const std::optional<EcfgDocument> document = parse_ok(
        "# leading comment\n"
        "\n"
        "   \t \n"
        "a:1   # trailing comment\n"
        "b : 2\n"
        "c:\t\"x\"\n"
        "\n"
        "table::# comment after an opener\n"
        "\ttab_indented:3\n"
        "\n"
        "after:4\n");
    T2D_REQUIRE(document.has_value());
    const EcfgValue& root = document->root();
    T2D_CHECK_EQ(root.find("a")->as_int(0), 1);
    T2D_CHECK_EQ(root.find("b")->as_int(0), 2); // spaces around the colon are allowed
    T2D_CHECK_EQ(root.find("c")->as_string(), std::string_view("x"));
    T2D_CHECK_EQ(root.find("after")->as_int(0), 4);
    T2D_REQUIRE(root.find("table") != nullptr);
    T2D_CHECK_EQ(root.find("table")->find("tab_indented")->as_int(0), 3); // a tab is four columns
}

T2D_TEST(arrays_may_span_lines_and_nest) {
    const std::optional<EcfgDocument> document = parse_ok(R"(
empty:[]
one:[1]
mixed:[1,"two",true,2.5]
nested:[[1,2],[3]]
spread:[
    1,
    2,
    3
]
spread_with_comment:[
    1, # first
    2
]
strings:["a,b","c[d]"]
)");
    T2D_REQUIRE(document.has_value());
    const EcfgValue& root = document->root();
    T2D_CHECK_EQ(root.find("empty")->size(), 0u);
    T2D_CHECK_EQ(root.find("one")->size(), 1u);
    T2D_REQUIRE(root.find("mixed") != nullptr);
    T2D_CHECK_EQ(root.find("mixed")->size(), 4u);
    T2D_CHECK_EQ(root.find("mixed")->children()[0].as_int(0), 1);
    T2D_CHECK_EQ(root.find("mixed")->children()[1].as_string(), std::string_view("two"));
    T2D_CHECK_EQ(root.find("mixed")->children()[2].as_bool(false), true);
    T2D_CHECK_EQ(root.find("mixed")->children()[3].as_float(0.0), 2.5);

    T2D_REQUIRE(root.find("nested") != nullptr);
    T2D_CHECK_EQ(root.find("nested")->size(), 2u);
    T2D_CHECK_EQ(root.find("nested")->children()[0].size(), 2u);
    T2D_CHECK_EQ(root.find("nested")->children()[1].children()[0].as_int(0), 3);

    T2D_REQUIRE(root.find("spread") != nullptr);
    T2D_CHECK_EQ(root.find("spread")->size(), 3u);
    T2D_CHECK_EQ(root.find("spread")->children()[2].as_int(0), 3);
    T2D_REQUIRE(root.find("spread_with_comment") != nullptr);
    T2D_CHECK_EQ(root.find("spread_with_comment")->size(), 2u);

    // A comma inside a quoted element is data, not a separator.
    T2D_REQUIRE(root.find("strings") != nullptr);
    T2D_CHECK_EQ(root.find("strings")->size(), 2u);
    T2D_CHECK_EQ(root.find("strings")->children()[0].as_string(), std::string_view("a,b"));
    T2D_CHECK_EQ(root.find("strings")->children()[1].as_string(), std::string_view("c[d]"));

    // Lines consumed by an array do not confuse the table that contains it: the array's continuation
    // lines must not turn into entries of the enclosing table.
    T2D_CHECK_EQ(root.size(), 7u);
    T2D_CHECK(root.find("1") == nullptr);
    T2D_CHECK(root.find("3") == nullptr);
}

T2D_TEST(text_blocks_are_verbatim) {
    const std::optional<EcfgDocument> document = parse_ok(
        "script:<<\n"
        "line one\n"
        "\n"
        "# not a comment here\n"
        "key:value  [1,2]  \"quoted\"\n"
        "   indented line\n"
        ">>\n"
        "after:1\n");
    T2D_REQUIRE(document.has_value());
    const EcfgValue& root = document->root();
    T2D_REQUIRE(root.find("script") != nullptr);
    T2D_CHECK_EQ(root.find("script")->as_string(),
                 std::string_view("line one\n\n# not a comment here\nkey:value  [1,2]  \"quoted\"\n   indented line\n"));
    T2D_CHECK_EQ(root.find("after")->as_int(0), 1);
    T2D_CHECK_EQ(root.size(), 2u);

    // An empty block is legal, and a block inside a table keeps the table's other entries.
    const std::optional<EcfgDocument> nested = parse_ok("table::\n    empty:<<\n    >>\n    other:2\n");
    T2D_REQUIRE(nested.has_value());
    T2D_REQUIRE(nested->find_path("table.empty") != nullptr);
    T2D_CHECK_EQ(nested->find_path("table.empty")->as_string("x"), std::string_view(""));
    T2D_CHECK_EQ(nested->find_path("table.other")->as_int(0), 2);
}

T2D_TEST(tables_nest_and_may_be_empty) {
    const std::optional<EcfgDocument> document = parse_ok(R"(
empty_table::
filled::
    a:1
    deeper::
        b:2
        deeper_still::
            c:3
    back:4
)");
    T2D_REQUIRE(document.has_value());
    const EcfgValue& root = document->root();
    T2D_REQUIRE(root.find("empty_table") != nullptr);
    T2D_CHECK(root.find("empty_table")->is_table());
    T2D_CHECK_EQ(root.find("empty_table")->size(), 0u);
    T2D_CHECK_EQ(root.find_path("filled.a")->as_int(0), 1);
    T2D_CHECK_EQ(root.find_path("filled.deeper.b")->as_int(0), 2);
    T2D_CHECK_EQ(root.find_path("filled.deeper.deeper_still.c")->as_int(0), 3);
    T2D_CHECK_EQ(root.find_path("filled.back")->as_int(0), 4);
    T2D_CHECK_EQ(root.find("filled")->size(), 3u);

    // The same key in two different tables is not a duplicate.
    const std::optional<EcfgDocument> repeated = parse_ok("first::\n    value:1\nsecond::\n    value:2\n");
    T2D_REQUIRE(repeated.has_value());
    T2D_CHECK_EQ(repeated->find_path("first.value")->as_int(0), 1);
    T2D_CHECK_EQ(repeated->find_path("second.value")->as_int(0), 2);
}

T2D_TEST(crlf_and_a_missing_final_newline_are_accepted) {
    const std::optional<EcfgDocument> document = parse_ok("a:1\r\nb:\"two\"\r\ntable::\r\n    c:3");
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find("a")->as_int(0), 1);
    T2D_CHECK_EQ(document->find("b")->as_string(), std::string_view("two"));
    T2D_CHECK_EQ(document->find_path("table.c")->as_int(0), 3);
}

T2D_TEST(malformed_files_are_rejected_with_a_line) {
    // Each case: the text, and the line the error must point at.
    struct Case {
        const char* text;
        u32 line;
        const char* what;
    };
    const Case cases[] = {
        {"a\n", 1, "a line without ':'"},
        {"a:", 1, "a missing value"},
        {"a:aaa", 1, "an unquoted string"},
        {"a:1\na:2\n", 2, "a duplicate key"},
        {":1\n", 1, "an empty key"},
        {"a b:1\n", 1, "a space in a key"},
        {"a:[1,2\n", 1, "an array that is never closed"},
        {"a:[1,]\n", 1, "a trailing comma"},
        {"a:[1,,2]\n", 1, "an empty array element"},
        {"a:\"unterminated\n", 1, "an unclosed string"},
        {"a:\"x\" trailing\n", 1, "text after the closing quote"},
        {"a:\"bad\\q\"\n", 1, "an unknown escape"},
        {"a:: extra\n", 1, "garbage after a table opener"},
        {"a:<<\nnever closed\n", 1, "an unterminated text block"},
        {"a:1\n    b:2\n", 2, "an unexpected indent"},
        {"a:1\n  b:2\n", 2, "an indent that matches no level"},
        {"table::\n    a:1\n  b:2\n", 3, "a dedent that matches no level"},
        {"a:99999999999999999999999\n", 1, "an integer that does not fit"},
        {"a:1.2.3\n", 1, "a malformed float"},
        {"a:nan\n", 1, "a bare word"},
    };
    for (const Case& test_case : cases) {
        const u32 line = parse_error_line(test_case.text);
        T2D_CHECK_MSG(line == test_case.line, "{}: error on line {} instead of {}", test_case.what, line,
                      test_case.line);
    }
}

T2D_TEST(an_empty_document_is_a_valid_empty_table) {
    for (const char* text : {"", "\n", "# only a comment\n", "   \n\n"}) {
        const std::optional<EcfgDocument> document = parse_ok(text);
        T2D_REQUIRE(document.has_value());
        T2D_CHECK(document->root().is_table());
        T2D_CHECK_EQ(document->root().size(), 0u);
        T2D_CHECK_EQ(document->find("anything"), nullptr);
    }
}

T2D_TEST(an_indented_file_is_read_the_way_it_looks) {
    // A whole file indented by four columns (a config pasted into a nested context) still parses.
    const std::optional<EcfgDocument> document = parse_ok("    a:1\n    table::\n        b:2\n");
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find("a")->as_int(0), 1);
    T2D_CHECK_EQ(document->find_path("table.b")->as_int(0), 2);
}

T2D_TEST(describe_renders_the_tree) {
    const std::optional<EcfgDocument> document = parse_ok("a:1\nb:\"x\"\nt::\n    c:true\n    d:[1,2]\n");
    T2D_REQUIRE(document.has_value());
    const std::string text = document->root().describe();
    T2D_CHECK_MSG(text.find("a: 1") != std::string::npos, "describe() lost the scalar: {}", text);
    T2D_CHECK_MSG(text.find("b: \"x\"") != std::string::npos, "describe() lost the string: {}", text);
    T2D_CHECK_MSG(text.find("c: true") != std::string::npos, "describe() lost the nested bool: {}", text);
    T2D_CHECK_MSG(text.find("d: [1, 2]") != std::string::npos, "describe() lost the array: {}", text);
}

// --- __include: a file assembled from parts -----------------------------------------------------------

namespace {

/// A directory of its own for one test, removed when the test ends. The include reader needs real files, and
/// a test that wrote into the source tree would leave them there.
class TempDirectory {
public:
    explicit TempDirectory(std::string_view name) {
        path = std::filesystem::temp_directory_path() / std::format("t2d-ecfg-{}-{}", name, counter_++);
        std::error_code code;
        std::filesystem::remove_all(path, code);
        std::filesystem::create_directories(path, code);
    }
    ~TempDirectory() {
        std::error_code code;
        std::filesystem::remove_all(path, code);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    /// Writes one file into the directory and returns its full path (callers that only want the file
    /// written ignore it, so it is not [[nodiscard]]).
    std::string write(std::string_view name, std::string_view text) const {
        const std::filesystem::path file = path / name;
        std::ofstream out(file, std::ios::binary);
        out << text;
        out.close();
        return file.string();
    }

    std::filesystem::path path;

private:
    inline static u32 counter_ = 0;
};

/// Loads a file that must load, and says why when it does not.
[[nodiscard]] std::optional<EcfgDocument> load_ok(const std::string& path) {
    EcfgError error;
    std::optional<EcfgDocument> document = EcfgDocument::load(path, &error);
    if (!document.has_value()) T2D_CHECK_MSG(false, "loading failed: {}", error.describe(path));
    return document;
}

}  // namespace

T2D_TEST(an_include_puts_a_file_where_the_line_is) {
    const TempDirectory dir("splice");
    dir.write("part.ecfg", "number1:7\nstring1:\"bbb\"\n");
    const std::string parent = dir.write("parent.ecfg", "__include: #part.ecfg\nbool1:true\n");
    const std::optional<EcfgDocument> document = load_ok(parent);
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find("number1")->as_int(-1), 7);
    T2D_CHECK_EQ(document->find("string1")->as_string(), std::string_view("bbb"));
    T2D_CHECK(document->find("bool1")->as_bool());
    // The directive itself is gone: it *was* the file, not an entry of its own.
    T2D_CHECK(document->find("__include") == nullptr);
    T2D_CHECK_EQ(document->root().size(), 3u);
}

T2D_TEST(an_include_inside_a_table_lands_in_that_table) {
    // The included file is written at the top level and lands inside a table: it is indented to the
    // include line, which is what makes a part reusable at any depth.
    const TempDirectory dir("table");
    dir.write("part.ecfg", "num1:1\nnum2:2\n");
    const std::string parent = dir.write(
        "parent.ecfg", "table1::\n    __include: #part.ecfg\n    num3:3\n");
    const std::optional<EcfgDocument> document = load_ok(parent);
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find_path("table1.num1")->as_int(-1), 1);
    T2D_CHECK_EQ(document->find_path("table1.num2")->as_int(-1), 2);
    T2D_CHECK_EQ(document->find_path("table1.num3")->as_int(-1), 3);
    T2D_CHECK(document->find("num1") == nullptr);   // not at the top level: it went into the table
}

T2D_TEST(an_included_text_block_is_copied_exactly) {
    // The body of a `<< ... >>` block is a value: indenting it to the include line would change it.
    const TempDirectory dir("block");
    dir.write("part.ecfg", "text1:<<\n    kept as written\n>>\n");
    const std::string parent = dir.write("parent.ecfg", "table1::\n    __include: #part.ecfg\n");
    const std::optional<EcfgDocument> document = load_ok(parent);
    T2D_REQUIRE(document.has_value());
    T2D_REQUIRE(document->find_path("table1.text1") != nullptr);
    T2D_CHECK_EQ(document->find_path("table1.text1")->as_string(),
                std::string_view("    kept as written\n"));
}

T2D_TEST(an_include_can_include_another) {
    const TempDirectory dir("nested");
    dir.write("c.ecfg", "deep:3\n");
    dir.write("b.ecfg", "__include: #c.ecfg\nmiddle:2\n");
    const std::string parent = dir.write("a.ecfg", "__include: #b.ecfg\ntop:1\n");
    const std::optional<EcfgDocument> document = load_ok(parent);
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find("deep")->as_int(-1), 3);
    T2D_CHECK_EQ(document->find("middle")->as_int(-1), 2);
    T2D_CHECK_EQ(document->find("top")->as_int(-1), 1);
}

T2D_TEST(an_include_is_relative_to_the_file_that_names_it) {
    // Not to the working directory: a content file sits in a pack and points at its neighbours.
    const TempDirectory dir("relative");
    std::filesystem::create_directories(dir.path / "parts");
    dir.write("parts/ore.ecfg", "ore1:\"stone\"\n");
    const std::string parent = dir.write("parent.ecfg", "__include: #parts/ore.ecfg\n");
    const std::optional<EcfgDocument> document = load_ok(parent);
    T2D_REQUIRE(document.has_value());
    T2D_REQUIRE(document->find("ore1") != nullptr);
    T2D_CHECK_EQ(document->find("ore1")->as_string(), std::string_view("stone"));
}

T2D_TEST(a_missing_include_is_an_error_that_names_it) {
    const TempDirectory dir("missing");
    const std::string parent = dir.write("parent.ecfg", "a:1\n__include: #gone.ecfg\n");
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::load(parent, &error);
    T2D_CHECK(!document.has_value());
    T2D_CHECK_MSG(error.message.find("gone.ecfg") != std::string::npos, "the message does not name the file: {}",
                  error.message);
    T2D_CHECK_EQ(error.line, 2u);   // the line that asked for it
}

T2D_TEST(an_include_that_loops_is_refused) {
    const TempDirectory dir("loop");
    dir.write("a.ecfg", "__include: #b.ecfg\n");
    const std::string b = dir.write("b.ecfg", "__include: #a.ecfg\n");
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::load(b, &error);
    T2D_CHECK(!document.has_value());
    T2D_CHECK_MSG(error.message.find("loop") != std::string::npos, "the message does not say it loops: {}",
                  error.message);
}

T2D_TEST(a_malformed_include_says_what_it_wants) {
    const TempDirectory dir("malformed");
    const std::string parent = dir.write("parent.ecfg", "__include: part.ecfg\n");   // no '#'
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::load(parent, &error);
    T2D_CHECK(!document.has_value());
    T2D_CHECK_MSG(error.message.find("__include: #") != std::string::npos,
                  "the message does not show the form: {}", error.message);
}

T2D_TEST(text_with_no_file_cannot_resolve_an_include) {
    // Plain parse(text) has nothing to be relative to, and says so instead of guessing a directory.
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::parse("__include: #part.ecfg\n", &error);
    T2D_CHECK(!document.has_value());
    T2D_CHECK_MSG(error.message.find("no file to be relative to") != std::string::npos,
                  "the message does not say why: {}", error.message);
}

T2D_TEST(a_syntax_error_inside_an_include_names_the_file_it_is_in) {
    const TempDirectory dir("error");
    dir.write("part.ecfg", "good:1\nbad:aaa\n");   // a bare word is an error
    const std::string parent = dir.write("parent.ecfg", "__include: #part.ecfg\n");
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::load(parent, &error);
    T2D_CHECK(!document.has_value());
    T2D_CHECK_MSG(error.message.find("part.ecfg:2") != std::string::npos,
                  "the error does not point into the included file: {}", error.message);
    T2D_CHECK_EQ(error.line, 2u);
}

T2D_TEST(a_file_that_says_it_is_not_a_main_file_refuses_to_load) {
    const TempDirectory dir("not_main");
    const std::string part = dir.write("_part.ecfg", "__not_main:true\nnum1:1\n");
    EcfgError error;
    const std::optional<EcfgDocument> document = EcfgDocument::load(part, &error);
    T2D_CHECK(!document.has_value());
    T2D_CHECK_MSG(error.message.find("__not_main") != std::string::npos,
                  "the message does not say why: {}", error.message);
    T2D_CHECK_EQ(error.line, 1u);   // the marker's own line
}

T2D_TEST(a_part_is_still_includeable_and_its_marker_stays_with_it) {
    // The marker is a property of the file: the part is refused when loaded, and the file that includes it
    // is a main file - it does not inherit the marker, or every pack with a part in it would refuse to load.
    const TempDirectory dir("not_main_include");
    dir.write("_part.ecfg", "__not_main:true\nnum1:1\n");
    const std::string parent = dir.write("parent.ecfg", "__include: #_part.ecfg\nbool1:true\n");
    const std::optional<EcfgDocument> document = load_ok(parent);
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find("num1")->as_int(-1), 1);
    T2D_CHECK(document->find("bool1")->as_bool());
    T2D_CHECK(document->find("__not_main") == nullptr);   // the marker did not travel
}

T2D_TEST(not_a_main_file_false_is_an_ordinary_file) {
    const TempDirectory dir("not_main_false");
    const std::string file = dir.write("main.ecfg", "__not_main:false\nnum1:1\n");
    const std::optional<EcfgDocument> document = load_ok(file);
    T2D_REQUIRE(document.has_value());
    T2D_CHECK_EQ(document->find("num1")->as_int(-1), 1);
}

T2D_TEST_MAIN
