// Tile2D - drawing laid out text: UTF-8 in, batched quads out.
//
// The layout itself is CPU work and lives in <t2d/text/text_layout.h>: this class only turns placed
// glyphs into atlas lookups and quads, so what a caller measures and what a caller sees come from
// one piece of code. The renderer is language agnostic in the only way that matters: it walks the
// string one code point at a time, asks the font set which font owns that character, and lays the
// result out with real metrics. Latin text uses the Latin font, Chinese text uses the CJK font, and
// a line can mix both.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/render/glyph_atlas.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/text/text_layout.h>

#include <string_view>

namespace t2d {

class TextRenderer {
public:
    [[nodiscard]] static Scope<TextRenderer> create(ore::rhi::GraphicsContext& context,
                                                    const GlyphAtlas::Options& options);
    ~TextRenderer();
    T2D_NON_MOVABLE(TextRenderer);

    /// Measures without rasterising anything (advances only), so it is cheap enough for layout code.
    /// The box it reports is the one draw() fills: a background drawn as [pen, pen + size] always
    /// covers the text.
    [[nodiscard]] TextMetrics measure(std::string_view text, const FontSet& fonts, const TextStyle& style,
                                      f32 max_width = 0.0f) const;

    /// Draws \p text with the top left corner of its first line box at \p pen (y down): the glyphs
    /// start one ascent below the pen, inside the box measure() reports. Lines longer than
    /// \p max_width wrap. The batch must already be begun with texture(): every glyph lives on page
    /// 0, and a glyph that did not fit is counted in atlas().dropped_glyphs().
    [[nodiscard]] f32 draw(SpriteBatch& batch, std::string_view text, const FontSet& fonts,
                           const TextStyle& style, Vec2 pen, f32 max_width = 0.0f);

    [[nodiscard]] ore::rhi::Texture* texture() const { return atlas_->page(0); }
    [[nodiscard]] GlyphAtlas& atlas() { return *atlas_; }
    [[nodiscard]] const GlyphAtlas& atlas() const { return *atlas_; }

private:
    TextRenderer() = default;

    Scope<GlyphAtlas> atlas_;
};

} // namespace t2d
