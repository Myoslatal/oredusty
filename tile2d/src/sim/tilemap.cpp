// Tile2D - the tile map: chunked storage, queries, collision resolution and serialisation.
//
// Storage is chunked (kChunkSize x kChunkSize) and allocated lazily: a 4096 x 4096 map costs one
// empty vector per chunk (about 384 KiB of headers) until something is written into that chunk.
//
// Collision contract (move_aabb): the box keeps its size, Result::position is its new top-left
// corner and Result::velocity has the blocked axis components zeroed. The box is moved one axis at
// a time, each axis is split into sub-steps of at most half a tile so a fast entity cannot tunnel
// through a one tile thick floor, and every snap nudges the position by at most a few ulps until
// the *reconstructed* box (position + size - the same arithmetic a caller and overlaps_solid()
// use) no longer reaches into the tile it hit. That keeps "landed on the tile top" exact while
// making the "never overlaps a solid tile" invariant hold bit for bit. Nothing here allocates.
#include <t2d/sim/tilemap.h>

#include <t2d/core/bitstream.h>
#include <t2d/core/log.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace t2d {
namespace {

/// Snap passes per axis and sub-step: one sub-step can enter at most one extra row/column.
constexpr i32 kMaxSnapIterations = 8;
/// Single ulp nudges used to make a snapped position clear the tile it hit exactly.
constexpr i32 kMaxFitNudges = 8;
/// Sub-step size relative to a tile; half a tile keeps a one tile thick floor from being skipped.
constexpr f32 kSubStepFraction = 0.5f;
/// Hard cap for absurd deltas: 16384 sub-steps cover 131072 px per call at a 16 px tile size.
constexpr i32 kMaxSubSteps = 1 << 14;
/// A one-way tile only stops a box whose bottom edge was at or above its top edge. This absorbs
/// float noise while staying far below a pixel so a slow drop-through is not caught again.
constexpr f32 kOneWayTolerance = 0.001f;
/// Grid traversal budget for raycast_solid() across a void that never turns solid.
constexpr i32 kMaxRaySteps = 1 << 20;
/// Serialisation limits: dimensions and the cell count are varints, 4096 x 4096 (32 MiB of tiles)
/// is the biggest map deserialize() hands back.
constexpr u32 kMaxDimension = 1u << 16;
constexpr u64 kMaxCells = 1ull << 24;
constexpr u8 kFormatVersion = 1;
constexpr u8 kFlagOutOfBoundsSolid = 1u << 0;

constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime = 0x100000001B3ull;

[[nodiscard]] constexpr u64 fnv_mix(u64 hash, u64 value, i32 byte_count) {
    for (i32 index = 0; index < byte_count; ++index) {
        hash = (hash ^ ((value >> (index * 8)) & 0xFFu)) * kFnvPrime;
    }
    return hash;
}

/// Cells a box touches: floor on the low side, ceil on the high side, so a box whose edge lies
/// exactly on a tile boundary does not touch the next cell (Aabb2::overlaps() semantics). Negative
/// cells are kept: callers decide what an out of bounds cell means.
[[nodiscard]] TileRect box_cells(const Aabb2& box, f32 tile_size) {
    const i32 x0 = floor_to_i32(box.min.x / tile_size);
    const i32 y0 = floor_to_i32(box.min.y / tile_size);
    // A box with zero extent (a point, or a collapsed rectangle) still touches exactly one cell.
    const i32 x1 = std::max(ceil_to_i32(box.max.x / tile_size), x0 + 1);
    const i32 y1 = std::max(ceil_to_i32(box.max.y / tile_size), y0 + 1);
    return TileRect{x0, y0, x1 - x0, y1 - y0};
}

} // namespace

// --- construction and accessors ---------------------------------------------

TileMap::TileMap(i32 width, i32 height, f32 tile_size, bool out_of_bounds_solid)
    : width_(width > 0 ? width : 0), height_(height > 0 ? height : 0),
      tile_size_((tile_size > 0.0f && std::isfinite(tile_size)) ? tile_size : 16.0f),
      out_of_bounds_solid_(out_of_bounds_solid) {
    chunks_x_ = (width_ + kChunkSize - 1) / kChunkSize;
    chunks_y_ = (height_ + kChunkSize - 1) / kChunkSize;
    // Chunk headers only: the tile storage of every chunk stays unallocated until it is written.
    chunks_.resize(static_cast<usize>(chunks_x_) * static_cast<usize>(chunks_y_));
}

i32 TileMap::width() const { return width_; }
i32 TileMap::height() const { return height_; }
f32 TileMap::tile_size() const { return tile_size_; }
bool TileMap::out_of_bounds_solid() const { return out_of_bounds_solid_; }

