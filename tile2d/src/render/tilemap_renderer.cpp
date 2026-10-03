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

void TilemapRenderer::draw_tile(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, TileId id,
                                i32 tile_x, i32 tile_y, u32 tint) {
    batch.draw_quad(map.tile_bounds(tile_x, tile_y), tile_uv_rect(tileset, id), tint);
    ++stats_.tiles_drawn;
}

void TilemapRenderer::draw_map(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, const Aabb2& view,
                               TileMap::LayerMask mask, u32 tint) {
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
                draw_tile(batch, map, tileset, id, x, y, tint);
            }
        }
    }
    stats_.map_layers = drawn_layers;
}

} // namespace t2d
