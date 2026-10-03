// UTF-8, the language tables and text layout: the CPU half of the text engine, no GPU needed.
#include <t2d/text/locale.h>
#include <t2d/text/utf8.h>

#include <support/test_support.h>

#include <string>
#include <vector>

using namespace t2d;

namespace {

/// "矿场" and friends: the strings the interface actually shows.
constexpr const char* kChinese = "矿场 世界 开始单人游戏";
constexpr const char* kTraditional = "礦場 世界 開始單人遊戲";

constexpr const char* kUiText = R"(
en::
    title:"MINE"
    row.start:"START SINGLE PLAYER"
    only.english:"ENGLISH ONLY"
zh-Hans::
    title:"矿场"
    row.start:"开始单人游戏"
zh-Hant::
    title:"礦場"
    row.start:"開始單人遊戲"
nonsense::
    title:"?"
)";

} // namespace

T2D_TEST(utf8_decodes_the_scripts_the_game_ships) {
    // ASCII, two and three byte sequences, and a four byte one.
    const std::string text = std::string("A") + "\xC3\xA9" + "\xE7\x9F\xBF" + "\xF0\x9F\x98\x80";
    const std::vector<u32> points = utf8_codepoints(text);
    T2D_REQUIRE(points.size() == 4u);
    T2D_CHECK_EQ(points[0], 0x41u);     // A
    T2D_CHECK_EQ(points[1], 0xE9u);     // é
    T2D_CHECK_EQ(points[2], 0x77FFu);   // 矿
    T2D_CHECK_EQ(points[3], 0x1F600u);  // an emoji, four bytes
    T2D_CHECK_EQ(utf8_length(text), 4u);

    // Round trip through the encoder.
    std::string rebuilt;
    for (const u32 point : points) utf8_encode(rebuilt, point);
    T2D_CHECK_EQ(rebuilt, text);

    // The Chinese strings of the two variants are different characters, not different encodings.
    T2D_CHECK_EQ(utf8_length(kChinese), 12u);
    T2D_CHECK_NE(utf8_codepoints(kChinese)[0], utf8_codepoints(kTraditional)[0]);
    T2D_CHECK_EQ(utf8_codepoints(kChinese)[0], 0x77FFu);       // 矿
    T2D_CHECK_EQ(utf8_codepoints(kTraditional)[0], 0x7926u);   // 礦
}

T2D_TEST(malformed_utf8_is_replaced_not_read_past_the_end) {
    const auto decode = [](std::string_view text, usize index) { return utf8_decode(text, index); };

    T2D_CHECK_EQ(decode("abc", 3).size, 0u);                       // end of text
    T2D_CHECK_EQ(decode("\xFF", 0).value, 0xFFFDu);                // invalid lead byte
    T2D_CHECK_EQ(decode("\xFF", 0).size, 1u);
    T2D_CHECK_EQ(decode("\xE7\x9F", 0).value, 0xFFFDu);            // truncated three byte sequence
    T2D_CHECK_EQ(decode("\xE7\x9F", 0).size, 1u);
    T2D_CHECK_EQ(decode("\xC0\x80", 0).value, 0xFFFDu);            // overlong encoding of NUL
    T2D_CHECK_EQ(decode("\xED\xA0\x80", 0).value, 0xFFFDu);        // a surrogate half
    T2D_CHECK_EQ(decode("\xF7\xBF\xBF\xBF", 0).value, 0xFFFDu);    // beyond U+10FFFF

    // A string of garbage still terminates: every byte is consumed exactly once.
    const std::string garbage = "\xFF\xFE\x80\xC3";
    usize consumed = 0;
    usize count = 0;
    while (consumed < garbage.size()) {
        const Codepoint point = utf8_decode(garbage, consumed);
        T2D_CHECK_GT(point.size, 0u);
        consumed += point.size;
        ++count;
        if (count > garbage.size()) break; // a loop that does not advance would spin here
    }
    T2D_CHECK_EQ(consumed, garbage.size());
    T2D_CHECK_EQ(utf8_length(garbage), count);

    // Encoding something impossible yields the replacement character, not a broken sequence.
    T2D_CHECK_EQ(utf8_from_codepoint(0xD800u), std::string("\xEF\xBF\xBD"));
    T2D_CHECK_EQ(utf8_from_codepoint(0x110000u), std::string("\xEF\xBF\xBD"));
}

