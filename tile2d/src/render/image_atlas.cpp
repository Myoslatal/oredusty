#include <t2d/render/image_atlas.h>

#include <t2d/core/log.h>

#include <ore/core/image.h>

#include <format>

namespace t2d {

Scope<ImageAtlas> ImageAtlas::create(ore::rhi::GraphicsContext& context, const Options& options) {
    Scope<ImageAtlas> atlas(new ImageAtlas());
    atlas->options_ = options;
    atlas->context_ = &context;
    if (options.page_size == 0 || options.cell_size == 0 || options.cell_size > options.page_size) {
        T2D_ERROR("image atlas: a {} page cannot hold {} cells", options.page_size, options.cell_size);
        return nullptr;
    }
    // An empty page, uploaded once per add(): a pack's art is small and loading happens between frames,
    // so re-uploading the region an image lands in is cheaper than keeping a second copy on the CPU.
    const ore::Image empty = ore::Image::create(options.page_size, options.page_size, 0u);
    atlas->texture_ = context.create_texture(empty, options.mipmaps, "image.atlas");
    if (atlas->texture_ == nullptr) {
        T2D_ERROR("image atlas: the page texture could not be created");
        return nullptr;
    }
    T2D_DEBUG("image atlas: {}x{} page, {} cells of {}", options.page_size, options.page_size,
              atlas->capacity(), options.cell_size);
    return atlas;
}

ImageAtlas::~ImageAtlas() = default;

usize ImageAtlas::capacity() const {
    const u32 cells = options_.page_size / options_.cell_size;
    return static_cast<usize>(cells) * cells;
}

bool ImageAtlas::add(std::string_view key, const ore::Image& image) {
    if (key.empty() || image.empty()) return false;
    if (uvs_.find(std::string(key)) != uvs_.end()) return false;   // one cell per key
    if (image.width > options_.cell_size || image.height > options_.cell_size) {
        T2D_WARN("image atlas: '{}' is {}x{}, which does not fit a {} cell", key, image.width, image.height,
                 options_.cell_size);
        ++dropped_;
        return false;
    }
    if (next_cell_ >= capacity()) {
        T2D_WARN("image atlas: the page is full ({} cells), '{}' was dropped", capacity(), key);
        ++dropped_;
        return false;
    }
    const u32 cells_per_row = options_.page_size / options_.cell_size;
    const u32 column = next_cell_ % cells_per_row;
    const u32 row = next_cell_ / cells_per_row;
    ++next_cell_;

    const u32 x = column * options_.cell_size;
    const u32 y = row * options_.cell_size;
    context_->update_texture_region(*texture_, image, x, y);

    // Half a texel of inset: with a linear filter the edge of an image would otherwise sample the
    // neighbouring cell, which shows up as a one pixel fringe of somebody else's art.
    const f32 page = static_cast<f32>(options_.page_size);
    const f32 half = 0.5f / page;
    uvs_.emplace(std::string(key),
                 Aabb2{Vec2{static_cast<f32>(x) / page + half, static_cast<f32>(y) / page + half},
                       Vec2{static_cast<f32>(x + image.width) / page - half,
                            static_cast<f32>(y + image.height) / page - half}});
    return true;
}

bool ImageAtlas::has(std::string_view key) const { return uvs_.find(std::string(key)) != uvs_.end(); }

Aabb2 ImageAtlas::uv(std::string_view key) const {
    const auto found = uvs_.find(std::string(key));
    if (found == uvs_.end()) return Aabb2{Vec2{}, Vec2{}};
    return found->second;
}

} // namespace t2d
