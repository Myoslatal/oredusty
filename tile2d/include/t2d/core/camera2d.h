// Tile2D - the 2D camera: which part of the world a viewport shows.
//
// The camera is pure arithmetic - no window, no GPU, no clock - so input code can ask it what is under
// the pointer and drawing code can ask it where a cell lands, and both get the same answer. That
// matters for a god view (docs/GAME_DESIGN.md section 1): with no player character the camera *is*
// where the player is looking, so a picking bug is a gameplay bug.
//
// Conventions, the same ones the rest of the framework uses:
//   * screen space is pixels with the origin at the viewport's top left corner, +Y points down;
//   * world space belongs to the caller. A tile game uses one world unit per cell, which makes zoom()
//     "pixels per cell" and the camera's centre a cell coordinate; a caller with its own world units
//     uses them here unchanged;
//   * zoom is pixels per world unit, never a fraction: a camera that fits a 512x512 map into a
//     1280x720 viewport sits at zoom ~1.4.
//
// The camera never decides what may be visible: it can look past the edge of a map (a cursor may sit
// outside one), and clamp_to() is there for the callers that want a bounded view instead.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

namespace t2d {

class Camera2D {
public:
    /// Zoom range used until a caller narrows it: from "a 4096 cell map fits a small window" to "one
    /// cell fills a 4K screen". Both ends are clamped rather than wrapped, so a scroll wheel that is
    /// held down cannot run the camera through zero and mirror the world.
    static constexpr f32 kDefaultMinZoom = 0.01f;
    static constexpr f32 kDefaultMaxZoom = 4096.0f;

    Camera2D() = default;
    /// A camera with a viewport and a zoom, centred on \p world_centre.
    Camera2D(Vec2 viewport, f32 zoom, Vec2 world_centre = Vec2{});

    // --- the viewport (screen pixels) ---------------------------------------
    /// Negative sizes are clamped to zero: a window that is being resized can report a silly size for
    /// one frame, and every other query has to stay finite.
    void set_viewport(Vec2 size);
    [[nodiscard]] Vec2 viewport() const { return viewport_; }
    [[nodiscard]] Vec2 viewport_centre() const { return viewport_ * 0.5f; }

    // --- the view (world) ---------------------------------------------------
    [[nodiscard]] Vec2 centre() const { return centre_; }
    void set_centre(Vec2 world) { centre_ = world; }
    /// Puts \p world at the centre of the viewport.
    void look_at(Vec2 world) { centre_ = world; }
    [[nodiscard]] f32 zoom() const { return zoom_; }
    void set_zoom(f32 pixels_per_unit);
    [[nodiscard]] f32 min_zoom() const { return min_zoom_; }
    [[nodiscard]] f32 max_zoom() const { return max_zoom_; }
    /// Sets the range the zoom is clamped to, and clamps the current zoom into it.
    void set_zoom_limits(f32 min_zoom, f32 max_zoom);

    // --- moving -------------------------------------------------------------
    /// Moves the view by a screen space delta: what dragging the map with the mouse does.
    void pan(Vec2 screen_delta);
    /// Moves the view by a world space delta.
    void move(Vec2 world_delta);
    /// Zooms by \p factor (1.15 for one wheel notch, say) keeping the world point under
    /// \p screen_anchor exactly where it is: the reason to zoom is to look closer at what you are
    /// already looking at. A factor that the limits clamp moves nothing at all.
    void zoom_at(Vec2 screen_anchor, f32 factor);
    /// The same, about the centre of the viewport.
    void zoom_by(f32 factor) { zoom_at(viewport_centre(), factor); }

    // --- fitting and bounding ----------------------------------------------
    /// Frames \p world_bounds: the zoom is the one that makes them fit inside the viewport with
    /// \p margin_px of screen space on every side, and the centre is their centre. A degenerate box
    /// (or a zero sized viewport) only moves the camera, never the zoom.
    void fit(Aabb2 world_bounds, f32 margin_px = 0.0f);
    /// Moves the camera the least amount that brings \p world_rect into view, keeping \p margin_px
    /// of screen space around it. This is what walks a cursor back on screen.
    void scroll_to_show(Aabb2 world_rect, f32 margin_px = 0.0f);
    /// Keeps the visible area inside \p world_bounds: the view stops at the edge of the map instead
    /// of wandering into the void, and a map smaller than the viewport stays centred.
    void clamp_to(Aabb2 world_bounds);

    // --- queries ------------------------------------------------------------
    [[nodiscard]] Vec2 screen_of(Vec2 world) const;
    [[nodiscard]] Vec2 world_of(Vec2 screen) const;
    /// The world rectangle the viewport covers, in world units.
    [[nodiscard]] Aabb2 visible_world() const;
    /// The cells the viewport touches, half open like TileRect: the culling range a renderer walks.
    /// \p cell_size is the world size of one cell (1.0 for a world measured in cells).
    [[nodiscard]] TileRect visible_cells(f32 cell_size = 1.0f) const;
    [[nodiscard]] bool sees(Vec2 world) const;
    [[nodiscard]] bool sees(const Aabb2& world) const;

private:
    Vec2 viewport_{};
    Vec2 centre_{};
    f32 zoom_ = 1.0f;
    f32 min_zoom_ = kDefaultMinZoom;
    f32 max_zoom_ = kDefaultMaxZoom;
};

} // namespace t2d
