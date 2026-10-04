#include <t2d/text/text_layout.h>

#include <t2d/text/utf8.h>

#include <algorithm>

namespace t2d {

const Font* FontSet::pick(u32 codepoint) const {
    const Font* preferred = is_cjk(codepoint) && cjk != nullptr ? cjk : latin;
    if (preferred != nullptr && preferred->glyph_index(codepoint) != 0) return preferred;
    const Font* other = preferred == latin ? cjk : latin;
    if (other != nullptr && other->glyph_index(codepoint) != 0) return other;
    return preferred != nullptr ? preferred : other;
}

LineBox line_box(const FontSet& fonts, const TextStyle& style) {
    const f32 size = static_cast<f32>(style.size_px);
    LineBox box;
    const Font* const candidates[] = {fonts.latin, fonts.cjk};
    for (const Font* font : candidates) {
        if (font == nullptr) continue;
        const FontMetrics& metrics = font->metrics();
        if (metrics.units_per_em <= 0.0f || metrics.ascender <= 0.0f) continue;
        const f32 scale = size / metrics.units_per_em;
        box.ascent = std::max(box.ascent, metrics.ascender * scale);
        box.descent = std::max(box.descent, std::max(0.0f, -metrics.descender) * scale);
    }
    // A set without usable vertical metrics still gets a box: one em above the baseline is the one
    // measure every font agrees on.
    if (box.ascent <= 0.0f) box.ascent = size;
    box.height = std::max(size * style.line_spacing, box.ascent + box.descent);
    return box;
}

void layout_text(std::string_view text, const FontSet& fonts, const TextStyle& style, f32 max_width,
                 std::vector<PlacedGlyph>& out, TextMetrics& metrics) {
    out.clear();
    metrics = TextMetrics{};
    const LineBox box = line_box(fonts, style);
    metrics.ascent = box.ascent;
    metrics.descent = box.descent;
    if (fonts.latin == nullptr && fonts.cjk == nullptr) return;

    const f32 line_height = box.height;
    f32 pen_x = 0.0f;
    f32 pen_y = 0.0f;
    f32 line_width = 0.0f;
    u32 lines = 1;
    u32 previous = 0;
    bool has_previous = false;

    for (usize index = 0; index < text.size();) {
        const Codepoint point = utf8_decode(text, index);
        index += point.size == 0 ? 1 : point.size;
        if (point.value == 0) break;

        if (point.value == 0x0Au || point.value == 0x0Du) { // an explicit newline
            metrics.width = std::max(metrics.width, line_width);
            pen_x = 0.0f;
            line_width = 0.0f;
            pen_y += line_height;
            ++lines;
            has_previous = false;
            continue;
        }

        const Font* font = fonts.pick(point.value);
        const u32 glyph_index = font != nullptr ? font->glyph_index(point.value) : 0u;
        if (font == nullptr || glyph_index == 0) {
            ++metrics.missing;
            continue;
        }
        const f32 scale = static_cast<f32>(style.size_px) / font->metrics().units_per_em;
        const f32 advance = font->advance(glyph_index) * scale + style.letter_spacing;

        // Wrap before drawing the character that would overflow the line.
        if (max_width > 0.0f && pen_x > 0.0f && pen_x + advance > max_width) {
            const bool may_break = !has_previous || can_break_between(previous, point.value);
            if (may_break) {
                metrics.width = std::max(metrics.width, line_width);
                pen_x = 0.0f;
                line_width = 0.0f;
                pen_y += line_height;
                ++lines;
                has_previous = false;
            }
        }

        PlacedGlyph placed;
        placed.font = font;
        placed.glyph = glyph_index;
        placed.x = pen_x;
        // The pen is the top of the line box, so the baseline of the line is one ascent below it.
        placed.y = pen_y + box.ascent;
        out.push_back(placed);

        pen_x += advance;
        line_width = std::max(line_width, pen_x);
        previous = point.value;
        has_previous = true;
        ++metrics.characters;
    }
    metrics.width = std::max(metrics.width, line_width);
    metrics.lines = lines;
    metrics.height = static_cast<f32>(lines) * line_height;
}

} // namespace t2d
