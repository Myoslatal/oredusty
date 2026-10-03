// Tile2D - the glyph atlas: rasterised glyphs packed into texture pages.
//
// Glyphs are cached by (font, glyph, pixel size) and packed with a shelf packer, so a UI that mixes
// Latin and CJK only rasterises each character once. A page holds a few thousand glyphs, which is far
// more than an interface needs; a glyph that no longer fits is counted and reported rather than
// silently dropped.
#pragma once

#include <ore/rhi/context.h>
#include <ore/rhi/texture.h>

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/text/font.h>
#include <t2d/text/raster.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace t2d {

/// Where a glyph lives in the atlas and how it sits relative to the pen.
struct GlyphSlot {
    Aabb2 uv{};            ///< normalised rectangle inside the page
    i32 left = 0;          ///< pixels from the pen to the bitmap's left edge
    i32 top = 0;           ///< pixels from the baseline up to the bitmap's top edge (y down, so negative above)
    u32 width = 0;
    u32 height = 0;
    u32 page = 0;
    bool has_ink = false;
    f32 advance = 0.0f;    ///< pixels
};

class GlyphAtlas {
public:
    struct Options {
        u32 page_size = 2048;
        u32 max_pages = 1;
        u32 padding = 1;
    };

    [[nodiscard]] static Scope<GlyphAtlas> create(ore::rhi::GraphicsContext& context, const Options& options);
    ~GlyphAtlas();
    T2D_NON_MOVABLE(GlyphAtlas);

    /// The slot for one glyph, rasterising and uploading it on first use. nullptr when the atlas is
    /// full (see dropped()).
    [[nodiscard]] const GlyphSlot* glyph(const Font& font, u32 glyph_index, u16 size_px);
    /// The glyph for a code point of \p font, or nullptr when the font has no such character.
    [[nodiscard]] const GlyphSlot* glyph_for_codepoint(const Font& font, u32 codepoint, u16 size_px);

    [[nodiscard]] ore::rhi::Texture* page(u32 index) const;
    /// UV of an opaque white texel reserved at the top left of page 0, so filled rectangles can be
    /// drawn from the same texture as the text (SpriteBatch::set_white_texel).
    [[nodiscard]] Vec2 white_uv() const { return white_uv_; }
    [[nodiscard]] u32 page_count() const { return static_cast<u32>(pages_.size()); }
    [[nodiscard]] usize cached_glyphs() const { return cache_.size(); }
    [[nodiscard]] u32 dropped_glyphs() const { return dropped_; }
    [[nodiscard]] u64 uploaded_bytes() const { return uploaded_bytes_; }
    [[nodiscard]] f32 fill_ratio() const;

private:
    GlyphAtlas() = default;

    struct Key {
        const Font* font = nullptr;
        u32 glyph = 0;
        u16 size = 0;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        [[nodiscard]] usize operator()(const Key& key) const noexcept {
            usize hash = reinterpret_cast<usize>(key.font);
            hash ^= static_cast<usize>(key.glyph) * 0x9E3779B97F4A7C15ull;
            hash ^= static_cast<usize>(key.size) << 7;
            return hash;
        }
    };
    struct Page {
        Scope<ore::rhi::Texture> texture;
        u32 cursor_x = 0;
        u32 cursor_y = 0;
        u32 row_height = 0;
    };

    [[nodiscard]] const GlyphSlot* insert(const Key& key);

    Options options_{};
    ore::rhi::GraphicsContext* context_ = nullptr;
    std::vector<Page> pages_;
    std::unordered_map<Key, GlyphSlot, KeyHash> cache_;
    u32 dropped_ = 0;
    u64 uploaded_bytes_ = 0;
    Vec2 white_uv_{0.0f, 0.0f};
};

} // namespace t2d
