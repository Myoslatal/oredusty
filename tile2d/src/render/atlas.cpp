#include <t2d/render/atlas.h>

#include <ore/core/image.h>

#include <array>
#include <cstring>

namespace t2d {
namespace {

/// 5x7 glyphs, one byte per row, bit 4 = leftmost pixel. Only the characters the HUD uses are
/// defined; everything else renders blank.
struct Glyph {
    char code;
    std::array<u8, kFontGlyphHeight> rows;
};

constexpr Glyph kGlyphs[] = {
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'%', {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00100, 0b01000}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100}},
    {'/', {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}},
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00110, 0b01000, 0b10000, 0b11111}},
    {'3', {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {':', {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000}},
    {'<', {0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010}},
    {'=', {0b00000, 0b00000, 0b11111, 0b00000, 0b11111, 0b00000, 0b00000}},
    {'>', {0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'[', {0b01110, 0b01000, 0b01000, 0b01000, 0b01000, 0b01000, 0b01110}},
    {']', {0b01110, 0b00010, 0b00010, 0b00010, 0b00010, 0b00010, 0b01110}},
    {'_', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111}},
    {'|', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
};

[[nodiscard]] const Glyph* find_glyph(char code) {
    if (code >= 'a' && code <= 'z') code = static_cast<char>(code - 'a' + 'A'); // the font is uppercase
    for (const Glyph& glyph : kGlyphs) {
        if (glyph.code == code) return &glyph;
    }
    return nullptr;
}

/// Fills one pixel of an image (bounds checked, so a stray coordinate cannot corrupt memory).
void put(ore::Image& image, i32 x, i32 y, u32 color) {
    if (x < 0 || y < 0 || x >= static_cast<i32>(image.width) || y >= static_cast<i32>(image.height)) return;
    image.set_pixel(static_cast<u32>(x), static_cast<u32>(y), color);
}

} // namespace

ore::Image make_font_atlas() {
    ore::Image atlas = ore::Image::create(kFontAtlasWidth, kFontAtlasHeight, 0x00000000u);
    const u32 white = ore::make_rgba(255, 255, 255, 255);
    for (u32 code = 32; code < 32 + kFontGlyphCells; ++code) {
        const Glyph* glyph = find_glyph(static_cast<char>(code));
        if (glyph == nullptr) continue;
        const u32 cell_x = (code - 32) % kFontAtlasColumns;
        const u32 cell_y = (code - 32) / kFontAtlasColumns;
        for (u32 row = 0; row < kFontGlyphHeight; ++row) {
            const u8 bits = glyph->rows[row];
            for (u32 column = 0; column < kFontGlyphWidth; ++column) {
                if ((bits & (1u << (kFontGlyphWidth - 1 - column))) == 0) continue;
                put(atlas, static_cast<i32>(cell_x * kFontCellSize + column),
                    static_cast<i32>(cell_y * kFontCellSize + row), white);
            }
        }
    }

    // The last cell is deliberately left out of the glyph grid and filled solid: the sprite batch
    // points its draw_rect() uvs at it, so it must be opaque white.
    const u32 white_cell = kFontAtlasColumns * kFontAtlasRows - 1;
    const i32 white_x = static_cast<i32>((white_cell % kFontAtlasColumns) * kFontCellSize);
    const i32 white_y = static_cast<i32>((white_cell / kFontAtlasColumns) * kFontCellSize);
    atlas.fill_rect(white_x, white_y, static_cast<i32>(kFontCellSize), static_cast<i32>(kFontCellSize), white);
    return atlas;
}

bool font_glyph_rect(char character, u32& out_x, u32& out_y) {
    const u32 code = static_cast<u32>(static_cast<u8>(character));
    if (code < 32 || code >= 32 + kFontGlyphCells) return false;
    out_x = ((code - 32) % kFontAtlasColumns) * kFontCellSize;
    out_y = ((code - 32) / kFontAtlasColumns) * kFontCellSize;
    return true;
}

f32 font_text_width(std::string_view text, f32 scale) {
    return static_cast<f32>(text.size()) * (static_cast<f32>(kFontGlyphWidth) + 1.0f) * scale;
}

} // namespace t2d
