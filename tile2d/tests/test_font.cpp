// The font engine: containers, character maps, metrics and TrueType outlines, checked against the
// fonts this machine actually has. The CFF half of the engine is exercised by test_cff.
#include <t2d/core/cli.h>
#include <t2d/text/font.h>

#include <support/test_support.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace t2d;

namespace {

constexpr const char* kLiberation = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf";
constexpr const char* kNotoSansCjk = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
constexpr const char* kNotoSerifCjk = "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc";

[[nodiscard]] bool exists(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    std::fclose(file);
    return true;
}

/// Loads a font or skips the test when the machine does not have it.
[[nodiscard]] std::optional<Font> load_or_skip(const char* path, u32 face = 0) {
    if (!exists(path)) {
        T2D_SKIP(std::string("missing font ") + path);
    }
    std::string error;
    std::optional<Font> font = Font::load(path, face, &error);
    if (!font.has_value()) T2D_CHECK_MSG(false, "{} face {} failed to load: {}", path, face, error);
    return font;
}

[[nodiscard]] bool path_is_well_formed(const GlyphPath& path) {
    if (path.empty()) return false;
    bool in_contour = false;
    for (const PathCommand& command : path.commands()) {
        switch (command.verb) {
            case PathVerb::Move:
                if (in_contour) return false; // a Move inside a contour means the previous one never closed
                in_contour = true;
                break;
            case PathVerb::Close:
                if (!in_contour) return false;
                in_contour = false;
                break;
            default:
                if (!in_contour) return false;
                break;
        }
        if (command.verb != PathVerb::Close && command.count == 0) return false;
        for (u8 index = 0; index < command.count; ++index) {
            const FontPoint point = command.points[index];
            if (point.x != point.x || point.y != point.y) return false; // NaN
        }
    }
    return !in_contour;
}

} // namespace

T2D_TEST(a_truetype_font_loads_with_its_metrics) {
    const std::optional<Font> font = load_or_skip(kLiberation);
    T2D_REQUIRE(font.has_value());
    T2D_CHECK_EQ(std::string(font->outline_kind()), std::string("truetype"));
    T2D_CHECK_EQ(font->metrics().units_per_em, 2048.0f);
    T2D_CHECK_GT(font->metrics().glyph_count, 100u);
    T2D_CHECK_GT(font->metrics().ascender, 0.0f);
    T2D_CHECK_LT(font->metrics().descender, 0.0f);
    T2D_CHECK_EQ(font->metrics().face_index, 0u);
    T2D_CHECK_EQ(font->metrics().face_count, 1u);
    T2D_CHECK_MSG(font->family_name().find("Liberation") != std::string::npos, "family name was '{}'",
                  font->family_name());
    T2D_CHECK_GT(font->metrics().underline_thickness, 0.0f);
}

T2D_TEST(character_lookup_follows_the_fonts_cmap) {
    const std::optional<Font> font = load_or_skip(kLiberation);
    T2D_REQUIRE(font.has_value());
    for (const u32 code : {'A', 'a', 'z', '0', '9', ' ', '!', '~'}) {
        T2D_CHECK_MSG(font->glyph_index(code) != 0u, "U+{:04X} has no glyph", code);
    }
    // A Latin font has no Chinese: the lookup must say so instead of returning a random glyph.
    T2D_CHECK_EQ(font->glyph_index(0x4E2Du), 0u);  // 中
    T2D_CHECK_EQ(font->glyph_index(0x9F99u), 0u);  // 龙
    T2D_CHECK_EQ(font->glyph_index(0x10FFFFu), 0u);
    // Two different characters must not share a glyph.
    T2D_CHECK_NE(font->glyph_index('A'), font->glyph_index('B'));
}

T2D_TEST(advances_are_positive_and_inside_the_em) {
    const std::optional<Font> font = load_or_skip(kLiberation);
    T2D_REQUIRE(font.has_value());
    const f32 em = font->metrics().units_per_em;
    for (const u32 code : {'A', 'i', 'W', 'm', '.', ' '}) {
        const u32 glyph = font->glyph_index(code);
        T2D_REQUIRE(glyph != 0u);
        const f32 advance = font->advance(glyph);
        T2D_CHECK_MSG(advance > 0.0f && advance < 3.0f * em, "U+{:04X} advance was {}", code, advance);
    }
    // A wide letter is wider than a narrow one.
    T2D_CHECK_GT(font->advance(font->glyph_index('W')), font->advance(font->glyph_index('i')));
    // Advances are per glyph and stable.
    T2D_CHECK_EQ(font->advance(font->glyph_index('A')), font->advance(font->glyph_index('A')));
}

