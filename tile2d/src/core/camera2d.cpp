#include <t2d/core/camera2d.h>

#include <algorithm>
#include <cmath>

namespace t2d {

Camera2D::Camera2D(Vec2 viewport, f32 zoom, Vec2 world_centre) {
    set_viewport(viewport);
    set_zoom(zoom);
    centre_ = world_centre;
}

void Camera2D::set_viewport(Vec2 size) {
    viewport_ = Vec2{std::max(size.x, 0.0f), std::max(size.y, 0.0f)};
}

void Camera2D::set_zoom(f32 pixels_per_unit) {
    if (!(pixels_per_unit > 0.0f)) pixels_per_unit = min_zoom_;   // also catches NaN
    zoom_ = clamp(pixels_per_unit, min_zoom_, max_zoom_);
}

void Camera2D::set_zoom_limits(f32 min_zoom, f32 max_zoom) {
    min_zoom_ = std::max(min_zoom, 1e-4f);
    max_zoom_ = std::max(max_zoom, min_zoom_);
    set_zoom(zoom_);
}

void Camera2D::pan(Vec2 screen_delta) {
    // The map follows the pointer: a drag to the right moves the view to the left.
    centre_ -= screen_delta / zoom_;
}

void Camera2D::move(Vec2 world_delta) { centre_ += world_delta; }

Vec2 Camera2D::screen_of(Vec2 world) const {
    return (world - centre_) * zoom_ + viewport_centre();
}

Vec2 Camera2D::world_of(Vec2 screen) const {
    return (screen - viewport_centre()) / zoom_ + centre_;
}

Aabb2 Camera2D::visible_world() const {
    return Aabb2{world_of(Vec2{0.0f, 0.0f}), world_of(viewport_)};
}

TileRect Camera2D::visible_cells(f32 cell_size) const {
    if (!(cell_size > 0.0f)) cell_size = 1.0f;
    const Aabb2 world = visible_world();
    // A viewport with no size (a window that is being resized, a hidden window) shows no cell at
    // all: without this a fractional centre would still report the one cell it sits in.
    if (!(world.max.x > world.min.x) || !(world.max.y > world.min.y)) return TileRect{};
    const i32 x0 = floor_to_i32(world.min.x / cell_size);
    const i32 y0 = floor_to_i32(world.min.y / cell_size);
    const i32 x1 = ceil_to_i32(world.max.x / cell_size);
    const i32 y1 = ceil_to_i32(world.max.y / cell_size);
    return TileRect{x0, y0, x1 - x0, y1 - y0};
}

bool Camera2D::sees(Vec2 world) const {
    return visible_world().contains(world);
}

bool Camera2D::sees(const Aabb2& world) const { return visible_world().overlaps(world); }

void Camera2D::zoom_at(Vec2 screen_anchor, f32 factor) {
    const Vec2 world = world_of(screen_anchor);
    const f32 before = zoom_;
    set_zoom(zoom_ * factor);
    if (zoom_ == before) return;   // clamped: a zoom that changes nothing must not move the view
    // Solved from screen_of(): the anchor's world point stays under the anchor.
    centre_ = world - (screen_anchor - viewport_centre()) / zoom_;
}

void Camera2D::fit(Aabb2 world_bounds, f32 margin_px) {
    const Vec2 bounds = world_bounds.size();
    const Vec2 usable{std::max(viewport_.x - 2.0f * margin_px, 0.0f),
                      std::max(viewport_.y - 2.0f * margin_px, 0.0f)};
    centre_ = world_bounds.center();
    if (!(bounds.x > 0.0f) || !(bounds.y > 0.0f) || !(usable.x > 0.0f) || !(usable.y > 0.0f)) return;
    set_zoom(std::min(usable.x / bounds.x, usable.y / bounds.y));
}

void Camera2D::scroll_to_show(Aabb2 world_rect, f32 margin_px) {
    const Aabb2 view = visible_world();
    const f32 margin = std::max(margin_px, 0.0f) / zoom_;   // screen margin in world units
    // The least move that brings the rectangle inside, one axis at a time: a cursor that is already
    // visible must not move the view at all.
    if (world_rect.min.x < view.min.x + margin) centre_.x -= (view.min.x + margin) - world_rect.min.x;
    else if (world_rect.max.x > view.max.x - margin) centre_.x += world_rect.max.x - (view.max.x - margin);
    if (world_rect.min.y < view.min.y + margin) centre_.y -= (view.min.y + margin) - world_rect.min.y;
    else if (world_rect.max.y > view.max.y - margin) centre_.y += world_rect.max.y - (view.max.y - margin);
}

void Camera2D::clamp_to(Aabb2 world_bounds) {
    const Vec2 half = viewport_ * 0.5f / zoom_;
    if (world_bounds.width() <= half.x * 2.0f) centre_.x = world_bounds.center().x;
    else centre_.x = clamp(centre_.x, world_bounds.min.x + half.x, world_bounds.max.x - half.x);
    if (world_bounds.height() <= half.y * 2.0f) centre_.y = world_bounds.center().y;
    else centre_.y = clamp(centre_.y, world_bounds.min.y + half.y, world_bounds.max.y - half.y);
}

} // namespace t2d
