// Tile2D - laying out and drawing text: UTF-8 in, batched quads out.
//
// The renderer is language agnostic in the only way that matters: it walks the string one code point
// at a time, asks the font set which font owns that character, and lays the result out with real
// metrics. Latin text uses the Latin font, Chinese text uses the CJK font, and a line can mix both.
// Line breaking follows the small set of rules a game interface needs (break after a space, break
// between CJK characters, never before closing punctuation or after opening punctuation).
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/render/glyph_atlas.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/text/font.h>

#include <string_view>
#include <vector>

namespace t2d {

/// The fonts a language needs. \p cjk may be null for a Latin only interface; \p latin is required.
struct FontSet {
    const Font* latin = nullptr;
    const Font* cjk = nullptr;

    /// The font that should draw \p codepoint: CJK characters go to the CJK font, everything else to
    /// the Latin one, and a character the chosen font lacks falls back to the other.
    [[nodiscard]] const Font* pick(u32 codepoint) const;
};

struct TextStyle {
    u16 size_px = 16;
    u32 color = 0xFFFFFFFFu;
    f32 line_spacing = 1.25f;
    f32 letter_spacing = 0.0f;
};

struct TextMetrics {
    f32 width = 0.0f;   ///< widest line
    f32 height = 0.0f;  ///< all lines including spacing
    u32 lines = 0;
    usize characters = 0;
    /// Characters no font in the set had. Reported rather than silently skipped: a missing glyph is a
    /// content or font problem, not something to hide.
    usize missing = 0;
};

class TextRenderer {
public:
    [[nodiscard]] static Scope<TextRenderer> create(ore::rhi::GraphicsContext& context,
                                                    const GlyphAtlas::Options& options);
    ~TextRenderer();
    T2D_NON_MOVABLE(TextRenderer);

    /// Measures without rasterising anything (advances only), so it is cheap enough for layout code.
    [[nodiscard]] TextMetrics measure(std::string_view text, const FontSet& fonts, const TextStyle& style,
                                      f32 max_width = 0.0f) const;

    /// Draws \p text with its top-left corner at \p pen (y down). Lines longer than \p max_width wrap.
    /// The batch must already be begun with texture(): every glyph lives on page 0, and a glyph that
    /// did not fit is counted in atlas().dropped_glyphs().
    [[nodiscard]] f32 draw(SpriteBatch& batch, std::string_view text, const FontSet& fonts,
                           const TextStyle& style, Vec2 pen, f32 max_width = 0.0f);

    [[nodiscard]] ore::rhi::Texture* texture() const { return atlas_->page(0); }
    [[nodiscard]] GlyphAtlas& atlas() { return *atlas_; }
    [[nodiscard]] const GlyphAtlas& atlas() const { return *atlas_; }

private:
    TextRenderer() = default;

    struct Placed {
        const GlyphSlot* slot = nullptr;
        f32 x = 0.0f;
        f32 y = 0.0f;
    };

    /// Shared by measure() and draw(): with \p rasterise false no glyph is rasterised, which is what
    /// keeps measuring cheap.
    void layout(std::string_view text, const FontSet& fonts, const TextStyle& style, f32 max_width,
                bool rasterise, std::vector<Placed>& out, TextMetrics& metrics) const;

    Scope<GlyphAtlas> atlas_;
};

} // namespace t2d
