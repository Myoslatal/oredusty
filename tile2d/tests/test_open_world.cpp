// Tile2D - open world tests: the relative coordinate space and its movable origin, chunk arithmetic,
// streaming, deterministic generation keyed by absolute chunks, edits, the store and the resident-only
// queries. The world is always inspected through its public API. The byte store is an in-memory
// std::map keyed by ChunkKey, which is itself a check that generation and saving never see a relative
// coordinate.
#include <support/test_support.h>

#include <t2d/core/camera2d.h>
#include <t2d/core/rng.h>
#include <t2d/sim/chunk_store.h>
#include <t2d/sim/open_world.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace t2d;

namespace {

/// The tileset the query tests use: one tile per flag the queries ask about. Game tilesets are the
/// game's data - the framework ships none.
[[nodiscard]] const Tileset& test_tileset() {
    static const Tileset tileset = [] {
        Tileset built;
        built.set_atlas_grid(8, 1);
        built.add(TileDef{kEmptyTile, TileFlag::None, 0, 0});
        built.add(TileDef{1, TileFlag::Solid, 1, 0});
        built.add(TileDef{2, TileFlag::Solid, 2, 0});
        built.add(TileDef{3, TileFlag::Solid, 3, 0});
        built.add(TileDef{4, TileFlag::OneWay, 4, 0});
        built.add(TileDef{5, TileFlag::Hazard, 5, 0});
        built.add(TileDef{6, TileFlag::Decor, 6, 0});
        built.add(TileDef{7, TileFlag::Decor, 7, 0});
        built.add(TileDef{8, TileFlag::Ladder, 8, 0});
        return built;
    }();
    return tileset;
}

[[nodiscard]] OpenWorldConfig world_config(u64 seed = 1u) {
    OpenWorldConfig config;
    config.tile_size = 16.0f;
    config.layers = 1;
    config.seed = seed;
    config.load_margin_chunks = 1;
    config.unload_margin_chunks = 2;
    config.max_loaded_chunks = 0;   // unlimited unless a test says otherwise
    return config;
}

[[nodiscard]] Aabb2 box_at(Vec2 top_left, Vec2 size) { return Aabb2::from_top_left(top_left, size); }

/// The relative view that exactly covers one relative chunk, in relative world units.
[[nodiscard]] Aabb2 view_of_chunk(i32 chunk_x, i32 chunk_y) {
    const f32 tile = 16.0f;
    const f32 size = static_cast<f32>(WorldChunk::kSize) * tile;
    return box_at(Vec2{static_cast<f32>(chunk_x) * size, static_cast<f32>(chunk_y) * size},
                  Vec2{size, size});
}

[[nodiscard]] bool ascending_by_y_then_x(const std::vector<ChunkKey>& keys) {
    for (usize index = 1; index < keys.size(); ++index) {
        if (keys[index - 1].y > keys[index].y) return false;
        if (keys[index - 1].y == keys[index].y && keys[index - 1].x >= keys[index].x) return false;
    }
    return true;
}

[[nodiscard]] bool ascending_relative_by_y_then_x(const std::vector<ChunkCoord>& coords) {
    for (usize index = 1; index < coords.size(); ++index) {
        if (coords[index - 1].y > coords[index].y) return false;
        if (coords[index - 1].y == coords[index].y && coords[index - 1].x >= coords[index].x) return false;
    }
    return true;
}

/// Writes whole-chunk content through tiles(), the way a generator is documented to: the chunk stays
/// clean because generation is not an edit.
void fill_chunk(WorldChunk& chunk, TileId id) {
    for (i32 layer = 0; layer < chunk.layers(); ++layer) {
        for (TileId& cell : chunk.tiles(layer)) cell = id;
    }
}

[[nodiscard]] OpenWorld::ChunkGenerator constant_generator(TileId id) {
    return [id](const ChunkKey&, WorldChunk& chunk) { fill_chunk(chunk, id); };
}

/// Fills every layer from (seed, ABSOLUTE chunk key) alone, which is what "a deterministic open world"
/// means when the origin may move.
[[nodiscard]] OpenWorld::ChunkGenerator pattern_generator(u64 seed, u32 variety = 5u) {
    return [seed, variety](const ChunkKey& key, WorldChunk& chunk) {
        Rng rng(OpenWorld::chunk_seed(seed, key));
        for (i32 layer = 0; layer < chunk.layers(); ++layer) {
            for (TileId& cell : chunk.tiles(layer)) {
                const u32 roll = rng.next_bounded(10u);
                cell = roll < 3u ? kEmptyTile : static_cast<TileId>(1u + roll % variety);
            }
        }
    };
}

struct ChunkKeyLess {
    [[nodiscard]] bool operator()(const ChunkKey& a, const ChunkKey& b) const {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    }
};

using ByteStore = std::map<ChunkKey, std::vector<u8>, ChunkKeyLess>;

[[nodiscard]] OpenWorld::ChunkLoader store_loader(ByteStore& store) {
    return [&store](const ChunkKey& key) -> std::optional<std::vector<u8>> {
        const auto found = store.find(key);
        if (found == store.end()) return std::nullopt;
        return found->second;
    };
}

[[nodiscard]] OpenWorld::ChunkSaver store_saver(ByteStore& store) {
    return [&store](const ChunkKey& key, ConstSpan<const u8> bytes) {
        store[key] = std::vector<u8>(bytes.begin(), bytes.end());
        return true;
    };
}

[[nodiscard]] OpenWorld::ChunkSaver counting_saver(ByteStore& store, int& saves) {
    return [&store, &saves](const ChunkKey& key, ConstSpan<const u8> bytes) {
        ++saves;
        store[key] = std::vector<u8>(bytes.begin(), bytes.end());
        return true;
    };
}

[[nodiscard]] OpenWorld::ChunkSaver failing_saver(int& attempts) {
    return [&attempts](const ChunkKey&, ConstSpan<const u8>) {
        ++attempts;
        return false;
    };
}

/// A real ChunkStore file under the system temporary directory: the persistence tests here use the
/// engine's own file store, not the in-memory map, and this owns the scratch directory around it.
struct ScratchStore {
    std::filesystem::path file;

    explicit ScratchStore(std::string_view name) : file(scratch_directory() / std::string(name)) {
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
    }
    ~ScratchStore() {
        std::error_code ignored;
        std::filesystem::remove_all(file.parent_path(), ignored);
    }
    ScratchStore(const ScratchStore&) = delete;
    ScratchStore& operator=(const ScratchStore&) = delete;

    [[nodiscard]] std::string string() const { return file.string(); }

private:
    [[nodiscard]] static std::filesystem::path scratch_directory() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               ("t2d_open_world_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
    }
};

[[nodiscard]] ChunkStore open_required_store(const ScratchStore& scratch) {
    std::string error;
    std::optional<ChunkStore> store = ChunkStore::open(scratch.string(), &error);
    T2D_REQUIRE(store.has_value());
    T2D_CHECK_MSG(error.empty(), "a successful open reported '{}'", error);
    return std::move(*store);
}

/// The resident chunk with this absolute key, through the relative public API.
[[nodiscard]] const WorldChunk* chunk_by_key(const OpenWorld& world, const ChunkKey& key) {
    const std::optional<ChunkCoord> relative = world.relative_of(key);
    if (!relative.has_value()) return nullptr;
    return world.find_chunk(*relative);
}

/// The FNV-1a walk WorldChunk::checksum() documents: layer count, then every tile id row major per
/// layer, bottom to top.
[[nodiscard]] u64 reference_chunk_checksum(const WorldChunk& chunk) {
    u64 hash = 0xCBF29CE484222325ull;
    const auto mix = [&hash](u64 value, i32 bytes) {
        for (i32 index = 0; index < bytes; ++index) {
            hash = (hash ^ ((value >> (index * 8)) & 0xFFu)) * 0x100000001B3ull;
        }
    };
    mix(static_cast<u64>(static_cast<u32>(chunk.layers())), 4);
    for (i32 layer = 0; layer < chunk.layers(); ++layer) {
        for (const TileId id : chunk.tiles(layer)) mix(id, 2);
    }
    return hash;
}

// --- hand rolled chunk blobs, so a test can feed deserialize() exactly the defect it means to -----

void push_varint(std::vector<u8>& out, u32 value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<u8>(value) | 0x80u);
        value >>= 7;
    }
    out.push_back(static_cast<u8>(value));
}

void push_u16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>(value >> 8));
}

[[nodiscard]] std::vector<u8> chunk_blob_header(u32 layers, u8 version = 1u) {
    std::vector<u8> bytes;
    bytes.push_back(static_cast<u8>('T'));
    bytes.push_back(static_cast<u8>('2'));
    bytes.push_back(static_cast<u8>('D'));
    bytes.push_back(static_cast<u8>('C'));
    bytes.push_back(version);
    push_varint(bytes, layers);
    return bytes;
}

void append_run(std::vector<u8>& bytes, u32 run, u16 id) {
    push_varint(bytes, run);
    push_u16(bytes, id);
}

[[nodiscard]] std::vector<u8> valid_empty_blob(u32 layers) {
    std::vector<u8> bytes = chunk_blob_header(layers);
    for (u32 layer = 0; layer < layers; ++layer) append_run(bytes, WorldChunk::kCellCount, kEmptyTile);
    return bytes;
}

} // namespace

// --------------------------------------------------------------------------- coordinates --

T2D_TEST(relative_chunk_coordinates_floor_divide_and_handle_int32_extremes) {
    T2D_CHECK(OpenWorld::chunk_of(0, 0) == (ChunkCoord{0, 0}));
    T2D_CHECK(OpenWorld::chunk_of(31, 31) == (ChunkCoord{0, 0}));
    T2D_CHECK(OpenWorld::chunk_of(32, 32) == (ChunkCoord{1, 1}));
    T2D_CHECK(OpenWorld::chunk_of(-1, -1) == (ChunkCoord{-1, -1}));
    T2D_CHECK(OpenWorld::chunk_of(-32, -32) == (ChunkCoord{-1, -1}));
    T2D_CHECK(OpenWorld::chunk_of(-33, -33) == (ChunkCoord{-2, -2}));
    T2D_CHECK(OpenWorld::chunk_of(-1, 32) == (ChunkCoord{-1, 1}));
    T2D_CHECK(OpenWorld::chunk_of(32, -1) == (ChunkCoord{1, -1}));

    T2D_CHECK_EQ(OpenWorld::local_of(-33), 31);
    T2D_CHECK_EQ(OpenWorld::local_of(-32), 0);
    T2D_CHECK_EQ(OpenWorld::local_of(-1), 31);
    T2D_CHECK_EQ(OpenWorld::local_of(0), 0);
    T2D_CHECK_EQ(OpenWorld::local_of(31), 31);
    T2D_CHECK_EQ(OpenWorld::local_of(32), 0);

    // The pair (chunk, local) reconstructs the cell, for every sign and for the i32 extremes. The
    // multiply goes through i64, because chunk.x * 32 at INT32_MIN is below i32's range.
    const i32 sample_cells[] = {-1000000, -33, -32, -1, 0, 1, 31, 32, 1000000,
                                std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()};
    for (const i32 cell : sample_cells) {
        const ChunkCoord chunk = OpenWorld::chunk_of(cell, cell);
        const i32 local = OpenWorld::local_of(cell);
        T2D_CHECK(local >= 0 && local < WorldChunk::kSize);
        T2D_CHECK_EQ(static_cast<i64>(chunk.x) * WorldChunk::kSize + local, static_cast<i64>(cell));
        T2D_CHECK_EQ(static_cast<i64>(chunk.y) * WorldChunk::kSize + local, static_cast<i64>(cell));
    }
    T2D_CHECK_EQ(OpenWorld::local_of(std::numeric_limits<i32>::min()), 0);
    T2D_CHECK_EQ(static_cast<i64>(OpenWorld::chunk_of(std::numeric_limits<i32>::max(),
                                                     std::numeric_limits<i32>::min()).x) * WorldChunk::kSize +
                     OpenWorld::local_of(std::numeric_limits<i32>::max()),
                 static_cast<i64>(std::numeric_limits<i32>::max()));

    // The same extremes through the world: set()/at() address the chunks both ends of i32 land in,
    // and relative bounds stay finite (they are relative f32, so no absolute value leaks in).
    OpenWorld world;
    T2D_CHECK(world.set(std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max(), 3));
    T2D_CHECK_EQ(world.at(std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()), 3);
    T2D_CHECK(world.set(std::numeric_limits<i32>::max(), std::numeric_limits<i32>::min(), 4));
    T2D_CHECK_EQ(world.at(std::numeric_limits<i32>::max(), std::numeric_limits<i32>::min()), 4);
    const Aabb2 cell_box = world.cell_bounds(std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max());
    T2D_CHECK(std::isfinite(cell_box.min.x) && std::isfinite(cell_box.max.x));
    T2D_CHECK(std::isfinite(cell_box.min.y) && std::isfinite(cell_box.max.y));
    const Aabb2 chunk_box =
        world.chunk_bounds(OpenWorld::chunk_of(std::numeric_limits<i32>::max(), std::numeric_limits<i32>::min()));
    T2D_CHECK(std::isfinite(chunk_box.min.x) && std::isfinite(chunk_box.max.x));
    T2D_CHECK(std::isfinite(chunk_box.min.y) && std::isfinite(chunk_box.max.y));
}