Vec2 TileMap::world_size() const {
    return Vec2{static_cast<f32>(width_) * tile_size_, static_cast<f32>(height_) * tile_size_};
}

bool TileMap::empty() const { return width_ <= 0 || height_ <= 0; }

bool TileMap::in_bounds(i32 x, i32 y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }

u32 TileMap::chunk_index(i32 chunk_x, i32 chunk_y) const {
    T2D_ASSERT(chunk_x >= 0 && chunk_x < chunks_x_ && chunk_y >= 0 && chunk_y < chunks_y_);
    return static_cast<u32>(chunk_y * chunks_x_ + chunk_x);
}

TileMap::Chunk* TileMap::chunk_at(i32 chunk_x, i32 chunk_y) {
    // nullptr covers both "outside the map" and "never written to": an unallocated chunk reads as
    // empty tiles, which is what keeps a large map cheap.
    if (chunk_x < 0 || chunk_x >= chunks_x_ || chunk_y < 0 || chunk_y >= chunks_y_) return nullptr;
    Chunk& chunk = chunks_[chunk_index(chunk_x, chunk_y)];
    return chunk.tiles.empty() ? nullptr : &chunk;
}

const TileMap::Chunk* TileMap::chunk_at(i32 chunk_x, i32 chunk_y) const {
    if (chunk_x < 0 || chunk_x >= chunks_x_ || chunk_y < 0 || chunk_y >= chunks_y_) return nullptr;
    const Chunk& chunk = chunks_[chunk_index(chunk_x, chunk_y)];
    return chunk.tiles.empty() ? nullptr : &chunk;
}

TileId TileMap::at(i32 x, i32 y) const {
    if (!in_bounds(x, y)) return kEmptyTile;
    const i32 chunk_x = x / kChunkSize;
    const i32 chunk_y = y / kChunkSize;
    const Chunk* chunk = chunk_at(chunk_x, chunk_y);
    if (chunk == nullptr) return kEmptyTile;
    const i32 local_x = x - chunk_x * kChunkSize;
    const i32 local_y = y - chunk_y * kChunkSize;
    return chunk->tiles[static_cast<usize>(local_y) * kChunkSize + local_x];
}

TileId TileMap::at_clamped(i32 x, i32 y) const {
    if (width_ <= 0 || height_ <= 0) return kEmptyTile;
    return at(clamp_i32(x, 0, width_ - 1), clamp_i32(y, 0, height_ - 1));
}

void TileMap::set(i32 x, i32 y, TileId id) {
    if (!in_bounds(x, y)) return;
    const i32 chunk_x = x / kChunkSize;
    const i32 chunk_y = y / kChunkSize;
    Chunk* chunk = chunk_at(chunk_x, chunk_y);
    if (chunk == nullptr) {
        if (id == kEmptyTile) return;   // writing empty into a missing chunk is a no-op: stay lazy
        chunk = &chunks_[chunk_index(chunk_x, chunk_y)];
        chunk->tiles.assign(static_cast<usize>(kChunkSize) * kChunkSize, kEmptyTile);
    }
    const i32 local_x = x - chunk_x * kChunkSize;
    const i32 local_y = y - chunk_y * kChunkSize;
    chunk->tiles[static_cast<usize>(local_y) * kChunkSize + local_x] = id;
}

void TileMap::fill(TileId id) {
    if (id == kEmptyTile) {
        // Release the storage entirely: an empty map must not hold a single chunk.
        for (Chunk& chunk : chunks_) std::vector<TileId>().swap(chunk.tiles);
        return;
    }
    // Only cells inside the map may be written: the padding of the last chunk row/column is not
    // part of the map and must stay empty (count_tiles() and checksum() ignore it).
    fill_rect(TileRect{0, 0, width_, height_}, id);
}

