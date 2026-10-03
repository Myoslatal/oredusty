#include <t2d/render/text_renderer.h>

#include <t2d/core/log.h>
#include <t2d/text/utf8.h>

#include <algorithm>
#include <cmath>

namespace t2d {

const Font* FontSet::pick(u32 codepoint) const {
    const Font* preferred = is_cjk(codepoint) && cjk != nullptr ? cjk : latin;
    if (preferred != nullptr && preferred->glyph_index(codepoint) != 0) return preferred;
    const Font* other = preferred == latin ? cjk : latin;
    if (other != nullptr && other->glyph_index(codepoint) != 0) return other;
    return preferred != nullptr ? preferred : other;
}

Scope<TextRenderer> TextRenderer::create(ore::rhi::GraphicsContext& context, const GlyphAtlas::Options& options) {
    Scope<GlyphAtlas> atlas = GlyphAtlas::create(context, options);
    if (atlas == nullptr) return nullptr;
    Scope<TextRenderer> renderer(new TextRenderer());
    renderer->atlas_ = std::move(atlas);
    return renderer;
}

TextRenderer::~TextRenderer() = default;

void TextRenderer::layout(std::string_view text, const FontSet& fonts, const TextStyle& style, f32 max_width,
                          bool rasterise, std::vector<Placed>& out, TextMetrics& metrics) const {
    out.clear();
    metrics = TextMetrics{};
    if (fonts.latin == nullptr && fonts.cjk == nullptr) return;

    const f32 line_height = static_cast<f32>(style.size_px) * style.line_spacing;
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

        if (rasterise) {
            const GlyphSlot* slot = atlas_->glyph(*font, glyph_index, style.size_px);
            if (slot != nullptr && slot->has_ink) {
                Placed placed;
                placed.slot = slot;
                placed.x = pen_x;
                placed.y = pen_y;
                out.push_back(placed);
            }
        }
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

TextMetrics TextRenderer::measure(std::string_view text, const FontSet& fonts, const TextStyle& style,
                                  f32 max_width) const {
    std::vector<Placed> ignored;
    TextMetrics metrics;
    layout(text, fonts, style, max_width, false, ignored, metrics);
    return metrics;
}

f32 TextRenderer::draw(SpriteBatch& batch, std::string_view text, const FontSet& fonts, const TextStyle& style,
                       Vec2 pen, f32 max_width) {
    std::vector<Placed> placed;
    TextMetrics metrics;
    layout(text, fonts, style, max_width, true, placed, metrics);

    const f32 inverse = 1.0f / static_cast<f32>(style.size_px);
    for (const Placed& item : placed) {
        const GlyphSlot& slot = *item.slot;
        // The bitmap's left/top are pixel offsets relative to the pen at the glyph's own size, so they
        // are scaled by 1/size to stay correct if the slot was rasterised at another size.
        const f32 left = static_cast<f32>(slot.left) * inverse * static_cast<f32>(style.size_px);
        const f32 top = static_cast<f32>(slot.top) * inverse * static_cast<f32>(style.size_px);
        const Aabb2 rect{Vec2{pen.x + item.x + left, pen.y + item.y + top},
                         Vec2{pen.x + item.x + left + static_cast<f32>(slot.width),
                              pen.y + item.y + top + static_cast<f32>(slot.height)}};
        batch.draw_quad(rect, slot.uv, style.color);
    }
    return metrics.height;
}

} // namespace t2d