// --------------------------------------------------------------------------- WorldChunk --

T2D_TEST(world_chunk_construction_bounds_and_layer_clamping) {
    WorldChunk one;
    T2D_CHECK_EQ(one.layers(), 1);
    T2D_CHECK_EQ(one.tiles(0).size(), WorldChunk::kCellCount);
    T2D_CHECK_EQ(WorldChunk(0).layers(), 1);
    T2D_CHECK_EQ(WorldChunk(-5).layers(), 1);
    T2D_CHECK_EQ(WorldChunk(99).layers(), TileMap::kMaxLayers);
    T2D_CHECK_EQ(WorldChunk(3).tiles(2).size(), WorldChunk::kCellCount);

    T2D_CHECK(one.tiles(1).empty());
    T2D_CHECK(one.tiles(-1).empty());
    T2D_CHECK(WorldChunk(2).tiles(2).empty());
    T2D_CHECK(WorldChunk(2).tiles(-1).empty());

    T2D_CHECK_EQ(one.at(0, 0, 0), kEmptyTile);
    T2D_CHECK_EQ(one.at(1, 0, 0), kEmptyTile);
    T2D_CHECK_EQ(one.at(0, -1, 0), kEmptyTile);
    T2D_CHECK_EQ(one.at(0, 0, -1), kEmptyTile);
    T2D_CHECK_EQ(one.at(0, 32, 0), kEmptyTile);
    T2D_CHECK_EQ(one.at(0, 0, 32), kEmptyTile);

    one.set(0, 5, 6, 3);
    T2D_CHECK_EQ(one.at(0, 5, 6), 3);
    T2D_CHECK_EQ(one.at(0, 6, 5), kEmptyTile);          // row major: x is the column
    T2D_CHECK_EQ(one.tiles(0)[static_cast<usize>(6 * WorldChunk::kSize + 5)], 3);
    T2D_CHECK_EQ(one.count_non_empty(), 1u);
    T2D_CHECK(one.dirty());

    one.set(0, -1, 0, 9);     // out of bounds writes are ignored
    one.set(0, 0, 32, 9);
    one.set(7, 0, 0, 9);      // and so are writes to a layer that does not exist
    T2D_CHECK_EQ(one.count_non_empty(), 1u);

    one.clear_dirty();
    T2D_CHECK_FALSE(one.dirty());
    one.set(0, 1, 1, 2);      // a valid write marks the chunk dirty again
    T2D_CHECK(one.dirty());
    one.clear_dirty();
    one.mark_dirty();
    T2D_CHECK(one.dirty());

    // tiles() is the raw storage a generator writes through; unlike set() it is not an edit.
    WorldChunk raw;
    raw.tiles(0)[0] = 5;
    T2D_CHECK_EQ(raw.at(0, 0, 0), 5);
    T2D_CHECK_FALSE(raw.dirty());
}

T2D_TEST(world_chunk_fill_counts_and_layer_guards) {
    WorldChunk chunk(2);
    chunk.fill(1, 4);
    T2D_CHECK(chunk.dirty());
    T2D_CHECK_EQ(chunk.count_non_empty(), WorldChunk::kCellCount);
    T2D_CHECK_EQ(chunk.at(1, 0, 0), 4);
    T2D_CHECK_EQ(chunk.at(1, 31, 31), 4);
    T2D_CHECK_EQ(chunk.at(0, 0, 0), kEmptyTile);   // layers are independent

    chunk.clear_dirty();
    chunk.fill(0, kEmptyTile);
    T2D_CHECK(chunk.dirty());

    chunk.clear_dirty();
    chunk.fill(5, 9);      // layer does not exist: ignored, and nothing was written
    chunk.fill(-1, 9);
    T2D_CHECK_FALSE(chunk.dirty());
    T2D_CHECK_EQ(chunk.count_non_empty(), WorldChunk::kCellCount);

    chunk.fill(0, 7);
    T2D_CHECK_EQ(chunk.count_non_empty(), 2u * WorldChunk::kCellCount);
    chunk.fill(0, kEmptyTile);
    T2D_CHECK_EQ(chunk.count_non_empty(), WorldChunk::kCellCount);
    T2D_CHECK_EQ(chunk.at(0, 0, 0), kEmptyTile);
    T2D_CHECK_EQ(chunk.at(1, 0, 0), 4);
}

T2D_TEST(world_chunk_checksum_covers_layers_and_tiles) {
    WorldChunk two_layers(2);
    T2D_CHECK_EQ(two_layers.checksum(), reference_chunk_checksum(two_layers));

    WorldChunk one_changed(2);
    one_changed.set(0, 0, 0, 1);
    T2D_CHECK_NE(two_layers.checksum(), one_changed.checksum());
    T2D_CHECK_EQ(one_changed.checksum(), reference_chunk_checksum(one_changed));

    WorldChunk other_layer(2);
    other_layer.set(1, 0, 0, 1);
    T2D_CHECK_NE(one_changed.checksum(), other_layer.checksum());
    T2D_CHECK_EQ(other_layer.checksum(), reference_chunk_checksum(other_layer));

    // The layer count is part of the hash: the same base layer on a one layer and a two layer chunk
    // must not collide.
    WorldChunk one_layer(1);
    one_layer.set(0, 0, 0, 1);
    T2D_CHECK_NE(one_layer.checksum(), one_changed.checksum());

    // A difference in the last cell is still a difference in the hash.
    WorldChunk edge(1);
    edge.set(0, 31, 31, 1);
    T2D_CHECK_NE(edge.checksum(), WorldChunk(1).checksum());
    T2D_CHECK_EQ(edge.checksum(), reference_chunk_checksum(edge));
}

T2D_TEST(world_chunk_serialisation_round_trips) {
    WorldChunk chunk(3);
    Rng rng(2024u);
    for (i32 layer = 0; layer < chunk.layers(); ++layer) {
        for (i32 y = 0; y < WorldChunk::kSize; ++y) {
            for (i32 x = 0; x < WorldChunk::kSize; ++x) {
                const u32 roll = rng.next_bounded(100u);
                chunk.set(layer, x, y, roll < 40u ? kEmptyTile : static_cast<TileId>(1u + roll % 6u));
            }
        }
    }
    const std::vector<u8> bytes = chunk.serialize();
    T2D_REQUIRE(!bytes.empty());
    T2D_CHECK_EQ(bytes[0], static_cast<u8>('T'));
    T2D_CHECK_EQ(bytes[1], static_cast<u8>('2'));
    T2D_CHECK_EQ(bytes[2], static_cast<u8>('D'));
    T2D_CHECK_EQ(bytes[3], static_cast<u8>('C'));
    T2D_CHECK_EQ(bytes[4], 1);
    T2D_CHECK_EQ(bytes[5], 3);   // one byte varint layer count

    const std::optional<WorldChunk> restored = WorldChunk::deserialize(bytes, 3);
    T2D_REQUIRE(restored.has_value());
    T2D_CHECK_EQ(restored->layers(), 3);
    T2D_CHECK_EQ(restored->checksum(), chunk.checksum());
    T2D_CHECK_FALSE(restored->dirty());
    for (i32 layer = 0; layer < chunk.layers(); ++layer) {
        T2D_CHECK_EQ(restored->tiles(layer).size(), chunk.tiles(layer).size());
        T2D_CHECK(std::equal(restored->tiles(layer).begin(), restored->tiles(layer).end(),
                             chunk.tiles(layer).begin()));
    }

    // An empty chunk still encodes valid runs: one run of 1024 empty cells per layer.
    const std::vector<u8> empty_bytes = WorldChunk().serialize();
    T2D_CHECK_EQ(empty_bytes.size(), 10u);
    T2D_CHECK_EQ(empty_bytes[4], 1);
    T2D_CHECK_EQ(empty_bytes[5], 1);
    T2D_CHECK_EQ(empty_bytes[6], 0x80);
    T2D_CHECK_EQ(empty_bytes[7], 0x08);
    T2D_CHECK_EQ(empty_bytes[8], 0);
    T2D_CHECK_EQ(empty_bytes[9], 0);
    const std::optional<WorldChunk> empty_back = WorldChunk::deserialize(empty_bytes, 1);
    T2D_REQUIRE(empty_back.has_value());
    T2D_CHECK_EQ(empty_back->checksum(), WorldChunk().checksum());
    T2D_CHECK_EQ(empty_back->count_non_empty(), 0u);

    // A filled layer compresses to one run too, and comes back with the same tiles.
    WorldChunk full(1);
    full.fill(0, 5);
    full.clear_dirty();
    const std::vector<u8> full_bytes = full.serialize();
    T2D_CHECK_EQ(full_bytes.size(), 10u);
    const std::optional<WorldChunk> full_back = WorldChunk::deserialize(full_bytes, 1);
    T2D_REQUIRE(full_back.has_value());
    T2D_CHECK_EQ(full_back->checksum(), full.checksum());
    T2D_CHECK_EQ(full_back->at(0, 0, 0), 5);
    T2D_CHECK_EQ(full_back->at(0, 31, 31), 5);
}

