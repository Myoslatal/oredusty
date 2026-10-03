// Tile2D - tileset and tilemap tests: storage, queries, collision resolution, serialisation and
// ASCII authoring. Everything runs against the built-in platformer tileset.
#include <support/test_support.h>

#include <t2d/core/rng.h>
#include <t2d/sim/tilemap.h>
#include <t2d/sim/tileset.h>

#include <bit>
#include <string>
#include <unordered_map>
#include <vector>

using namespace t2d;

namespace {

const Tileset& platformer() { return Tileset::default_platformer(); }

[[nodiscard]] Aabb2 box_at(Vec2 top_left, Vec2 size) { return Aabb2::from_top_left(top_left, size); }

/// Independent re-implementation of "does this box touch a solid tile", so the movement invariant
/// is checked without going through the query under test.
[[maybe_unused]] [[nodiscard]] bool touches_solid(const TileMap& map, const Aabb2& box, const Tileset& tileset) {
    const f32 tile = map.tile_size();
    const i32 x0 = floor_to_i32(box.min.x / tile);
    const i32 y0 = floor_to_i32(box.min.y / tile);
    const i32 x1 = ceil_to_i32(box.max.x / tile);
    const i32 y1 = ceil_to_i32(box.max.y / tile);
    for (i32 y = y0; y < y1; ++y) {
        for (i32 x = x0; x < x1; ++x) {
            if (tileset.is_solid(map.at(x, y))) return true;
        }
    }
    return false;
}

[[maybe_unused]] [[nodiscard]] bool maps_equal(const TileMap& a, const TileMap& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    if (a.layer_count() != b.layer_count()) return false;
    if (a.tile_size() != b.tile_size()) return false;
    if (a.out_of_bounds_solid() != b.out_of_bounds_solid()) return false;
    for (i32 layer = 0; layer < a.layer_count(); ++layer) {
        for (i32 y = 0; y < a.height(); ++y) {
            for (i32 x = 0; x < a.width(); ++x) {
                if (a.at(layer, x, y) != b.at(layer, x, y)) return false;
            }
        }
    }
    return true;
}

/// The FNV-1a walk TileMap::checksum() documents: dimensions, the layer count, then every tile id in
/// row major order per layer, bottom to top.
[[maybe_unused]] [[nodiscard]] u64 reference_checksum(const TileMap& map) {
    u64 hash = 0xCBF29CE484222325ull;
    const auto mix = [&hash](u64 value, i32 bytes) {
        for (i32 index = 0; index < bytes; ++index) {
            hash = (hash ^ ((value >> (index * 8)) & 0xFFu)) * 0x100000001B3ull;
        }
    };
    mix(static_cast<u64>(static_cast<u32>(map.width())), 4);
    mix(static_cast<u64>(static_cast<u32>(map.height())), 4);
    mix(static_cast<u64>(static_cast<u32>(map.layer_count())), 4);
    for (i32 layer = 0; layer < map.layer_count(); ++layer) {
        for (i32 y = 0; y < map.height(); ++y) {
            for (i32 x = 0; x < map.width(); ++x) mix(map.at(layer, x, y), 2);
        }
    }
    return hash;
}

// --- hand rolled serialisation blobs, used to feed deserialize() broken input -------------------

void push_varint(std::vector<u8>& out, u32 value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<u8>(value) | 0x80u);
        value >>= 7;
    }
    out.push_back(static_cast<u8>(value));
}

[[maybe_unused]] void push_u16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>(value >> 8));
}

void push_u32(std::vector<u8>& out, u32 value) {
    for (i32 index = 0; index < 4; ++index) out.push_back(static_cast<u8>((value >> (index * 8)) & 0xFFu));
}

/// A hand rolled map blob header, so a test can feed deserialize() exactly the defect it means to.
[[maybe_unused]] void push_header(std::vector<u8>& out, u32 width, u32 height, u32 cells, f32 tile_size,
                                  u8 version = 2, u8 flags = 1, u32 layers = 1) {
    out.push_back(static_cast<u8>('T'));
    out.push_back(static_cast<u8>('2'));
    out.push_back(static_cast<u8>('D'));
    out.push_back(static_cast<u8>('M'));
    out.push_back(version);
    out.push_back(flags);
    push_varint(out, width);
    push_varint(out, height);
    push_varint(out, layers);
    push_u32(out, std::bit_cast<u32>(tile_size));
    push_varint(out, cells);
}

/// A map with a different pattern on every layer, so a round trip has something to lose.
[[maybe_unused]] [[nodiscard]] TileMap random_map(i32 width, i32 height, i32 layers, u64 seed) {
    Rng rng(seed);
    TileMap map(width, height, 16.0f, true, layers);
    for (i32 layer = 0; layer < layers; ++layer) {
        for (i32 y = 0; y < height; ++y) {
            for (i32 x = 0; x < width; ++x) {
                const u32 roll = rng.next_bounded(100);
                TileId id = kEmptyTile;
                if (roll < 25u) id = 1;
                else if (roll < 32u) id = 4;
                else if (roll < 38u) id = 5;
                else if (roll < 45u) id = 7;
                else if (roll < 50u) id = 10;
                map.set(layer, x, y, id);
            }
        }
    }
    return map;
}

[[maybe_unused]] [[nodiscard]] TileMap random_map(i32 width, i32 height, u64 seed) {
    return random_map(width, height, 1, seed);
}

} // namespace

// -------------------------------------------------------------------------------------- tileset --