void TileMap::fill_rect(const TileRect& rect, TileId id) {
    if (width_ <= 0 || height_ <= 0) return;
    const i32 x0 = std::max(rect.x, 0);
    const i32 y0 = std::max(rect.y, 0);
    const i32 x1 = std::min(rect.right(), width_);
    const i32 y1 = std::min(rect.bottom(), height_);
    if (x0 >= x1 || y0 >= y1) return;
    // Walk chunk blocks so a big fill does not re-resolve the chunk for every single cell.
    for (i32 chunk_y = y0 / kChunkSize; chunk_y <= (y1 - 1) / kChunkSize; ++chunk_y) {
        const i32 chunk_top = chunk_y * kChunkSize;
        const i32 local_y0 = std::max(y0, chunk_top) - chunk_top;
        const i32 local_y1 = std::min(y1, chunk_top + kChunkSize) - chunk_top;
        for (i32 chunk_x = x0 / kChunkSize; chunk_x <= (x1 - 1) / kChunkSize; ++chunk_x) {
            const i32 chunk_left = chunk_x * kChunkSize;
            const i32 local_x0 = std::max(x0, chunk_left) - chunk_left;
            const i32 local_x1 = std::min(x1, chunk_left + kChunkSize) - chunk_left;
            Chunk* chunk = chunk_at(chunk_x, chunk_y);
            if (chunk == nullptr) {
                if (id == kEmptyTile) continue;   // clearing an unallocated chunk is a no-op
                chunk = &chunks_[chunk_index(chunk_x, chunk_y)];
                chunk->tiles.assign(static_cast<usize>(kChunkSize) * kChunkSize, kEmptyTile);
            }
            for (i32 local_y = local_y0; local_y < local_y1; ++local_y) {
                TileId* row = chunk->tiles.data() + static_cast<usize>(local_y) * kChunkSize;
                for (i32 local_x = local_x0; local_x < local_x1; ++local_x) row[local_x] = id;
            }
        }
    }
}

usize TileMap::count_tiles(TileId id) const {
    // Chunks cover whole 32 x 32 blocks, so the last chunk row/column holds cells outside the map.
    // Those are never written (see fill()) and must never be counted either.
    usize matched = 0;
    usize filled = 0;
    for (i32 chunk_y = 0; chunk_y < chunks_y_; ++chunk_y) {
        const i32 row_end = std::min((chunk_y + 1) * kChunkSize, height_);
        for (i32 chunk_x = 0; chunk_x < chunks_x_; ++chunk_x) {
            const Chunk* chunk = chunk_at(chunk_x, chunk_y);
            if (chunk == nullptr || chunk->tiles.empty()) continue;
            const i32 column_end = std::min((chunk_x + 1) * kChunkSize, width_);
            for (i32 y = chunk_y * kChunkSize; y < row_end; ++y) {
                const usize local_y = static_cast<usize>(y - chunk_y * kChunkSize);
                for (i32 x = chunk_x * kChunkSize; x < column_end; ++x) {
                    const usize local_x = static_cast<usize>(x - chunk_x * kChunkSize);
                    const TileId tile = chunk->tiles[local_y * kChunkSize + local_x];
                    if (tile != kEmptyTile) ++filled;
                    if (tile == id) ++matched;
                }
            }
        }
    }
    if (id == kEmptyTile) {
        const usize cells = static_cast<usize>(width_) * static_cast<usize>(height_);
        return cells > filled ? cells - filled : 0;
    }
    return matched;
}

// --- coordinate helpers -----------------------------------------------------

Vec2 TileMap::tile_to_world(i32 x, i32 y) const {
    return Vec2{static_cast<f32>(x) * tile_size_, static_cast<f32>(y) * tile_size_};
}

Aabb2 TileMap::tile_bounds(i32 x, i32 y) const {
    const Vec2 min = tile_to_world(x, y);
    return Aabb2{min, Vec2{min.x + tile_size_, min.y + tile_size_}};
}

TileRect TileMap::world_to_tiles(const Aabb2& box) const { return box_cells(box, tile_size_); }

TileRect TileMap::visible_tiles(const Aabb2& view) const {
    const TileRect cells = box_cells(view, tile_size_);
    const i32 x0 = std::max(cells.x, 0);
    const i32 y0 = std::max(cells.y, 0);
    const i32 x1 = std::min(cells.right(), width_);
    const i32 y1 = std::min(cells.bottom(), height_);
    if (x1 <= x0 || y1 <= y0) return TileRect{x0, y0, 0, 0};
    return TileRect{x0, y0, x1 - x0, y1 - y0};
}

// --- queries ----------------------------------------------------------------

bool TileMap::is_solid(i32 x, i32 y, const Tileset& tileset) const {
    if (!in_bounds(x, y)) return out_of_bounds_solid_;   // walls around the level by default
    return tileset.is_solid(at(x, y));
}

bool TileMap::is_one_way(i32 x, i32 y, const Tileset& tileset) const {
    return in_bounds(x, y) && tileset.is_one_way(at(x, y));
}

bool TileMap::is_hazard(i32 x, i32 y, const Tileset& tileset) const {
    return in_bounds(x, y) && tileset.is_hazard(at(x, y));
}

