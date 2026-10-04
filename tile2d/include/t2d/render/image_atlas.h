// Tile2D - packing images into one page so a batch can draw all of them with one texture.
//
// A sprite batch binds one texture per batch, so drawing N images means N batches - which is fine for
// a handful and wrong for a tile set. This packs them into a fixed grid of cells instead: add() copies
// an image into the next free cell and remembers where it went, and the caller draws every one of them
// with the same texture.
//
// An image larger than a cell is refused rather than scaled: silently scaling art is worse than saying
// no, and a caller that wants a different cell size can ask for one.
//
// The mine game does **not** use this for its content art: the designer's rule is one picture per
// content entry, in whatever size it was drawn in, drawn from its own texture
// (docs/GAME_DESIGN.md section 1.18). What still wants a page is the other case - many small pictures
// that are always drawn together (UI icons, particles) - so this stays a framework utility with no
// user in the game rather than being deleted.
#pragma once

#include <ore/rhi/context.h>
#include <ore/rhi/texture.h>

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <string>
#include <string_view>
#include <unordered_map>

namespace ore {
struct Image;
}

namespace t2d {

using t2d::Scope;

class ImageAtlas {
public:
    struct Options {
        u32 page_size = 1024;
        /// One cell per image; an image must fit inside it.
        u32 cell_size = 64;
        bool mipmaps = false;
    };

    [[nodiscard]] static Scope<ImageAtlas> create(ore::rhi::GraphicsContext& context, const Options& options);
    ~ImageAtlas();
    T2D_NON_MOVABLE(ImageAtlas);

    /// Copies \p image into the next free cell and remembers it under \p key. False (and dropped() +1)
    /// when the page is full or the image does not fit a cell.
    bool add(std::string_view key, const ore::Image& image);
    [[nodiscard]] bool has(std::string_view key) const;
    /// The image's own rectangle inside the page, normalised and inset by half a texel so a linear
    /// filter cannot sample the neighbouring cell. An unknown key gives an empty rectangle.
    [[nodiscard]] Aabb2 uv(std::string_view key) const;

    [[nodiscard]] u32 page_size() const { return options_.page_size; }
    [[nodiscard]] u32 cell_size() const { return options_.cell_size; }
    [[nodiscard]] usize count() const { return uvs_.size(); }
    [[nodiscard]] usize capacity() const;
    [[nodiscard]] u32 dropped() const { return dropped_; }
    [[nodiscard]] ore::rhi::Texture& texture() const { return *texture_; }

private:
    ImageAtlas() = default;

    Options options_{};
    ore::rhi::GraphicsContext* context_ = nullptr;
    Scope<ore::rhi::Texture> texture_;
    std::unordered_map<std::string, Aabb2> uvs_;
    u32 next_cell_ = 0;
    u32 dropped_ = 0;
};

} // namespace t2d
