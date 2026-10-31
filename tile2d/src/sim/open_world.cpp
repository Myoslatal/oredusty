// Tile2D - the relative-coordinate open world: streaming, deterministic generation and the store.
//
// The world is a hash map of resident chunks keyed by ABSOLUTE ChunkKey. Everything outside this file
// addresses them relative to a movable origin, so f32 geometry stays near zero however far the world
// extends; the absolute key is what generation and the store see, so rebasing only changes the naming,
// never the content or the save. update() derives the keep set from the relative view, translates it
// to absolute keys and evicts by key; load order is relative and nearest-first.
//
// Queries never generate or load: a chunk that is not resident is empty space to a const query. The
// store, the generator and the world are all single threaded and deterministic - chunk_seed() is a
// pure function of (world seed, absolute key), so two worlds with the same seed agree in any order.
#include <t2d/sim/open_world.h>

#include <t2d/core/bitstream.h>
#include <t2d/core/log.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace t2d {
namespace {

/// A view touching more chunks than this is refused rather than materialised. A 4K screen at one
/// pixel per cell touches fewer than 10k; 2^20 leaves room for the margin rings and makes a nonsense
/// view (or a NaN that slipped through) cheap.
constexpr u64 kMaxChunksInView = u64{1} << 20;

/// Cell walking in the box queries is bounded the same way: anything bigger walks the resident chunks
/// instead of a rectangle of cells that could never exist.
constexpr u64 kMaxScanCells = u64{1} << 20;

constexpr u8 kChunkFormatVersion = 1;

constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;

[[nodiscard]] constexpr u64 fnv_mix(u64 hash, u64 value, i32 byte_count) {
    for (i32 index = 0; index < byte_count; ++index) {
        hash = (hash ^ ((value >> (index * 8)) & 0xFFu)) * kFnvPrime;
    }
    return hash;
}

/// The splitmix64 finaliser, the mixing step chunk_seed() documents.
[[nodiscard]] constexpr u64 splitmix_finalize(u64 value) {
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

[[nodiscard]] constexpr f64 relative_limit() { return static_cast<f64>(OpenWorld::kMaxRelativeCell); }

/// A view is usable when both corners are finite and the box has no negative side. A zero sized box
/// is legal (a point still touches one cell, the same rule TileMap::world_to_tiles() uses).
[[nodiscard]] bool finite_box(const Aabb2& box) {
    return std::isfinite(box.min.x) && std::isfinite(box.min.y) && std::isfinite(box.max.x) &&
           std::isfinite(box.max.y) && box.max.x >= box.min.x && box.max.y >= box.min.y;
}

[[nodiscard]] f64 clamp_relative(f64 cell) {
    const f64 limit = relative_limit();
    if (!(cell > -limit)) return -limit;   // also catches NaN
    if (cell >= limit) return limit;
    return cell;
}

/// The relative cells a view touches, refusing a view outside the practical relative range. Floor on
/// the low side, ceil on the high side, at least one cell - exactly TileMap's collision convention,
/// but in the small relative coordinate space.
[[nodiscard]] bool view_cell_range(const Aabb2& view, f32 tile, i64& x0, i64& y0, i64& x1, i64& y1) {
    if (!finite_box(view)) return false;
    const f64 tile_f = static_cast<f64>(tile);
    const f64 limit = relative_limit();
    const f64 floor_x = std::floor(static_cast<f64>(view.min.x) / tile_f);
    const f64 floor_y = std::floor(static_cast<f64>(view.min.y) / tile_f);
    const f64 ceil_x = std::ceil(static_cast<f64>(view.max.x) / tile_f);
    const f64 ceil_y = std::ceil(static_cast<f64>(view.max.y) / tile_f);
    if (floor_x < -limit || floor_y < -limit || ceil_x > limit || ceil_y > limit) return false;
    x0 = static_cast<i64>(floor_x);
    y0 = static_cast<i64>(floor_y);
    x1 = std::max(static_cast<i64>(ceil_x), x0 + 1);
    y1 = std::max(static_cast<i64>(ceil_y), y0 + 1);
    return true;
}

struct CellRange {
    i64 x0 = 0;
    i64 y0 = 0;
    i64 x1 = 0;   ///< exclusive
    i64 y1 = 0;   ///< exclusive
};

/// The cells a relative box touches for the overlap queries. Coordinates outside the practical relative
/// range are clamped to it; the facility promises nothing about cells beyond that range.
[[nodiscard]] bool box_cell_range(const Aabb2& box, f32 tile, CellRange& range) {
    if (!finite_box(box)) return false;
    const f64 tile_f = static_cast<f64>(tile);
    const i64 x0 = static_cast<i64>(clamp_relative(std::floor(static_cast<f64>(box.min.x) / tile_f)));
    const i64 y0 = static_cast<i64>(clamp_relative(std::floor(static_cast<f64>(box.min.y) / tile_f)));
    const i64 x1 = static_cast<i64>(clamp_relative(std::ceil(static_cast<f64>(box.max.x) / tile_f)));
    const i64 y1 = static_cast<i64>(clamp_relative(std::ceil(static_cast<f64>(box.max.y) / tile_f)));
    range.x0 = x0;
    range.y0 = y0;
    range.x1 = std::max(x1, x0 + 1);
    range.y1 = std::max(y1, y0 + 1);
    return true;
}

template <class Predicate>
[[nodiscard]] bool any_cell_in_resident_chunks(const OpenWorld& world, const CellRange& range,
                                               Predicate& predicate) {
    const ChunkKey origin = world.origin();
    for (const ChunkKey& key : world.resident_chunks()) {
        // Resident keys are origin + i32 offsets at creation time and the origin is clamped, so this
        // subtraction stays far inside i64 even after a rebase.
        const i64 chunk_x = key.x - origin.x;
        const i64 chunk_y = key.y - origin.y;
        const i64 start_x = chunk_x * WorldChunk::kSize;
        const i64 start_y = chunk_y * WorldChunk::kSize;
        if (!(start_x < range.x1 && start_x + WorldChunk::kSize > range.x0 &&
              start_y < range.y1 && start_y + WorldChunk::kSize > range.y0)) {
            continue;
        }
        const i64 x0 = std::max(range.x0, start_x);
        const i64 y0 = std::max(range.y0, start_y);
        const i64 x1 = std::min(range.x1, start_x + WorldChunk::kSize);
        const i64 y1 = std::min(range.y1, start_y + WorldChunk::kSize);
        for (i64 y = y0; y < y1; ++y) {
            for (i64 x = x0; x < x1; ++x) {
                if (predicate(static_cast<i32>(x), static_cast<i32>(y))) return true;
            }
        }
    }
    return false;
}

/// "Does any relative cell of this box satisfy the predicate": the shape every overlap query has.
/// Normal entity sized boxes walk cells; boxes bigger than the scan budget walk the resident chunks,
/// which is bounded by memory instead of by the box.
template <class Predicate>
[[nodiscard]] bool any_cell(const OpenWorld& world, f32 tile, const Aabb2& box, Predicate&& predicate) {
    CellRange range{};
    if (!box_cell_range(box, tile, range)) return false;
    const u64 width = static_cast<u64>(range.x1 - range.x0);
    const u64 height = static_cast<u64>(range.y1 - range.y0);
    if (width > kMaxScanCells || height > kMaxScanCells || width * height > kMaxScanCells) {
        return any_cell_in_resident_chunks(world, range, predicate);
    }
    for (i64 y = range.y0; y < range.y1; ++y) {
        for (i64 x = range.x0; x < range.x1; ++x) {
            if (predicate(static_cast<i32>(x), static_cast<i32>(y))) return true;
        }
    }
    return false;
}

[[nodiscard]] u64 chunk_distance(const ChunkCoord& a, const ChunkCoord& b) {
    const i64 dx = static_cast<i64>(a.x) - static_cast<i64>(b.x);
    const i64 dy = static_cast<i64>(a.y) - static_cast<i64>(b.y);
    return static_cast<u64>(dx > 0 ? dx : -dx) + static_cast<u64>(dy > 0 ? dy : -dy);
}

/// Distance of a resident chunk (absolute key) from a relative view centre. The subtraction is bounded
/// by construction: keys are origin + i32 offsets and the origin is clamped.
[[nodiscard]] u64 key_distance(const ChunkKey& key, const ChunkKey& origin, const ChunkCoord& centre) {
    const i64 dx = (key.x - origin.x) - static_cast<i64>(centre.x);
    const i64 dy = (key.y - origin.y) - static_cast<i64>(centre.y);
    return static_cast<u64>(dx > 0 ? dx : -dx) + static_cast<u64>(dy > 0 ? dy : -dy);
}

/// Total order for "which chunk to give up": farthest from the view centre first, ties by y then x,
/// so budget eviction does not depend on the hash map's iteration order.
[[nodiscard]] bool key_farther(const ChunkKey& candidate, const ChunkKey& current, const ChunkKey& origin,
                               const ChunkCoord& centre) {
    const u64 candidate_distance = key_distance(candidate, origin, centre);
    const u64 current_distance = key_distance(current, origin, centre);
    if (candidate_distance != current_distance) return candidate_distance > current_distance;
    const i64 candidate_y = candidate.y - origin.y;
    const i64 current_y = current.y - origin.y;
    if (candidate_y != current_y) return candidate_y > current_y;
    return (candidate.x - origin.x) > (current.x - origin.x);
}

[[nodiscard]] bool key_less(const ChunkKey& a, const ChunkKey& b) {
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
}

[[nodiscard]] bool coord_less(const ChunkCoord& a, const ChunkCoord& b) {
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
}

} // namespace

// --- chunk hashes ------------------------------------------------------------

usize ChunkKeyHash::operator()(const ChunkKey& key) const noexcept {
    u64 hash = static_cast<u64>(key.x) * 0x9E3779B97F4A7C15ull;
    hash ^= static_cast<u64>(key.y) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return static_cast<usize>(hash);
}

usize ChunkCoordHash::operator()(const ChunkCoord& coord) const noexcept {
    u64 hash = static_cast<u64>(static_cast<u32>(coord.x)) * 0x9E3779B97F4A7C15ull;
    hash ^= static_cast<u64>(static_cast<u32>(coord.y)) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return static_cast<usize>(hash);
}

// --- WorldChunk --------------------------------------------------------------

WorldChunk::WorldChunk(i32 layers)
    : layers_(clamp_i32(layers, 1, TileMap::kMaxLayers)),
      cells_(static_cast<usize>(layers_) * kCellCount, kEmptyTile) {}

i32 WorldChunk::layers() const { return layers_; }

TileId WorldChunk::at(i32 layer, i32 local_x, i32 local_y) const {
    if (layer < 0 || layer >= layers_) return kEmptyTile;
    if (local_x < 0 || local_x >= kSize || local_y < 0 || local_y >= kSize) return kEmptyTile;
    const usize index = static_cast<usize>(layer) * kCellCount +
                        static_cast<usize>(local_y) * static_cast<usize>(kSize) +
                        static_cast<usize>(local_x);
    return cells_[index];
}

void WorldChunk::set(i32 layer, i32 local_x, i32 local_y, TileId id) {
    if (layer < 0 || layer >= layers_) return;
    if (local_x < 0 || local_x >= kSize || local_y < 0 || local_y >= kSize) return;
    const usize index = static_cast<usize>(layer) * kCellCount +
                        static_cast<usize>(local_y) * static_cast<usize>(kSize) +
                        static_cast<usize>(local_x);
    cells_[index] = id;
    mark_dirty();
}

void WorldChunk::fill(i32 layer, TileId id) {
    if (layer < 0 || layer >= layers_) return;
    const usize offset = static_cast<usize>(layer) * kCellCount;
    std::fill(cells_.begin() + static_cast<isize>(offset),
              cells_.begin() + static_cast<isize>(offset + kCellCount), id);
    mark_dirty();
}

Span<TileId> WorldChunk::tiles(i32 layer) {
    if (layer < 0 || layer >= layers_) return {};
    return Span<TileId>(cells_.data() + static_cast<usize>(layer) * kCellCount, kCellCount);
}

ConstSpan<const TileId> WorldChunk::tiles(i32 layer) const {
    if (layer < 0 || layer >= layers_) return {};
    return ConstSpan<const TileId>(cells_.data() + static_cast<usize>(layer) * kCellCount, kCellCount);
}

bool WorldChunk::dirty() const { return dirty_; }

void WorldChunk::mark_dirty() { dirty_ = true; }

void WorldChunk::clear_dirty() { dirty_ = false; }

usize WorldChunk::count_non_empty() const {
    usize count = 0;
    for (const TileId id : cells_) {
        if (id != kEmptyTile) ++count;
    }
    return count;
}

u64 WorldChunk::checksum() const {
    u64 hash = kFnvOffsetBasis;
    hash = fnv_mix(hash, static_cast<u64>(static_cast<u32>(layers_)), 4);
    for (const TileId id : cells_) hash = fnv_mix(hash, id, 2);
    return hash;
}

std::vector<u8> WorldChunk::serialize() const {
    std::vector<u8> bytes;
    ByteWriter writer(bytes);
    writer.write_u8('T');
    writer.write_u8('2');
    writer.write_u8('D');
    writer.write_u8('C');
    writer.write_u8(kChunkFormatVersion);
    writer.write_varint(static_cast<u32>(layers_));
    for (i32 layer = 0; layer < layers_; ++layer) {
        const usize offset = static_cast<usize>(layer) * kCellCount;
        usize index = 0;
        while (index < kCellCount) {
            const TileId id = cells_[offset + index];
            usize run = 1;
            while (index + run < kCellCount && cells_[offset + index + run] == id) ++run;
            writer.write_varint(static_cast<u32>(run));
            writer.write_u16(id);
            index += run;
        }
    }
    return bytes;
}

std::optional<WorldChunk> WorldChunk::deserialize(ConstSpan<const u8> data, i32 expected_layers) {
    ByteReader reader(data);
    if (reader.read_u8() != 'T' || reader.read_u8() != '2' || reader.read_u8() != 'D' ||
        reader.read_u8() != 'C') {
        return std::nullopt;
    }
    if (reader.read_u8() != kChunkFormatVersion) return std::nullopt;
    const u32 layers = reader.read_varint();
    if (!reader.ok()) return std::nullopt;
    if (layers < 1u || layers > static_cast<u32>(TileMap::kMaxLayers)) return std::nullopt;
    if (expected_layers < 1 || expected_layers > TileMap::kMaxLayers) return std::nullopt;
    if (layers != static_cast<u32>(expected_layers)) return std::nullopt;

    WorldChunk chunk(static_cast<i32>(layers));
    for (u32 layer = 0; layer < layers; ++layer) {
        usize index = 0;
        while (index < kCellCount) {
            const u32 run = reader.read_varint();
            const TileId id = reader.read_u16();
            if (!reader.ok()) return std::nullopt;
            if (run == 0u) return std::nullopt;
            if (static_cast<u64>(run) > static_cast<u64>(kCellCount - index)) return std::nullopt;
            const usize offset = static_cast<usize>(layer) * kCellCount;
            std::fill(chunk.cells_.begin() + static_cast<isize>(offset + index),
                      chunk.cells_.begin() + static_cast<isize>(offset + index + run), id);
            index += run;
        }
    }
    if (!reader.empty()) return std::nullopt;   // trailing bytes: not a clean chunk blob
    return chunk;
}

// --- OpenWorld: construction -------------------------------------------------

OpenWorld::OpenWorld(const OpenWorldConfig& config) : config_(config) {
    if (!std::isfinite(config_.tile_size) || !(config_.tile_size > 0.0f)) config_.tile_size = 16.0f;
    config_.layers = clamp_i32(config_.layers, 1, TileMap::kMaxLayers);
    if (config_.load_margin_chunks < 0) config_.load_margin_chunks = 0;
    if (config_.unload_margin_chunks < config_.load_margin_chunks) {
        config_.unload_margin_chunks = config_.load_margin_chunks;
    }
}

const OpenWorldConfig& OpenWorld::config() const { return config_; }
f32 OpenWorld::tile_size() const { return config_.tile_size; }
i32 OpenWorld::layers() const { return config_.layers; }
u64 OpenWorld::seed() const { return config_.seed; }

void OpenWorld::set_generator(ChunkGenerator generator) { generator_ = std::move(generator); }

void OpenWorld::set_store(ChunkLoader loader, ChunkSaver saver) {
    loader_ = std::move(loader);
    saver_ = std::move(saver);
}

// --- OpenWorld: origin -------------------------------------------------------

ChunkKey OpenWorld::origin() const { return origin_; }

ChunkKey OpenWorld::absolute_of(const ChunkCoord& relative) const {
    return ChunkKey{origin_.x + static_cast<i64>(relative.x), origin_.y + static_cast<i64>(relative.y)};
}

std::optional<ChunkCoord> OpenWorld::relative_of(const ChunkKey& absolute) const {
    const i64 max_offset = static_cast<i64>(std::numeric_limits<i32>::max());
    const i64 min_offset = static_cast<i64>(std::numeric_limits<i32>::min());
    // Compare against the reachable offsets instead of subtracting first: absolute may be any i64 and
    // the subtraction itself would overflow at the extremes.
    if (absolute.x > origin_.x + max_offset || absolute.x < origin_.x + min_offset) return std::nullopt;
    if (absolute.y > origin_.y + max_offset || absolute.y < origin_.y + min_offset) return std::nullopt;
    return ChunkCoord{static_cast<i32>(absolute.x - origin_.x),
                      static_cast<i32>(absolute.y - origin_.y)};
}

i64 OpenWorld::absolute_cell_x(i32 relative_x) const {
    return origin_.x * WorldChunk::kSize + static_cast<i64>(relative_x);
}

i64 OpenWorld::absolute_cell_y(i32 relative_y) const {
    return origin_.y * WorldChunk::kSize + static_cast<i64>(relative_y);
}

OpenWorld::OriginShift OpenWorld::set_origin(const ChunkKey& new_origin) {
    const auto clamp_chunk = [](i64 value) {
        if (value < -kMaxAbsoluteChunk) return -kMaxAbsoluteChunk;
        if (value > kMaxAbsoluteChunk) return kMaxAbsoluteChunk;
        return value;
    };
    const ChunkKey clamped{clamp_chunk(new_origin.x), clamp_chunk(new_origin.y)};
    const OriginShift shift{(clamped.x - origin_.x) * WorldChunk::kSize,
                            (clamped.y - origin_.y) * WorldChunk::kSize};
    origin_ = clamped;
    return shift;
}

OpenWorld::OriginShift OpenWorld::recenter(const Camera2D& camera) {
    const f64 tile = static_cast<f64>(config_.tile_size);
    const i32 cell_x = static_cast<i32>(clamp_relative(std::floor(static_cast<f64>(camera.centre().x) / tile)));
    const i32 cell_y = static_cast<i32>(clamp_relative(std::floor(static_cast<f64>(camera.centre().y) / tile)));
    const ChunkCoord relative = chunk_of(cell_x, cell_y);
    return set_origin(ChunkKey{origin_.x + static_cast<i64>(relative.x),
                               origin_.y + static_cast<i64>(relative.y)});
}

Vec2 OpenWorld::OriginShift::position_delta(f32 tile_size) const {
    // new relative = absolute - new_origin = old relative - shift, so the caller adds -shift * tile.
    return Vec2{-static_cast<f32>(x) * tile_size, -static_cast<f32>(y) * tile_size};
}

// --- OpenWorld: determinism --------------------------------------------------

u64 OpenWorld::chunk_seed(u64 world_seed, const ChunkKey& key) {
    u64 state = world_seed + 0x9E3779B97F4A7C15ull;
    state = splitmix_finalize(state ^ (static_cast<u64>(key.x) * 0xD6E8FEB86659FD93ull));
    state = splitmix_finalize(state ^ (static_cast<u64>(key.y) * 0xA0761D6478BD642Full));
    return splitmix_finalize(state);
}

Rng OpenWorld::chunk_rng(const ChunkKey& key) const { return Rng(chunk_seed(config_.seed, key)); }

// --- OpenWorld: streaming ----------------------------------------------------

ChunkKey OpenWorld::key_of(const ChunkCoord& relative) const {
    return ChunkKey{origin_.x + static_cast<i64>(relative.x),
                    origin_.y + static_cast<i64>(relative.y)};
}

std::vector<ChunkCoord> OpenWorld::chunks_in(const Aabb2& view, i32 margin_chunks) const {
    i64 x0 = 0;
    i64 y0 = 0;
    i64 x1 = 0;
    i64 y1 = 0;
    if (!view_cell_range(view, config_.tile_size, x0, y0, x1, y1)) return {};

    const i64 margin = margin_chunks > 0 ? margin_chunks : 0;
    const ChunkCoord first = chunk_of(static_cast<i32>(x0), static_cast<i32>(y0));
    const ChunkCoord last = chunk_of(static_cast<i32>(x1 - 1), static_cast<i32>(y1 - 1));
    const i64 low_x = static_cast<i64>(first.x) - margin;
    const i64 low_y = static_cast<i64>(first.y) - margin;
    const i64 high_x = static_cast<i64>(last.x) + margin;
    const i64 high_y = static_cast<i64>(last.y) + margin;
    const i64 i32_min = static_cast<i64>(std::numeric_limits<i32>::min());
    const i64 i32_max = static_cast<i64>(std::numeric_limits<i32>::max());
    if (low_x < i32_min || low_y < i32_min || high_x > i32_max || high_y > i32_max) return {};

    const u64 width = static_cast<u64>(high_x - low_x + 1);
    const u64 height = static_cast<u64>(high_y - low_y + 1);
    if (width > kMaxChunksInView || height > kMaxChunksInView || width * height > kMaxChunksInView) {
        return {};
    }

    std::vector<ChunkCoord> chunks;
    chunks.reserve(static_cast<usize>(width * height));
    for (i64 chunk_y = low_y; chunk_y <= high_y; ++chunk_y) {
        for (i64 chunk_x = low_x; chunk_x <= high_x; ++chunk_x) {
            chunks.push_back(ChunkCoord{static_cast<i32>(chunk_x), static_cast<i32>(chunk_y)});
        }
    }
    return chunks;
}

usize OpenWorld::update(const Aabb2& view) {
    if (!finite_box(view)) return chunks_.size();
    const std::vector<ChunkCoord> load_list = chunks_in(view, config_.load_margin_chunks);
    const std::vector<ChunkCoord> keep_list = chunks_in(view, config_.unload_margin_chunks);
    if (load_list.empty() || keep_list.empty()) return chunks_.size();   // refused view: no-op

    // The keep/wanted sets are absolute: rebasing the origin only renames chunks, it must never make
    // update() think a resident chunk left the view.
    std::unordered_set<ChunkKey, ChunkKeyHash> keep_keys;
    keep_keys.reserve(keep_list.size());
    for (const ChunkCoord& coord : keep_list) keep_keys.insert(key_of(coord));
    std::unordered_set<ChunkKey, ChunkKeyHash> wanted_keys;
    wanted_keys.reserve(load_list.size());
    for (const ChunkCoord& coord : load_list) wanted_keys.insert(key_of(coord));

    // (1) Chunks whose absolute key left the unload ring: drop clean ones, save dirty ones first, keep
    // a dirty one whose save failed (or that has no saver) where it is.
    std::vector<ChunkKey> leaving;
    leaving.reserve(chunks_.size());
    for (const auto& entry : chunks_) {
        if (!keep_keys.contains(entry.first)) leaving.push_back(entry.first);
    }
    std::sort(leaving.begin(), leaving.end(), key_less);
    for (const ChunkKey& key : leaving) evict_resident(key);

    // (2) Missing chunks of the load ring, nearest the view centre first. The centre is taken in f64
    // so two huge corners cannot overflow f32, then clamped to the practical relative range.
    const f64 tile = static_cast<f64>(config_.tile_size);
    const f64 centre_x = ((static_cast<f64>(view.min.x) + static_cast<f64>(view.max.x)) * 0.5) / tile;
    const f64 centre_y = ((static_cast<f64>(view.min.y) + static_cast<f64>(view.max.y)) * 0.5) / tile;
    const i32 centre_cell_x = static_cast<i32>(clamp_relative(std::floor(centre_x)));
    const i32 centre_cell_y = static_cast<i32>(clamp_relative(std::floor(centre_y)));
    const ChunkCoord centre = chunk_of(centre_cell_x, centre_cell_y);

    std::vector<ChunkCoord> missing;
    missing.reserve(load_list.size());
    for (const ChunkCoord& coord : load_list) {
        if (!is_loaded(coord)) missing.push_back(coord);
    }
    std::sort(missing.begin(), missing.end(), [&centre](const ChunkCoord& a, const ChunkCoord& b) {
        const u64 distance_a = chunk_distance(a, centre);
        const u64 distance_b = chunk_distance(b, centre);
        if (distance_a != distance_b) return distance_a < distance_b;
        return coord_less(a, b);
    });

    for (const ChunkCoord& coord : missing) {
        if (config_.max_loaded_chunks != 0 && chunks_.size() >= config_.max_loaded_chunks) {
            if (!make_room(wanted_keys, centre)) {
                ++stats_.budget_refusals;
                continue;
            }
        }
        ensure_chunk(coord);
    }
    return chunks_.size();
}

usize OpenWorld::update(const Camera2D& camera) { return update(camera.visible_world()); }

WorldChunk* OpenWorld::find_chunk(const ChunkCoord& relative) {
    const auto found = chunks_.find(key_of(relative));
    return found != chunks_.end() ? &found->second : nullptr;
}

const WorldChunk* OpenWorld::find_chunk(const ChunkCoord& relative) const {
    const auto found = chunks_.find(key_of(relative));
    return found != chunks_.end() ? &found->second : nullptr;
}

bool OpenWorld::is_loaded(const ChunkCoord& relative) const {
    return chunks_.find(key_of(relative)) != chunks_.end();
}

WorldChunk& OpenWorld::ensure_chunk(const ChunkCoord& relative) {
    const ChunkKey key = key_of(relative);
    const auto found = chunks_.find(key);
    if (found != chunks_.end()) return found->second;

    WorldChunk chunk(config_.layers);
    bool restored = false;
    if (loader_) {
        std::optional<std::vector<u8>> payload = loader_(key);
        if (payload.has_value() && !payload->empty()) {
            std::optional<WorldChunk> parsed = WorldChunk::deserialize(
                ConstSpan<const u8>(payload->data(), payload->size()), config_.layers);
            if (parsed.has_value()) {
                chunk = std::move(*parsed);
                restored = true;
                ++stats_.chunks_restored;
            } else {
                // A store can outlive the format that wrote it. Say which absolute chunk, count it,
                // and let the generator rebuild it: refusing the world over one bad payload is worse.
                T2D_WARN("open world: stored chunk ({}, {}) is malformed ({} bytes); generating it instead",
                         key.x, key.y, payload->size());
                ++stats_.restore_failures;
            }
        }
    }
    if (!restored) {
        if (generator_) generator_(key, chunk);
        // A generator may build its content through set()/fill(), which mark the chunk dirty; the
        // world still returns it clean, because generated content is reproducible and must not be
        // saved on every eviction. The dirty flag is the edit flag, not the write flag.
        chunk.clear_dirty();
        ++stats_.chunks_generated;
    }
    const auto inserted = chunks_.emplace(key, std::move(chunk));
    return inserted.first->second;
}

bool OpenWorld::unload_chunk(const ChunkCoord& relative) {
    const ChunkKey key = key_of(relative);
    const auto found = chunks_.find(key);
    if (found == chunks_.end()) return true;
    if (!found->second.dirty()) {
        chunks_.erase(found);
        return true;
    }
    if (!save_chunk(found->second, key)) return false;   // no saver, or the saver said no
    chunks_.erase(found);
    return true;
}

std::vector<ChunkKey> OpenWorld::resident_chunks() const {
    std::vector<ChunkKey> keys;
    keys.reserve(chunks_.size());
    for (const auto& entry : chunks_) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end(), key_less);
    return keys;
}

