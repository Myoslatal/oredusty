// Tile2D - the procedurally generated bitmap font atlas.
//
// The framework ships no binary art: the 5x7 bitmap font is drawn into an RGBA8 image at startup,
// which keeps the repository text only and makes the tests independent of assets. Game art (tilesets,
// sprites) comes from the game's own data, not from here.
#pragma once

#include <t2d/core/types.h>

#include <string_view>

namespace ore { struct Image; }

namespace t2d {

/// Atlas geometry of the 5x7 bitmap font: 16 columns of 8 x 8 pixel cells. ASCII 32..127 needs 96
/// cells, i.e. six full rows; a seventh row holds a single solid white cell that draw_rect()
/// samples for filled shapes (a full atlas would have no opaque texel to point at).
inline constexpr u32 kFontAtlasColumns = 16;
inline constexpr u32 kFontAtlasRows = 7;
inline constexpr u32 kFontGlyphCells = kFontAtlasColumns * 6;
inline constexpr u32 kFontCellSize = 8;
inline constexpr u32 kFontGlyphWidth = 5;
inline constexpr u32 kFontGlyphHeight = 7;
inline constexpr u32 kFontAtlasWidth = kFontAtlasColumns * kFontCellSize;
inline constexpr u32 kFontAtlasHeight = kFontAtlasRows * kFontCellSize;

/// Draws the bitmap font. Missing glyphs render as blank cells.
[[nodiscard]] ore::Image make_font_atlas();
/// Atlas rectangle (in pixels) of one character.
[[nodiscard]] bool font_glyph_rect(char character, u32& out_x, u32& out_y);
/// Width in pixels of a string at the given scale.
[[nodiscard]] f32 font_text_width(std::string_view text, f32 scale);

} // namespace t2d
