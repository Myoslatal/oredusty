// Tile2D - turns a tile map into batched quads.
//
// The renderer knows nothing about what a tile means: it walks the map layers the caller asks for,
// bottom to top, and draws every non empty cell at the uv its tileset definition names. Game art,
// entity sprites and overlays are the game's own drawing.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/sim/tilemap.h>
#include <t2d/sim/tileset.h>

namespace t2d {

struct TilemapRenderStats {
    u32 tiles_considered = 0;
    u32 tiles_drawn = 0;
    u32 tiles_culled = 0;
    /// Map layers the last draw_map() actually visited.
    u32 map_layers = 0;
};

/// Draws the tile layers of a map with camera culling, bottom to top.
class TilemapRenderer {
public:
    /// Draws the map layers in \p mask that intersect \p view, bottom to top: a higher layer covers a
    /// lower one. \p tint multiplies every tile, so a caller can draw a layer dimmed (the sandbox
    /// does) or a whole map in one colour.
    void draw_map(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, const Aabb2& view,
                  TileMap::LayerMask mask = TileMap::kAllLayers, u32 tint = 0xFFFFFFFFu);

    [[nodiscard]] const TilemapRenderStats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

private:
    void draw_tile(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, TileId id, i32 tile_x,
                   i32 tile_y, u32 tint);

    TilemapRenderStats stats_{};
};

/// Atlas uv rectangle of one tile cell, in normalised coordinates: the tileset's atlas grid decides
/// what a cell index means.
[[nodiscard]] Aabb2 tile_uv_rect(const Tileset& tileset, TileId id);

} // namespace t2d
