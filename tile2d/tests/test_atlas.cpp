// Content tests: the built-in tileset, the procedurally drawn atlases and the shipped level must
// agree with each other. They are pure CPU tests, so they run everywhere.
//
// They exist because three silent mismatches shipped at once and produced a frame that looked
// "almost right": tiles pointing one cell off in the atlas (drawing the wrong art, and breaking the
// background-brick layer test that keys on the column), a level whose ground characters were not in
// the authoring legend (so the parser dropped them and the terrain vanished), and no opaque cell in
// the font atlas for filled rectangles.
#include <ore/core/image.h>

#include <t2d/core/cli.h>
#include <t2d/render/atlas.h>
#include <t2d/sim/tilemap.h>
#include <t2d/sim/tileset.h>

#include <support/test_support.h>

#include <string>
#include <vector>

using namespace t2d;

namespace {

/// True when any pixel of the cell is not fully transparent. p cell_size is 16 for the tileset
/// atlas and 8 for the font atlas, which is why it cannot be a constant here.
[[nodiscard]] bool cell_has_pixels(const ore::Image& atlas, u32 cell_x, u32 cell_y, u32 cell_size) {
    for (u32 y = 0; y < cell_size; ++y) {
        for (u32 x = 0; x < cell_size; ++x) {
            const u32 packed = atlas.pixel(cell_x * cell_size + x, cell_y * cell_size + y);
            if (((packed >> 24) & 0xFFu) != 0u) return true;
        }
    }
    return false;
}

[[nodiscard]] bool cell_is_fully_opaque(const ore::Image& atlas, u32 cell_x, u32 cell_y, u32 cell_size) {
    for (u32 y = 0; y < cell_size; ++y) {
        for (u32 x = 0; x < cell_size; ++x) {
            const u32 packed = atlas.pixel(cell_x * cell_size + x, cell_y * cell_size + y);
            if (((packed >> 24) & 0xFFu) != 0xFFu) return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char character : text) {
        if (character == '\n') {
            if (!current.empty() && current.back() == '\r') current.pop_back();
            if (!current.empty()) lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

} // namespace

T2D_TEST(every_tile_has_art_in_the_atlas_cell_it_points_at) {
    const ore::Image atlas = make_tileset_atlas();
    const Tileset& tileset = Tileset::default_platformer();
    T2D_CHECK_GT(tileset.count(), 8u);
    for (const TileDef& definition : tileset.definitions()) {
        if (definition.id == kEmptyTile) continue;
        T2D_CHECK_MSG(cell_has_pixels(atlas, definition.atlas_x, definition.atlas_y, kTileCellSize),
                      "tile {} points at the blank atlas cell ({}, {})", definition.id,
                      definition.atlas_x, definition.atlas_y);
    }
}

T2D_TEST(every_tile_owns_a_distinct_atlas_cell) {
    const Tileset& tileset = Tileset::default_platformer();
    for (const TileDef& a : tileset.definitions()) {
        if (a.id == kEmptyTile) continue;
        u32 sharing = 0;
        for (const TileDef& b : tileset.definitions()) {
            if (b.id == kEmptyTile || b.id == a.id) continue;
            if (a.atlas_x == b.atlas_x && a.atlas_y == b.atlas_y) ++sharing;
        }
        T2D_CHECK_MSG(sharing == 0u, "tile {} shares its atlas cell ({}, {}) with {} other tile(s)",
                      a.id, a.atlas_x, a.atlas_y, sharing);
    }
}

T2D_TEST(the_sprites_the_renderer_samples_are_drawn) {
    const ore::Image atlas = make_tileset_atlas();
    // draw_players() samples the idle and running sprite cells.
    T2D_CHECK(cell_has_pixels(atlas, 0, 1, kTileCellSize));
    T2D_CHECK(cell_has_pixels(atlas, 1, 1, kTileCellSize));
    // draw_pickups() samples the coin tile.
    const TileDef coin = Tileset::default_platformer().atlas_of(6);
    T2D_CHECK(cell_has_pixels(atlas, coin.atlas_x, coin.atlas_y, kTileCellSize));
    // The background brick is recognised by its column, so that column must be the brick's.
    const TileDef brick = Tileset::default_platformer().atlas_of(7);
    T2D_CHECK_EQ(brick.atlas_x, 7u);
}

T2D_TEST(the_font_atlas_reserves_an_opaque_cell_for_filled_rectangles) {
    const ore::Image atlas = make_font_atlas();
    const u32 white_cell = kFontAtlasColumns * kFontAtlasRows - 1;
    T2D_CHECK(cell_is_fully_opaque(atlas, white_cell % kFontAtlasColumns, white_cell / kFontAtlasColumns,
                                   kFontCellSize));

    for (const char character : std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:%-")) {
        u32 cell_x = 0;
        u32 cell_y = 0;
        T2D_REQUIRE(font_glyph_rect(character, cell_x, cell_y));
        T2D_CHECK_MSG(cell_has_pixels(atlas, cell_x / kFontCellSize, cell_y / kFontCellSize, kFontCellSize),
                      "glyph '{}' is blank", character);
    }
    // The space glyph is blank by design and the atlas holds exactly the ASCII range.
    u32 cell_x = 0;
    u32 cell_y = 0;
    T2D_CHECK(font_glyph_rect(' ', cell_x, cell_y));
    T2D_CHECK_FALSE(font_glyph_rect('\x01', cell_x, cell_y));
}

T2D_TEST(the_shipped_level_uses_only_characters_the_legend_defines) {
    const std::string path = std::string(T2D_SOURCE_DIR) + "/assets/levels/demo.txt";
    const auto text = read_text(path);
    T2D_REQUIRE(text.has_value());
    const std::vector<std::string> lines = split_lines(*text);
    T2D_REQUIRE(!lines.empty());

    u32 authored_tiles = 0;
    for (const std::string& row : lines) {
        for (const char character : row) {
            if (character == '.') continue;
            ++authored_tiles;
            T2D_CHECK_MSG(TileMap::default_legend().count(character) == 1,
                          "the level uses '{}', which the legend does not define", character);
        }
    }
    T2D_CHECK_GT(authored_tiles, 200u);

    const auto map = TileMap::from_ascii(lines, TileMap::default_legend(), 16.0f);
    T2D_REQUIRE(map.has_value());
    u32 parsed_tiles = 0;
    for (i32 y = 0; y < map->height(); ++y) {
        for (i32 x = 0; x < map->width(); ++x) {
            if (map->at(x, y) != kEmptyTile) ++parsed_tiles;
        }
    }
    // Every authored character must survive the parse: an unknown one is silently empty.
    T2D_CHECK_EQ(parsed_tiles, authored_tiles);
}

T2D_TEST(the_legend_round_trips_every_tile_id) {
    const Tileset& tileset = Tileset::default_platformer();
    for (const TileDef& definition : tileset.definitions()) {
        const auto found = TileMap::default_reverse_legend().find(definition.id);
        T2D_CHECK_MSG(found != TileMap::default_reverse_legend().end(), "tile {} has no legend character",
                      definition.id);
        if (found == TileMap::default_reverse_legend().end()) continue;
        const auto forward = TileMap::default_legend().find(found->second);
        T2D_REQUIRE(forward != TileMap::default_legend().end());
        T2D_CHECK_EQ(forward->second, definition.id);
    }
}

T2D_TEST_MAIN