bool TileMap::overlaps_solid(const Aabb2& box, const Tileset& tileset) const {
    const Vec2 world = world_size();
    if (out_of_bounds_solid_ &&
        (box.min.x < 0.0f || box.min.y < 0.0f || box.max.x > world.x || box.max.y > world.y)) {
        return true;
    }
    const TileRect cells = box_cells(box, tile_size_);
    const i32 x0 = std::max(cells.x, 0);
    const i32 y0 = std::max(cells.y, 0);
    const i32 x1 = std::min(cells.right(), width_);
    const i32 y1 = std::min(cells.bottom(), height_);
    for (i32 y = y0; y < y1; ++y) {
        for (i32 x = x0; x < x1; ++x) {
            if (tileset.is_solid(at(x, y))) return true;
        }
    }
    return false;
}

bool TileMap::overlaps_hazard(const Aabb2& box, const Tileset& tileset) const {
    const TileRect cells = box_cells(box, tile_size_);
    const i32 x0 = std::max(cells.x, 0);
    const i32 y0 = std::max(cells.y, 0);
    const i32 x1 = std::min(cells.right(), width_);
    const i32 y1 = std::min(cells.bottom(), height_);
    for (i32 y = y0; y < y1; ++y) {
        for (i32 x = x0; x < x1; ++x) {
            if (tileset.is_hazard(at(x, y))) return true;
        }
    }
    return false;
}

bool TileMap::is_on_ground(const Aabb2& box, const Tileset& tileset) const {
    // One pixel probe below the box, in the same "cells touched" convention the rest of the file
    // uses: a box resting on a tile has its bottom edge just above or exactly on the tile top.
    const Aabb2 probe{Vec2{box.min.x, box.max.y}, Vec2{box.max.x, box.max.y + 1.0f}};
    const Vec2 world = world_size();
    if (out_of_bounds_solid_ &&
        (probe.min.x < 0.0f || probe.min.y < 0.0f || probe.max.x > world.x || probe.max.y > world.y)) {
        return true;
    }
    const TileRect cells = box_cells(probe, tile_size_);
    const i32 x0 = std::max(cells.x, 0);
    const i32 y0 = std::max(cells.y, 0);
    const i32 x1 = std::min(cells.right(), width_);
    const i32 y1 = std::min(cells.bottom(), height_);
    for (i32 y = y0; y < y1; ++y) {
        for (i32 x = x0; x < x1; ++x) {
            if (tileset.blocks_falling(at(x, y))) return true;
        }
    }
    return false;
}

std::optional<Vec2> TileMap::raycast_solid(Vec2 origin, Vec2 direction, f32 max_distance,
                                           const Tileset& tileset) const {
    if (max_distance < 0.0f) return std::nullopt;
    const Vec2 heading = normalize(direction);
    if (heading.x == 0.0f && heading.y == 0.0f) return std::nullopt;

    i32 cell_x = floor_to_i32(origin.x / tile_size_);
    i32 cell_y = floor_to_i32(origin.y / tile_size_);
    if (is_solid(cell_x, cell_y, tileset)) return origin;   // already inside a wall

    const f32 infinity = std::numeric_limits<f32>::infinity();
    const i32 step_x = heading.x > 0.0f ? 1 : (heading.x < 0.0f ? -1 : 0);
    const i32 step_y = heading.y > 0.0f ? 1 : (heading.y < 0.0f ? -1 : 0);
    const f32 delta_x = step_x != 0 ? tile_size_ / abs_f32(heading.x) : infinity;
    const f32 delta_y = step_y != 0 ? tile_size_ / abs_f32(heading.y) : infinity;
    // Distance along the ray to the next vertical/horizontal grid line. The origin always lies
    // inside [cell * tile, (cell + 1) * tile), so both are >= 0 (one may be exactly 0 when the ray
    // leaves a boundary towards the lower cell).
    const f32 next_x = step_x > 0 ? static_cast<f32>(cell_x + 1) * tile_size_
                                   : (step_x < 0 ? static_cast<f32>(cell_x) * tile_size_ : infinity);
    const f32 next_y = step_y > 0 ? static_cast<f32>(cell_y + 1) * tile_size_
                                   : (step_y < 0 ? static_cast<f32>(cell_y) * tile_size_ : infinity);
    f32 travel_x = step_x != 0 ? (next_x - origin.x) / heading.x : infinity;
    f32 travel_y = step_y != 0 ? (next_y - origin.y) / heading.y : infinity;

    f32 distance = 0.0f;
    for (i32 step_index = 0; step_index < kMaxRaySteps; ++step_index) {
        if (travel_x < travel_y) {
            distance = travel_x;
            cell_x += step_x;
            travel_x += delta_x;
        } else {
            distance = travel_y;
            cell_y += step_y;
            travel_y += delta_y;
        }
        if (distance > max_distance) break;
        if (is_solid(cell_x, cell_y, tileset)) return origin + heading * distance;
    }
    return std::nullopt;
}