usize OpenWorld::resident_chunk_count() const { return chunks_.size(); }

// --- OpenWorld: relative cell queries ----------------------------------------

TileId OpenWorld::at(i32 cell_x, i32 cell_y) const { return at(0, cell_x, cell_y); }

TileId OpenWorld::at(i32 layer, i32 cell_x, i32 cell_y) const {
    if (layer < 0 || layer >= config_.layers) return kEmptyTile;
    const ChunkCoord coord = chunk_of(cell_x, cell_y);
    const auto found = chunks_.find(key_of(coord));
    if (found == chunks_.end()) return kEmptyTile;
    return found->second.at(layer, local_of(cell_x), local_of(cell_y));
}

TileId OpenWorld::topmost(i32 cell_x, i32 cell_y, LayerMask mask) const {
    for (i32 layer = config_.layers - 1; layer >= 0; --layer) {
        if ((mask & TileMap::layer_mask(layer)) == 0u) continue;
        const TileId id = at(layer, cell_x, cell_y);
        if (id != kEmptyTile) return id;
    }
    return kEmptyTile;
}

bool OpenWorld::set(i32 cell_x, i32 cell_y, TileId id) { return set(0, cell_x, cell_y, id); }

bool OpenWorld::set(i32 layer, i32 cell_x, i32 cell_y, TileId id) {
    if (layer < 0 || layer >= config_.layers) return false;
    WorldChunk& chunk = ensure_chunk(chunk_of(cell_x, cell_y));
    chunk.set(layer, local_of(cell_x), local_of(cell_y), id);
    return true;
}