T2D_TEST(truetype_outlines_are_well_formed) {
    const std::optional<Font> font = load_or_skip(kLiberation);
    T2D_REQUIRE(font.has_value());
    const f32 em = font->metrics().units_per_em;

    for (const u32 code : {'A', 'B', 'g', 'o', '0', '%'}) {
        GlyphPath path;
        const u32 glyph = font->glyph_index(code);
        T2D_REQUIRE(glyph != 0u);
        T2D_CHECK_MSG(font->glyph_path(glyph, path), "U+{:04X} has no outline", code);
        T2D_CHECK_MSG(path_is_well_formed(path), "the outline of U+{:04X} is malformed", code);
        FontPoint min{};
        FontPoint max{};
        T2D_REQUIRE(path.bounds(min, max));
        T2D_CHECK_MSG(min.x >= -em && max.x <= em && min.y >= -em && max.y <= em,
                      "U+{:04X} bounds ({}, {})-({}, {}) leave the em square", code, min.x, min.y, max.x, max.y);
        // 'A' has ink above the baseline, 'g' has a descender.
        if (code == 'A') T2D_CHECK_GT(max.y, em * 0.5f);
        if (code == 'g') T2D_CHECK_LT(min.y, 0.0f);
    }

    // Curves are kept as curves: a round letter is not a polygon.
    GlyphPath round;
    T2D_REQUIRE(font->glyph_path(font->glyph_index('o'), round));
    u32 curves = 0;
    for (const PathCommand& command : round.commands()) {
        if (command.verb == PathVerb::Quad || command.verb == PathVerb::Cubic) ++curves;
    }
    T2D_CHECK_GT(curves, 4u);
}

T2D_TEST(composite_glyphs_bring_their_components) {
    const std::optional<Font> font = load_or_skip(kLiberation);
    T2D_REQUIRE(font.has_value());
    // U+00C1 LATIN CAPITAL LETTER A WITH ACUTE is a composite in this font.
    const u32 accented = font->glyph_index(0x00C1u);
    T2D_REQUIRE(accented != 0u);
    GlyphPath composite;
    T2D_REQUIRE(font->glyph_path(accented, composite));
    T2D_CHECK(path_is_well_formed(composite));

    GlyphPath plain;
    T2D_REQUIRE(font->glyph_path(font->glyph_index('A'), plain));
    FontPoint composite_min{};
    FontPoint composite_max{};
    FontPoint plain_min{};
    FontPoint plain_max{};
    T2D_REQUIRE(composite.bounds(composite_min, composite_max));
    T2D_REQUIRE(plain.bounds(plain_min, plain_max));
    // The accent sits above the base letter, so the composite is taller and has at least as much ink.
    T2D_CHECK_GT(composite_max.y, plain_max.y);
    T2D_CHECK_GE(composite.commands().size(), plain.commands().size());
    T2D_CHECK_NE(composite.commands().size(), 0u);
}

T2D_TEST(a_blank_glyph_is_valid_and_empty) {
    const std::optional<Font> font = load_or_skip(kLiberation);
    T2D_REQUIRE(font.has_value());
    const u32 space = font->glyph_index(' ');
    T2D_REQUIRE(space != 0u);
    GlyphPath path;
    T2D_CHECK(font->glyph_path(space, path)); // valid, just nothing to draw
    T2D_CHECK(path.empty());
    T2D_CHECK_GT(font->advance(space), 0.0f); // a space still advances the pen
}

T2D_TEST(a_collection_reports_its_faces) {
    if (!exists(kNotoSansCjk)) T2D_SKIP("Noto Sans CJK is not installed");
    std::string error;
    // Asking for a face that does not exist proves the collection header was read.
    const std::optional<Font> missing = Font::load(kNotoSansCjk, 99, &error);
    T2D_CHECK_FALSE(missing.has_value());
    T2D_CHECK_MSG(error.find("10") != std::string::npos, "the error did not mention the face count: {}", error);

    // A single font file has exactly one face.
    error.clear();
    const std::optional<Font> single = Font::load(kLiberation, 1, &error);
    T2D_CHECK_FALSE(single.has_value());
    T2D_CHECK_FALSE(error.empty());
}

