#include <t2d/render/tilemap_renderer.h>

#include <algorithm>

namespace t2d {

Aabb2 tile_uv_rect(const Tileset& tileset, TileId id) {
    const TileDef definition = tileset.atlas_of(id);
    const f32 columns = static_cast<f32>(std::max(1u, tileset.atlas_columns()));
    const f32 rows = static_cast<f32>(std::max(1u, tileset.atlas_rows()));
    return Aabb2{Vec2{static_cast<f32>(definition.atlas_x) / columns,
                      static_cast<f32>(definition.atlas_y) / rows},
                 Vec2{static_cast<f32>(definition.atlas_x + 1) / columns,
                      static_cast<f32>(definition.atlas_y + 1) / rows}};
}

void TilemapRenderer::draw_tile(SpriteBatch& batch, const Aabb2& bounds, const Tileset& tileset, TileId id,
                                u32 tint) {
    batch.draw_quad(bounds, tile_uv_rect(tileset, id), tint);
    ++stats_.tiles_drawn;
}

void TilemapRenderer::draw_map(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, const Aabb2& view,
                               TileMap::LayerMask mask, u32 tint) {
    // A map is not streamed: the three chunk counters describe draw_world() only, so a caller that
    // switches between the two never reads a stale chunk count from the map path.
    stats_.chunks_considered = 0;
    stats_.chunks_drawn = 0;
    stats_.chunks_culled = 0;

    const TileRect visible = map.visible_tiles(view);
    u32 drawn_layers = 0;

    for (i32 map_layer = 0; map_layer < map.layer_count(); ++map_layer) {
        if ((mask & TileMap::layer_mask(map_layer)) == 0u) continue;
        ++drawn_layers;
        for (i32 y = visible.y; y < visible.bottom(); ++y) {
            for (i32 x = visible.x; x < visible.right(); ++x) {
                if (!map.in_bounds(x, y)) continue;
                const TileId id = map.at(map_layer, x, y);
                ++stats_.tiles_considered;
                if (id == kEmptyTile) {
                    ++stats_.tiles_culled;
                    continue;
                }
                draw_tile(batch, map.tile_bounds(x, y), tileset, id, tint);
            }
        }
    }
    stats_.map_layers = drawn_layers;
}

void TilemapRenderer::draw_world(SpriteBatch& batch, const OpenWorld& world, const Tileset& tileset,
                                 const Aabb2& view, TileMap::LayerMask mask, u32 tint) {
    // The chunk counters describe this call: the world's culling range is the view's chunks and
    // nothing else.
    stats_.chunks_considered = 0;
    stats_.chunks_drawn = 0;
    stats_.chunks_culled = 0;

    u32 visited_layers = 0;
    for (i32 layer = 0; layer < world.layers(); ++layer) {
        if ((mask & TileMap::layer_mask(layer)) != 0u) ++visited_layers;
    }

    const std::vector<ChunkCoord> chunks = world.chunks_in(view, 0);
    stats_.chunks_considered = static_cast<u32>(chunks.size());
    for (const ChunkCoord& coord : chunks) {
        const WorldChunk* chunk = world.find_chunk(coord);
        if (chunk == nullptr) {
            // The view touches a chunk the streamer has not made resident: that is a hole, not an
            // error, and drawing nothing is the only honest answer.
            ++stats_.chunks_culled;
            continue;
        }
        ++stats_.chunks_drawn;

        // Bottom to top inside one chunk, so a higher layer covers a lower one; the plane is
        // partitioned by chunks, so the order chunks are visited in cannot change a pixel.
        for (i32 layer = 0; layer < world.layers(); ++layer) {
            if ((mask & TileMap::layer_mask(layer)) == 0u) continue;
            for (i32 local_y = 0; local_y < WorldChunk::kSize; ++local_y) {
                for (i32 local_x = 0; local_x < WorldChunk::kSize; ++local_x) {
                    // Relative cells are i32 (chunks_in() guards the practical range, so chunk * 32
                    // stays inside i32) and the quad comes from the world's own cell_bounds(),
                    // exactly the geometry a query would use - never an absolute f32 coordinate.
                    const i32 cell_x = static_cast<i32>(static_cast<i64>(coord.x) * WorldChunk::kSize + local_x);
                    const i32 cell_y = static_cast<i32>(static_cast<i64>(coord.y) * WorldChunk::kSize + local_y);
                    const Aabb2 bounds = world.cell_bounds(cell_x, cell_y);
                    if (!bounds.overlaps(view)) continue;
                    ++stats_.tiles_considered;
                    const TileId id = chunk->at(layer, local_x, local_y);
                    if (id == kEmptyTile) {
                        ++stats_.tiles_culled;
                        continue;
                    }
                    draw_tile(batch, bounds, tileset, id, tint);
                }
            }
        }
    }
    stats_.map_layers = visited_layers;
}

} // namespace t2d