T2D_TEST(tileset_default_platformer) {
    const Tileset& set = platformer();
    T2D_CHECK_EQ(set.count(), 11u);
    T2D_CHECK_EQ(set.definitions().size(), 11u);
    T2D_CHECK(set.find(kEmptyTile) != nullptr);
    T2D_CHECK(set.find(10) != nullptr);
    T2D_CHECK(set.find(11) == nullptr);
    T2D_CHECK(set.find(60000) == nullptr);

    // documented ids: 1 stone, 2 dirt, 3 grass top and 10 crate are solid
    T2D_CHECK(set.is_solid(1) && set.is_solid(2) && set.is_solid(3) && set.is_solid(10));
    T2D_CHECK_FALSE(set.is_solid(kEmptyTile) || set.is_solid(4) || set.is_solid(5) || set.is_solid(6));
    T2D_CHECK(set.is_one_way(4));
    T2D_CHECK_FALSE(set.is_one_way(1) || set.is_one_way(5));
    T2D_CHECK(set.is_hazard(5));
    T2D_CHECK_FALSE(set.is_hazard(1) || set.is_hazard(6));
    T2D_CHECK(set.is_decor(6) && set.is_decor(7));
    T2D_CHECK_FALSE(set.is_decor(1) || set.is_decor(5));
    T2D_CHECK(set.blocks_falling(1) && set.blocks_falling(4));
    T2D_CHECK_FALSE(set.blocks_falling(5) || set.blocks_falling(6) || set.blocks_falling(8) ||
                    set.blocks_falling(9) || set.blocks_falling(kEmptyTile));

    const TileDef* ladder = set.find(8);
    T2D_REQUIRE(ladder != nullptr);
    T2D_CHECK(has_flag(ladder->flags, TileFlag::Ladder));
    const TileDef* water = set.find(9);
    T2D_REQUIRE(water != nullptr);
    T2D_CHECK(has_flag(water->flags, TileFlag::Water));

    // unknown ids are inert and have no atlas cell
    T2D_CHECK_FALSE(set.is_solid(60000) || set.is_one_way(60000) || set.is_hazard(60000) ||
                    set.is_decor(60000) || set.blocks_falling(60000));
    T2D_CHECK_EQ(set.atlas_of(60000).id, kEmptyTile);
    T2D_CHECK_EQ(set.atlas_of(60000).atlas_x, 0);
    T2D_CHECK_EQ(set.atlas_of(60000).atlas_y, 0);

    for (TileId a = 1; a <= 10; ++a) {
        for (TileId b = static_cast<TileId>(a + 1); b <= 10; ++b) {
            const TileDef left = set.atlas_of(a);
            const TileDef right = set.atlas_of(b);
            T2D_CHECK(left.atlas_x != right.atlas_x || left.atlas_y != right.atlas_y);
        }
    }

    T2D_CHECK_EQ(tile_flag_name(TileFlag::Solid), std::string_view{"solid"});
    T2D_CHECK_EQ(tile_flag_name(TileFlag::OneWay), std::string_view{"oneway"});
    T2D_CHECK_EQ(tile_flag_name(TileFlag::None), std::string_view{"none"});
    T2D_CHECK(&Tileset::default_platformer() == &set);
}

T2D_TEST(tileset_add_replaces_same_id) {
    Tileset set;
    T2D_CHECK_EQ(set.count(), 0u);
    T2D_CHECK(set.find(1) == nullptr);

    set.add(TileDef{1, TileFlag::Solid, 2, 3});
    T2D_CHECK_EQ(set.count(), 1u);
    T2D_REQUIRE(set.find(1) != nullptr);
    T2D_CHECK_EQ(set.find(1)->atlas_x, 2);
    T2D_CHECK_EQ(set.find(1)->atlas_y, 3);
    T2D_CHECK(set.is_solid(1));

    // re-adding an id updates it in place instead of duplicating the definition
    set.add(TileDef{1, TileFlag::Hazard, 4, 5});
    T2D_CHECK_EQ(set.count(), 1u);
    T2D_REQUIRE(set.find(1) != nullptr);
    T2D_CHECK(set.is_hazard(1));
    T2D_CHECK_FALSE(set.is_solid(1));
    T2D_CHECK_EQ(set.find(1)->atlas_x, 4);

    set.add(TileDef{2, TileFlag::OneWay, 0, 1});
    T2D_CHECK_EQ(set.count(), 2u);
    T2D_CHECK_EQ(set.definitions().size(), 2u);
    T2D_CHECK(set.is_one_way(2));
    T2D_CHECK_FALSE(set.is_one_way(1));
}

// ------------------------------------------------------------------------------------ structure --

T2D_TEST(map_construct_and_dimensions) {
    const TileMap blank;
    T2D_CHECK_EQ(blank.width(), 0);
    T2D_CHECK_EQ(blank.height(), 0);
    T2D_CHECK(blank.empty());
    T2D_CHECK_EQ(blank.tile_size(), 16.0f);
    T2D_CHECK(blank.out_of_bounds_solid());
    T2D_CHECK_EQ(blank.world_size(), Vec2{});
    T2D_CHECK_EQ(blank.count_tiles(kEmptyTile), 0u);
    T2D_CHECK_EQ(blank.at(0, 0), kEmptyTile);

    const TileMap map(40, 25, 8.0f, false);
    T2D_CHECK_EQ(map.width(), 40);
    T2D_CHECK_EQ(map.height(), 25);
    T2D_CHECK_FALSE(map.empty());
    T2D_CHECK_EQ(map.tile_size(), 8.0f);
    T2D_CHECK_FALSE(map.out_of_bounds_solid());
    T2D_CHECK_EQ(map.world_size(), (Vec2{320.0f, 200.0f}));
    T2D_CHECK_EQ(map.count_tiles(kEmptyTile), 1000u);
    T2D_CHECK_EQ(map.count_tiles(1), 0u);

    // a big map is constructible and readable without materialising every cell
    const TileMap huge(4096, 4096);
    T2D_CHECK_EQ(huge.count_tiles(kEmptyTile), 16777216u);

    // invalid dimensions clamp to zero, an invalid tile size falls back to the documented default
    const TileMap broken(-4, -9, 0.0f);
    T2D_CHECK(broken.empty());
    T2D_CHECK_EQ(broken.tile_size(), 16.0f);
    T2D_CHECK_EQ(broken.world_size(), Vec2{});
    T2D_CHECK_EQ(broken.count_tiles(kEmptyTile), 0u);
    T2D_CHECK_EQ(TileMap::kChunkSize, 32);
}