// --- movement ---------------------------------------------------------------

TileMap::MoveResult TileMap::move_aabb(const Aabb2& box, Vec2 velocity, f32 delta_seconds,
                                       const Tileset& tileset, bool drop_through) const {
    MoveResult result;
    result.position = box.min;   // top-left corner, the box keeps its size
    result.velocity = velocity;
    const Vec2 size = box.size();
    T2D_ASSERT(size.x >= 0.0f && size.y >= 0.0f);

    // A map without cells is a void (free movement) or, with the default walls, entirely solid.
    if (width_ <= 0 || height_ <= 0) {
        if (out_of_bounds_solid_) {
            result.velocity = Vec2{};
            result.hit_x = true;
            result.hit_y = true;
            result.on_ground = true;
            return result;
        }
        result.position = box.min + velocity * delta_seconds;
        return result;
    }

    const f32 tile = tile_size_;
    const f32 world_right = static_cast<f32>(width_) * tile;
    const f32 world_bottom = static_cast<f32>(height_) * tile;

    // Cells of a probe box, clamped to the map: cells outside are handled by is_solid() (which
    // honours out_of_bounds_solid()) and the explicit wall checks, so no loop walks an unbounded
    // region.
    const auto clamped_cells = [&](const Aabb2& probe) {
        const TileRect cells = box_cells(probe, tile);
        const i32 x0 = std::max(cells.x, 0);
        const i32 y0 = std::max(cells.y, 0);
        const i32 x1 = std::min(cells.right(), width_);
        const i32 y1 = std::min(cells.bottom(), height_);
        return TileRect{x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    };

    // Places an axis coordinate so the reconstructed box clears the limit: at or below it when the
    // move is positive (right/down), at or above it otherwise. A few single ulp nudges are enough
    // because the candidate is already within one rounding error of the limit.
    const auto fit = [&](bool axis_x, bool positive, f32 limit) {
        const f32 extent = axis_x ? size.x : size.y;
        f32 value = positive ? (limit - extent) : limit;
        for (i32 attempt = 0; attempt < kMaxFitNudges; ++attempt) {
            const Aabb2 probe = Aabb2::from_top_left(
                axis_x ? Vec2{value, result.position.y} : Vec2{result.position.x, value}, size);
            const f32 reached = axis_x ? (positive ? probe.max.x : probe.min.x)
                                       : (positive ? probe.max.y : probe.min.y);
            if (positive ? (reached <= limit) : (reached >= limit)) break;
            value = std::nextafter(value, positive ? -std::numeric_limits<f32>::infinity()
                                                   : std::numeric_limits<f32>::infinity());
        }
        return value;
    };

    // What the box would rest on with the same one pixel probe is_on_ground() uses: 0 nothing,
    // 1 solid, 2 one-way.
    const auto support = [&](const Aabb2& settled) {
        const Aabb2 probe{Vec2{settled.min.x, settled.max.y},
                          Vec2{settled.max.x, settled.max.y + 1.0f}};
        if (out_of_bounds_solid_ &&
            (probe.min.x < 0.0f || probe.min.y < 0.0f || probe.max.x > world_right ||
             probe.max.y > world_bottom)) {
            return static_cast<u8>(1);
        }
        const TileRect cells = clamped_cells(probe);
        u8 kind = 0;
        for (i32 y = cells.y; y < cells.bottom(); ++y) {
            for (i32 x = cells.x; x < cells.right(); ++x) {
                const TileId id = at(x, y);
                if (tileset.is_solid(id)) return static_cast<u8>(1);
                if (tileset.is_one_way(id)) kind = 2;
            }
        }
        return kind;
    };

    // Kind of the tile that produced the current snap (0 none, 1 solid, 2 one-way); move_y() uses
    // it to report on_one_way.
    u8 blocker_kind = 0;

    // Moves one axis and snaps the box out of every cell the blocker reports. Returns true when the
    // axis was blocked.
    const auto resolve_axis = [&](bool axis_x, f32 amount, auto&& blocker) {
        const bool positive = amount > 0.0f;
        if (axis_x) {
            result.position.x += amount;
        } else {
            result.position.y += amount;
        }
        bool blocked = false;
        for (i32 iteration = 0; iteration < kMaxSnapIterations; ++iteration) {
            const Aabb2 probe = Aabb2::from_top_left(result.position, size);
            const TileRect cells = clamped_cells(probe);
            f32 limit = 0.0f;
            bool found = false;
            // The extreme blocking edge is the one the box has to be pushed back to.
            const auto consider = [&](f32 edge, u8 kind) {
                if (!found || (positive ? edge < limit : edge > limit)) {
                    limit = edge;
                    blocker_kind = kind;
                    found = true;
                }
            };
            for (i32 y = cells.y; y < cells.bottom(); ++y) {
                for (i32 x = cells.x; x < cells.right(); ++x) {
                    const u8 kind = blocker(x, y);
                    if (kind == 0u) continue;
                    const i32 line = axis_x ? x : y;
                    consider(static_cast<f32>(positive ? line : line + 1) * tile, kind);
                }
            }
            if (out_of_bounds_solid_) {
                const f32 reached = axis_x ? (positive ? probe.max.x : probe.min.x)
                                           : (positive ? probe.max.y : probe.min.y);
                const f32 wall = axis_x ? (positive ? world_right : 0.0f)
                                        : (positive ? world_bottom : 0.0f);
                if (positive ? (reached > wall) : (reached < wall)) consider(wall, 1u);
            }
            if (!found) break;
            blocked = true;
            const f32 snapped = fit(axis_x, positive, limit);
            const f32 current = axis_x ? result.position.x : result.position.y;
            if (snapped == current) break;   // fixed point: the box cannot be pushed out further
            if (axis_x) {
                result.position.x = snapped;
            } else {
                result.position.y = snapped;
            }
        }
        return blocked;
    };

    const auto move_x = [&](f32 amount) {
        const bool blocked = resolve_axis(true, amount, [&](i32 x, i32 y) -> u8 {
            return is_solid(x, y, tileset) ? 1u : 0u;
        });
        if (blocked) {
            result.velocity.x = 0.0f;
            result.hit_x = true;
        }
        return blocked;
    };

    const auto move_y = [&](f32 amount) {
        const f32 previous_bottom = result.position.y + size.y;
        const bool falling = amount > 0.0f;
        const bool blocked = resolve_axis(false, amount, [&](i32 x, i32 y) -> u8 {
            const TileId id = at(x, y);
            if (tileset.is_solid(id)) return 1u;
            if (!falling || drop_through || !tileset.is_one_way(id)) return 0u;
            // Only a box that was at or above the platform top edge before the step is stopped.
            if (previous_bottom > static_cast<f32>(y) * tile + kOneWayTolerance) return 0u;
            return 2u;
        });
        if (blocked) {
            result.velocity.y = 0.0f;
            result.hit_y = true;
            if (falling) {
                result.on_ground = true;
                result.on_one_way = blocker_kind == 2u;
            } else {
                result.hit_ceiling = true;
            }
        }
        return blocked;
    };

    // Sub-stepping: at most half a tile per step, so a 600 px/s entity at 60 Hz (10 px) and a
    // deliberately huge dt (150 px at 0.25 s) both stop on a one tile thick floor.
    const Vec2 delta = velocity * delta_seconds;
    const f32 largest = std::max(abs_f32(delta.x), abs_f32(delta.y));
    const f32 step_limit = tile * kSubStepFraction;
    i32 steps = 1;
    if (largest > step_limit) {
        const f32 wanted = std::ceil(largest / step_limit);
        steps = wanted >= static_cast<f32>(kMaxSubSteps) ? kMaxSubSteps : static_cast<i32>(wanted);
        if (steps < 1) steps = 1;
    }
    const Vec2 step = delta / static_cast<f32>(steps);
    f32 step_x = step.x;
    f32 step_y = step.y;
    for (i32 index = 0; index < steps; ++index) {
        if (step_x != 0.0f && move_x(step_x)) step_x = 0.0f;
        if (step_y != 0.0f && move_y(step_y)) step_y = 0.0f;
        if (step_x == 0.0f && step_y == 0.0f) break;
    }

    // A move that left no vertical velocity (nothing fell: a resting entity, a zero delta or a
    // ceiling bump) still reports what the box rests on. A falling or rising box is airborne by
    // definition - probing it would report the platform it is currently dropping through.
    if (!result.on_ground && result.velocity.y == 0.0f) {
        const u8 kind = support(Aabb2::from_top_left(result.position, size));
        result.on_ground = kind != 0u;
        result.on_one_way = kind == 2u;
    }
    return result;
}

// --- serialisation ----------------------------------------------------------
//
// Layout (little endian, varints are LEB128 like ByteWriter::write_varint):
//   "T2DM" magic | u8 version | u8 flags (bit 0: out_of_bounds_solid) | varint width | varint height
//   | f32 tile_size | varint cell_count | (varint run_length | u16 tile_id)*  in row major order.

std::vector<u8> TileMap::serialize() const {
    std::vector<u8> bytes;
    ByteWriter writer(bytes);
    writer.write_u8('T');
    writer.write_u8('2');
    writer.write_u8('D');
    writer.write_u8('M');
    writer.write_u8(kFormatVersion);
    writer.write_u8(out_of_bounds_solid_ ? kFlagOutOfBoundsSolid : 0u);
    writer.write_varint(static_cast<u32>(width_));
    writer.write_varint(static_cast<u32>(height_));
    writer.write_f32(tile_size_);
    writer.write_varint(static_cast<u32>(static_cast<u64>(width_) * static_cast<u64>(height_)));

    u32 run_length = 0;
    TileId run_tile = kEmptyTile;
    const auto emit = [&](TileId tile) {
        if (run_length != 0 && tile == run_tile) {
            ++run_length;
            return;
        }
        if (run_length != 0) {
            writer.write_varint(run_length);
            writer.write_u16(run_tile);
        }
        run_tile = tile;
        run_length = 1;
    };
    // Row major: rows top to bottom, each row left to right, chunk columns in between.
    for (i32 chunk_y = 0; chunk_y < chunks_y_; ++chunk_y) {
        const i32 row_begin = chunk_y * kChunkSize;
        const i32 row_end = std::min(row_begin + kChunkSize, height_);
        for (i32 y = row_begin; y < row_end; ++y) {
            const usize local_y = static_cast<usize>(y - row_begin);
            for (i32 chunk_x = 0; chunk_x < chunks_x_; ++chunk_x) {
                const i32 column_begin = chunk_x * kChunkSize;
                const i32 column_end = std::min(column_begin + kChunkSize, width_);
                const Chunk* chunk = chunk_at(chunk_x, chunk_y);
                if (chunk == nullptr) {
                    for (i32 x = column_begin; x < column_end; ++x) emit(kEmptyTile);
                    continue;
                }
                const TileId* row = chunk->tiles.data() + local_y * kChunkSize;
                for (i32 x = column_begin; x < column_end; ++x) emit(row[x - column_begin]);
            }
        }
    }
    if (run_length != 0) {
        writer.write_varint(run_length);
        writer.write_u16(run_tile);
    }
    return bytes;
}

std::optional<TileMap> TileMap::deserialize(ConstSpan<const u8> data) {
    ByteReader reader(data);
    if (reader.read_u8() != 'T' || reader.read_u8() != '2' || reader.read_u8() != 'D' ||
        reader.read_u8() != 'M') {
        return std::nullopt;
    }
    if (reader.read_u8() != kFormatVersion) return std::nullopt;
    const u8 flags = reader.read_u8();
    if ((flags & ~kFlagOutOfBoundsSolid) != 0u) return std::nullopt;
    const u32 width = reader.read_varint();
    const u32 height = reader.read_varint();
    const f32 tile_size = reader.read_f32();
    const u32 cells = reader.read_varint();
    if (!reader.ok() || !std::isfinite(tile_size) || tile_size <= 0.0f) return std::nullopt;
    if (width > kMaxDimension || height > kMaxDimension) return std::nullopt;
    if (static_cast<u64>(width) * static_cast<u64>(height) != static_cast<u64>(cells)) {
        return std::nullopt;
    }
    if (static_cast<u64>(cells) > kMaxCells) return std::nullopt;

    TileMap map(static_cast<i32>(width), static_cast<i32>(height), tile_size,
                (flags & kFlagOutOfBoundsSolid) != 0u);
    // Runs are written per row so an empty run does not allocate the chunks it crosses.
    const auto write_run = [&map](u64 start, u32 count, TileId tile) {
        const u64 line = static_cast<u64>(map.width());
        if (line == 0) return;
        u64 index = start;
        u32 remaining = count;
        while (remaining > 0) {
            const i32 x = static_cast<i32>(index % line);
            const i32 y = static_cast<i32>(index / line);
            const u32 span = std::min<u32>(remaining, static_cast<u32>(line - static_cast<u64>(x)));
            map.fill_rect(TileRect{x, y, static_cast<i32>(span), 1}, tile);
            index += span;
            remaining -= span;
        }
    };

    u64 index = 0;
    while (index < static_cast<u64>(cells)) {
        const u32 run_length = reader.read_varint();
        const u16 tile = reader.read_u16();
        if (!reader.ok()) return std::nullopt;
        if (run_length == 0u) return std::nullopt;                                // malformed run
        if (static_cast<u64>(run_length) > static_cast<u64>(cells) - index) {
            return std::nullopt;                                                  // too many cells
        }
        write_run(index, run_length, tile);
        index += run_length;
    }
    if (!reader.empty()) return std::nullopt;   // trailing bytes: not a clean map blob
    return map;
}

u64 TileMap::checksum() const {
    // FNV-1a over the dimensions and then every tile id in row major order, little endian.
    u64 hash = kFnvOffsetBasis;
    hash = fnv_mix(hash, static_cast<u64>(static_cast<u32>(width_)), 4);
    hash = fnv_mix(hash, static_cast<u64>(static_cast<u32>(height_)), 4);
    for (i32 chunk_y = 0; chunk_y < chunks_y_; ++chunk_y) {
        const i32 row_begin = chunk_y * kChunkSize;
        const i32 row_end = std::min(row_begin + kChunkSize, height_);
        for (i32 y = row_begin; y < row_end; ++y) {
            const usize local_y = static_cast<usize>(y - row_begin);
            for (i32 chunk_x = 0; chunk_x < chunks_x_; ++chunk_x) {
                const i32 column_begin = chunk_x * kChunkSize;
                const i32 column_end = std::min(column_begin + kChunkSize, width_);
                const Chunk* chunk = chunk_at(chunk_x, chunk_y);
                const TileId* row =
                    chunk != nullptr ? chunk->tiles.data() + local_y * kChunkSize : nullptr;
                for (i32 x = column_begin; x < column_end; ++x) {
                    hash = fnv_mix(hash, row != nullptr ? row[x - column_begin] : kEmptyTile, 2);
                }
            }
        }
    }
    return hash;
}

// --- authoring --------------------------------------------------------------

std::optional<TileMap> TileMap::from_ascii(ConstSpan<const std::string> rows,
                                           const std::unordered_map<char, TileId>& legend,
                                           f32 tile_size) {
    if (rows.empty()) return std::nullopt;
    usize width = 0;
    for (const std::string& row : rows) width = std::max(width, row.size());
    if (width == 0) return std::nullopt;                       // zero sized map
    for (const std::string& row : rows) {
        if (row.size() != width) return std::nullopt;          // ragged: every row must be as wide
    }
    if (!std::isfinite(tile_size) || tile_size <= 0.0f) return std::nullopt;
    if (width > kMaxDimension || rows.size() > kMaxDimension) return std::nullopt;
    TileMap map(static_cast<i32>(width), static_cast<i32>(rows.size()), tile_size);
    u32 unknown_count = 0;
    std::string unknown_characters;
    for (usize y = 0; y < rows.size(); ++y) {
        const std::string& row = rows[y];
        for (usize x = 0; x < width; ++x) {
            const auto found = legend.find(row[x]);
            if (found == legend.end()) {
                // Unknown characters stay empty, but silently dropping them once cost the demo level
                // its entire ground: report them, with the first position where one appears.
                ++unknown_count;
                if (unknown_characters.size() < 8 && unknown_characters.find(row[x]) == std::string::npos) {
                    unknown_characters.push_back(row[x]);
                }
                if (unknown_count == 1) {
                    T2D_WARN("level: unknown tile character '{}' at ({}, {}) is left empty", row[x],
                             static_cast<i32>(x), static_cast<i32>(y));
                }
                continue;
            }
            map.set(static_cast<i32>(x), static_cast<i32>(y), found->second);
        }
    }
    if (unknown_count > 0) {
        T2D_WARN("level: {} cell(s) used unknown character(s) '{}' and were left empty", unknown_count,
                 unknown_characters);
    }
    return map;
}

std::vector<std::string> TileMap::to_ascii(const std::unordered_map<TileId, char>& legend,
                                           char unknown) const {
    std::vector<std::string> rows;
    if (width_ <= 0 || height_ <= 0) return rows;
    rows.reserve(static_cast<usize>(height_));
    for (i32 y = 0; y < height_; ++y) {
        std::string row(static_cast<usize>(width_), unknown);
        for (i32 x = 0; x < width_; ++x) {
            const auto found = legend.find(at(x, y));
            if (found == legend.end()) continue;               // no character: keep unknown
            row[static_cast<usize>(x)] = found->second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

const std::unordered_map<char, TileId>& TileMap::default_legend() {
    static const std::unordered_map<char, TileId> legend = {
        {'.', kEmptyTile}, {'#', 1}, {'2', 2}, {'3', 3}, {'=', 4}, {'^', 5}, {'o', 6},
        {':', 7}, {'H', 8}, {'~', 9}, {'C', 10},
    };
    return legend;
}

const std::unordered_map<TileId, char>& TileMap::default_reverse_legend() {
    static const std::unordered_map<TileId, char> legend = {
        {kEmptyTile, '.'}, {1, '#'}, {2, '2'}, {3, '3'}, {4, '='}, {5, '^'}, {6, 'o'},
        {7, ':'}, {8, 'H'}, {9, '~'}, {10, 'C'},
    };
    return legend;
}

} // namespace t2d
