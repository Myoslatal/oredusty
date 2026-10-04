// UTF-8, the language tables and text layout: the CPU half of the text engine, no GPU needed.
#include <t2d/text/locale.h>
#include <t2d/text/raster.h>
#include <t2d/text/text_layout.h>
#include <t2d/text/utf8.h>

#include <support/test_support.h>

#include <algorithm>
#include <cstdio>
#include <optional>
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

/// The two faces the interface is laid out against: a Latin one and the CJK collection.
constexpr const char* kLiberation = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf";
constexpr const char* kNotoSansCjk = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";

[[nodiscard]] bool font_exists(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    std::fclose(file);
    return true;
}

/// Loads a font or skips the test when the machine does not have it.
[[nodiscard]] std::optional<Font> load_or_skip(const char* path, u32 face = 0) {
    if (!font_exists(path)) T2D_SKIP(std::string("missing font ") + path);
    std::string error;
    std::optional<Font> font = Font::load(path, face, &error);
    if (!font.has_value()) T2D_CHECK_MSG(false, "{} face {} failed to load: {}", path, face, error);
    return font;
}

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

T2D_TEST(the_line_box_holds_its_text_and_starts_at_the_pen) {
    const std::optional<Font> latin = load_or_skip(kLiberation);
    if (!latin.has_value()) return;
    const std::optional<Font> cjk = load_or_skip(kNotoSansCjk, 0);

    FontSet fonts;
    fonts.latin = &*latin;
    if (cjk.has_value()) fonts.cjk = &*cjk;

    TextStyle style;
    style.size_px = 20;
    const LineBox box = line_box(fonts, style);

    // The ascent is the tallest face's hhea ascent in pixels. Nothing about the string enters here,
    // which is what lets a label and the value beside it share one baseline.
    f32 expected_ascent = latin->metrics().ascender * (20.0f / latin->metrics().units_per_em);
    if (cjk.has_value()) {
        expected_ascent = std::max(expected_ascent, cjk->metrics().ascender * (20.0f / cjk->metrics().units_per_em));
    }
    T2D_CHECK_NEAR(box.ascent, expected_ascent, 0.001f);
    T2D_CHECK_GT(box.descent, 0.0f);
    T2D_CHECK_GE(box.height, box.ascent + box.descent);

    // The promise the box makes: a glyph rasterised at the pen's own size inks nothing outside it.
    // Before the pen became the top of the line box, every glyph was drawn one ascent above the pen,
    // which is exactly why a highlighted row's text hung over the row above it.
    for (const u32 codepoint : {static_cast<u32>('A'), static_cast<u32>('g'), 0x77FFu /* 矿 */}) {
        const Font* font = fonts.pick(codepoint);
        T2D_REQUIRE(font != nullptr);
        const u32 glyph = font->glyph_index(codepoint);
        T2D_REQUIRE(glyph != 0u);
        GlyphPath path;
        T2D_REQUIRE(font->glyph_path(glyph, path));
        // No padding: the transparent pixel the atlas keeps around a glyph is a sampling guard, not
        // part of the text, and counting it would make this a test of the rasteriser's margins.
        const GlyphBitmap bitmap = rasterize_glyph(path, 20.0f / font->metrics().units_per_em, 0);

        T2D_REQUIRE(bitmap.has_ink);
        const f32 ink_top = box.ascent + static_cast<f32>(bitmap.top);
        const f32 ink_bottom = ink_top + static_cast<f32>(bitmap.bitmap.height);
        T2D_CHECK_MSG(ink_top >= 0.0f, "U+{:04X} inks {} px above the top of the line box", codepoint, -ink_top);
        T2D_CHECK_MSG(ink_bottom <= box.height, "U+{:04X} inks {} px below the bottom of the line box", codepoint,
                      ink_bottom - box.height);
    }

    std::vector<PlacedGlyph> placed;
    TextMetrics one;
    layout_text("A", fonts, style, 0.0f, placed, one);
    T2D_REQUIRE(placed.size() == 1u);
    T2D_CHECK_EQ(placed[0].font, fonts.latin);
    T2D_CHECK_NEAR(placed[0].x, 0.0f, 0.001f);
    // The glyph's baseline is one ascent below the pen, never at the pen.
    T2D_CHECK_NEAR(placed[0].y, box.ascent, 0.001f);
    T2D_CHECK_NEAR(one.height, box.height, 0.001f);

    // A second line is one line box lower and starts at the left again.
    TextMetrics two;
    layout_text("A\nB", fonts, style, 0.0f, placed, two);
    T2D_REQUIRE(placed.size() == 2u);
    T2D_CHECK_NEAR(placed[1].y - placed[0].y, box.height, 0.001f);
    T2D_CHECK_NEAR(placed[1].x, 0.0f, 0.001f);
    T2D_CHECK_EQ(two.lines, 2u);
    T2D_CHECK_NEAR(two.height, box.height * 2.0f, 0.001f);

    // The box is the same whatever the string is: that is what a row's background relies on.
    TextMetrics mixed;
    layout_text("矿场 世界 Ag", fonts, style, 0.0f, placed, mixed);
    T2D_CHECK_EQ(mixed.height, one.height);
    T2D_CHECK_EQ(mixed.ascent, one.ascent);
    T2D_CHECK_GT(mixed.width, one.width);

    // Wrapping adds a line the same way.
    TextMetrics wrapped;
    layout_text("AAAA AAAA", fonts, style, 30.0f, placed, wrapped);
    T2D_CHECK_GE(wrapped.lines, 2u);
    T2D_CHECK_NEAR(wrapped.height, box.height * static_cast<f32>(wrapped.lines), 0.001f);

    // The caller's line spacing is a floor: a looser one is honoured, a tighter one cannot cut the
    // font's own box and let one line land on the next.
    TextStyle loose = style;
    loose.line_spacing = 3.0f;
    T2D_CHECK_NEAR(line_box(fonts, loose).height, 60.0f, 0.001f);
    TextStyle tight = style;
    tight.line_spacing = 0.5f;
    T2D_CHECK_NEAR(line_box(fonts, tight).height, box.ascent + box.descent, 0.001f);

    // A set with no fonts at all still describes a box: one em above the baseline.
    const LineBox bare = line_box(FontSet{}, style);
    T2D_CHECK_NEAR(bare.ascent, 20.0f, 0.001f);
    T2D_CHECK_NEAR(bare.descent, 0.0f, 0.001f);
    TextMetrics nothing;
    layout_text("A", FontSet{}, style, 0.0f, placed, nothing);
    T2D_CHECK_EQ(placed.size(), 0u);
    T2D_CHECK_EQ(nothing.height, 0.0f);
}

T2D_TEST_MAIN
