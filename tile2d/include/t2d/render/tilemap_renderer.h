// Tile2D - turns a tile map into batched quads.
//
// The renderer knows nothing about what a tile means: it walks the layers the caller asks for, bottom
// to top, and draws every non empty cell at the uv its tileset definition names. Two shapes of world
// are supported: a bounded TileMap and a streamed OpenWorld, whose chunks are culled before a single
// cell is looked at. Game art, entity sprites and overlays are the game's own drawing.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>
#include <t2d/render/sprite_batch.h>
#include <t2d/sim/open_world.h>
#include <t2d/sim/tilemap.h>
#include <t2d/sim/tileset.h>

namespace t2d {

struct TilemapRenderStats {
    u32 tiles_considered = 0;
    u32 tiles_drawn = 0;
    u32 tiles_culled = 0;
    /// Chunks the last draw_world() considered: world.chunks_in(view, 0). A draw_world() resets the
    /// three chunk counters so they describe that call; draw_map() leaves them at 0.
    u32 chunks_considered = 0;
    /// Of the considered chunks, the resident ones, whose cells were walked.
    u32 chunks_drawn = 0;
    /// Of the considered chunks, the ones with no resident content: a hole in the streamed world is
    /// counted here, never drawn.
    u32 chunks_culled = 0;
    /// Layers the last draw_map() or draw_world() actually visited.
    u32 map_layers = 0;
};

/// Draws the tile layers of a TileMap or a resident OpenWorld with camera culling, bottom to top.
/// An OpenWorld is addressed in its own RELATIVE coordinates (see t2d/sim/open_world.h); draw_map()'s
/// TileMap keeps its absolute authored cells, unchanged.
class TilemapRenderer {
public:
    /// Draws the map layers in \p mask that intersect \p view, bottom to top: a higher layer covers a
    /// lower one. \p tint multiplies every tile, so a caller can draw a layer dimmed (the sandbox
    /// does) or a whole map in one colour.
    void draw_map(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, const Aabb2& view,
                  TileMap::LayerMask mask = TileMap::kAllLayers, u32 tint = 0xFFFFFFFFu);

    /// Draws a streamed world in its RELATIVE coordinate space: \p view is relative world units and
    /// every quad comes from OpenWorld::cell_bounds(relative cell), so no absolute world position ever
    /// reaches the batch and f32 stays precise however far the absolute origin is. Only the chunks
    /// world.chunks_in(\p view, 0) touches are considered; a chunk that is not resident is skipped and
    /// counted as culled; a resident chunk contributes the cells that intersect \p view, its layers
    /// bottom to top (a higher layer covers a lower one in that chunk). Chunks partition the plane, so
    /// the order chunks are visited in cannot change a pixel. Missing chunks are holes: call
    /// OpenWorld::update() before drawing to make the view resident. \p mask and \p tint mean exactly
    /// what they mean for draw_map().
    void draw_world(SpriteBatch& batch, const OpenWorld& world, const Tileset& tileset, const Aabb2& view,
                    TileMap::LayerMask mask = TileMap::kAllLayers, u32 tint = 0xFFFFFFFFu);

    [[nodiscard]] const TilemapRenderStats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }

private:
    /// One quad at \p bounds with the tile's uv and tint. The caller resolves the geometry: a TileMap
    /// cell comes from TileMap::tile_bounds(), a world cell from OpenWorld::cell_bounds().
    void draw_tile(SpriteBatch& batch, const Aabb2& bounds, const Tileset& tileset, TileId id, u32 tint);

    TilemapRenderStats stats_{};
};

/// Atlas uv rectangle of one tile cell, in normalised coordinates: the tileset's atlas grid decides
/// what a cell index means.
[[nodiscard]] Aabb2 tile_uv_rect(const Tileset& tileset, TileId id);

} // namespace t2d