T2D_TEST(chunk_deserialize_rejects_every_malformed_class) {
    T2D_CHECK_FALSE(WorldChunk::deserialize(ConstSpan<const u8>{}, 1).has_value());

    const std::vector<u8> valid = valid_empty_blob(1);
    T2D_REQUIRE(WorldChunk::deserialize(valid, 1).has_value());
    T2D_CHECK_EQ(WorldChunk::deserialize(valid, 2), std::nullopt);   // wrong expected layer count

    std::vector<u8> bad_magic = valid;
    bad_magic.at(0) = static_cast<u8>('X');
    T2D_CHECK_FALSE(WorldChunk::deserialize(bad_magic, 1).has_value());

    std::vector<u8> bad_version = valid;
    bad_version.at(4) = 2;
    T2D_CHECK_FALSE(WorldChunk::deserialize(bad_version, 1).has_value());

    std::vector<u8> no_layers = chunk_blob_header(0);
    T2D_CHECK_FALSE(WorldChunk::deserialize(no_layers, 1).has_value());

    std::vector<u8> too_many_layers = chunk_blob_header(static_cast<u32>(TileMap::kMaxLayers) + 1u);
    T2D_CHECK_FALSE(WorldChunk::deserialize(too_many_layers, TileMap::kMaxLayers).has_value());

    std::vector<u8> layer_mismatch = valid_empty_blob(2);
    T2D_CHECK_FALSE(WorldChunk::deserialize(layer_mismatch, 1).has_value());
    T2D_CHECK_FALSE(WorldChunk::deserialize(valid, 0).has_value());
    T2D_CHECK_FALSE(WorldChunk::deserialize(valid, TileMap::kMaxLayers + 1).has_value());

    std::vector<u8> zero_run = chunk_blob_header(1);
    append_run(zero_run, 0u, kEmptyTile);
    append_run(zero_run, WorldChunk::kCellCount, kEmptyTile);
    T2D_CHECK_FALSE(WorldChunk::deserialize(zero_run, 1).has_value());

    std::vector<u8> run_too_long = chunk_blob_header(1);
    append_run(run_too_long, static_cast<u32>(WorldChunk::kCellCount) + 1u, 1u);
    T2D_CHECK_FALSE(WorldChunk::deserialize(run_too_long, 1).has_value());

    // A run that fits the first layer but not the second one is still refused.
    std::vector<u8> second_layer_overrun = chunk_blob_header(2);
    append_run(second_layer_overrun, WorldChunk::kCellCount, 0u);
    append_run(second_layer_overrun, 1u, 0u);
    append_run(second_layer_overrun, WorldChunk::kCellCount, 0u);
    T2D_CHECK_FALSE(WorldChunk::deserialize(second_layer_overrun, 2).has_value());

    std::vector<u8> truncated = valid;
    truncated.pop_back();
    T2D_CHECK_FALSE(WorldChunk::deserialize(truncated, 1).has_value());
    T2D_CHECK_FALSE(WorldChunk::deserialize(ConstSpan<const u8>(valid.data(), 6), 1).has_value());

    std::vector<u8> trailing = valid;
    trailing.push_back(0u);
    T2D_CHECK_FALSE(WorldChunk::deserialize(trailing, 1).has_value());

    // One layer of runs is not enough for a two layer blob.
    std::vector<u8> missing_layer = chunk_blob_header(2);
    append_run(missing_layer, WorldChunk::kCellCount, 0u);
    T2D_CHECK_FALSE(WorldChunk::deserialize(missing_layer, 2).has_value());

    T2D_CHECK(WorldChunk::deserialize(valid_empty_blob(2), 2).has_value());
}

// --------------------------------------------------------------------------- generation --

T2D_TEST(chunk_seed_and_rng_are_deterministic_and_varied) {
    const ChunkKey origin{0, 0};
    const ChunkKey right{1, 0};
    const ChunkKey down{0, 1};
    const ChunkKey diagonal{-1, -1};

    T2D_CHECK_EQ(OpenWorld::chunk_seed(7u, origin), OpenWorld::chunk_seed(7u, origin));
    T2D_CHECK_NE(OpenWorld::chunk_seed(7u, origin), OpenWorld::chunk_seed(8u, origin));
    T2D_CHECK_NE(OpenWorld::chunk_seed(7u, origin), OpenWorld::chunk_seed(7u, right));
    T2D_CHECK_NE(OpenWorld::chunk_seed(7u, origin), OpenWorld::chunk_seed(7u, down));
    T2D_CHECK_NE(OpenWorld::chunk_seed(7u, origin), OpenWorld::chunk_seed(7u, diagonal));
    T2D_CHECK_NE(OpenWorld::chunk_seed(0u, right), OpenWorld::chunk_seed(0u, down));

    // Absolute identity is all that matters: the same key far from the origin is the same seed.
    T2D_CHECK_EQ(OpenWorld::chunk_seed(7u, ChunkKey{1000000, -1000000}),
                 OpenWorld::chunk_seed(7u, ChunkKey{1000000, -1000000}));

    std::vector<u64> neighbourhood;
    for (i64 y = -3; y <= 3; ++y) {
        for (i64 x = -3; x <= 3; ++x) neighbourhood.push_back(OpenWorld::chunk_seed(99u, ChunkKey{x, y}));
    }
    std::sort(neighbourhood.begin(), neighbourhood.end());
    const auto unique_end = std::unique(neighbourhood.begin(), neighbourhood.end());
    T2D_CHECK_EQ(static_cast<usize>(unique_end - neighbourhood.begin()), 49u);

    OpenWorld world(world_config(7u));
    T2D_CHECK_EQ(world.seed(), 7u);
    Rng from_world = world.chunk_rng(right);
    Rng from_seed(OpenWorld::chunk_seed(7u, right));
    for (i32 draw = 0; draw < 4; ++draw) {
        T2D_CHECK_EQ(from_world.next_u64(), from_seed.next_u64());
    }
    T2D_CHECK_NE(world.chunk_rng(origin).next_u64(), world.chunk_rng(right).next_u64());
}

T2D_TEST(generation_is_absolute_in_any_load_order_or_origin) {
    constexpr u64 seed = 0x1234ABCDu;
    OpenWorld first(world_config(seed));
    OpenWorld second(world_config(seed));
    first.set_generator(pattern_generator(seed));
    second.set_generator(pattern_generator(seed));

    const std::vector<ChunkCoord> coords{{0, 0}, {1, 0}, {-1, -2}, {5, 3}};
    for (const ChunkCoord& coord : coords) first.ensure_chunk(coord);
    for (auto it = coords.rbegin(); it != coords.rend(); ++it) second.ensure_chunk(*it);

    for (const ChunkCoord& coord : coords) {
        const WorldChunk* a = first.find_chunk(coord);
        const WorldChunk* b = second.find_chunk(coord);
        T2D_REQUIRE(a != nullptr);
        T2D_REQUIRE(b != nullptr);
        T2D_CHECK_EQ(a->checksum(), b->checksum());
        T2D_CHECK_FALSE(a->dirty());
        T2D_CHECK_FALSE(b->dirty());
    }

    // Rebase the second world: the absolute chunk {1, 0} keeps its content and its bytes.
    const ChunkKey absolute_right{1, 0};
    const WorldChunk* before = first.find_chunk(ChunkCoord{1, 0});
    T2D_REQUIRE(before != nullptr);
    const u64 checksum = before->checksum();
    const std::vector<u8> bytes = before->serialize();
    const OpenWorld::OriginShift shift = second.set_origin(ChunkKey{1000, -1000});
    T2D_CHECK_EQ(shift.x, 1000 * WorldChunk::kSize);
    T2D_CHECK_EQ(shift.y, -1000 * WorldChunk::kSize);
    const std::optional<ChunkCoord> moved = second.relative_of(absolute_right);
    T2D_REQUIRE(moved.has_value());
    T2D_CHECK_EQ(second.absolute_of(*moved), absolute_right);
    T2D_CHECK_EQ(chunk_by_key(second, absolute_right)->checksum(), checksum);
    T2D_CHECK(chunk_by_key(second, absolute_right)->serialize() == bytes);

    // A third world that starts at the far origin generates the same absolute chunk from scratch.
    OpenWorld rebased(world_config(seed));
    rebased.set_generator(pattern_generator(seed));
    const OpenWorld::OriginShift rebased_shift = rebased.set_origin(ChunkKey{1000, -1000});
    T2D_CHECK_EQ(rebased_shift.x, 1000 * WorldChunk::kSize);
    const std::optional<ChunkCoord> rebased_relative = rebased.relative_of(absolute_right);
    T2D_REQUIRE(rebased_relative.has_value());
    const WorldChunk& regenerated = rebased.ensure_chunk(*rebased_relative);
    T2D_CHECK_EQ(regenerated.checksum(), checksum);
    T2D_CHECK(regenerated.serialize() == bytes);

    // Different absolute chunks really are different content.
    const WorldChunk* neighbour = first.find_chunk(ChunkCoord{5, 3});
    T2D_REQUIRE(neighbour != nullptr);
    T2D_CHECK_NE(checksum, neighbour->checksum());

    // Unloading a clean generated chunk and loading it again gives byte for byte the same chunk.
    T2D_CHECK(first.unload_chunk(ChunkCoord{1, 0}));
    T2D_CHECK_FALSE(first.is_loaded(ChunkCoord{1, 0}));
    const WorldChunk& reloaded = first.ensure_chunk(ChunkCoord{1, 0});
    T2D_CHECK_EQ(reloaded.checksum(), checksum);
    T2D_CHECK(reloaded.serialize() == bytes);
}

T2D_TEST(a_generator_may_write_through_set_and_is_still_clean) {
    ByteStore store;
    int saves = 0;
    OpenWorld world(world_config(0x4321u));
    world.set_generator([](const ChunkKey&, WorldChunk& chunk) {
        chunk.fill(0, 3);        // a generator writing the obvious way marks the chunk dirty...
        chunk.set(0, 7, 7, 4);   // ...and so does this
    });
    world.set_store(nullptr, counting_saver(store, saves));

    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    for (const ChunkCoord& coord : world.chunks_in(view_of_chunk(0, 0), 1)) {
        T2D_CHECK_FALSE(world.find_chunk(coord)->dirty());
    }
    T2D_CHECK_EQ(world.at(0, 0), 3);
    T2D_CHECK_EQ(world.at(7, 7), 4);

    // Nothing generated may reach the saver: not on flush, not on clean eviction.
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(saves, 0);
    world.update(view_of_chunk(100, 100));
    T2D_CHECK_EQ(saves, 0);
    T2D_CHECK_EQ(world.stats().chunks_saved, 0u);
}

// --------------------------------------------------------------------------- chunks_in --

T2D_TEST(chunks_in_covers_the_relative_view_and_the_margin_ring) {
    const OpenWorld world(world_config());

    const std::vector<ChunkCoord> one = world.chunks_in(view_of_chunk(0, 0), 0);
    T2D_CHECK_EQ(one.size(), 1u);
    T2D_CHECK(one.size() == 1u && one[0] == (ChunkCoord{0, 0}));

    const std::vector<ChunkCoord> ring = world.chunks_in(view_of_chunk(0, 0), 1);
    T2D_CHECK_EQ(ring.size(), 9u);
    T2D_CHECK(ascending_relative_by_y_then_x(ring));
    T2D_CHECK(ring.front() == (ChunkCoord{-1, -1}));
    T2D_CHECK(ring.back() == (ChunkCoord{1, 1}));

    // A view crossing the origin touches four chunks, negative coordinates included.
    const std::vector<ChunkCoord> four = world.chunks_in(box_at(Vec2{-16.0f, -16.0f}, Vec2{32.0f, 32.0f}), 0);
    T2D_CHECK_EQ(four.size(), 4u);
    T2D_CHECK(ascending_relative_by_y_then_x(four));
    T2D_CHECK(four.size() == 4u && four[0] == (ChunkCoord{-1, -1}));
    T2D_CHECK(four.size() == 4u && four[1] == (ChunkCoord{0, -1}));
    T2D_CHECK(four.size() == 4u && four[2] == (ChunkCoord{-1, 0}));
    T2D_CHECK(four.size() == 4u && four[3] == (ChunkCoord{0, 0}));

    const std::vector<ChunkCoord> ring_of_four =
        world.chunks_in(box_at(Vec2{-16.0f, -16.0f}, Vec2{32.0f, 32.0f}), 1);
    T2D_CHECK_EQ(ring_of_four.size(), 16u);
    T2D_CHECK(ring_of_four.front() == (ChunkCoord{-2, -2}));
    T2D_CHECK(ring_of_four.back() == (ChunkCoord{1, 1}));

    // A 1024 x 1024 view spans two chunks per axis; a 5120 x 5120 view spans ten.
    T2D_CHECK_EQ(world.chunks_in(box_at(Vec2{}, Vec2{1024.0f, 1024.0f}), 0).size(), 4u);
    T2D_CHECK_EQ(world.chunks_in(box_at(Vec2{}, Vec2{5120.0f, 5120.0f}), 0).size(), 100u);
    T2D_CHECK_EQ(world.chunks_in(box_at(Vec2{-1024.0f, -1024.0f}, Vec2{1024.0f, 1024.0f}), 0).size(), 4u);

    // A zero sized box still touches the cell it sits in, exactly like TileMap.
    T2D_CHECK_EQ(world.chunks_in(box_at(Vec2{16.0f, 16.0f}, Vec2{}), 0).size(), 1u);
    // Negative margins mean no margin, not a negative ring.
    T2D_CHECK_EQ(world.chunks_in(view_of_chunk(0, 0), -3).size(), 1u);

    // Invalid views: non-finite or a negative side.
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{0.0f, 0.0f}, Vec2{nan, 16.0f}}, 0).empty());
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{0.0f, 0.0f}, Vec2{16.0f, nan}}, 0).empty());
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{0.0f, 0.0f}, Vec2{inf, 16.0f}}, 0).empty());
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{16.0f, 0.0f}, Vec2{0.0f, 16.0f}}, 0).empty());
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{0.0f, 16.0f}, Vec2{16.0f, 0.0f}}, 0).empty());
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{16.0f, 16.0f}, Vec2{-16.0f, 16.0f}}, 0).empty());

    // A view outside the practical relative range is refused, and so is one that would touch more
    // chunks than the allocation guard allows, so relative f32 stays meaningful and bounded.
    T2D_CHECK(world.chunks_in(box_at(Vec2{1.0e11f, 0.0f}, Vec2{16.0f, 16.0f}), 0).empty());
    T2D_CHECK(world.chunks_in(box_at(Vec2{-1.0e11f, 0.0f}, Vec2{16.0f, 16.0f}), 0).empty());
    const f32 whole_range = static_cast<f32>(static_cast<f64>(OpenWorld::kMaxRelativeCell) * 16.0);
    T2D_CHECK(world.chunks_in(box_at(Vec2{}, Vec2{whole_range, whole_range}), 0).empty());
    T2D_CHECK_EQ(OpenWorld::kMaxRelativeCell, 1 << 30);
    T2D_CHECK_EQ(OpenWorld::kMaxRelativeChunk, 1 << 25);
    T2D_CHECK_EQ(OpenWorld::kMaxAbsoluteChunk, i64{1} << 35);
}