T2D_TEST(map_bounds_and_oob_queries) {
    TileMap map(8, 4);   // 128 x 64 px
    T2D_CHECK(map.in_bounds(0, 0) && map.in_bounds(7, 3));
    T2D_CHECK_FALSE(map.in_bounds(8, 0) || map.in_bounds(0, 4) || map.in_bounds(-1, 0) ||
                    map.in_bounds(0, -1) || map.in_bounds(-1, -1));

    // writes outside are ignored, reads outside are empty
    map.set(-1, 2, 1);
    map.set(8, 2, 1);
    map.set(2, 4, 1);
    map.set(2, -1, 1);
    T2D_CHECK_EQ(map.count_tiles(1), 0u);
    T2D_CHECK_EQ(map.at(-1, 0), kEmptyTile);
    T2D_CHECK_EQ(map.at(8, 0), kEmptyTile);
    T2D_CHECK_EQ(map.at(0, 4), kEmptyTile);
    T2D_CHECK_EQ(map.at(0, -1), kEmptyTile);

    // at_clamped pins the corner for neighbour lookups
    map.set(0, 3, 7);
    T2D_CHECK_EQ(map.at_clamped(-5, 99), 7);
    T2D_CHECK_EQ(map.at_clamped(100, -100), map.at(7, 0));
    T2D_CHECK_EQ(map.at_clamped(3, 2), map.at(3, 2));
    T2D_CHECK_EQ(TileMap().at_clamped(0, 0), kEmptyTile);

    // out of bounds is solid by default, never one-way or hazardous
    T2D_CHECK(map.is_solid(-1, 0, platformer()));
    T2D_CHECK(map.is_solid(0, -1, platformer()));
    T2D_CHECK(map.is_solid(8, 0, platformer()));
    T2D_CHECK(map.is_solid(0, 4, platformer()));
    T2D_CHECK_FALSE(map.is_one_way(-1, 0, platformer()));
    T2D_CHECK_FALSE(map.is_hazard(0, -1, platformer()));
    T2D_CHECK_FALSE(map.is_hazard(0, 0, platformer()));

    // per cell queries follow the tileset flags
    map.set(3, 1, 1);   // stone
    map.set(4, 1, 4);   // one-way platform
    map.set(5, 1, 5);   // spikes
    map.set(6, 1, 7);   // background brick (decor)
    T2D_CHECK(map.is_solid(3, 1, platformer()));
    T2D_CHECK_FALSE(map.is_one_way(3, 1, platformer()));
    T2D_CHECK(map.is_one_way(4, 1, platformer()));
    T2D_CHECK_FALSE(map.is_solid(4, 1, platformer()));
    T2D_CHECK(map.is_hazard(5, 1, platformer()));
    T2D_CHECK_FALSE(map.is_hazard(4, 1, platformer()));
    T2D_CHECK_FALSE(map.is_solid(6, 1, platformer()));

    // box queries: a box that ends exactly on a tile edge does not overlap the next cell
    T2D_CHECK(map.overlaps_solid(box_at({48.0f, 16.0f}, {16.0f, 16.0f}), platformer()));
    T2D_CHECK(map.overlaps_solid(box_at({32.0f, 16.0f}, {16.1f, 16.0f}), platformer()));
    T2D_CHECK_FALSE(map.overlaps_solid(box_at({32.0f, 16.0f}, {16.0f, 16.0f}), platformer()));
    T2D_CHECK_FALSE(map.overlaps_solid(box_at({48.0f, 0.0f}, {16.0f, 16.0f}), platformer()));
    T2D_CHECK_FALSE(map.overlaps_solid(box_at({64.0f, 16.0f}, {16.0f, 16.0f}), platformer()));
    T2D_CHECK(map.overlaps_hazard(box_at({80.0f, 16.0f}, {16.0f, 16.0f}), platformer()));
    T2D_CHECK_FALSE(map.overlaps_hazard(box_at({96.0f, 16.0f}, {16.0f, 16.0f}), platformer()));
    T2D_CHECK_FALSE(map.overlaps_solid(box_at({48.0f, 32.0f}, {16.0f, 16.0f}), platformer()));

    // boxes that leave the map are solid while the flag is set
    T2D_CHECK(map.overlaps_solid(box_at({-1.0f, 0.0f}, {8.0f, 8.0f}), platformer()));
    T2D_CHECK(map.overlaps_solid(box_at({0.0f, 60.0f}, {8.0f, 8.0f}), platformer()));
    T2D_CHECK_FALSE(map.overlaps_solid(box_at({-1.0f, 0.0f}, {1.0f, 1.0f}), platformer()) == false &&
                    map.overlaps_solid(box_at({-1.0f, 0.0f}, {1.0f, 1.0f}), platformer()));

    TileMap open(8, 4, 16.0f, false);
    T2D_CHECK_FALSE(open.overlaps_solid(box_at({-1.0f, 0.0f}, {8.0f, 8.0f}), platformer()));
    T2D_CHECK_FALSE(open.overlaps_solid(box_at({0.0f, 60.0f}, {8.0f, 8.0f}), platformer()));
    T2D_CHECK_FALSE(open.is_solid(-1, 0, platformer()));
    T2D_CHECK_FALSE(open.is_solid(0, 4, platformer()));
    T2D_CHECK_FALSE(open.overlaps_hazard(box_at({-100.0f, -100.0f}, {8.0f, 8.0f}), platformer()));
    T2D_CHECK_FALSE(open.is_on_ground(box_at({0.0f, -16.0f}, {8.0f, 8.0f}), platformer()));
}

