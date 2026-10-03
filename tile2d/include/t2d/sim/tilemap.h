// Tile2D - the tile map: storage, queries, collision resolution and serialisation.
//
// Storage is chunked (kChunkSize x kChunkSize) so that big maps stay cheap to allocate and dirty
// regions can be tracked later; every query is deterministic and allocation free.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/sim/tileset.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace t2d {

class TileMap {
public:
    static constexpr i32 kChunkSize = 32;

    struct MoveResult {
        Vec2 position{};
        Vec2 velocity{};
        bool hit_x = false;
        bool hit_y = false;
        bool on_ground = false;
        bool hit_ceiling = false;
        bool on_one_way = false;
    };

    TileMap() = default;
    TileMap(i32 width, i32 height, f32 tile_size = 16.0f, bool out_of_bounds_solid = true);

    [[nodiscard]] i32 width() const;
    [[nodiscard]] i32 height() const;
    [[nodiscard]] f32 tile_size() const;
    [[nodiscard]] bool out_of_bounds_solid() const;
    [[nodiscard]] Vec2 world_size() const;
    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool in_bounds(i32 x, i32 y) const;
    [[nodiscard]] TileId at(i32 x, i32 y) const;        ///< out of bounds -> kEmptyTile
    [[nodiscard]] TileId at_clamped(i32 x, i32 y) const;
    void set(i32 x, i32 y, TileId id);                   ///< out of bounds is ignored
    void fill(TileId id);
    void fill_rect(const TileRect& rect, TileId id);

    // --- coordinate helpers -------------------------------------------------
    [[nodiscard]] Vec2 tile_to_world(i32 x, i32 y) const;      ///< top-left corner of the cell
    [[nodiscard]] Aabb2 tile_bounds(i32 x, i32 y) const;
    [[nodiscard]] TileRect world_to_tiles(const Aabb2& box) const;   ///< cells touched by the box
    [[nodiscard]] TileRect visible_tiles(const Aabb2& view) const;   ///< for renderers

    // --- queries ------------------------------------------------------------
    [[nodiscard]] bool is_solid(i32 x, i32 y, const Tileset& tileset) const;
    [[nodiscard]] bool is_one_way(i32 x, i32 y, const Tileset& tileset) const;
    [[nodiscard]] bool is_hazard(i32 x, i32 y, const Tileset& tileset) const;
    [[nodiscard]] bool overlaps_solid(const Aabb2& box, const Tileset& tileset) const;
    [[nodiscard]] bool overlaps_hazard(const Aabb2& box, const Tileset& tileset) const;
    /// True when the box rests on a solid or one-way tile (1 pixel probe below the box).
    [[nodiscard]] bool is_on_ground(const Aabb2& box, const Tileset& tileset) const;
    /// First solid cell along a ray (used by line of sight / projectile helpers).
    [[nodiscard]] std::optional<Vec2> raycast_solid(Vec2 origin, Vec2 direction, f32 max_distance,
                                                   const Tileset& tileset) const;

    // --- movement -----------------------------------------------------------
    /// Moves \p box by (velocity * delta_seconds) resolving tile collisions. The box is the entity
    /// bounding box; the returned position is the resolved one. One-way platforms only stop a
    /// downward move when the box was above them before the step, unless \p drop_through is set.
    [[nodiscard]] MoveResult move_aabb(const Aabb2& box, Vec2 velocity, f32 delta_seconds, const Tileset& tileset,
                                       bool drop_through = false) const;

    // --- serialisation ------------------------------------------------------
    /// RLE compressed: "varint run length + u16 tile id" pairs in row major order.
    [[nodiscard]] std::vector<u8> serialize() const;
    [[nodiscard]] static std::optional<TileMap> deserialize(ConstSpan<const u8> data);
    [[nodiscard]] u64 checksum() const;
    [[nodiscard]] usize count_tiles(TileId id) const;

    // --- authoring ----------------------------------------------------------
    /// Builds a map from ASCII art, one character per cell; characters missing from the legend
    /// become empty tiles. Throws away nothing: the result is optional so malformed input (ragged
    /// rows, zero size) can be rejected.
    [[nodiscard]] static std::optional<TileMap> from_ascii(ConstSpan<const std::string> rows,
                                                           const std::unordered_map<char, TileId>& legend,
                                                           f32 tile_size = 16.0f);
    [[nodiscard]] std::vector<std::string> to_ascii(const std::unordered_map<TileId, char>& legend,
                                                    char unknown = '?') const;
    /// The legend used by from_ascii()/to_ascii() for the built-in tileset
    /// ('.' empty, '#' stone, '=' one-way, '^' spikes, 'o' coin, ':' background, 'H' ladder, '~' water, 'C' crate).
    [[nodiscard]] static const std::unordered_map<char, TileId>& default_legend();
    [[nodiscard]] static const std::unordered_map<TileId, char>& default_reverse_legend();

private:
    struct Chunk {
        std::vector<TileId> tiles;   ///< kChunkSize * kChunkSize
    };

    [[nodiscard]] u32 chunk_index(i32 chunk_x, i32 chunk_y) const;
    [[nodiscard]] Chunk* chunk_at(i32 chunk_x, i32 chunk_y);
    [[nodiscard]] const Chunk* chunk_at(i32 chunk_x, i32 chunk_y) const;

    i32 width_ = 0;
    i32 height_ = 0;
    i32 chunks_x_ = 0;
    i32 chunks_y_ = 0;
    f32 tile_size_ = 16.0f;
    bool out_of_bounds_solid_ = true;
    std::vector<Chunk> chunks_;
};

} // namespace t2d
