#include <mefemma/draw_list.h>

#include <algorithm>

namespace mefemma {

void DrawList::clear() {
    commands_.clear();
    rects_ = 0;
    sprites_ = 0;
    texts_ = 0;
}

void DrawList::rect(const Aabb2& rect, u32 color) {
    DrawCommand command;
    command.kind = DrawCommand::Kind::Rect;
    command.rect = rect;
    command.color = color;
    commands_.push_back(std::move(command));
    ++rects_;
}

void DrawList::rect_outline(const Aabb2& bounds, t2d::f32 thickness, u32 color) {
    if (!(thickness > 0.0f)) return;
    const t2d::f32 width = bounds.width();
    const t2d::f32 height = bounds.height();
    if (width <= 0.0f || height <= 0.0f) return;
    // A line thicker than half the box would make the four sides overlap into a filled box; drawing it as
    // one rectangle says the same thing without pretending the outline is still an outline.
    if (thickness * 2.0f >= std::min(width, height)) {
        rect(bounds, color);
        return;
    }
    const auto side = [&](t2d::f32 x0, t2d::f32 y0, t2d::f32 x1, t2d::f32 y1) {
        rect(Aabb2{Vec2{x0, y0}, Vec2{x1, y1}}, color);
    };
    side(bounds.min.x, bounds.min.y, bounds.max.x, bounds.min.y + thickness);                        // top
    side(bounds.min.x, bounds.max.y - thickness, bounds.max.x, bounds.max.y);                        // bottom
    side(bounds.min.x, bounds.min.y + thickness, bounds.min.x + thickness, bounds.max.y - thickness); // left
    side(bounds.max.x - thickness, bounds.min.y + thickness, bounds.max.x, bounds.max.y - thickness); // right
}

void DrawList::sprite(std::string texture, const Aabb2& rect, const Aabb2& uv, u32 tint) {
    DrawCommand command;
    command.kind = DrawCommand::Kind::Sprite;
    command.rect = rect;
    command.uv = uv;
    command.color = tint;
    command.texture = std::move(texture);
    commands_.push_back(std::move(command));
    ++sprites_;
}

void DrawList::text(std::string text, Vec2 pen, u16 size_px, u32 color, t2d::f32 max_width) {
    if (text.empty()) return;
    DrawCommand command;
    command.kind = DrawCommand::Kind::Text;
    command.pen = pen;
    command.color = color;
    command.size_px = size_px;
    command.max_width = max_width;
    command.text = std::move(text);
    commands_.push_back(std::move(command));
    ++texts_;
}

} // namespace mefemma