bool OpenWorld::is_solid(i32 cell_x, i32 cell_y, const Tileset& tileset, LayerMask mask) const {
    for (i32 layer = 0; layer < config_.layers; ++layer) {
        if ((mask & TileMap::layer_mask(layer)) == 0u) continue;
        if (tileset.is_solid(at(layer, cell_x, cell_y))) return true;
    }
    return false;
}

bool OpenWorld::is_one_way(i32 cell_x, i32 cell_y, const Tileset& tileset, LayerMask mask) const {
    for (i32 layer = 0; layer < config_.layers; ++layer) {
        if ((mask & TileMap::layer_mask(layer)) == 0u) continue;
        if (tileset.is_one_way(at(layer, cell_x, cell_y))) return true;
    }
    return false;
}

bool OpenWorld::is_hazard(i32 cell_x, i32 cell_y, const Tileset& tileset, LayerMask mask) const {
    for (i32 layer = 0; layer < config_.layers; ++layer) {
        if ((mask & TileMap::layer_mask(layer)) == 0u) continue;
        if (tileset.is_hazard(at(layer, cell_x, cell_y))) return true;
    }
    return false;
}

bool OpenWorld::overlaps_solid(const Aabb2& box, const Tileset& tileset, LayerMask mask) const {
    return any_cell(*this, config_.tile_size, box,
                    [&](i32 x, i32 y) { return is_solid(x, y, tileset, mask); });
}