// --------------------------------------------------------------------------- streaming --

T2D_TEST(streaming_loads_the_view_margin_but_not_the_world) {
    OpenWorld world(world_config(11u));
    int generated = 0;
    world.set_generator([&generated](const ChunkKey&, WorldChunk& chunk) {
        ++generated;
        fill_chunk(chunk, 1);
    });

    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(generated, 9);
    T2D_CHECK_EQ(world.stats().chunks_generated, 9u);
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    for (i32 y = -1; y <= 1; ++y) {
        for (i32 x = -1; x <= 1; ++x) T2D_CHECK(world.is_loaded(ChunkCoord{x, y}));
    }
    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{2, 0}));
    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{0, 2}));
    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{-2, 0}));
    T2D_CHECK_EQ(world.at(0, 0), 1);
    T2D_CHECK_EQ(world.at(512 + 16, 0), kEmptyTile);   // chunk (1, 0) is not resident

    // Updating the same view again loads nothing new.
    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(generated, 9);
    T2D_CHECK_EQ(world.stats().chunks_generated, 9u);

    world.reset_stats();
    T2D_CHECK_EQ(world.stats().chunks_generated, 0u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 0u);
}

T2D_TEST(unload_margin_hysteresis_stops_load_unload_thrash) {
    OpenWorld world(world_config(12u));
    world.set_generator(constant_generator(1));

    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 0u);

    // Moving one chunk right adds the new column but keeps the old ring: the unload margin is two,
    // the load margin is one. Without hysteresis the old column would be dropped here.
    T2D_CHECK_EQ(world.update(view_of_chunk(1, 0)), 12u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 0u);
    T2D_CHECK(world.is_loaded(ChunkCoord{-1, 0}));
    T2D_CHECK(world.is_loaded(ChunkCoord{2, 0}));

    // Sitting on the boundary and jiggling must not load or unload anything.
    for (i32 step = 0; step < 5; ++step) {
        T2D_CHECK_EQ(world.update(view_of_chunk(step % 2, 0)), 12u);
    }
    T2D_CHECK_EQ(world.stats().chunks_evicted, 0u);
    T2D_CHECK_EQ(world.stats().chunks_generated, 12u);
}

T2D_TEST(clean_chunks_reload_identically_without_a_store) {
    constexpr u64 seed = 0xABCDEFu;
    OpenWorld world(world_config(seed));
    world.set_generator(pattern_generator(seed, 4u));

    const WorldChunk& first = world.ensure_chunk(ChunkCoord{2, 3});
    const u64 checksum = first.checksum();
    const usize non_empty = first.count_non_empty();
    const std::vector<u8> bytes = first.serialize();
    T2D_CHECK_FALSE(first.dirty());
    T2D_CHECK(world.unload_chunk(ChunkCoord{2, 3}));
    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{2, 3}));

    const WorldChunk& second = world.ensure_chunk(ChunkCoord{2, 3});
    T2D_CHECK_EQ(second.checksum(), checksum);
    T2D_CHECK_EQ(second.count_non_empty(), non_empty);
    T2D_CHECK(second.serialize() == bytes);   // byte for byte the same chunk
    T2D_CHECK_EQ(world.stats().chunks_generated, 2u);
    T2D_CHECK_EQ(world.stats().chunks_restored, 0u);
}

// --------------------------------------------------------------------------- edits + store --

T2D_TEST(dirty_edits_are_saved_on_eviction_and_restored_exactly) {
    ByteStore store;
    OpenWorld world(world_config(21u));
    world.set_generator(constant_generator(1));
    world.set_store(store_loader(store), store_saver(store));

    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(world.stats().chunks_generated, 9u);
    T2D_CHECK(world.set(5, 5, 9));   // relative cells, inside the absolute chunk {0, 0} at origin 0
    T2D_CHECK(world.set(0, 0, 8));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);
    T2D_CHECK(store.empty());

    // Walking far away evicts the ring; the edited chunk has to be saved first, under its absolute key.
    T2D_CHECK_EQ(world.update(view_of_chunk(100, 0)), 9u);
    T2D_CHECK_EQ(store.size(), 1u);
    T2D_CHECK_EQ(world.stats().chunks_saved, 1u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 9u);
    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{0, 0}));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);

    // The payload is a real chunk blob holding both edits and the generated background.
    const auto found = store.find(ChunkKey{0, 0});
    T2D_REQUIRE(found != store.end());
    const std::optional<WorldChunk> saved = WorldChunk::deserialize(found->second, 1);
    T2D_REQUIRE(saved.has_value());
    T2D_CHECK_EQ(saved->at(0, 5, 5), 9);
    T2D_CHECK_EQ(saved->at(0, 0, 0), 8);
    T2D_CHECK_EQ(saved->at(0, 1, 1), 1);

    // Coming back restores the chunk from the store instead of generating over the edits.
    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(world.stats().chunks_restored, 1u);
    T2D_CHECK(world.is_loaded(ChunkCoord{0, 0}));
    T2D_CHECK_EQ(world.at(5, 5), 9);
    T2D_CHECK_EQ(world.at(0, 0), 8);
    T2D_CHECK_EQ(world.at(1, 1), 1);
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
}

T2D_TEST(a_failing_save_keeps_the_dirty_chunk_resident) {
    int save_attempts = 0;
    OpenWorld world(world_config(22u));
    world.set_generator(constant_generator(1));
    world.set_store(nullptr, failing_saver(save_attempts));

    world.update(view_of_chunk(0, 0));
    T2D_CHECK(world.set(2, 2, 7));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);

    world.update(view_of_chunk(100, 0));
    T2D_CHECK_EQ(save_attempts, 1);
    T2D_CHECK_EQ(world.stats().save_failures, 1u);
    T2D_CHECK_EQ(world.stats().chunks_saved, 0u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 8u);   // the eight clean chunks still left
    T2D_CHECK(world.is_loaded(ChunkCoord{0, 0}));
    T2D_CHECK(world.find_chunk(ChunkCoord{0, 0})->dirty());
    T2D_CHECK_EQ(world.at(2, 2), 7);
    T2D_CHECK_EQ(world.resident_chunk_count(), 10u);  // nine new ones, plus the kept edit

    // Returning does not lose the edit either: the chunk never left.
    world.update(view_of_chunk(0, 0));
    T2D_CHECK_EQ(world.at(2, 2), 7);
    T2D_CHECK(world.find_chunk(ChunkCoord{0, 0})->dirty());
}

T2D_TEST(without_a_saver_edits_never_leave_and_flush_says_so) {
    OpenWorld world(world_config(23u));
    world.set_generator(constant_generator(1));

    world.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK(world.unload_chunk(ChunkCoord{0, 0}));   // clean: nothing to save, it just goes
    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{0, 0}));

    T2D_CHECK(world.set(3, 3, 5));                     // makes the chunk resident again and dirties it
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);
    T2D_CHECK_FALSE(world.unload_chunk(ChunkCoord{0, 0}));
    T2D_CHECK(world.is_loaded(ChunkCoord{0, 0}));
    T2D_CHECK_EQ(world.at(3, 3), 5);
    T2D_CHECK_FALSE(world.flush());
    T2D_CHECK_FALSE(world.flush(ChunkCoord{0, 0}));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);

    T2D_CHECK(world.unload_chunk(ChunkCoord{9, 9}));   // never resident: nothing to unload
    T2D_CHECK(world.flush(ChunkCoord{9, 9}));          // and nothing to flush
}

T2D_TEST(flush_saves_each_dirty_chunk_once_and_clears_dirty) {
    ByteStore store;
    int saves = 0;
    OpenWorld world(world_config(24u));
    world.set_generator(constant_generator(2));
    world.set_store(store_loader(store), counting_saver(store, saves));

    world.ensure_chunk(ChunkCoord{0, 0});
    world.ensure_chunk(ChunkCoord{1, 0});
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    T2D_CHECK(world.flush());          // generated chunks are clean: flush has nothing to write
    T2D_CHECK_EQ(saves, 0);
    T2D_CHECK(store.empty());

    T2D_CHECK(world.set(1, 1, 3));
    T2D_CHECK(world.set(40, 1, 4));    // relative cell (40, 1) is chunk (1, 0)
    T2D_CHECK_EQ(world.dirty_chunk_count(), 2u);
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(saves, 2);
    T2D_CHECK_EQ(store.size(), 2u);
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    T2D_CHECK_EQ(world.stats().chunks_saved, 2u);
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(saves, 2);            // a clean chunk is not written twice

    T2D_CHECK(world.set(2, 2, 5));
    T2D_CHECK(world.set(41, 2, 6));
    T2D_CHECK(world.flush(ChunkCoord{0, 0}));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);
    T2D_CHECK(world.flush(ChunkCoord{1, 0}));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    T2D_CHECK_EQ(saves, 4);
}

T2D_TEST(generated_chunks_are_never_saved_but_an_edit_is_saved_once) {
    ByteStore store;
    int saves = 0;
    OpenWorld world(world_config(25u));
    world.set_generator(constant_generator(1));
    world.set_store(nullptr, counting_saver(store, saves));

    T2D_CHECK_EQ(world.update(box_at(Vec2{}, Vec2{1024.0f, 1024.0f})), 16u);
    T2D_CHECK_EQ(world.stats().chunks_generated, 16u);
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(saves, 0);            // nothing generated was ever marked dirty

    world.update(view_of_chunk(100, 100));
    T2D_CHECK_EQ(saves, 0);            // clean eviction writes nothing either
    T2D_CHECK_EQ(world.stats().chunks_evicted, 16u);

    world.update(box_at(Vec2{}, Vec2{1024.0f, 1024.0f}));
    T2D_CHECK(world.set(7, 7, 6));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(saves, 1);            // exactly one save for one edited chunk
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    T2D_CHECK_EQ(world.stats().chunks_saved, 1u);
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(saves, 1);
}

