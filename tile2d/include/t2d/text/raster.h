// Tile2D - glyph rasterising: outlines in, anti-aliased coverage out.
//
// Coverage is computed analytically, not by supersampling: for every edge and every pixel it touches,
// the exact area of the pixel covered by the edge's trapezoid is integrated, so a horizontal or
// vertical edge lands on its true fraction of a pixel and small CJK strokes stay crisp instead of
// banding at the sampling grid. The result is one byte of coverage per pixel, which is what an atlas
// page wants to hold.
#pragma once

#include <t2d/core/types.h>
#include <t2d/text/font.h>

#include <vector>

namespace t2d {

/// An 8-bit coverage bitmap, one byte per pixel, 0 = nothing, 255 = fully covered.
struct CoverageBitmap {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;

    [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
    [[nodiscard]] u8 at(u32 x, u32 y) const { return pixels[static_cast<usize>(y) * width + x]; }
    [[nodiscard]] u64 ink() const; ///< sum of all coverage bytes: the covered area in 1/255 pixels
};

/// A rasterised glyph, positioned relative to the pen.
struct GlyphBitmap {
    CoverageBitmap bitmap;
    /// Pixel offset of the bitmap's left edge from the pen position (can be negative).
    i32 left = 0;
    /// Pixel offset of the bitmap's top edge from the baseline, y down (negative above the baseline).
    i32 top = 0;
    /// False when the glyph has no outline to draw (a space): the bitmap is empty but the pen still
    /// advances.
    bool has_ink = false;
};

/// Rasterises \p path (font units, y up, baseline at 0) at \p scale pixels per font unit.
///
/// \p padding adds transparent pixels around the ink, which keeps neighbouring atlas slots from
/// bleeding into each other under bilinear sampling. \p flip_y is on by default: font units point up,
/// bitmaps point down.
[[nodiscard]] GlyphBitmap rasterize_glyph(const GlyphPath& path, f32 scale, u32 padding = 1,
                                          bool flip_y = true);

} // namespace t2d
