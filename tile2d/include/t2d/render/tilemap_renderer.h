// Tile2D - turns simulation state into batched quads (tilemap, players, pickups, debug overlays).
#pragma once

#include <t2d/client/local_client.h>
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
    u32 layers = 0;
    u32 sprites = 0;
};

/// Draws a tilemap with camera culling, in the order backgrounds, tiles, foreground decor.
class TilemapRenderer {
public:
    struct Options {
        /// Tint applied to the background layer so it reads as "behind".
        u32 background_tint = 0xFF6A5A50u;
        u32 foreground_tint = 0xFFFFFFFFu;
        bool draw_debug_boxes = false;
    };

    TilemapRenderer() = default;
    explicit TilemapRenderer(const Options& options) : options_(options) {}

    /// Draws every layer of \p map that intersects \p view.
    void draw_map(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, const Aabb2& view);
    void draw_players(SpriteBatch& batch, const std::vector<RenderPlayer>& players, PlayerId local_id,
                      const PlayerTuning& tuning);
    void draw_pickups(SpriteBatch& batch, ConstSpan<Pickup> pickups);
    /// Debug overlay: player bounding boxes and their velocity.
    void draw_debug(SpriteBatch& batch, const std::vector<RenderPlayer>& players, const PlayerTuning& tuning);

    [[nodiscard]] const TilemapRenderStats& stats() const { return stats_; }
    void reset_stats() { stats_ = {}; }
    [[nodiscard]] Options& options() { return options_; }

private:
    void draw_tile(SpriteBatch& batch, const TileMap& map, TileId id, i32 tile_x, i32 tile_y, u32 tint);

    Options options_{};
    TilemapRenderStats stats_{};
};

/// Atlas uv rectangle of one tile cell, in normalised coordinates.
[[nodiscard]] Aabb2 tile_uv_rect(const Tileset& tileset, TileId id);

} // namespace t2d