T2D_TEST(malformed_or_mismatched_store_payloads_fall_back_to_generation) {
    int generation_seed = 0x77;
    OpenWorld world(world_config(static_cast<u64>(generation_seed)));
    world.set_generator(constant_generator(2));
    world.set_store(
        [](const ChunkKey&) -> std::optional<std::vector<u8>> {
            return std::vector<u8>{static_cast<u8>('T'), static_cast<u8>('2'), static_cast<u8>('D'),
                                   static_cast<u8>('C'), 1u, 0u};   // a zero layer count
        },
        nullptr);

    world.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(world.stats().restore_failures, 1u);
    T2D_CHECK_EQ(world.stats().chunks_generated, 1u);
    T2D_CHECK_EQ(world.stats().chunks_restored, 0u);
    T2D_CHECK_EQ(world.at(0, 0), 2);

    // The fallback is generation, not an empty chunk: it matches a fresh world with the same seed.
    OpenWorld reference(world_config(static_cast<u64>(generation_seed)));
    reference.set_generator(constant_generator(2));
    reference.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(world.find_chunk(ChunkCoord{0, 0})->checksum(),
                 reference.find_chunk(ChunkCoord{0, 0})->checksum());

    // An empty payload is the documented "the store has nothing": generate, no failure counted.
    world.set_store(
        [](const ChunkKey&) -> std::optional<std::vector<u8>> {
            return std::optional<std::vector<u8>>{std::vector<u8>{}};
        },
        nullptr);
    world.ensure_chunk(ChunkCoord{1, 0});
    T2D_CHECK_EQ(world.stats().restore_failures, 1u);
    T2D_CHECK_EQ(world.at(32, 0), 2);

    // A payload with the wrong layer count is malformed for this world, not silently accepted.
    OpenWorldConfig two_layers = world_config(77u);
    two_layers.layers = 2;
    OpenWorld layered(two_layers);
    layered.set_generator(constant_generator(2));
    layered.set_store(
        [](const ChunkKey&) -> std::optional<std::vector<u8>> {
            return valid_empty_blob(1);   // one layer, but the world has two
        },
        nullptr);
    layered.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(layered.stats().restore_failures, 1u);
    T2D_CHECK_EQ(layered.at(0, 0), 2);
    T2D_CHECK_EQ(layered.layers(), 2);
}

T2D_TEST(the_loader_wins_over_the_generator) {
    WorldChunk stored(2);
    stored.set(0, 0, 0, 7);
    stored.set(1, 1, 1, 8);
    const std::vector<u8> payload = stored.serialize();

    OpenWorldConfig config = world_config(26u);
    config.layers = 2;
    OpenWorld world(config);
    world.set_generator(constant_generator(2));   // would fill every cell of both layers with 2
    world.set_store(
        [&payload](const ChunkKey&) -> std::optional<std::vector<u8>> { return payload; }, nullptr);

    const WorldChunk& chunk = world.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(world.stats().chunks_restored, 1u);
    T2D_CHECK_EQ(world.stats().chunks_generated, 0u);
    T2D_CHECK_FALSE(chunk.dirty());
    T2D_CHECK_EQ(world.at(0, 0), 7);
    T2D_CHECK_EQ(world.at(1, 1, 1), 8);
    T2D_CHECK_EQ(world.topmost(1, 1), 8);
    T2D_CHECK_EQ(world.at(5, 5), kEmptyTile);      // the generator did not run
    T2D_CHECK_EQ(world.at(5, 5, 1), kEmptyTile);
}

// --------------------------------------------------------------------------- resident edits --

T2D_TEST(set_on_a_missing_chunk_generates_it_and_marks_it_dirty) {
    OpenWorld world(world_config(27u));
    int generated = 0;
    world.set_generator([&generated](const ChunkKey&, WorldChunk& chunk) {
        ++generated;
        fill_chunk(chunk, 1);
    });

    T2D_CHECK_FALSE(world.is_loaded(ChunkCoord{5, 5}));
    T2D_CHECK(world.set(5 * WorldChunk::kSize + 3, 5 * WorldChunk::kSize + 4, 6));
    T2D_CHECK(world.is_loaded(ChunkCoord{5, 5}));
    T2D_CHECK_EQ(generated, 1);
    T2D_CHECK_EQ(world.at(5 * WorldChunk::kSize + 3, 5 * WorldChunk::kSize + 4), 6);
    T2D_CHECK_EQ(world.at(5 * WorldChunk::kSize, 5 * WorldChunk::kSize), 1);   // generated background
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);

    T2D_CHECK(world.set(-1, -1, 4));   // negative relative cells land in chunk (-1, -1)
    T2D_CHECK_EQ(world.at(-1, -1), 4);
    T2D_CHECK_EQ(world.dirty_chunk_count(), 2u);
    T2D_CHECK(world.is_loaded(ChunkCoord{-1, -1}));

    T2D_CHECK_FALSE(world.set(3, 0, 0, 1));   // this world has one layer
    T2D_CHECK_FALSE(world.set(-1, 0, 0, 1));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 2u);
}

// --------------------------------------------------------------------------- queries --

T2D_TEST(queries_read_resident_chunks_and_honour_layers_and_masks) {
    OpenWorldConfig config = world_config(28u);
    config.layers = 2;
    OpenWorld world(config);
    world.set_generator([](const ChunkKey& key, WorldChunk& chunk) {
        for (i32 layer = 0; layer < chunk.layers(); ++layer) {
            for (TileId& cell : chunk.tiles(layer)) cell = layer == 0 ? TileId{6} : kEmptyTile;
        }
        if (key == (ChunkKey{0, 0})) {
            chunk.tiles(1)[static_cast<usize>(3 * WorldChunk::kSize + 2)] = 1;   // solid
            chunk.tiles(1)[static_cast<usize>(4 * WorldChunk::kSize + 4)] = 4;   // one-way
            chunk.tiles(1)[static_cast<usize>(6 * WorldChunk::kSize + 6)] = 5;   // hazard
        }
    });

    // Before anything is resident every query reads empty space, and none of them loads.
    T2D_CHECK_EQ(world.at(2, 3), kEmptyTile);
    T2D_CHECK_EQ(world.topmost(2, 3), kEmptyTile);
    T2D_CHECK_FALSE(world.is_solid(2, 3, test_tileset()));
    T2D_CHECK_FALSE(world.overlaps_solid(box_at(Vec2{}, Vec2{16.0f, 16.0f}), test_tileset()));
    T2D_CHECK_EQ(world.resident_chunk_count(), 0u);

    world.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(world.at(2, 3), 6);          // base layer (decor)
    T2D_CHECK_EQ(world.at(1, 2, 3), 1);       // structure layer
    T2D_CHECK_EQ(world.topmost(2, 3), 1);     // the higher layer is what is "on" the cell
    T2D_CHECK_EQ(world.topmost(2, 3, TileMap::layer_mask(0)), 6);
    T2D_CHECK_EQ(world.topmost(2, 3, TileMap::layer_mask(1)), 1);
    T2D_CHECK_EQ(world.topmost(2, 3, TileMap::kNoLayers), kEmptyTile);
    T2D_CHECK_EQ(world.topmost(7, 7), 6);
    T2D_CHECK_EQ(world.at(9, 2, 3), kEmptyTile);   // no such layer
    T2D_CHECK_FALSE(world.set(9, 2, 3, 1));

    T2D_CHECK(world.is_solid(2, 3, test_tileset()));
    T2D_CHECK_FALSE(world.is_solid(3, 2, test_tileset()));   // (x, y) order, not (y, x)
    T2D_CHECK(world.is_solid(2, 3, test_tileset(), TileMap::layer_mask(1)));
    T2D_CHECK_FALSE(world.is_solid(2, 3, test_tileset(), TileMap::layer_mask(0)));
    T2D_CHECK(world.is_one_way(4, 4, test_tileset()));
    T2D_CHECK_FALSE(world.is_one_way(4, 4, test_tileset(), TileMap::layer_mask(0)));
    T2D_CHECK(world.is_hazard(6, 6, test_tileset()));
    T2D_CHECK_FALSE(world.is_hazard(6, 6, test_tileset(), TileMap::layer_mask(0)));
    T2D_CHECK_FALSE(world.is_hazard(5, 5, test_tileset()));

    // A missing chunk reads empty even when its neighbour is full.
    T2D_CHECK_EQ(world.at(32, 0), kEmptyTile);
    T2D_CHECK_EQ(world.at(0, -1), kEmptyTile);
    T2D_CHECK_EQ(world.topmost(32, 0), kEmptyTile);
    T2D_CHECK_FALSE(world.is_solid(32, 0, test_tileset()));
    T2D_CHECK_EQ(world.resident_chunk_count(), 1u);

    // Edits go through the layer-aware setter as well.
    T2D_CHECK(world.set(1, 5, 5, 2));
    T2D_CHECK_EQ(world.at(1, 5, 5), 2);
    T2D_CHECK_EQ(world.at(5, 5), 6);
    T2D_CHECK_EQ(world.topmost(5, 5), 2);
}

T2D_TEST(overlaps_queries_cross_chunk_boundaries) {
    OpenWorld world(world_config(29u));
    const Tileset& tileset = test_tileset();

    // Only the first cell of chunk (1, 0) is solid: a box ending before the boundary must miss it,
    // one that steps over 512 px must find it.
    T2D_CHECK(world.set(32, 5, 2));
    T2D_CHECK(world.overlaps_solid(box_at(Vec2{500.0f, 80.0f}, Vec2{20.0f, 16.0f}), tileset));
    T2D_CHECK_FALSE(world.overlaps_solid(box_at(Vec2{496.0f, 80.0f}, Vec2{16.0f, 16.0f}), tileset));
    T2D_CHECK(world.set(31, 5, 1));
    T2D_CHECK(world.overlaps_solid(box_at(Vec2{496.0f, 80.0f}, Vec2{16.0f, 16.0f}), tileset));

    // The same across the origin: cell -1 is solid, cell -2 is not.
    T2D_CHECK(world.set(-1, 0, 1));
    T2D_CHECK(world.overlaps_solid(box_at(Vec2{-16.0f, 0.0f}, Vec2{16.0f, 16.0f}), tileset));
    T2D_CHECK_FALSE(world.overlaps_solid(box_at(Vec2{-32.0f, 0.0f}, Vec2{16.0f, 16.0f}), tileset));

    // Hazard queries use the same cell walk.
    T2D_CHECK(world.set(31, 6, 5));
    T2D_CHECK(world.overlaps_hazard(box_at(Vec2{496.0f, 96.0f}, Vec2{32.0f, 16.0f}), tileset));
    T2D_CHECK_FALSE(world.overlaps_hazard(box_at(Vec2{0.0f, 0.0f}, Vec2{16.0f, 16.0f}), tileset));

    // A huge box walks resident chunks instead of an impossible rectangle; it still finds the tile.
    const Aabb2 huge{Vec2{-1.0e30f, -1.0e30f}, Vec2{1.0e30f, 1.0e30f}};
    T2D_CHECK(world.overlaps_solid(huge, tileset));
    OpenWorld empty_world(world_config(30u));
    T2D_CHECK_FALSE(empty_world.overlaps_solid(huge, test_tileset()));
    T2D_CHECK_FALSE(empty_world.overlaps_hazard(huge, test_tileset()));
}

// --------------------------------------------------------------------------- camera + budget --