T2D_TEST(map_chunk_boundaries) {
    TileMap map(100, 100);
    map.set(31, 31, 1);
    map.set(32, 31, 2);
    map.set(33, 31, 3);
    map.set(31, 32, 4);
    map.set(32, 32, 5);
    map.set(33, 33, 6);
    map.set(63, 63, 7);
    map.set(64, 64, 8);
    map.set(95, 95, 9);
    map.set(96, 96, 10);

    T2D_CHECK_EQ(map.at(31, 31), 1);
    T2D_CHECK_EQ(map.at(32, 31), 2);
    T2D_CHECK_EQ(map.at(33, 31), 3);
    T2D_CHECK_EQ(map.at(31, 32), 4);
    T2D_CHECK_EQ(map.at(32, 32), 5);
    T2D_CHECK_EQ(map.at(33, 33), 6);
    T2D_CHECK_EQ(map.at(63, 63), 7);
    T2D_CHECK_EQ(map.at(64, 64), 8);
    T2D_CHECK_EQ(map.at(95, 95), 9);
    T2D_CHECK_EQ(map.at(96, 96), 10);
    T2D_CHECK_EQ(map.at(30, 31), kEmptyTile);
    T2D_CHECK_EQ(map.at(31, 30), kEmptyTile);
    T2D_CHECK_EQ(map.at(32, 33), kEmptyTile);
    T2D_CHECK_EQ(map.count_tiles(kEmptyTile), 10000u - 10u);

    // a fill spanning four chunks keeps every cell
    map.fill_rect(TileRect{30, 30, 5, 5}, 20);
    T2D_CHECK_EQ(map.count_tiles(20), 25u);
    T2D_CHECK_EQ(map.at(30, 30), 20);
    T2D_CHECK_EQ(map.at(34, 34), 20);
    T2D_CHECK_EQ(map.at(29, 30), kEmptyTile);
    T2D_CHECK_EQ(map.at(35, 34), kEmptyTile);
    T2D_CHECK_EQ(map.at(31, 31), 20);       // overwritten by the fill
    T2D_CHECK_EQ(map.count_tiles(1), 0u);

    // a map that is not a multiple of the chunk size reads back exactly
    TileMap partial(40, 40);
    partial.set(39, 39, 1);
    T2D_CHECK_EQ(partial.at(39, 39), 1);
    T2D_CHECK_EQ(partial.at(40, 39), kEmptyTile);
    T2D_CHECK_EQ(partial.count_tiles(kEmptyTile), 1599u);
    partial.fill_rect(TileRect{0, 0, 40, 40}, 2);
    T2D_CHECK_EQ(partial.count_tiles(2), 1600u);
    T2D_CHECK_EQ(partial.count_tiles(kEmptyTile), 0u);
}

T2D_TEST(map_fill_and_fill_rect) {
    TileMap map(70, 40);   // 3 x 2 chunks with partial edges
    map.fill(3);
    T2D_CHECK_EQ(map.count_tiles(3), 70u * 40u);
    T2D_CHECK_EQ(map.count_tiles(kEmptyTile), 0u);
    map.fill(kEmptyTile);
    T2D_CHECK_EQ(map.count_tiles(kEmptyTile), 70u * 40u);
    T2D_CHECK_EQ(map.count_tiles(3), 0u);

    map.fill_rect(TileRect{5, 5, 10, 4}, 1);
    T2D_CHECK_EQ(map.count_tiles(1), 40u);
    T2D_CHECK_EQ(map.at(5, 5), 1);
    T2D_CHECK_EQ(map.at(14, 8), 1);
    T2D_CHECK_EQ(map.at(15, 5), kEmptyTile);
    T2D_CHECK_EQ(map.at(5, 9), kEmptyTile);
    T2D_CHECK_EQ(map.at(4, 5), kEmptyTile);

    // a rect that leaves the map is clipped, a rect outside is a no-op
    map.fill_rect(TileRect{-5, -5, 10, 10}, 2);
    T2D_CHECK_EQ(map.count_tiles(2), 25u);
    T2D_CHECK_EQ(map.at(0, 0), 2);
    T2D_CHECK_EQ(map.at(4, 4), 2);
    T2D_CHECK_EQ(map.at(5, 5), 1);
    map.fill_rect(TileRect{60, 30, 20, 20}, 2);
    T2D_CHECK_EQ(map.count_tiles(2), 125u);
    T2D_CHECK_EQ(map.at(69, 39), 2);
    map.fill_rect(TileRect{200, 200, 5, 5}, 2);
    map.fill_rect(TileRect{0, 0, 0, 0}, 2);
    map.fill_rect(TileRect{10, 10, -3, -3}, 2);
    T2D_CHECK_EQ(map.count_tiles(2), 125u);

    // clearing a rect and clearing the whole map
    map.fill_rect(TileRect{60, 30, 10, 10}, kEmptyTile);
    T2D_CHECK_EQ(map.count_tiles(2), 25u);
    map.fill_rect(TileRect{0, 0, 70, 40}, kEmptyTile);
    T2D_CHECK_EQ(map.count_tiles(kEmptyTile), 70u * 40u);
    T2D_CHECK_EQ(map.count_tiles(1), 0u);
    T2D_CHECK_EQ(map.count_tiles(2), 0u);
}