T2D_TEST(script_classification_drives_font_and_breaking_choices) {
    T2D_CHECK(is_cjk(0x77FFu));   // 矿
    T2D_CHECK(is_cjk(0x7926u));   // 礦
    T2D_CHECK(is_cjk(0x3002u));   // 。
    T2D_CHECK(is_cjk(0x30A2u));   // ア
    T2D_CHECK(is_cjk(0xAC00u));   // 가
    T2D_CHECK_FALSE(is_cjk('A'));
    T2D_CHECK_FALSE(is_cjk(0xE9u)); // é

    T2D_CHECK(is_closing_punctuation(0x3002u));
    T2D_CHECK(is_closing_punctuation(0xFF0Cu));
    T2D_CHECK_FALSE(is_closing_punctuation(0x300Cu));
    T2D_CHECK(is_opening_punctuation(0x300Cu));
    T2D_CHECK(is_breaking_space(' '));
    T2D_CHECK(is_breaking_space(0x3000u)); // ideographic space
    T2D_CHECK_FALSE(is_breaking_space(0x00A0u));
}

T2D_TEST(line_breaking_follows_the_rules_an_interface_needs) {
    T2D_CHECK(can_break_between(' ', 'a'));            // after a space
    T2D_CHECK(can_break_between(0x77FFu, 0x754Cu));    // between two ideographs
    T2D_CHECK_FALSE(can_break_between('a', 'b'));      // never inside a Latin word
    T2D_CHECK_FALSE(can_break_between(0x77FFu, 0x3002u)); // never before 。
    T2D_CHECK_FALSE(can_break_between(0x300Cu, 0x77FFu)); // never after 「
    T2D_CHECK(can_break_between('a', 0x0Au));          // an explicit newline
    T2D_CHECK_FALSE(can_break_between(0u, 'a'));
}

T2D_TEST(languages_parse_from_the_spellings_people_use) {
    T2D_CHECK_EQ(parse_language("en").value(), Language::English);
    T2D_CHECK_EQ(parse_language("EN").value(), Language::English);
    T2D_CHECK_EQ(parse_language("zh").value(), Language::SimplifiedChinese);
    T2D_CHECK_EQ(parse_language("zh-Hans").value(), Language::SimplifiedChinese);
    T2D_CHECK_EQ(parse_language("zh_CN").value(), Language::SimplifiedChinese);
    T2D_CHECK_EQ(parse_language("zh-Hans-CN").value(), Language::SimplifiedChinese);
    T2D_CHECK_EQ(parse_language("zh-Hant").value(), Language::TraditionalChinese);
    T2D_CHECK_EQ(parse_language("zh_TW").value(), Language::TraditionalChinese);
    T2D_CHECK_EQ(parse_language("zh-HK").value(), Language::TraditionalChinese);
    T2D_CHECK_FALSE(parse_language("klingon").has_value());
    T2D_CHECK_FALSE(parse_language("").has_value());

    T2D_CHECK_EQ(std::string(language_code(Language::SimplifiedChinese)), std::string("zh-Hans"));
    T2D_CHECK_EQ(std::string(language_code(Language::TraditionalChinese)), std::string("zh-Hant"));
    // The endonym is what a language menu shows, so it must be the language's own script.
    T2D_CHECK_EQ(utf8_codepoints(language_name(Language::SimplifiedChinese))[0], 0x7B80u);   // 简
    T2D_CHECK_EQ(utf8_codepoints(language_name(Language::TraditionalChinese))[0], 0x7E41u);  // 繁
    T2D_CHECK(is_chinese(Language::SimplifiedChinese));
    T2D_CHECK(is_chinese(Language::TraditionalChinese));
    T2D_CHECK_FALSE(is_chinese(Language::English));
}

