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

void fill_cell(ore::Image& image, u32 cell_x, u32 cell_y, u32 color) {
    image.fill_rect(cell_x * kTileCellSize, cell_y * kTileCellSize, kTileCellSize, kTileCellSize, color);
}

} // namespace

ore::Image make_tileset_atlas() {
    ore::Image atlas = ore::Image::create(kTileAtlasSize, kTileAtlasSize, 0x00000000u);
    const u32 stone = ore::make_rgba(96, 100, 110);
    const u32 stone_dark = ore::make_rgba(66, 70, 80);
    const u32 dirt = ore::make_rgba(120, 84, 56);
    const u32 dirt_dark = ore::make_rgba(92, 62, 40);
    const u32 grass = ore::make_rgba(88, 172, 78);
    const u32 grass_dark = ore::make_rgba(62, 132, 58);
    const u32 wood = ore::make_rgba(150, 108, 66);
    const u32 wood_dark = ore::make_rgba(112, 78, 46);
    const u32 spike = ore::make_rgba(206, 210, 220);
    const u32 coin = ore::make_rgba(246, 200, 66);
    const u32 brick = ore::make_rgba(58, 46, 52);
    const u32 brick_line = ore::make_rgba(40, 32, 38);
    const u32 water = ore::make_rgba(52, 110, 190);

    // 1 stone, 2 dirt, 3 grass, 4 one-way platform, 5 spikes, 6 coin, 7 background brick,
    // 8 ladder, 9 water, 10 crate - matching Tileset::default_platformer().
    fill_cell(atlas, 1, 0, stone);
    for (u32 y = 0; y < kTileCellSize; ++y) {
        for (u32 x = 0; x < kTileCellSize; ++x) {
            const bool mortar = (y % 8 == 7) || ((x + (y / 8) * 8) % 16 == 15);
            if (mortar) atlas.set_pixel(x + kTileCellSize, y, stone_dark);
        }
    }

    fill_cell(atlas, 2, 0, dirt);
    for (u32 y = 0; y < kTileCellSize; ++y) {
        for (u32 x = 0; x < kTileCellSize; ++x) {
            if (((x * 7 + y * 13) % 11) == 0) atlas.set_pixel(x + 2 * kTileCellSize, y, dirt_dark);
        }
    }

    fill_cell(atlas, 3, 0, dirt);
    atlas.fill_rect(3 * kTileCellSize, 0, kTileCellSize, 4, grass);
    atlas.fill_rect(3 * kTileCellSize, 4, kTileCellSize, 1, grass_dark);
    for (u32 x = 0; x < kTileCellSize; x += 3) {
        atlas.fill_rect(3 * kTileCellSize + x, 4, 1, 2, grass);
    }

    fill_cell(atlas, 4, 0, 0x00000000u);
    atlas.fill_rect(4 * kTileCellSize, 2, kTileCellSize, 4, wood);
    atlas.fill_rect(4 * kTileCellSize, 6, kTileCellSize, 1, wood_dark);

    fill_cell(atlas, 5, 0, 0x00000000u);
    for (u32 spike_index = 0; spike_index < 2; ++spike_index) {
        for (u32 row = 0; row < 8; ++row) {
            const u32 half_width = row / 2 + 1;
            for (u32 column = 0; column < half_width; ++column) {
                const u32 base_x = 5 * kTileCellSize + spike_index * 8 + 3 - column;
                put(atlas, static_cast<i32>(base_x), static_cast<i32>(15 - row), spike);
                put(atlas, static_cast<i32>(base_x + 2 * column + 1), static_cast<i32>(15 - row), spike);
            }
        }
    }

    fill_cell(atlas, 6, 0, 0x00000000u);
    for (u32 y = 0; y < kTileCellSize; ++y) {
        for (u32 x = 0; x < kTileCellSize; ++x) {
            const i32 dx = static_cast<i32>(x) - 8;
            const i32 dy = static_cast<i32>(y) - 8;
            const i32 distance = (dx * dx) / 4 + dy * dy;
            if (distance <= 30) {
                atlas.set_pixel(x + 6 * kTileCellSize, y, distance <= 12 ? coin : ore::make_rgba(214, 158, 40));
            }
        }
    }

    fill_cell(atlas, 7, 0, brick);
    for (u32 y = 0; y < kTileCellSize; ++y) {
        for (u32 x = 0; x < kTileCellSize; ++x) {
            const bool line = (y % 6 == 5) || ((x + (y / 6) * 8) % 16 == 15);
            if (line) atlas.set_pixel(x + 7 * kTileCellSize, y, brick_line);
        }
    }

    fill_cell(atlas, 8, 0, 0x00000000u);
    atlas.fill_rect(8 * kTileCellSize + 2, 0, 2, kTileCellSize, wood);
    atlas.fill_rect(8 * kTileCellSize + 10, 0, 2, kTileCellSize, wood);
    for (u32 y = 2; y < kTileCellSize; y += 5) {
        atlas.fill_rect(8 * kTileCellSize + 2, y, 10, 2, wood_dark);
    }

    fill_cell(atlas, 9, 0, water);
    for (u32 y = 0; y < kTileCellSize; ++y) {
        for (u32 x = 0; x < kTileCellSize; ++x) {
            if (((x + y * 2) % 8) < 3) atlas.set_pixel(x + 9 * kTileCellSize, y, ore::make_rgba(74, 140, 214));
        }
    }

    fill_cell(atlas, 10, 0, wood);
    atlas.fill_rect(10 * kTileCellSize, 0, kTileCellSize, 1, wood_dark);
    atlas.fill_rect(10 * kTileCellSize, 15, kTileCellSize, 1, wood_dark);
    atlas.fill_rect(10 * kTileCellSize, 0, 1, kTileCellSize, wood_dark);
    atlas.fill_rect(10 * kTileCellSize + 15, 0, 1, kTileCellSize, wood_dark);
    for (u32 i = 0; i < kTileCellSize; ++i) {
        put(atlas, static_cast<i32>(11 * kTileCellSize + i), static_cast<i32>(i), wood_dark);
        put(atlas, static_cast<i32>(11 * kTileCellSize + i), static_cast<i32>(15 - i), wood_dark);
    }

    // Player sprites on row 1, columns 0 and 1 (idle and running).
    const u32 skin = ore::make_rgba(240, 200, 160);
    const u32 shirt = ore::make_rgba(70, 130, 220);
    const u32 shirt_dark = ore::make_rgba(48, 96, 176);
    const u32 eye = ore::make_rgba(30, 30, 40);
    for (u32 frame = 0; frame < 2; ++frame) {
        const u32 origin_x = frame * kTileCellSize;
        const u32 origin_y = kTileCellSize;
        atlas.fill_rect(origin_x + 3, origin_y + 1, 8, 5, skin);      // head
        atlas.fill_rect(origin_x + 3, origin_y + 6, 8, 6, shirt);     // torso
        atlas.fill_rect(origin_x + 3, origin_y + 6, 8, 2, shirt_dark);
        atlas.fill_rect(origin_x + 2, origin_y + 8, 1, 4, skin);      // arms
        atlas.fill_rect(origin_x + 11, origin_y + 8, 1, 4, skin);
        const u32 leg_offset = frame == 0 ? 0 : 1;
        atlas.fill_rect(origin_x + 4 + leg_offset, origin_y + 12, 2, 3, shirt_dark);  // legs
        atlas.fill_rect(origin_x + 8 - leg_offset, origin_y + 12, 2, 3, shirt_dark);
        atlas.fill_rect(origin_x + 5, origin_y + 3, 2, 2, eye);       // eyes
        atlas.fill_rect(origin_x + 9, origin_y + 3, 2, 2, eye);
    }
    return atlas;
}