T2D_TEST(map_coordinate_helpers) {
    const TileMap map(10, 10, 16.0f);
    T2D_CHECK_EQ(map.tile_to_world(0, 0), Vec2{});
    T2D_CHECK_EQ(map.tile_to_world(3, 4), (Vec2{48.0f, 64.0f}));
    T2D_CHECK_EQ(map.tile_to_world(-2, -1), (Vec2{-32.0f, -16.0f}));
    T2D_CHECK_EQ(map.tile_bounds(1, 2), (Aabb2{Vec2{16.0f, 32.0f}, Vec2{32.0f, 48.0f}}));
    T2D_CHECK_EQ(map.tile_bounds(0, 0).size(), (Vec2{16.0f, 16.0f}));
    T2D_CHECK_EQ(map.tile_bounds(1, 2).center(), (Vec2{24.0f, 40.0f}));
    // +Y points down: row 0 is the top row
    T2D_CHECK(map.tile_bounds(0, 0).min.y < map.tile_bounds(0, 1).min.y);
    T2D_CHECK_EQ(map.world_size(), (Vec2{160.0f, 160.0f}));

    // a non power of two tile size still maps corners exactly
    const TileMap odd(4, 4, 10.5f);
    T2D_CHECK_EQ(odd.tile_to_world(2, 3), (Vec2{21.0f, 31.5f}));
    T2D_CHECK_EQ(odd.world_size(), (Vec2{42.0f, 42.0f}));
    T2D_CHECK_EQ(odd.world_to_tiles(box_at({21.0f, 31.5f}, {10.5f, 10.5f})), (TileRect{2, 3, 1, 1}));
}

T2D_TEST(map_world_to_tiles_rounding) {
    const TileMap map(10, 10, 16.0f);
    T2D_CHECK_EQ(map.world_to_tiles(box_at({0.0f, 0.0f}, {16.0f, 16.0f})), (TileRect{0, 0, 1, 1}));
    // ending exactly on a boundary does not touch the next cell
    T2D_CHECK_EQ(map.world_to_tiles(box_at({0.0f, 0.0f}, {15.999f, 15.999f})), (TileRect{0, 0, 1, 1}));
    T2D_CHECK_EQ(map.world_to_tiles(box_at({0.0f, 0.0f}, {16.5f, 16.0f})), (TileRect{0, 0, 2, 1}));
    T2D_CHECK_EQ(map.world_to_tiles(box_at({8.0f, 8.0f}, {8.0f, 8.0f})), (TileRect{0, 0, 1, 1}));
    T2D_CHECK_EQ(map.world_to_tiles(box_at({16.0f, 16.0f}, {16.0f, 16.0f})), (TileRect{1, 1, 1, 1}));
    T2D_CHECK_EQ(map.world_to_tiles(box_at({-1.0f, -1.0f}, {2.0f, 2.0f})), (TileRect{-1, -1, 2, 2}));
    T2D_CHECK_EQ(map.world_to_tiles(box_at({31.0f, 32.0f}, {2.0f, 2.0f})), (TileRect{1, 2, 2, 1}));
    T2D_CHECK_EQ(map.world_to_tiles(box_at({16.0f, 4.0f}, {0.0f, 0.0f})), (TileRect{1, 0, 1, 1}));
}

T2D_TEST(map_visible_tiles) {
    const TileMap map(10, 10, 16.0f);   // 160 x 160 px
    T2D_CHECK_EQ(map.visible_tiles(box_at({0.0f, 0.0f}, {160.0f, 160.0f})), (TileRect{0, 0, 10, 10}));
    T2D_CHECK_EQ(map.visible_tiles(box_at({-32.0f, -32.0f}, {64.0f, 64.0f})), (TileRect{0, 0, 2, 2}));
    T2D_CHECK_EQ(map.visible_tiles(box_at({16.0f, 32.0f}, {16.0f, 16.0f})), (TileRect{1, 2, 1, 1}));
    T2D_CHECK_EQ(map.visible_tiles(box_at({15.0f, 15.0f}, {2.0f, 2.0f})), (TileRect{0, 0, 2, 2}));
    T2D_CHECK_EQ(map.visible_tiles(box_at({8.0f, 8.0f}, {0.0f, 0.0f})), (TileRect{0, 0, 1, 1}));
    T2D_CHECK_EQ(map.visible_tiles(box_at({-160.0f, -160.0f}, {640.0f, 640.0f})), (TileRect{0, 0, 10, 10}));
    T2D_CHECK(map.visible_tiles(box_at({200.0f, 0.0f}, {16.0f, 16.0f})).empty());
    T2D_CHECK(map.visible_tiles(box_at({0.0f, -40.0f}, {16.0f, 16.0f})).empty());
    T2D_CHECK(map.visible_tiles(box_at({160.0f, 160.0f}, {16.0f, 16.0f})).empty());
    T2D_CHECK(TileMap().visible_tiles(box_at({0.0f, 0.0f}, {16.0f, 16.0f})).empty());
    T2D_CHECK(TileMap(4, 4, 16.0f).visible_tiles(box_at({-1.0f, -1.0f}, {1.0f, 1.0f})).empty());
}