T2D_TEST(the_string_table_falls_back_and_reports_what_is_missing) {
    EcfgError error;
    std::vector<std::string> unknown;
    const Locale locale = Locale::from_ecfg_text(kUiText, &error, &unknown);
    T2D_CHECK(error.line == 0u);
    T2D_CHECK_EQ(locale.count(Language::English), 3u);
    T2D_CHECK_EQ(locale.count(Language::SimplifiedChinese), 2u);
    T2D_CHECK_EQ(locale.count(Language::TraditionalChinese), 2u);
    T2D_REQUIRE(unknown.size() == 1u);
    T2D_CHECK_EQ(unknown[0], std::string("nonsense"));

    Locale active = locale;
    active.set_language(Language::SimplifiedChinese);
    T2D_CHECK_EQ(active.text("title"), std::string_view("矿场"));
    T2D_CHECK_EQ(active.text("row.start"), std::string_view("开始单人游戏"));
    // Missing in this language: English answers, and the id is reported as untranslated.
    bool translated = true;
    T2D_CHECK_EQ(active.text("only.english", translated), std::string_view("ENGLISH ONLY"));
    T2D_CHECK_FALSE(translated);
    // Missing everywhere: the id itself comes back, which is visible instead of blank.
    T2D_CHECK_EQ(active.text("nowhere.at.all"), std::string_view("nowhere.at.all"));
    const std::vector<std::string> missing = active.missing_for_active_language();
    T2D_REQUIRE(missing.size() == 1u);
    T2D_CHECK_EQ(missing[0], std::string("only.english"));

    active.set_language(Language::TraditionalChinese);
    T2D_CHECK_EQ(active.text("title"), std::string_view("礦場"));
    // English never reports missing strings: it is the fallback.
    Locale english = locale;
    T2D_CHECK_EQ(english.missing_for_active_language().size(), 0u);
}

T2D_TEST(an_empty_or_broken_string_file_still_yields_a_usable_locale) {
    Locale empty = Locale::from_ecfg_text("");
    T2D_CHECK_EQ(empty.count(Language::English), 0u);
    T2D_CHECK_EQ(empty.text("title"), std::string_view("title")); // the id, never an empty label

    EcfgError error;
    const Locale broken = Locale::from_ecfg_text("en::\n    title\n", &error);
    T2D_CHECK_EQ(broken.count(Language::English), 0u);
    T2D_CHECK_EQ(error.line, 2u);
}

T2D_TEST(the_shipped_interface_strings_cover_every_language) {
    const std::string path = std::string(T2D_SOURCE_DIR) + "/assets/text/ui.ecfg";
    EcfgError error;
    std::optional<EcfgDocument> document = EcfgDocument::load(path, &error);
    if (!document.has_value()) {
        if (error.line == 0) T2D_SKIP("assets/text/ui.ecfg is not next to the source tree");
        T2D_CHECK_MSG(false, "ui.ecfg failed to parse: {}", error.describe(path));
        return;
    }
    std::vector<std::string> unknown;
    const Locale locale = Locale::from_ecfg(*document, &unknown);
    T2D_CHECK_EQ(unknown.size(), 0u);
    T2D_CHECK_GT(locale.count(Language::English), 20u);

    // Every language must translate every string English has: a half translated interface is a bug,
    // not a detail, and this is the test that catches it.
    for (const Language language : {Language::SimplifiedChinese, Language::TraditionalChinese}) {
        for (const std::string_view id : locale.strings().ids(Language::English)) {
            const std::string_view text = locale.strings().find(language, id);
            T2D_CHECK_MSG(!text.empty(), "'{}' is missing from {}", id, language_code(language));
        }
    }

    // The two Chinese variants really are different text.
    T2D_CHECK_NE(locale.strings().find(Language::SimplifiedChinese, "title"),
                 locale.strings().find(Language::TraditionalChinese, "title"));
    T2D_CHECK_EQ(locale.strings().find(Language::SimplifiedChinese, "row.start"), std::string_view("开始单人游戏"));
    T2D_CHECK_EQ(locale.strings().find(Language::TraditionalChinese, "row.start"), std::string_view("開始單人遊戲"));
}

T2D_TEST_MAIN