bool OpenWorld::overlaps_hazard(const Aabb2& box, const Tileset& tileset, LayerMask mask) const {
    return any_cell(*this, config_.tile_size, box,
                    [&](i32 x, i32 y) { return is_hazard(x, y, tileset, mask); });
}

usize OpenWorld::dirty_chunk_count() const {
    usize count = 0;
    for (const auto& entry : chunks_) {
        if (entry.second.dirty()) ++count;
    }
    return count;
}

// --- OpenWorld: persistence --------------------------------------------------

bool OpenWorld::flush() {
    // Sorted absolute keys: a store sees the same writes in the same order every run.
    const std::vector<ChunkKey> keys = resident_chunks();
    for (const ChunkKey& key : keys) {
        const auto found = chunks_.find(key);
        if (found == chunks_.end() || !found->second.dirty()) continue;
        save_chunk(found->second, key);   // clears dirty only on success
    }
    for (const auto& entry : chunks_) {
        if (entry.second.dirty()) return false;
    }
    return true;
}

bool OpenWorld::flush(const ChunkCoord& relative) {
    const ChunkKey key = key_of(relative);
    const auto found = chunks_.find(key);
    if (found == chunks_.end()) return true;
    if (!found->second.dirty()) return true;
    return save_chunk(found->second, key);
}