T2D_TEST(map_on_ground_probe) {
    TileMap map(10, 10);
    map.fill_rect(TileRect{0, 5, 10, 1}, 1);   // floor, top edge at y = 80
    const Vec2 size{12.0f, 16.0f};
    T2D_CHECK(map.is_on_ground(box_at({0.0f, 64.0f}, size), platformer()));
    T2D_CHECK(map.is_on_ground(box_at({0.0f, 63.9f}, size), platformer()));
    T2D_CHECK_FALSE(map.is_on_ground(box_at({0.0f, 63.0f}, size), platformer()));
    T2D_CHECK_FALSE(map.is_on_ground(box_at({0.0f, 0.0f}, size), platformer()));
    T2D_CHECK_FALSE(map.is_on_ground(box_at({0.0f, 30.0f}, size), platformer()));

    // one-way tiles support an entity, hazards and decor do not
    map.fill_rect(TileRect{0, 7, 10, 1}, 4);
    T2D_CHECK(map.is_on_ground(box_at({0.0f, 96.0f}, size), platformer()));
    map.fill_rect(TileRect{0, 8, 10, 1}, 5);
    T2D_CHECK_FALSE(map.is_on_ground(box_at({0.0f, 112.0f}, size), platformer()));
    map.fill_rect(TileRect{0, 8, 10, 1}, 7);
    T2D_CHECK_FALSE(map.is_on_ground(box_at({0.0f, 112.0f}, size), platformer()));

    // only the columns under the box matter
    TileMap narrow(10, 10);
    narrow.set(3, 5, 1);
    T2D_CHECK(narrow.is_on_ground(box_at({48.0f, 64.0f}, {8.0f, 16.0f}), platformer()));
    T2D_CHECK_FALSE(narrow.is_on_ground(box_at({64.0f, 64.0f}, {8.0f, 16.0f}), platformer()));

    // leaving the map through the bottom rests on the wall, but only when the flag is set
    const TileMap open(10, 10, 16.0f, false);
    T2D_CHECK(map.is_on_ground(box_at({0.0f, 144.0f}, size), platformer()));
    T2D_CHECK_FALSE(open.is_on_ground(box_at({0.0f, 144.0f}, size), platformer()));
}

// --------------------------------------------------------------------------------------- layers --

T2D_TEST(map_layers_are_independent_grids) {
    TileMap map(40, 40, 16.0f, true, 3);
    T2D_CHECK_EQ(map.layer_count(), 3);
    map.set(0, 4, 4, 1);
    map.set(1, 4, 4, 2);
    map.set(2, 4, 4, 3);
    T2D_CHECK_EQ(map.at(0, 4, 4), 1);
    T2D_CHECK_EQ(map.at(1, 4, 4), 2);
    T2D_CHECK_EQ(map.at(2, 4, 4), 3);
    T2D_CHECK_EQ(map.at(4, 4), 1);          // the two argument form is the base layer
    T2D_CHECK_EQ(map.topmost(4, 4), 3);     // and topmost() is "what is on this cell"
    T2D_CHECK_EQ(map.topmost(4, 4, TileMap::layer_mask(1)), 2);
    T2D_CHECK_EQ(map.topmost(4, 4, TileMap::layer_mask(0) | TileMap::layer_mask(2)), 3);
    T2D_CHECK_EQ(map.topmost(5, 5), kEmptyTile);

    // Layers do not share chunks: filling one leaves the others exactly as they were.
    map.fill(1, 9);
    T2D_CHECK_EQ(map.count_tiles(1, 9), 1600u);
    T2D_CHECK_EQ(map.count_tiles(0, 9), 0u);
    T2D_CHECK_EQ(map.count_tiles(2, 9), 0u);
    T2D_CHECK_EQ(map.at(0, 4, 4), 1);
    T2D_CHECK_EQ(map.at(1, 4, 4), 9);
    T2D_CHECK_EQ(map.count_tiles(9), 1600u);   // every layer

    // A layer that does not exist is refused, not written somewhere else.
    map.set(7, 1, 1, 5);
    map.set(-1, 1, 1, 5);
    map.fill_rect(9, TileRect{0, 0, 4, 4}, 5);
    T2D_CHECK_EQ(map.count_tiles(5), 0u);
    T2D_CHECK_EQ(map.at(7, 1, 1), kEmptyTile);
    T2D_CHECK_EQ(map.layer_count(), 3);

    // The layer count is clamped into what a mask can address.
    T2D_CHECK_EQ(TileMap(4, 4, 16.0f, true, 99).layer_count(), TileMap::kMaxLayers);
    T2D_CHECK_EQ(TileMap(4, 4, 16.0f, true, 0).layer_count(), 1);
    T2D_CHECK_EQ(TileMap(4, 4).layer_count(), 1);   // every existing caller keeps one layer

    // An unqualified fill means every layer.
    map.fill(kEmptyTile);
    T2D_CHECK_EQ(map.count_tiles(1), 0u);
    T2D_CHECK_EQ(map.count_tiles(kEmptyTile), 3u * 1600u);
}