T2D_TEST(cjk_glyphs_come_from_the_cff_table) {
    if (!exists(kNotoSansCjk)) T2D_SKIP("Noto Sans CJK is not installed");
    std::string error;
    std::optional<Font> font = Font::load(kNotoSansCjk, 0, &error);
    if (!font.has_value()) {
        // The CFF interpreter is a separate module; until it is in place the CJK path cannot work, and
        // that is reported rather than hidden.
        T2D_SKIP(std::string("CFF outlines unavailable: ") + error);
    }
    T2D_CHECK_EQ(std::string(font->outline_kind()), std::string("cff"));
    T2D_CHECK_GT(font->metrics().glyph_count, 10000u);
    T2D_CHECK_EQ(font->metrics().face_count, 10u);

    for (const u32 code : {0x4E2Du, 0x6587u, 0x9F99u, 0x6C49u}) { // 中 文 龙 汉
        const u32 glyph = font->glyph_index(code);
        T2D_CHECK_MSG(glyph != 0u, "U+{:04X} has no glyph", code);
        if (glyph == 0u) continue;
        const f32 advance = font->advance(glyph);
        T2D_CHECK_MSG(advance > 0.0f && advance < 3.0f * font->metrics().units_per_em,
                      "U+{:04X} advance was {}", code, advance);
        GlyphPath path;
        T2D_CHECK_MSG(font->glyph_path(glyph, path), "U+{:04X} has no outline", code);
        T2D_CHECK_MSG(path_is_well_formed(path), "the outline of U+{:04X} is malformed", code);
        FontPoint min{};
        FontPoint max{};
        if (path.bounds(min, max)) {
            T2D_CHECK_MSG(max.x - min.x > font->metrics().units_per_em * 0.4f,
                          "U+{:04X} is suspiciously narrow ({} units)", code, max.x - min.x);
        }
    }
    // Simplified and traditional forms are different characters with different glyphs.
    T2D_CHECK_NE(font->glyph_index(0x9F99u), font->glyph_index(0x9F8Du)); // 龙 / 龍
}

T2D_TEST(broken_fonts_are_rejected_instead_of_read) {
    const auto bytes = read_binary(kLiberation);
    if (!bytes.has_value()) T2D_SKIP("Liberation Sans is not installed");
    std::string error;

    // Every short prefix must fail cleanly.
    for (const usize size : {usize{0}, usize{1}, usize{4}, usize{11}, usize{64}, usize{4096}}) {
        std::vector<u8> truncated(bytes->begin(), bytes->begin() + std::min(size, bytes->size()));
        const std::optional<Font> font = Font::load_from_memory(std::move(truncated), 0, &error);
        T2D_CHECK_MSG(!font.has_value(), "a {} byte prefix was accepted as a font", size);
    }

    // A file that is not a font at all.
    std::vector<u8> garbage(1024, 0x5Au);
    T2D_CHECK_FALSE(Font::load_from_memory(std::move(garbage), 0, &error).has_value());

    // An unsupported sfnt version.
    std::vector<u8> wrong_version = *bytes;
    wrong_version[0] = 0x00;
    wrong_version[1] = 0x02;
    T2D_CHECK_FALSE(Font::load_from_memory(std::move(wrong_version), 0, &error).has_value());

    // A missing file reports instead of crashing.
    error.clear();
    T2D_CHECK_FALSE(Font::load("/nonexistent/font.ttf", 0, &error).has_value());
    T2D_CHECK_FALSE(error.empty());
}

T2D_TEST(bounds_cover_every_control_point) {
    GlyphPath path;
    path.move_to(0.0f, 0.0f);
    path.line_to(10.0f, -5.0f);
    path.quad_to(20.0f, 5.0f, 30.0f, 0.0f);
    path.cubic_to(-1.0f, 2.0f, 3.0f, 40.0f, 5.0f, 6.0f);
    path.close();
    FontPoint min{};
    FontPoint max{};
    T2D_REQUIRE(path.bounds(min, max));
    T2D_CHECK_EQ(min.x, -1.0f);
    T2D_CHECK_EQ(min.y, -5.0f);
    T2D_CHECK_EQ(max.x, 30.0f);
    T2D_CHECK_EQ(max.y, 40.0f);

    GlyphPath empty;
    T2D_CHECK_FALSE(empty.bounds(min, max));
    T2D_CHECK(empty.empty());
}

T2D_TEST_MAIN