ore::Image make_font_atlas() {
    ore::Image atlas = ore::Image::create(kFontAtlasWidth, kFontAtlasHeight, 0x00000000u);
    const u32 white = ore::make_rgba(255, 255, 255, 255);
    for (u32 code = 32; code < 32 + kFontAtlasColumns * kFontAtlasRows; ++code) {
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
    return atlas;
}

bool font_glyph_rect(char character, u32& out_x, u32& out_y) {
    const u32 code = static_cast<u32>(static_cast<u8>(character));
    if (code < 32 || code >= 32 + kFontAtlasColumns * kFontAtlasRows) return false;
    out_x = ((code - 32) % kFontAtlasColumns) * kFontCellSize;
    out_y = ((code - 32) / kFontAtlasColumns) * kFontCellSize;
    return true;
}

f32 font_text_width(std::string_view text, f32 scale) {
    return static_cast<f32>(text.size()) * (static_cast<f32>(kFontGlyphWidth) + 1.0f) * scale;
}

Vec2 tile_atlas_uv(TileId id) {
    const Tileset& tileset = Tileset::default_platformer();
    const TileDef definition = tileset.atlas_of(id);
    return Vec2{static_cast<f32>(definition.atlas_x), static_cast<f32>(definition.atlas_y)};
}

} // namespace t2d
