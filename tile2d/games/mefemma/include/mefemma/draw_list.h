// Mefemma - what a scene asks to be drawn, in the order it asks.
//
// A scene does not touch the GPU. It fills a DrawList with commands - rectangles, pictures and lines of
// text - and the renderer turns the list into batches. That split is what makes a scene's drawing
// testable without a window: a test compares the commands a scene submitted, and the same list is what
// the real renderer consumes.
//
// The order is the drawing order. The renderer preserves it, flushing a batch when the texture changes,
// so a picture drawn after a rectangle really is drawn after it.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <string>
#include <vector>

namespace mefemma {

using t2d::Aabb2;
using t2d::Vec2;
using t2d::u16;
using t2d::u32;
using t2d::u8;
using t2d::usize;

/// A colour in the layout the renderer uses: 0xAABBGGRR (red in the low byte). The same layout
/// t2d::SpriteBatch reads, so a colour authored here is drawn as authored.
[[nodiscard]] constexpr u32 rgba(u8 red, u8 green, u8 blue, u8 alpha = 255) {
    return static_cast<u32>(red) | (static_cast<u32>(green) << 8) | (static_cast<u32>(blue) << 16) |
           (static_cast<u32>(alpha) << 24);
}

/// No tint: a picture drawn as its file says.
inline constexpr u32 kWhite = 0xFFFFFFFFu;
/// The uv rectangle of a whole picture: one content entry, one texture, no atlas.
[[nodiscard]] constexpr Aabb2 full_uv() { return Aabb2{Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}}; }

/// One command. It is a value, deliberately plain: a test can compare it, a log can print it, and the
/// renderer has no state a command depends on beyond the texture name.
struct DrawCommand {
    enum class Kind : u8 { Rect, Sprite, Text };

    Kind kind = Kind::Rect;
    /// Rect and Sprite: the rectangle. Text: unused.
    Aabb2 rect{};
    /// Sprite: which part of the picture. The whole picture unless a game cut it.
    Aabb2 uv = full_uv();
    /// Text: the top left corner of the first line box.
    Vec2 pen{};
    u32 color = kWhite;
    /// Text: the font size. A line box is never shorter than the font's own, so a caller that lays text
    /// out itself should measure with GameServices::measure_text().
    u16 size_px = 16;
    /// Text: wrap at this width; 0 means no wrapping.
    t2d::f32 max_width = 0.0f;
    /// Sprite: the picture's name, resolved by the renderer through the asset paths (textures/<name>.png).
    std::string texture;
    /// Text: the UTF-8 string.
    std::string text;
};

/// A frame's worth of drawing, in submission order.
class DrawList {
public:
    void clear();

    void rect(const Aabb2& rect, u32 color);
    /// Four rectangles, one per side. Thickness is in pixels and is clamped to the rectangle: a 2 px
    /// outline around a 1 px box is a filled box, not a negative one.
    void rect_outline(const Aabb2& rect, t2d::f32 thickness, u32 color);
    /// A picture from the asset paths. \p uv selects a part of it; full_uv() is all of it.
    void sprite(std::string texture, const Aabb2& rect, const Aabb2& uv = full_uv(), u32 tint = kWhite);
    /// One line of text, with the pen at the top left corner of its line box.
    void text(std::string text, Vec2 pen, u16 size_px, u32 color, t2d::f32 max_width = 0.0f);

    [[nodiscard]] const std::vector<DrawCommand>& commands() const { return commands_; }
    [[nodiscard]] bool empty() const { return commands_.empty(); }
    [[nodiscard]] usize size() const { return commands_.size(); }

    [[nodiscard]] usize rect_count() const { return rects_; }
    [[nodiscard]] usize sprite_count() const { return sprites_; }
    [[nodiscard]] usize text_count() const { return texts_; }

private:
    std::vector<DrawCommand> commands_;
    usize rects_ = 0;
    usize sprites_ = 0;
    usize texts_ = 0;
};

} // namespace mefemma