T2D_TEST(update_from_a_camera_loads_its_visible_world) {
    constexpr u64 seed = 0x31u;
    OpenWorld via_camera(world_config(seed));
    OpenWorld via_view(world_config(seed));
    via_camera.set_generator(constant_generator(1));
    via_view.set_generator(constant_generator(1));

    const Camera2D camera(Vec2{512.0f, 512.0f}, 1.0f, Vec2{256.0f, 256.0f});
    const usize camera_count = via_camera.update(camera);
    const usize view_count = via_view.update(camera.visible_world());
    T2D_CHECK_EQ(camera_count, 9u);
    T2D_CHECK_EQ(view_count, 9u);
    T2D_CHECK(via_camera.resident_chunks() == via_view.resident_chunks());
    T2D_CHECK(via_camera.is_loaded(ChunkCoord{0, 0}));

    // A camera looking at chunk (2, 2) from a 512 px viewport spans chunks 1..2 per axis; the load
    // margin adds a ring (0..3 on both axes), and the older chunks still sit inside the unload margin
    // (2), so hysteresis keeps all 9 of them: 9 + 12 new = 21 resident.
    const Camera2D far_camera(Vec2{512.0f, 512.0f}, 1.0f, Vec2{1024.0f, 1024.0f});
    T2D_CHECK_EQ(via_camera.update(far_camera), 21u);
    T2D_CHECK(via_camera.is_loaded(ChunkCoord{1, 1}));
    T2D_CHECK(via_camera.is_loaded(ChunkCoord{3, 3}));
    T2D_CHECK_FALSE(via_camera.is_loaded(ChunkCoord{4, 4}));
    T2D_CHECK(via_camera.is_loaded(ChunkCoord{-1, -1}));   // the first ring is still inside the unload margin
}

T2D_TEST(soft_budget_refuses_loads_and_gives_up_far_clean_ring_chunks) {
    // A cap below the load ring: update() loads what fits, nearest the centre first, and counts the
    // refusals instead of silently walking past its budget.
    OpenWorldConfig tiny_config = world_config(32u);
    tiny_config.max_loaded_chunks = 2;
    OpenWorld tiny(tiny_config);
    tiny.set_generator(constant_generator(1));
    T2D_CHECK_EQ(tiny.update(view_of_chunk(0, 0)), 2u);
    T2D_CHECK_EQ(tiny.stats().chunks_generated, 2u);
    T2D_CHECK_EQ(tiny.stats().budget_refusals, 7u);
    T2D_CHECK(tiny.is_loaded(ChunkCoord{0, 0}));
    T2D_CHECK(tiny.is_loaded(ChunkCoord{0, -1}));   // distance 1, tie broken by y then x
    T2D_CHECK_FALSE(tiny.is_loaded(ChunkCoord{-1, 0}));

    OpenWorldConfig unlimited_config = world_config(32u);
    unlimited_config.max_loaded_chunks = 0;
    OpenWorld unlimited(unlimited_config);
    unlimited.set_generator(constant_generator(1));
    T2D_CHECK_EQ(unlimited.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(unlimited.stats().budget_refusals, 0u);

    // A cap equal to one ring: moving one chunk right must first give up the clean chunks the load
    // no longer needs, rather than refusing to load.
    OpenWorldConfig slotted_config = world_config(33u);
    slotted_config.max_loaded_chunks = 9;
    OpenWorld slotted(slotted_config);
    slotted.set_generator(constant_generator(1));
    T2D_CHECK_EQ(slotted.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK_EQ(slotted.update(view_of_chunk(1, 0)), 9u);
    T2D_CHECK_EQ(slotted.stats().chunks_evicted, 3u);
    T2D_CHECK_EQ(slotted.stats().budget_refusals, 0u);
    T2D_CHECK_FALSE(slotted.is_loaded(ChunkCoord{-1, 0}));
    T2D_CHECK(slotted.is_loaded(ChunkCoord{0, 0}));
    T2D_CHECK(slotted.is_loaded(ChunkCoord{2, 0}));
    T2D_CHECK(ascending_by_y_then_x(slotted.resident_chunks()));
}

T2D_TEST(invalid_views_do_not_thrash_the_resident_set) {
    OpenWorld world(world_config(34u));
    world.set_generator(constant_generator(1));
    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    world.reset_stats();

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    T2D_CHECK_EQ(world.update(Aabb2{Vec2{0.0f, 0.0f}, Vec2{nan, 16.0f}}), 9u);
    T2D_CHECK_EQ(world.update(Aabb2{Vec2{0.0f, 0.0f}, Vec2{16.0f, nan}}), 9u);
    T2D_CHECK_EQ(world.update(Aabb2{Vec2{0.0f, 0.0f}, Vec2{inf, 16.0f}}), 9u);
    T2D_CHECK_EQ(world.update(Aabb2{Vec2{16.0f, 0.0f}, Vec2{0.0f, 16.0f}}), 9u);
    T2D_CHECK_EQ(world.update(Aabb2{Vec2{0.0f, 16.0f}, Vec2{16.0f, 0.0f}}), 9u);
    // A finite view outside the practical relative range, or one touching more than the chunk cap, is
    // a no-op as well.
    T2D_CHECK_EQ(world.update(box_at(Vec2{1.0e11f, 0.0f}, Vec2{16.0f, 16.0f})), 9u);
    T2D_CHECK_EQ(world.update(Aabb2{Vec2{-1.0e30f, -1.0e30f}, Vec2{1.0e30f, 1.0e30f}}), 9u);

    T2D_CHECK_EQ(world.resident_chunk_count(), 9u);
    T2D_CHECK_EQ(world.stats().chunks_generated, 0u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 0u);
    T2D_CHECK_EQ(world.stats().chunks_saved, 0u);
    T2D_CHECK_EQ(world.stats().save_failures, 0u);
    T2D_CHECK_EQ(world.stats().budget_refusals, 0u);
    T2D_CHECK(world.chunks_in(Aabb2{Vec2{0.0f, 0.0f}, Vec2{nan, 16.0f}}, 0).empty());
    T2D_CHECK(world.chunks_in(box_at(Vec2{1.0e11f, 0.0f}, Vec2{16.0f, 16.0f}), 0).empty());
}

// --------------------------------------------------------------------------- config + bounds --

T2D_TEST(config_is_clamped_and_reported_back) {
    OpenWorldConfig zero;
    zero.tile_size = 0.0f;
    zero.layers = 0;
    zero.load_margin_chunks = -5;
    zero.unload_margin_chunks = -1;
    zero.max_loaded_chunks = 0;
    const OpenWorld zeroed(zero);
    T2D_CHECK_EQ(zeroed.tile_size(), 16.0f);
    T2D_CHECK_EQ(zeroed.layers(), 1);
    T2D_CHECK_EQ(zeroed.config().load_margin_chunks, 0);
    T2D_CHECK_EQ(zeroed.config().unload_margin_chunks, 0);
    T2D_CHECK_EQ(zeroed.config().max_loaded_chunks, 0u);
    T2D_CHECK_EQ(zeroed.seed(), 0u);
    T2D_CHECK(zeroed.origin() == (ChunkKey{0, 0}));

    OpenWorldConfig nan_config = zero;
    nan_config.tile_size = std::numeric_limits<f32>::quiet_NaN();
    nan_config.layers = 99;
    nan_config.load_margin_chunks = 3;
    nan_config.unload_margin_chunks = 1;
    const OpenWorld clamped(nan_config);
    T2D_CHECK_EQ(clamped.tile_size(), 16.0f);
    T2D_CHECK_EQ(clamped.layers(), TileMap::kMaxLayers);
    T2D_CHECK_EQ(clamped.config().load_margin_chunks, 3);
    T2D_CHECK_EQ(clamped.config().unload_margin_chunks, 3);   // effective = max(load, unload)

    OpenWorldConfig negative = zero;
    negative.tile_size = -8.0f;
    negative.layers = -2;
    negative.load_margin_chunks = 2;
    negative.unload_margin_chunks = -3;
    const OpenWorld bottomed(negative);
    T2D_CHECK_EQ(bottomed.tile_size(), 16.0f);
    T2D_CHECK_EQ(bottomed.layers(), 1);
    T2D_CHECK_EQ(bottomed.config().unload_margin_chunks, 2);

    const OpenWorld defaults;
    T2D_CHECK_EQ(defaults.tile_size(), 16.0f);
    T2D_CHECK_EQ(defaults.layers(), 1);
    T2D_CHECK_EQ(defaults.config().load_margin_chunks, 1);
    T2D_CHECK_EQ(defaults.config().unload_margin_chunks, 2);
    T2D_CHECK_EQ(defaults.config().max_loaded_chunks, 2048u);
    T2D_CHECK_EQ(defaults.seed(), 0u);
}

T2D_TEST(relative_bounds_are_negative_safe) {
    const OpenWorld world(world_config());
    T2D_CHECK_EQ(world.chunk_bounds(ChunkCoord{0, 0}), (Aabb2{Vec2{0.0f, 0.0f}, Vec2{512.0f, 512.0f}}));
    T2D_CHECK_EQ(world.chunk_bounds(ChunkCoord{1, 0}), (Aabb2{Vec2{512.0f, 0.0f}, Vec2{1024.0f, 512.0f}}));
    T2D_CHECK_EQ(world.chunk_bounds(ChunkCoord{-1, -1}),
                 (Aabb2{Vec2{-512.0f, -512.0f}, Vec2{0.0f, 0.0f}}));
    T2D_CHECK_EQ(world.cell_bounds(0, 0), (Aabb2{Vec2{0.0f, 0.0f}, Vec2{16.0f, 16.0f}}));
    T2D_CHECK_EQ(world.cell_bounds(-1, -1), (Aabb2{Vec2{-16.0f, -16.0f}, Vec2{0.0f, 0.0f}}));
    T2D_CHECK_EQ(world.cell_bounds(31, 32), (Aabb2{Vec2{496.0f, 512.0f}, Vec2{512.0f, 528.0f}}));

    OpenWorldConfig odd = world_config();
    odd.tile_size = 10.5f;
    const OpenWorld odd_world(odd);
    T2D_CHECK_EQ(odd_world.chunk_bounds(ChunkCoord{2, 3}),
                 (Aabb2{Vec2{672.0f, 1008.0f}, Vec2{1008.0f, 1344.0f}}));
    T2D_CHECK_EQ(odd_world.cell_bounds(-2, 5), (Aabb2{Vec2{-21.0f, 52.5f}, Vec2{-10.5f, 63.0f}}));
}

T2D_TEST(resident_chunks_are_absolute_keys_sorted_and_counted) {
    OpenWorld world(world_config(35u));
    world.set_generator(constant_generator(1));

    T2D_CHECK_EQ(world.resident_chunk_count(), 0u);
    world.ensure_chunk(ChunkCoord{0, 0});
    world.ensure_chunk(ChunkCoord{1, 0});
    world.ensure_chunk(ChunkCoord{0, -1});
    world.ensure_chunk(ChunkCoord{-2, 3});
    world.ensure_chunk(ChunkCoord{0, 0});   // already resident: no duplicate
    T2D_CHECK_EQ(world.resident_chunk_count(), 4u);

    const std::vector<ChunkKey> sorted = world.resident_chunks();
    T2D_REQUIRE(sorted.size() == 4u);
    T2D_CHECK(ascending_by_y_then_x(sorted));
    T2D_CHECK(sorted[0] == (ChunkKey{0, -1}));
    T2D_CHECK(sorted[1] == (ChunkKey{0, 0}));
    T2D_CHECK(sorted[2] == (ChunkKey{1, 0}));
    T2D_CHECK(sorted[3] == (ChunkKey{-2, 3}));

    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    world.set(1, 1, 2);      // absolute chunk {0, 0}
    world.set(33, 1, 2);     // absolute chunk {1, 0}
    T2D_CHECK_EQ(world.dirty_chunk_count(), 2u);
    world.set(2, 2, 3);      // same chunk, still one dirty chunk
    T2D_CHECK_EQ(world.dirty_chunk_count(), 2u);
}

T2D_TEST(the_store_receives_a_valid_chunk_blob_under_its_absolute_key) {
    ByteStore store;
    OpenWorldConfig config = world_config(36u);
    config.layers = 2;
    OpenWorld world(config);
    world.set_generator(pattern_generator(36u, 6u));
    world.set_store(store_loader(store), store_saver(store));

    world.ensure_chunk(ChunkCoord{1, 2});
    T2D_CHECK(world.set(0, 32 + 3, 64 + 4, 7));   // relative cell (35, 68) lives in chunk (1, 2)
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);
    T2D_CHECK(world.flush());
    T2D_CHECK_EQ(world.dirty_chunk_count(), 0u);
    T2D_CHECK_EQ(store.size(), 1u);

    const auto found = store.find(ChunkKey{1, 2});   // the ABSOLUTE key, not the relative one
    T2D_REQUIRE(found != store.end());
    const std::vector<u8>& payload = found->second;
    T2D_REQUIRE(payload.size() >= 6u);
    T2D_CHECK_EQ(payload[0], static_cast<u8>('T'));
    T2D_CHECK_EQ(payload[1], static_cast<u8>('2'));
    T2D_CHECK_EQ(payload[2], static_cast<u8>('D'));
    T2D_CHECK_EQ(payload[3], static_cast<u8>('C'));
    T2D_CHECK_EQ(payload[4], 1);
    T2D_CHECK_EQ(payload[5], 2);   // two layers

    const std::optional<WorldChunk> saved = WorldChunk::deserialize(payload, 2);
    T2D_REQUIRE(saved.has_value());
    T2D_CHECK_EQ(saved->at(0, 3, 4), 7);   // local (3, 4) of chunk (1, 2)
    T2D_CHECK_EQ(saved->checksum(), world.find_chunk(ChunkCoord{1, 2})->checksum());

    // A second world reading the same store restores the edited chunk instead of generating it.
    OpenWorld reader(config);
    reader.set_generator(pattern_generator(36u, 6u));
    reader.set_store(store_loader(store), store_saver(store));
    const WorldChunk& restored = reader.ensure_chunk(ChunkCoord{1, 2});
    T2D_CHECK_EQ(reader.stats().chunks_restored, 1u);
    T2D_CHECK_EQ(reader.stats().chunks_generated, 0u);
    T2D_CHECK_FALSE(restored.dirty());
    T2D_CHECK_EQ(reader.at(32 + 3, 64 + 4), 7);
    T2D_CHECK_EQ(restored.checksum(), saved->checksum());
}

// --------------------------------------------------------------------------- origin --

T2D_TEST(the_default_origin_is_zero_and_conversions_round_trip) {
    OpenWorld world(world_config());
    T2D_CHECK(world.origin() == (ChunkKey{0, 0}));
    T2D_CHECK(world.absolute_of(ChunkCoord{0, 0}) == (ChunkKey{0, 0}));
    T2D_CHECK(world.absolute_of(ChunkCoord{2, -3}) == (ChunkKey{2, -3}));
    T2D_CHECK_EQ(world.absolute_cell_x(-5), -5);
    T2D_CHECK_EQ(world.absolute_cell_y(7), 7);
    const std::optional<ChunkCoord> origin_relative = world.relative_of(ChunkKey{0, 0});
    T2D_REQUIRE(origin_relative.has_value());
    T2D_CHECK(*origin_relative == (ChunkCoord{0, 0}));
    const std::optional<ChunkCoord> relative = world.relative_of(ChunkKey{2, -3});
    T2D_REQUIRE(relative.has_value());
    T2D_CHECK(*relative == (ChunkCoord{2, -3}));

    const OpenWorld::OriginShift shift = world.set_origin(ChunkKey{3, -4});
    T2D_CHECK_EQ(shift.x, 3 * WorldChunk::kSize);   // the shift is in absolute cells
    T2D_CHECK_EQ(shift.y, -4 * WorldChunk::kSize);
    T2D_CHECK(world.origin() == (ChunkKey{3, -4}));
    T2D_CHECK(world.absolute_of(ChunkCoord{1, 1}) == (ChunkKey{4, -3}));
    const std::optional<ChunkCoord> back = world.relative_of(ChunkKey{4, -3});
    T2D_REQUIRE(back.has_value());
    T2D_CHECK(*back == (ChunkCoord{1, 1}));
    T2D_CHECK_EQ(world.absolute_cell_x(0), 96);
    T2D_CHECK_EQ(world.absolute_cell_y(0), -128);
    const Vec2 delta = shift.position_delta(16.0f);
    T2D_CHECK_EQ(delta.x, -96.0f * 16.0f);
    T2D_CHECK_EQ(delta.y, 128.0f * 16.0f);

    // relative_of refuses to truncate instead of wrapping.
    const i64 max_offset = static_cast<i64>(std::numeric_limits<i32>::max());
    T2D_CHECK(world.relative_of(ChunkKey{world.origin().x + max_offset, world.origin().y}).has_value());
    T2D_CHECK_FALSE(
        world.relative_of(ChunkKey{world.origin().x + max_offset + 1, world.origin().y}).has_value());
    T2D_CHECK(world.relative_of(ChunkKey{world.origin().x - max_offset - 1, world.origin().y}).has_value());
    T2D_CHECK_FALSE(
        world.relative_of(ChunkKey{world.origin().x - max_offset - 2, world.origin().y}).has_value());
    T2D_CHECK_FALSE(
        world.relative_of(ChunkKey{world.origin().x, world.origin().y + max_offset + 1}).has_value());

    // set_origin clamps to the practical absolute range and reports the clamp by its shift.
    const OpenWorld::OriginShift clamped =
        world.set_origin(ChunkKey{OpenWorld::kMaxAbsoluteChunk + 100, -OpenWorld::kMaxAbsoluteChunk - 100});
    T2D_CHECK(world.origin() == (ChunkKey{OpenWorld::kMaxAbsoluteChunk, -OpenWorld::kMaxAbsoluteChunk}));
    T2D_CHECK_EQ(clamped.x, (OpenWorld::kMaxAbsoluteChunk - 3) * WorldChunk::kSize);
    T2D_CHECK_EQ(clamped.y, (-OpenWorld::kMaxAbsoluteChunk + 4) * WorldChunk::kSize);
}

T2D_TEST(rebasing_keeps_resident_chunks_content_and_dirty_flags) {
    OpenWorld world(world_config(0xA1u));
    world.set_generator(pattern_generator(0xA1u, 4u));
    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK(world.set(5, 5, 9));
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);

    const std::vector<ChunkKey> keys_before = world.resident_chunks();
    std::map<ChunkKey, u64, ChunkKeyLess> checksums;
    for (const ChunkKey& key : keys_before) {
        const WorldChunk* chunk = chunk_by_key(world, key);
        T2D_REQUIRE(chunk != nullptr);
        checksums[key] = chunk->checksum();
    }

    const OpenWorld::OriginShift shift = world.set_origin(ChunkKey{1000, -1000});
    T2D_CHECK_EQ(shift.x, 1000 * WorldChunk::kSize);
    T2D_CHECK_EQ(shift.y, -1000 * WorldChunk::kSize);
    T2D_CHECK(world.resident_chunks() == keys_before);   // re-addressed, not reloaded
    T2D_CHECK_EQ(world.resident_chunk_count(), 9u);
    T2D_CHECK_EQ(world.dirty_chunk_count(), 1u);
    T2D_CHECK_EQ(world.stats().chunks_evicted, 0u);
    T2D_CHECK_EQ(world.stats().chunks_saved, 0u);

    for (const ChunkKey& key : keys_before) {
        const WorldChunk* chunk = chunk_by_key(world, key);
        T2D_REQUIRE(chunk != nullptr);
        T2D_CHECK_EQ(chunk->checksum(), checksums.at(key));
    }
    // The edited absolute cell (5, 5) is a different relative cell after the rebase, but the same
    // absolute place: origin cells + relative cell is invariant.
    const i32 relative_x = 5 - 1000 * WorldChunk::kSize;
    const i32 relative_y = 5 - (-1000 * WorldChunk::kSize);
    T2D_CHECK_EQ(world.absolute_cell_x(relative_x), 5);
    T2D_CHECK_EQ(world.absolute_cell_y(relative_y), 5);
    T2D_CHECK_EQ(world.at(relative_x, relative_y), 9);
}

