#include <t2d/render/text_renderer.h>

#include <t2d/core/log.h>

#include <vector>

namespace t2d {

Scope<TextRenderer> TextRenderer::create(ore::rhi::GraphicsContext& context, const GlyphAtlas::Options& options) {
    Scope<GlyphAtlas> atlas = GlyphAtlas::create(context, options);
    if (atlas == nullptr) return nullptr;
    Scope<TextRenderer> renderer(new TextRenderer());
    renderer->atlas_ = std::move(atlas);
    return renderer;
}

TextRenderer::~TextRenderer() = default;

TextMetrics TextRenderer::measure(std::string_view text, const FontSet& fonts, const TextStyle& style,
                                  f32 max_width) const {
    std::vector<PlacedGlyph> ignored;
    TextMetrics metrics;
    layout_text(text, fonts, style, max_width, ignored, metrics);
    return metrics;
}

f32 TextRenderer::draw(SpriteBatch& batch, std::string_view text, const FontSet& fonts, const TextStyle& style,
                       Vec2 pen, f32 max_width) {
    std::vector<PlacedGlyph> placed;
    TextMetrics metrics;
    layout_text(text, fonts, style, max_width, placed, metrics);

    const f32 inverse = 1.0f / static_cast<f32>(style.size_px);
    for (const PlacedGlyph& item : placed) {
        const GlyphSlot* slot = atlas_->glyph(*item.font, item.glyph, style.size_px);
        if (slot == nullptr || !slot->has_ink) continue;
        // The bitmap's left/top are pixel offsets relative to the pen at the glyph's own size, so they
        // are scaled by 1/size to stay correct if the slot was rasterised at another size.
        const f32 left = static_cast<f32>(slot->left) * inverse * static_cast<f32>(style.size_px);
        const f32 top = static_cast<f32>(slot->top) * inverse * static_cast<f32>(style.size_px);
        const Aabb2 rect{Vec2{pen.x + item.x + left, pen.y + item.y + top},
                         Vec2{pen.x + item.x + left + static_cast<f32>(slot->width),
                              pen.y + item.y + top + static_cast<f32>(slot->height)}};
        batch.draw_quad(rect, slot->uv, style.color);
    }
    return metrics.height;
}

} // namespace t2d
