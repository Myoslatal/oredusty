// Tile2D - the tile map: storage, queries, collision resolution and serialisation.
//
// Storage is chunked (kChunkSize x kChunkSize) so that big maps stay cheap to allocate and dirty
// regions can be tracked later; every query is deterministic and allocation free.
//
// A map holds one or more *tile layers*: independent grids of the same size, drawn bottom to top.
// What a layer means - ground, ore, structures, logistics - is the game's business, not the map's;
// the map only numbers them from 0 (the base layer) upwards. Collision queries take a LayerMask, so
// a game can say "walls and machines block, the ground layer does not" without the map guessing.
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
    /// How many tile layers one map can hold. Layers cost one chunk header per chunk until they are
    /// written to, so a high layer number is cheap; the cap keeps a mask a single u32.
    static constexpr i32 kMaxLayers = 32;

    /// Which layers a query looks at: bit i is layer i. The default is every layer, which for the
    /// one layer maps this framework started with is exactly the behaviour those callers had.
    using LayerMask = u32;
    static constexpr LayerMask kAllLayers = 0xFFFFFFFFu;
    static constexpr LayerMask kNoLayers = 0u;

    [[nodiscard]] static constexpr LayerMask layer_mask(i32 layer) {
        return (layer >= 0 && layer < kMaxLayers) ? (1u << layer) : 0u;
    }

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
    /// \p layers is clamped to [1, kMaxLayers]. A one layer map is what every existing caller gets.
    TileMap(i32 width, i32 height, f32 tile_size = 16.0f, bool out_of_bounds_solid = true, i32 layers = 1);

    [[nodiscard]] i32 width() const;
    [[nodiscard]] i32 height() const;
    [[nodiscard]] i32 layer_count() const;
    [[nodiscard]] f32 tile_size() const;
    [[nodiscard]] bool out_of_bounds_solid() const;
    [[nodiscard]] Vec2 world_size() const;
    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool in_bounds(i32 x, i32 y) const;
    /// The two argument forms address the base layer (0): they are what a single layer map means by
    /// "the tile here", and keeping them keeps every existing caller unchanged.
    [[nodiscard]] TileId at(i32 x, i32 y) const;              ///< out of bounds -> kEmptyTile
    [[nodiscard]] TileId at(i32 layer, i32 x, i32 y) const;
    [[nodiscard]] TileId at_clamped(i32 x, i32 y) const;
    [[nodiscard]] TileId at_clamped(i32 layer, i32 x, i32 y) const;
    /// Topmost non-empty tile at (x, y) among \p mask, or kEmptyTile: "what is on this cell".
    [[nodiscard]] TileId topmost(i32 x, i32 y, LayerMask mask = kAllLayers) const;
    void set(i32 x, i32 y, TileId id);                        ///< out of bounds is ignored
    void set(i32 layer, i32 x, i32 y, TileId id);
    /// Fills every layer (a map wide clear is what an unqualified fill has always meant).
    void fill(TileId id);
    void fill(i32 layer, TileId id);
    void fill_rect(const TileRect& rect, TileId id);           ///< every layer
    void fill_rect(i32 layer, const TileRect& rect, TileId id);

    // --- coordinate helpers -------------------------------------------------
    [[nodiscard]] Vec2 tile_to_world(i32 x, i32 y) const;      ///< top-left corner of the cell
    [[nodiscard]] Aabb2 tile_bounds(i32 x, i32 y) const;
    [[nodiscard]] TileRect world_to_tiles(const Aabb2& box) const;   ///< cells touched by the box
    [[nodiscard]] TileRect visible_tiles(const Aabb2& view) const;   ///< for renderers

    // --- queries ------------------------------------------------------------
    [[nodiscard]] bool is_solid(i32 x, i32 y, const Tileset& tileset, LayerMask mask = kAllLayers) const;
    [[nodiscard]] bool is_one_way(i32 x, i32 y, const Tileset& tileset, LayerMask mask = kAllLayers) const;
    [[nodiscard]] bool is_hazard(i32 x, i32 y, const Tileset& tileset, LayerMask mask = kAllLayers) const;
    [[nodiscard]] bool overlaps_solid(const Aabb2& box, const Tileset& tileset,
                                      LayerMask mask = kAllLayers) const;
    [[nodiscard]] bool overlaps_hazard(const Aabb2& box, const Tileset& tileset,
                                       LayerMask mask = kAllLayers) const;
    /// True when the box rests on a solid or one-way tile (1 pixel probe below the box).
    [[nodiscard]] bool is_on_ground(const Aabb2& box, const Tileset& tileset,
                                    LayerMask mask = kAllLayers) const;
    /// First solid cell along a ray (used by line of sight / projectile helpers).
    [[nodiscard]] std::optional<Vec2> raycast_solid(Vec2 origin, Vec2 direction, f32 max_distance,
                                                   const Tileset& tileset,
                                                   LayerMask mask = kAllLayers) const;

    // --- movement -----------------------------------------------------------
    /// Moves \p box by (velocity * delta_seconds) resolving tile collisions. The box is the entity
    /// bounding box; the returned position is the resolved one. One-way platforms only stop a
    /// downward move when the box was above them before the step, unless \p drop_through is set.
    [[nodiscard]] MoveResult move_aabb(const Aabb2& box, Vec2 velocity, f32 delta_seconds, const Tileset& tileset,
                                       bool drop_through = false, LayerMask mask = kAllLayers) const;

    // --- serialisation ------------------------------------------------------
    /// RLE compressed, layer by layer: "varint run length + u16 tile id" pairs in row major order.
    [[nodiscard]] std::vector<u8> serialize() const;
    [[nodiscard]] static std::optional<TileMap> deserialize(ConstSpan<const u8> data);
    [[nodiscard]] u64 checksum() const;                        ///< every layer, bottom to top
    [[nodiscard]] usize count_tiles(TileId id) const;           ///< every layer
    [[nodiscard]] usize count_tiles(i32 layer, TileId id) const;

    // --- authoring ----------------------------------------------------------
    /// Builds a one layer map from ASCII art, one character per cell; characters missing from the
    /// legend become empty tiles. Throws away nothing: the result is optional so malformed input
    /// (ragged rows, zero size) can be rejected.
    [[nodiscard]] static std::optional<TileMap> from_ascii(ConstSpan<const std::string> rows,
                                                           const std::unordered_map<char, TileId>& legend,
                                                           f32 tile_size = 16.0f);
    /// Draws ASCII art into one layer of an existing map at (\p origin_x, \p origin_y). Cells outside
    /// the map are ignored; unknown characters are reported and left empty, exactly like from_ascii().
    void stamp_ascii(i32 layer, ConstSpan<const std::string> rows,
                     const std::unordered_map<char, TileId>& legend, i32 origin_x = 0, i32 origin_y = 0);
    [[nodiscard]] std::vector<std::string> to_ascii(const std::unordered_map<TileId, char>& legend,
                                                    char unknown = '?') const;   ///< base layer
    [[nodiscard]] std::vector<std::string> to_ascii(i32 layer, const std::unordered_map<TileId, char>& legend,
                                                    char unknown = '?') const;
    /// The legend used by from_ascii()/to_ascii() for the built-in tileset
    /// ('.' empty, '#' stone, '=' one-way, '^' spikes, 'o' coin, ':' background, 'H' ladder, '~' water, 'C' crate).
    [[nodiscard]] static const std::unordered_map<char, TileId>& default_legend();
    [[nodiscard]] static const std::unordered_map<TileId, char>& default_reverse_legend();

private:
    struct Chunk {
        std::vector<TileId> tiles;   ///< kChunkSize * kChunkSize
    };

    [[nodiscard]] u32 chunks_per_layer() const;
    [[nodiscard]] u32 chunk_index(i32 layer, i32 chunk_x, i32 chunk_y) const;
    [[nodiscard]] Chunk* chunk_at(i32 layer, i32 chunk_x, i32 chunk_y);
    [[nodiscard]] const Chunk* chunk_at(i32 layer, i32 chunk_x, i32 chunk_y) const;

    i32 width_ = 0;
    i32 height_ = 0;
    i32 layers_ = 1;
    i32 chunks_x_ = 0;
    i32 chunks_y_ = 0;
    f32 tile_size_ = 16.0f;
    bool out_of_bounds_solid_ = true;
    /// Layer major: layer * chunks_per_layer() + chunk_y * chunks_x_ + chunk_x.
    std::vector<Chunk> chunks_;
};

} // namespace t2d