const OpenWorld::Stats& OpenWorld::stats() const { return stats_; }

void OpenWorld::reset_stats() { stats_ = Stats{}; }

bool OpenWorld::save_chunk(WorldChunk& chunk, const ChunkKey& key) {
    if (!saver_) return false;
    const std::vector<u8> bytes = chunk.serialize();
    if (!saver_(key, ConstSpan<const u8>(bytes.data(), bytes.size()))) {
        ++stats_.save_failures;
        return false;
    }
    ++stats_.chunks_saved;
    chunk.clear_dirty();
    return true;
}

void OpenWorld::evict_resident(const ChunkKey& key) {
    const auto found = chunks_.find(key);
    if (found == chunks_.end()) return;
    if (found->second.dirty() && !save_chunk(found->second, key)) return;
    chunks_.erase(found);
    ++stats_.chunks_evicted;
}

bool OpenWorld::make_room(const std::unordered_set<ChunkKey, ChunkKeyHash>& wanted,
                          const ChunkCoord& centre) {
    ChunkKey victim{};
    bool found = false;
    for (const auto& entry : chunks_) {
        if (wanted.contains(entry.first)) continue;   // the load still needs it
        if (entry.second.dirty()) continue;           // an edit is not something to throw away
        if (!found || key_farther(entry.first, victim, origin_, centre)) {
            victim = entry.first;
            found = true;
        }
    }
    if (!found) return false;
    chunks_.erase(victim);
    ++stats_.chunks_evicted;
    return true;
}

