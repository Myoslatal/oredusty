#include <t2d/render/glyph_atlas.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <format>

namespace t2d {

Scope<GlyphAtlas> GlyphAtlas::create(ore::rhi::GraphicsContext& context, const Options& options) {
    Scope<GlyphAtlas> atlas(new GlyphAtlas());
    atlas->options_ = options;
    atlas->context_ = &context;
    if (options.page_size == 0 || options.max_pages == 0) {
        T2D_ERROR("glyph atlas: page_size and max_pages must be non zero");
        return nullptr;
    }
    atlas->pages_.resize(options.max_pages);
    // White with the coverage in alpha: the sprite shader multiplies the texel by the vertex colour,
    // so a glyph can be tinted any colour the interface wants.
    const ore::Image empty = ore::Image::create(options.page_size, options.page_size, ore::make_rgba(255, 255, 255, 0));
    for (u32 index = 0; index < options.max_pages; ++index) {
        Page& page = atlas->pages_[index];
        page.texture = context.create_texture(empty, false, std::format("glyph.atlas.{}", index));
        if (page.texture == nullptr) {
            T2D_ERROR("glyph atlas: page {} could not be created", index);
            return nullptr;
        }
    }
    // Reserve the first pixel of page 0 as opaque white: rectangles are drawn from the same texture as
    // the text, so the batch needs one texel that is guaranteed to be solid.
    const ore::Image white = ore::Image::create(1, 1, ore::make_rgba(255, 255, 255, 255));
    context.update_texture_region(*atlas->pages_[0].texture, white, 0, 0);
    atlas->pages_[0].cursor_x = 1;
    atlas->pages_[0].row_height = 1;
    atlas->white_uv_ = Vec2{0.5f / static_cast<f32>(options.page_size), 0.5f / static_cast<f32>(options.page_size)};
    T2D_DEBUG("glyph atlas: {} page(s) of {}x{}", options.max_pages, options.page_size, options.page_size);
    return atlas;
}

GlyphAtlas::~GlyphAtlas() = default;

const GlyphSlot* GlyphAtlas::glyph(const Font& font, u32 glyph_index, u16 size_px) {
    if (glyph_index == 0 || size_px == 0) return nullptr;
    const Key key{&font, glyph_index, size_px};
    if (const auto found = cache_.find(key); found != cache_.end()) return &found->second;
    return insert(key);
}

const GlyphSlot* GlyphAtlas::glyph_for_codepoint(const Font& font, u32 codepoint, u16 size_px) {
    const u32 index = font.glyph_index(codepoint);
    if (index == 0) return nullptr;
    return glyph(font, index, size_px);
}

const GlyphSlot* GlyphAtlas::insert(const Key& key) {
    const f32 scale = static_cast<f32>(key.size) / key.font->metrics().units_per_em;
    GlyphPath path;
    if (!key.font->glyph_path(key.glyph, path)) {
        T2D_WARN("glyph atlas: glyph {} of '{}' has no outline", key.glyph, key.font->family_name());
        return nullptr;
    }
    const GlyphBitmap bitmap = rasterize_glyph(path, scale, options_.padding);

    const u32 slot_width = std::max(bitmap.bitmap.width, 1u);
    const u32 slot_height = std::max(bitmap.bitmap.height, 1u);
    if (slot_width > options_.page_size || slot_height > options_.page_size) {
        ++dropped_;
        T2D_WARN("glyph atlas: glyph {} at {} px is larger than a page", key.glyph, key.size);
        return nullptr;
    }

    for (u32 index = 0; index < pages_.size(); ++index) {
        Page& page = pages_[index];
        // Shelf packing: fill a row left to right, then start a new row above it.
        if (page.cursor_x + slot_width > options_.page_size) {
            page.cursor_x = 0;
            page.cursor_y += page.row_height;
            page.row_height = 0;
        }
        if (page.cursor_y + slot_height > options_.page_size) continue; // this page is full

        const u32 x = page.cursor_x;
        const u32 y = page.cursor_y;
        if (!bitmap.bitmap.empty()) {
            // Only the glyph's own rectangle is built and uploaded: keeping a CPU copy of the whole
            // page would cost 16 MB per page for nothing.
            ore::Image patch = ore::Image::create(bitmap.bitmap.width, bitmap.bitmap.height,
                                                 ore::make_rgba(255, 255, 255, 0));
            for (u32 row = 0; row < bitmap.bitmap.height; ++row) {
                for (u32 column = 0; column < bitmap.bitmap.width; ++column) {
                    const u8 coverage = bitmap.bitmap.at(column, row);
                    if (coverage == 0) continue;
                    patch.set_pixel(column, row, ore::make_rgba(255, 255, 255, coverage));
                }
            }
            context_->update_texture_region(*page.texture, patch, x, y);
            uploaded_bytes_ += static_cast<u64>(bitmap.bitmap.width) * bitmap.bitmap.height * 4u;
        }
        page.cursor_x += slot_width;
        page.row_height = std::max(page.row_height, slot_height);

        GlyphSlot slot;
        const f32 inverse = 1.0f / static_cast<f32>(options_.page_size);
        slot.uv = Aabb2{Vec2{static_cast<f32>(x) * inverse, static_cast<f32>(y) * inverse},
                        Vec2{static_cast<f32>(x + slot_width) * inverse, static_cast<f32>(y + slot_height) * inverse}};
        slot.left = bitmap.left;
        slot.top = bitmap.top;
        slot.width = bitmap.bitmap.width;
        slot.height = bitmap.bitmap.height;
        slot.page = index;
        slot.has_ink = bitmap.has_ink;
        slot.advance = key.font->advance(key.glyph) * scale;
        const auto inserted = cache_.emplace(key, slot);
        return &inserted.first->second;
    }

    ++dropped_;
    return nullptr;
}

ore::rhi::Texture* GlyphAtlas::page(u32 index) const {
    return index < pages_.size() ? pages_[index].texture.get() : nullptr;
}

f32 GlyphAtlas::fill_ratio() const {
    if (pages_.empty()) return 0.0f;
    const f32 page_area = static_cast<f32>(options_.page_size) * static_cast<f32>(options_.page_size);
    f32 used = 0.0f;
    for (const Page& page : pages_) {
        used += static_cast<f32>(page.cursor_y + page.row_height) * static_cast<f32>(options_.page_size);
    }
    return used / (page_area * static_cast<f32>(pages_.size()));
}

} // namespace t2d