T2D_TEST(the_store_key_is_absolute_across_a_rebase) {
    ByteStore store;
    OpenWorld world(world_config(0xB1u));
    world.set_generator(constant_generator(1));
    world.set_store(store_loader(store), store_saver(store));

    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    T2D_CHECK(world.set(5, 5, 9));
    T2D_CHECK(world.flush());
    const auto found = store.find(ChunkKey{0, 0});
    T2D_REQUIRE(found != store.end());
    T2D_CHECK_EQ(store.size(), 1u);

    const OpenWorld::OriginShift shift = world.set_origin(ChunkKey{1000, -1000});
    T2D_CHECK_EQ(shift.x, 1000 * WorldChunk::kSize);
    const std::optional<ChunkCoord> relative = world.relative_of(ChunkKey{0, 0});
    T2D_REQUIRE(relative.has_value());
    T2D_CHECK(world.is_loaded(*relative));   // rebasing does not drop the resident chunk
    T2D_CHECK(world.unload_chunk(*relative));   // clean after flush: unload without a write
    T2D_CHECK_FALSE(world.is_loaded(*relative));

    const WorldChunk& restored = world.ensure_chunk(*relative);
    T2D_CHECK_EQ(world.stats().chunks_restored, 1u);
    T2D_CHECK_EQ(store.size(), 1u);   // still exactly the same absolute key
    T2D_CHECK_EQ(world.absolute_of(*relative), (ChunkKey{0, 0}));
    T2D_CHECK_EQ(restored.at(0, 5, 5), 9);
    const i32 relative_x = 5 - 1000 * WorldChunk::kSize;
    const i32 relative_y = 5 - (-1000 * WorldChunk::kSize);
    T2D_CHECK_EQ(world.at(relative_x, relative_y), 9);
}

T2D_TEST(recenter_puts_the_origin_on_the_camera_chunk) {
    OpenWorld world(world_config(0xC1u));
    world.set_generator(constant_generator(1));
    const Camera2D camera(Vec2{1280.0f, 720.0f}, 1.0f, Vec2{300.0f, -200.0f});
    T2D_CHECK_EQ(world.update(camera), 25u);   // 5 x 5 relative chunks around chunk (0, -1)
    const std::vector<ChunkKey> keys_before = world.resident_chunks();

    const i64 absolute_x = world.absolute_cell_x(18);    // floor(300 / 16)
    const i64 absolute_y = world.absolute_cell_y(-13);   // floor(-200 / 16)
    const OpenWorld::OriginShift shift = world.recenter(camera);
    T2D_CHECK(world.origin() == (ChunkKey{0, -1}));
    T2D_CHECK_EQ(shift.x, 0);
    T2D_CHECK_EQ(shift.y, -WorldChunk::kSize);
    const Vec2 delta = shift.position_delta(16.0f);
    T2D_CHECK_EQ(delta.x, 0.0f);
    T2D_CHECK_EQ(delta.y, static_cast<f32>(WorldChunk::kSize) * 16.0f);
    const Vec2 moved_centre = camera.centre() + delta;
    T2D_CHECK_EQ(moved_centre.x, 300.0f);
    T2D_CHECK_EQ(moved_centre.y, 312.0f);
    T2D_CHECK_EQ(world.absolute_cell_x(18), absolute_x);
    T2D_CHECK_EQ(world.absolute_cell_y(19), absolute_y);

    // Applying the shift to the camera and updating streams exactly the same absolute chunks.
    Camera2D moved_camera = camera;
    moved_camera.move(delta);
    T2D_CHECK_EQ(world.update(moved_camera), keys_before.size());
    T2D_CHECK(world.resident_chunks() == keys_before);

    // A camera that drifted far away in relative space is what recenter() exists for: the origin
    // moves to the camera's absolute chunk and the shift brings the camera back to relative (0, 0).
    const f32 far_relative_x = 1000.0f * static_cast<f32>(WorldChunk::kSize) * 16.0f;
    moved_camera.set_centre(Vec2{far_relative_x, 0.0f});
    const i64 absolute_before_x = world.absolute_cell_x(32000);   // camera cell under the old origin:
    const i64 absolute_before_y = world.absolute_cell_y(0);       // {0, -1} + 32000 cells in x
    const OpenWorld::OriginShift far_shift = world.recenter(moved_camera);
    T2D_CHECK(world.origin() == (ChunkKey{1000, -1}));
    T2D_CHECK_EQ(far_shift.x, 1000 * WorldChunk::kSize);
    T2D_CHECK_EQ(far_shift.y, 0);
    const Vec2 far_delta = far_shift.position_delta(16.0f);
    T2D_CHECK_EQ(moved_camera.centre().x + far_delta.x, 0.0f);
    T2D_CHECK_EQ(world.absolute_cell_x(0), absolute_before_x);
    T2D_CHECK_EQ(world.absolute_cell_y(0), absolute_before_y);
}