// --- OpenWorld: relative coordinates -----------------------------------------

ChunkCoord OpenWorld::chunk_of(i32 relative_x, i32 relative_y) noexcept {
    // i32 floor division by a power of two, spelled with / and % so INT32_MIN is safe too.
    const auto floor_div = [](i32 value) {
        const i32 quotient = value / WorldChunk::kSize;
        if (value % WorldChunk::kSize != 0 && value < 0) return quotient - 1;
        return quotient;
    };
    return ChunkCoord{floor_div(relative_x), floor_div(relative_y)};
}

i32 OpenWorld::local_of(i32 relative_cell) noexcept {
    // The floored remainder *is* the local coordinate: 0..kSize-1, and unlike
    // "cell - floored_chunk * kSize" it cannot overflow at INT32_MIN (that multiply would).
    i32 remainder = relative_cell % WorldChunk::kSize;
    if (remainder < 0) remainder += WorldChunk::kSize;
    return remainder;
}

Aabb2 OpenWorld::chunk_bounds(const ChunkCoord& relative) const {
    // f64 intermediate: relative chunks are i32 and the origin is deliberately not part of this, so
    // the result is a small relative-space AABB the renderer can trust.
    const f64 span = static_cast<f64>(WorldChunk::kSize) * static_cast<f64>(config_.tile_size);
    const f64 x = static_cast<f64>(relative.x) * span;
    const f64 y = static_cast<f64>(relative.y) * span;
    return Aabb2{Vec2{static_cast<f32>(x), static_cast<f32>(y)},
                 Vec2{static_cast<f32>(x + span), static_cast<f32>(y + span)}};
}

Aabb2 OpenWorld::cell_bounds(i32 cell_x, i32 cell_y) const {
    const f64 tile = static_cast<f64>(config_.tile_size);
    const f64 x = static_cast<f64>(cell_x) * tile;
    const f64 y = static_cast<f64>(cell_y) * tile;
    return Aabb2{Vec2{static_cast<f32>(x), static_cast<f32>(y)},
                 Vec2{static_cast<f32>(x + tile), static_cast<f32>(y + tile)}};
}

} // namespace t2d
