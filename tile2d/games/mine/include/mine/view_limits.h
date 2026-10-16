// Mine - how far the game's camera may zoom.
//
// The designer's numbers (docs/GAME_DESIGN.md section 1.30): in the **world view** a cell is drawn
// between **40 and 100 pixels**. Below that a player sees so much of the mine that a cell is a speck and
// the pointer cannot pick anything out; above it one cell fills the screen and there is nothing left to
// look at.
//
// The **sandbox** is a debug tool and keeps its own much wider range (sandbox.h): it exists to look at
// content from any distance - a whole 4096 cell map at once, or one cell's art filling the window - not
// to play the game. Two views, two ranges, on purpose.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

namespace mine {

using t2d::f32;

/// The zoom range of the world view's camera, in pixels per cell.
inline constexpr f32 kMinViewZoom = 40.0f;
inline constexpr f32 kMaxViewZoom = 100.0f;

/// \p zoom brought into that range. Every way of changing the zoom goes through it: the wheel, +/-,
/// `--view`, and fitting the layer - which can then only fit as far as the limit allows, because a map
/// of a hundred cells a side is bigger than the window at 40 pixels a cell and the limit wins.
[[nodiscard]] constexpr f32 clamp_view_zoom(f32 zoom) {
    return t2d::clamp(zoom, kMinViewZoom, kMaxViewZoom);
}

} // namespace mine