T2D_TEST(a_far_absolute_origin_keeps_the_relative_space_small) {
    const ChunkKey far_origin{1000000, -1000000};
    OpenWorld world(world_config(0xD1u));
    world.set_generator(pattern_generator(0xD1u, 4u));
    const OpenWorld::OriginShift shift = world.set_origin(far_origin);
    T2D_CHECK_EQ(shift.x, 1000000 * WorldChunk::kSize);
    T2D_CHECK_EQ(shift.y, -1000000 * WorldChunk::kSize);
    T2D_CHECK(world.origin() == far_origin);
    T2D_CHECK(world.absolute_of(ChunkCoord{0, 0}) == far_origin);
    T2D_CHECK_EQ(world.absolute_cell_x(0), 32000000);
    T2D_CHECK_EQ(world.absolute_cell_y(0), -32000000);

    T2D_CHECK_EQ(world.update(view_of_chunk(0, 0)), 9u);
    const std::vector<ChunkKey> resident = world.resident_chunks();
    T2D_REQUIRE(resident.size() == 9u);
    for (const ChunkKey& key : resident) {
        T2D_CHECK(key.x >= far_origin.x - 1 && key.x <= far_origin.x + 1);
        T2D_CHECK(key.y >= far_origin.y - 1 && key.y <= far_origin.y + 1);
    }
    T2D_CHECK(world.set(5, 5, 9));
    T2D_CHECK_EQ(world.at(5, 5), 9);

    // Relative geometry is near zero, never in the tens of millions: the origin does not leak.
    T2D_CHECK_EQ(world.cell_bounds(0, 0), (Aabb2{Vec2{0.0f, 0.0f}, Vec2{16.0f, 16.0f}}));
    T2D_CHECK_EQ(world.chunk_bounds(ChunkCoord{0, 0}), (Aabb2{Vec2{0.0f, 0.0f}, Vec2{512.0f, 512.0f}}));
    const std::vector<ChunkCoord> chunks = world.chunks_in(view_of_chunk(0, 0), 1);
    T2D_CHECK_EQ(chunks.size(), 9u);
    for (const ChunkCoord& coord : chunks) {
        T2D_CHECK(coord.x >= -1 && coord.x <= 1);
        T2D_CHECK(coord.y >= -1 && coord.y <= 1);
    }

    // Generation identity does not depend on the origin: the same absolute chunk, generated from a
    // world that never moved, has the same bytes.
    OpenWorld origin_world(world_config(0xD1u));
    origin_world.set_generator(pattern_generator(0xD1u, 4u));
    const std::optional<ChunkCoord> relative_zero = world.relative_of(ChunkKey{0, 0});
    T2D_REQUIRE(relative_zero.has_value());
    T2D_CHECK(*relative_zero == (ChunkCoord{-1000000, 1000000}));
    const WorldChunk& far_chunk = world.ensure_chunk(*relative_zero);
    const WorldChunk& origin_chunk = origin_world.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(far_chunk.checksum(), origin_chunk.checksum());
    T2D_CHECK(far_chunk.serialize() == origin_chunk.serialize());
    T2D_CHECK_EQ(world.absolute_of(*relative_zero), (ChunkKey{0, 0}));
}

// The practical limit of the coordinate design: absolute chunk +/-2^35, i.e. +/-2^40 cells. An absolute
// f32 coordinate there is quantised to hundreds of cells, so nothing may depend on one. This is the same
// scene as the million-chunk case above, moved to the edge of the clamp: the relative space is still
// 0..1024 world units, and the store is still keyed by two exact i64 values.
T2D_TEST(near_the_absolute_limit_the_relative_world_streams_edits_and_restores) {
    const ChunkKey far{OpenWorld::kMaxAbsoluteChunk - 4, -(OpenWorld::kMaxAbsoluteChunk - 4)};
    T2D_CHECK(far.x == 34359738364LL);
    T2D_CHECK(far.y == -34359738364LL);

    OpenWorld world(world_config(0xF1u));
    world.set_generator(pattern_generator(0xF1u, 4u));
    const OpenWorld::OriginShift shift = world.set_origin(far);
    T2D_CHECK_EQ(shift.x, far.x * WorldChunk::kSize);
    T2D_CHECK_EQ(shift.y, far.y * WorldChunk::kSize);
    T2D_CHECK(world.origin() == far);
    T2D_CHECK(world.absolute_of(ChunkCoord{0, 0}) == far);
    T2D_CHECK(world.absolute_of(ChunkCoord{1, 0}) == (ChunkKey{far.x + 1, far.y}));
    T2D_CHECK_EQ(world.absolute_cell_x(0), far.x * WorldChunk::kSize);
    T2D_CHECK_EQ(world.absolute_cell_y(0), far.y * WorldChunk::kSize);
    T2D_CHECK_EQ(world.absolute_cell_x(0), OpenWorld::kMaxAbsoluteCell - 4 * WorldChunk::kSize);
    T2D_CHECK_EQ(world.absolute_cell_y(0), -(OpenWorld::kMaxAbsoluteCell - 4 * WorldChunk::kSize));

    // The relative round trip still fits, and the same absolute key from a default-origin world cannot be
    // named at all: the identity generation and the store agree on is the absolute key.
    const std::optional<ChunkCoord> relative_zero = world.relative_of(far);
    T2D_REQUIRE(relative_zero.has_value());
    T2D_CHECK(*relative_zero == (ChunkCoord{0, 0}));
    OpenWorld origin_world(world_config(0xF1u));
    T2D_CHECK_FALSE(origin_world.relative_of(far).has_value());

    // The two-chunk relative view the Mefemma scene frames: 1024 x 512 relative world units.
    const Aabb2 view = box_at(Vec2{0.0f, 0.0f}, Vec2{1024.0f, 512.0f});
    T2D_CHECK_EQ(world.update(view), 12u);   // 2 x 1 view chunks plus the load margin ring
    T2D_CHECK(world.find_chunk(ChunkCoord{0, 0}) != nullptr);
    T2D_CHECK(world.find_chunk(ChunkCoord{1, 0}) != nullptr);
    T2D_CHECK_EQ(world.cell_bounds(0, 0), (Aabb2{Vec2{0.0f, 0.0f}, Vec2{16.0f, 16.0f}}));
    T2D_CHECK_EQ(world.cell_bounds(-1, -1), (Aabb2{Vec2{-16.0f, -16.0f}, Vec2{0.0f, 0.0f}}));
    T2D_CHECK_EQ(world.chunk_bounds(ChunkCoord{0, 0}), (Aabb2{Vec2{0.0f, 0.0f}, Vec2{512.0f, 512.0f}}));
    T2D_CHECK_EQ(world.chunk_bounds(ChunkCoord{1, 0}), (Aabb2{Vec2{512.0f, 0.0f}, Vec2{1024.0f, 512.0f}}));
    const std::vector<ChunkCoord> chunks = world.chunks_in(view, 1);
    T2D_CHECK_EQ(chunks.size(), 12u);
    for (const ChunkCoord& coord : chunks) {
        T2D_CHECK(coord.x >= -1 && coord.x <= 2);
        T2D_CHECK(coord.y >= -1 && coord.y <= 1);
    }

    // Generation sees the absolute key alone: an independently rebased world produces the same bytes.
    OpenWorld twin(world_config(0xF1u));
    twin.set_generator(pattern_generator(0xF1u, 4u));
    T2D_CHECK_EQ(twin.set_origin(far).x, far.x * WorldChunk::kSize);
    const WorldChunk& far_chunk = world.ensure_chunk(ChunkCoord{0, 0});
    const WorldChunk& twin_chunk = twin.ensure_chunk(ChunkCoord{0, 0});
    T2D_CHECK_EQ(far_chunk.checksum(), twin_chunk.checksum());
    T2D_CHECK(far_chunk.serialize() == twin_chunk.serialize());

    // An edit flushed into a real ChunkStore under the far absolute key, then a fresh world reads it back
    // by that key; every chunk that was never edited is generated again, not stored.
    ScratchStore scratch("near-limit.t2dk");
    {
        ChunkStore store = open_required_store(scratch);
        world.set_store(store.loader(), store.saver());
        T2D_CHECK(world.set(5, 5, 9));
        T2D_CHECK(world.flush());
        T2D_CHECK_EQ(store.chunk_count(), 1u);
        T2D_CHECK(store.contains(far));
        T2D_CHECK_FALSE(store.contains((ChunkKey{far.x + 1, far.y})));
    }
    {
        OpenWorld restored(world_config(0xF1u));
        restored.set_generator(pattern_generator(0xF1u, 4u));
        T2D_CHECK_EQ(restored.set_origin(far).x, far.x * WorldChunk::kSize);
        ChunkStore store = open_required_store(scratch);
        restored.set_store(store.loader(), store.saver());
        T2D_CHECK_EQ(restored.update(view), 12u);
        T2D_CHECK_EQ(restored.at(5, 5), 9);
        // Unedited cells are the generator's, byte for byte the twin world's.
        T2D_CHECK_EQ(restored.at(6, 6), twin_chunk.at(0, 6, 6));
        T2D_CHECK_EQ(restored.at(0, 0), twin_chunk.at(0, 0, 0));
    }

    // The clamp is exact at +/-kMaxAbsoluteChunk, and the shift reports the move in absolute cells.
    OpenWorld clamp_world(world_config(0xF1u));
    const OpenWorld::OriginShift to_high =
        clamp_world.set_origin(ChunkKey{OpenWorld::kMaxAbsoluteChunk + 7, -(OpenWorld::kMaxAbsoluteChunk + 9)});
    T2D_CHECK(clamp_world.origin() == (ChunkKey{OpenWorld::kMaxAbsoluteChunk, -OpenWorld::kMaxAbsoluteChunk}));
    T2D_CHECK_EQ(to_high.x, OpenWorld::kMaxAbsoluteChunk * WorldChunk::kSize);
    T2D_CHECK_EQ(to_high.y, -OpenWorld::kMaxAbsoluteChunk * WorldChunk::kSize);
    T2D_CHECK_EQ(clamp_world.absolute_cell_x(0), OpenWorld::kMaxAbsoluteCell);
    T2D_CHECK_EQ(clamp_world.absolute_cell_y(0), -OpenWorld::kMaxAbsoluteCell);
    const OpenWorld::OriginShift to_low =
        clamp_world.set_origin(ChunkKey{-OpenWorld::kMaxAbsoluteChunk - 3, OpenWorld::kMaxAbsoluteChunk + 2});
    T2D_CHECK(clamp_world.origin() == (ChunkKey{-OpenWorld::kMaxAbsoluteChunk, OpenWorld::kMaxAbsoluteChunk}));
    T2D_CHECK_EQ(to_low.x, -2 * OpenWorld::kMaxAbsoluteChunk * WorldChunk::kSize);
    T2D_CHECK_EQ(to_low.y, 2 * OpenWorld::kMaxAbsoluteChunk * WorldChunk::kSize);
    T2D_CHECK_EQ(clamp_world.absolute_cell_x(0), -OpenWorld::kMaxAbsoluteCell);
    T2D_CHECK_EQ(clamp_world.absolute_cell_y(0), OpenWorld::kMaxAbsoluteCell);
}

T2D_TEST_MAIN