T2D_TEST(layer_masks_decide_what_blocks) {
    // The same cell holds a decorative ground tile on layer 0 and a wall on layer 1: whether it
    // blocks is the caller's question, answered by the mask.
    TileMap map(8, 4, 16.0f, true, 2);
    map.fill(0, 7);          // background brick: decor, never solid
    map.set(1, 3, 2, 1);     // stone on the structure layer, cell (3, 2)
    const Tileset& set = platformer();
    T2D_CHECK(set.is_decor(7));
    T2D_CHECK(set.is_solid(1));

    T2D_CHECK(map.is_solid(3, 2, set));                                // every layer by default
    T2D_CHECK_FALSE(map.is_solid(3, 2, set, TileMap::layer_mask(0)));  // the ground layer does not block
    T2D_CHECK(map.is_solid(3, 2, set, TileMap::layer_mask(1)));
    T2D_CHECK_FALSE(map.is_solid(2, 2, set));
    T2D_CHECK_FALSE(map.is_solid(3, 2, set, TileMap::kNoLayers));

    const Aabb2 box = box_at(Vec2{48.0f, 32.0f}, Vec2{16.0f, 16.0f});
    T2D_CHECK(map.overlaps_solid(box, set));
    T2D_CHECK_FALSE(map.overlaps_solid(box, set, TileMap::layer_mask(0)));
    T2D_CHECK(map.overlaps_solid(box, set, TileMap::layer_mask(1)));
    T2D_CHECK(map.raycast_solid(Vec2{8.0f, 40.0f}, Vec2{1.0f, 0.0f}, 64.0f, set, TileMap::layer_mask(1)).has_value());
    T2D_CHECK_FALSE(
        map.raycast_solid(Vec2{8.0f, 40.0f}, Vec2{1.0f, 0.0f}, 64.0f, set, TileMap::layer_mask(0)).has_value());

    // Movement follows the mask: the box stops on the wall, and walks through it when the wall's
    // layer is not part of the mask.
    const Vec2 size{8.0f, 8.0f};
    const Aabb2 start = box_at(Vec2{16.0f, 32.0f}, size);
    const TileMap::MoveResult blocked =
        map.move_aabb(start, Vec2{100.0f, 0.0f}, 0.5f, set, false, TileMap::layer_mask(1));
    T2D_CHECK(blocked.hit_x);
    T2D_CHECK_LT(blocked.position.x + size.x, 48.0f + 0.001f);
    const TileMap::MoveResult through =
        map.move_aabb(start, Vec2{100.0f, 0.0f}, 0.5f, set, false, TileMap::layer_mask(0));
    T2D_CHECK_FALSE(through.hit_x);
    T2D_CHECK_GT(through.position.x, 48.0f);
    // A mask that names nothing at all is a map with no tiles: free movement, nothing to stand on.
    const TileMap::MoveResult void_walk =
        map.move_aabb(start, Vec2{100.0f, 0.0f}, 0.5f, set, false, TileMap::kNoLayers);
    T2D_CHECK_FALSE(void_walk.hit_x);
    T2D_CHECK_GT(void_walk.position.x, 48.0f);

    // is_on_ground() asks the same question: a floor tile on a layer the mask excludes is not floor.
    TileMap floor_map(4, 4, 16.0f, true, 2);
    floor_map.set(1, 1, 2, 1);   // stone on the structure layer, cell (1, 2)
    const Aabb2 resting = box_at(Vec2{16.0f, 16.0f}, Vec2{16.0f, 16.0f});
    T2D_CHECK(floor_map.is_on_ground(resting, set));
    T2D_CHECK(floor_map.is_on_ground(resting, set, TileMap::layer_mask(1)));
    T2D_CHECK_FALSE(floor_map.is_on_ground(resting, set, TileMap::layer_mask(0)));
}

T2D_TEST(checksum_covers_every_layer) {
    TileMap map(40, 40, 16.0f, true, 2);
    map.set(0, 5, 5, 1);
    T2D_CHECK_EQ(map.checksum(), reference_checksum(map));
    const u64 before = map.checksum();
    map.set(1, 5, 5, 2);   // the same cell, one layer up
    T2D_CHECK_NE(map.checksum(), before);
    T2D_CHECK_EQ(map.checksum(), reference_checksum(map));

    // The layer count is part of the hash, so two maps that differ only in that are not equal.
    TileMap one(4, 4, 16.0f, true, 1);
    TileMap two(4, 4, 16.0f, true, 2);
    one.set(0, 1, 1, 1);
    two.set(0, 1, 1, 1);
    T2D_CHECK_NE(one.checksum(), two.checksum());
    T2D_CHECK_EQ(one.checksum(), reference_checksum(one));
    T2D_CHECK_EQ(two.checksum(), reference_checksum(two));
}

T2D_TEST(a_multi_layer_map_round_trips_through_serialisation) {
    const TileMap map = random_map(70, 45, 3, 12345u);   // spans several chunk boundaries
    const usize cells = 70u * 45u;
    for (i32 layer = 0; layer < 3; ++layer) {
        // Every layer has something on it: a round trip that dropped the layers past the first would
        // otherwise pass while losing them.
        T2D_CHECK_LT(map.count_tiles(layer, kEmptyTile), cells);
    }
    T2D_CHECK_NE(map.count_tiles(0, kEmptyTile), map.count_tiles(1, kEmptyTile));   // different patterns

    const std::vector<u8> bytes = map.serialize();
    const std::optional<TileMap> restored = TileMap::deserialize(bytes);
    T2D_REQUIRE(restored.has_value());
    T2D_CHECK_EQ(restored->layer_count(), 3);
    T2D_CHECK(maps_equal(map, *restored));
    T2D_CHECK_EQ(restored->checksum(), map.checksum());

    // A one layer map still round trips, and an empty layer costs nothing.
    const TileMap single = random_map(40, 40, 7u);
    const std::optional<TileMap> single_back = TileMap::deserialize(single.serialize());
    T2D_REQUIRE(single_back.has_value());
    T2D_CHECK(maps_equal(single, *single_back));
    TileMap empty_layers(20, 20, 16.0f, true, 4);
    const std::optional<TileMap> empty_back = TileMap::deserialize(empty_layers.serialize());
    T2D_REQUIRE(empty_back.has_value());
    T2D_CHECK_EQ(empty_back->layer_count(), 4);
    T2D_CHECK(maps_equal(empty_layers, *empty_back));
    T2D_CHECK_EQ(empty_back->count_tiles(kEmptyTile), 4u * 400u);
}

