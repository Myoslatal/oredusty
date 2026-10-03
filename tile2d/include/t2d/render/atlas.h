// Tile2D - procedurally generated texture atlases.
//
// The framework ships no binary art: the tileset atlas and the bitmap font are drawn into RGBA8
// images at startup, which keeps the repository text only and makes the tests independent of assets.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/sim/tileset.h>

namespace ore { struct Image; }

namespace t2d {

/// Atlas geometry: 16 x 16 cells of 16 x 16 pixels.
inline constexpr u32 kTileAtlasSize = 256;
inline constexpr u32 kTileCellSize = 16;
inline constexpr u32 kTileAtlasColumns = kTileAtlasSize / kTileCellSize;

/// Atlas geometry of the 5x7 bitmap font: 16 x 6 cells of 8 x 8 pixels (ASCII 32..127).
inline constexpr u32 kFontAtlasColumns = 16;
inline constexpr u32 kFontAtlasRows = 6;
inline constexpr u32 kFontCellSize = 8;
inline constexpr u32 kFontGlyphWidth = 5;
inline constexpr u32 kFontGlyphHeight = 7;
inline constexpr u32 kFontAtlasWidth = kFontAtlasColumns * kFontCellSize;
inline constexpr u32 kFontAtlasHeight = kFontAtlasRows * kFontCellSize;

/// Draws the built-in tileset: index (atlas_x, atlas_y) matches Tileset::default_platformer().
[[nodiscard]] ore::Image make_tileset_atlas();
/// Draws the bitmap font. Missing glyphs render as blank cells.
[[nodiscard]] ore::Image make_font_atlas();
/// Atlas rectangle (in pixels) of one character.
[[nodiscard]] bool font_glyph_rect(char character, u32& out_x, u32& out_y);
/// Width in pixels of a string at the given scale.
[[nodiscard]] f32 font_text_width(std::string_view text, f32 scale);

/// Tile atlas cell of a tile id for the built-in tileset (helper for the renderer).
[[nodiscard]] Vec2 tile_atlas_uv(TileId id);

} // namespace t2d