T2D_TEST(a_map_blob_it_cannot_trust_is_refused) {
    std::vector<u8> good;
    push_header(good, 2, 2, 4, 16.0f);
    push_varint(good, 4);
    push_u16(good, 1);
    T2D_REQUIRE(TileMap::deserialize(good).has_value());   // the control case parses

    // Version 1 is the single layer format: refused, never guessed at.
    std::vector<u8> old;
    push_header(old, 2, 2, 4, 16.0f, 1);
    push_varint(old, 4);
    push_u16(old, 1);
    T2D_CHECK_FALSE(TileMap::deserialize(old).has_value());

    // No layers at all, and more layers than a mask can address.
    for (const u32 layers : {0u, 33u}) {
        std::vector<u8> bad;
        push_header(bad, 2, 2, 4, 16.0f, 2, 1, layers);
        push_varint(bad, 4);
        push_u16(bad, 1);
        T2D_CHECK_FALSE(TileMap::deserialize(bad).has_value());
    }

    // A run that reaches past the end of its layer.
    std::vector<u8> overflow;
    push_header(overflow, 2, 2, 4, 16.0f);
    push_varint(overflow, 5);
    push_u16(overflow, 1);
    T2D_CHECK_FALSE(TileMap::deserialize(overflow).has_value());

    // Two layers announced, one layer of runs written: the second layer is missing, not empty.
    std::vector<u8> truncated_layers;
    push_header(truncated_layers, 2, 2, 4, 16.0f, 2, 1, 2);
    push_varint(truncated_layers, 4);
    push_u16(truncated_layers, 1);
    T2D_CHECK_FALSE(TileMap::deserialize(truncated_layers).has_value());

    // A cell count that does not match the dimensions.
    std::vector<u8> mismatched;
    push_header(mismatched, 2, 2, 5, 16.0f);
    push_varint(mismatched, 5);
    push_u16(mismatched, 1);
    T2D_CHECK_FALSE(TileMap::deserialize(mismatched).has_value());

    // 4096 x 4096 is the biggest layer; thirty two of them is not a map, it is an allocation.
    std::vector<u8> huge;
    push_header(huge, 4096, 4096, 4096u * 4096u, 16.0f, 2, 1, 32);
    T2D_CHECK_FALSE(TileMap::deserialize(huge).has_value());

    // Trailing bytes are not a clean blob.
    std::vector<u8> trailing = good;
    trailing.push_back(0u);
    T2D_CHECK_FALSE(TileMap::deserialize(trailing).has_value());

    // A truncated blob of every length is refused rather than half read.
    for (usize length = 0; length < good.size(); ++length) {
        T2D_CHECK_FALSE(TileMap::deserialize(ConstSpan<const u8>(good.data(), length)).has_value());
    }
}

T2D_TEST(ascii_art_stamps_into_one_layer) {
    TileMap map(6, 3, 16.0f, true, 2);
    const std::vector<std::string> art{"##..", "....", "..=="};
    map.stamp_ascii(1, art, TileMap::default_legend(), 1, 0);

    constexpr TileId kStone = 1;
    constexpr TileId kPlatform = 4;
    T2D_CHECK_EQ(map.at(1, 1, 0), kStone);       // art (0,0) landed on map (1,0)
    T2D_CHECK_EQ(map.at(1, 2, 0), kStone);
    T2D_CHECK_EQ(map.at(1, 3, 2), kPlatform);    // art (2,2) landed on map (3,2)
    T2D_CHECK_EQ(map.at(1, 4, 2), kPlatform);
    T2D_CHECK_EQ(map.at(1, 0, 0), kEmptyTile);
    T2D_CHECK_EQ(map.count_tiles(1, kStone), 2u);
    T2D_CHECK_EQ(map.count_tiles(0, kStone), 0u);   // nothing leaked into the base layer

    // The art comes back out of the layer it went into, and the other layer stays empty.
    const std::vector<std::string> rows = map.to_ascii(1, TileMap::default_reverse_legend());
    T2D_REQUIRE(rows.size() == 3u);
    T2D_CHECK_EQ(rows[0], std::string(".##..."));
    T2D_CHECK_EQ(rows[1], std::string("......"));
    T2D_CHECK_EQ(rows[2], std::string("...==."));
    const std::vector<std::string> base = map.to_ascii(TileMap::default_reverse_legend());
    T2D_CHECK_EQ(base[0], std::string("......"));

    // A layer that does not exist, and an offset that falls outside the map: reported, not applied.
    const u32 before = static_cast<u32>(map.count_tiles(kStone) + map.count_tiles(kPlatform));
    map.stamp_ascii(2, art, TileMap::default_legend());
    map.stamp_ascii(1, art, TileMap::default_legend(), 100, 100);
    map.stamp_ascii(-1, art, TileMap::default_legend());
    T2D_CHECK_EQ(static_cast<u32>(map.count_tiles(kStone) + map.count_tiles(kPlatform)), before);
    T2D_CHECK(map.to_ascii(2, TileMap::default_reverse_legend()).empty());

    // from_ascii() is the one layer case of the same thing.
    const std::optional<TileMap> built = TileMap::from_ascii(art, TileMap::default_legend(), 16.0f);
    T2D_REQUIRE(built.has_value());
    T2D_CHECK_EQ(built->layer_count(), 1);
    T2D_CHECK_EQ(built->at(0, 0), kStone);
    T2D_CHECK_EQ(built->at(2, 2), kPlatform);
}

T2D_TEST_MAIN
