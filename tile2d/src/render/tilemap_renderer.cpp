#include <t2d/render/tilemap_renderer.h>

#include <t2d/render/atlas.h>

#include <algorithm>

namespace t2d {
namespace {

/// Layer of a tile: backgrounds are drawn first, decor last.
enum class TileLayer : u8 { Background = 0, Main = 1, Foreground = 2 };

[[nodiscard]] TileLayer layer_of(const Tileset& tileset, TileId id) {
    const TileDef* definition = tileset.find(id);
    if (definition == nullptr) return TileLayer::Main;
    if (has_flag(definition->flags, TileFlag::Decor)) {
        // The "background brick" tile is authored as decor and drawn behind everything else.
        return definition->atlas_x == 7 ? TileLayer::Background : TileLayer::Foreground;
    }
    return TileLayer::Main;
}

constexpr u32 kPlayerTint = 0xFFFFFFFFu;
constexpr u32 kRemoteTint = 0xFFC8B4A0u;   ///< slightly bluish so other players stand out
constexpr u32 kDeadTint = 0x80FFFFFFu;

} // namespace

Aabb2 tile_uv_rect(const Tileset& tileset, TileId id) {
    const TileDef definition = tileset.atlas_of(id);
    const f32 cell = 1.0f / static_cast<f32>(kTileAtlasColumns);
    const f32 inset = 0.0f; // no bleeding: the sampler is nearest
    return Aabb2{Vec2{static_cast<f32>(definition.atlas_x) * cell + inset,
                      static_cast<f32>(definition.atlas_y) * cell + inset},
                 Vec2{static_cast<f32>(definition.atlas_x + 1) * cell - inset,
                      static_cast<f32>(definition.atlas_y + 1) * cell - inset}};
}

void TilemapRenderer::draw_tile(SpriteBatch& batch, const TileMap& map, TileId id, i32 tile_x, i32 tile_y,
                                u32 tint) {
    batch.draw_quad(map.tile_bounds(tile_x, tile_y), tile_uv_rect(Tileset::default_platformer(), id), tint);
    ++stats_.tiles_drawn;
}

void TilemapRenderer::draw_map(SpriteBatch& batch, const TileMap& map, const Tileset& tileset, const Aabb2& view,
                               TileMap::LayerMask mask) {
    const TileRect visible = map.visible_tiles(view);
    u32 drawn_layers = 0;

    // Map layers are drawn in order, so a higher layer covers a lower one: that is what makes a
    // ground layer, an ore layer and a structures layer compose into one picture.
    for (i32 map_layer = 0; map_layer < map.layer_count(); ++map_layer) {
        if ((mask & TileMap::layer_mask(map_layer)) == 0u) continue;
        ++drawn_layers;
        for (u32 layer_index = 0; layer_index < 3; ++layer_index) {
            const auto layer = static_cast<TileLayer>(layer_index);
            const u32 tint = layer == TileLayer::Background ? options_.background_tint
                                                           : (layer == TileLayer::Foreground
                                                                  ? options_.foreground_tint
                                                                  : 0xFFFFFFFFu);
            for (i32 y = visible.y; y < visible.bottom(); ++y) {
                for (i32 x = visible.x; x < visible.right(); ++x) {
                    if (!map.in_bounds(x, y)) continue;
                    const TileId id = map.at(map_layer, x, y);
                    ++stats_.tiles_considered;
                    if (id == kEmptyTile) {
                        ++stats_.tiles_culled;
                        continue;
                    }
                    if (layer_of(tileset, id) != layer) {
                        ++stats_.tiles_culled;
                        continue;
                    }
                    draw_tile(batch, map, id, x, y, tint);
                }
            }
        }
    }
    stats_.layers = 3u * drawn_layers;
    stats_.map_layers = drawn_layers;
}

void TilemapRenderer::draw_players(SpriteBatch& batch, const std::vector<RenderPlayer>& players, PlayerId local_id,
                                   const PlayerTuning& tuning) {
    for (const RenderPlayer& player : players) {
        const Aabb2 bounds = Aabb2::from_top_left(player.position, tuning.size);
        // Atlas cells (0,1) and (1,1) hold the idle and running sprite.
        const bool running = abs_f32(player.velocity.x) > 4.0f;
        const bool facing_right = (player.flags & kPlayerFacingRight) != 0;
        const f32 cell = 1.0f / static_cast<f32>(kTileAtlasColumns);
        const f32 row = 1.0f * cell;
        const f32 column = (running ? 1.0f : 0.0f) * cell;
        Aabb2 uv{Vec2{column, row}, Vec2{column + cell, row + cell}};
        if (!facing_right) std::swap(uv.min.x, uv.max.x); // mirror horizontally

        u32 tint = player.local ? kPlayerTint : kRemoteTint;
        if ((player.flags & kPlayerAlive) == 0u) tint = kDeadTint;
        batch.draw_quad(bounds.expanded(1.0f), uv, tint);
        ++stats_.sprites;
    }
    (void)local_id;
}

void TilemapRenderer::draw_pickups(SpriteBatch& batch, ConstSpan<Pickup> pickups) {
    const Aabb2 uv = tile_uv_rect(Tileset::default_platformer(), 6);
    for (const Pickup& pickup : pickups) {
        if (!pickup.alive) continue;
        batch.draw_quad(Aabb2::from_top_left(pickup.position, kPickupSize), uv, 0xFFFFFFFFu);
        ++stats_.sprites;
    }
}

void TilemapRenderer::draw_debug(SpriteBatch& batch, const std::vector<RenderPlayer>& players,
                                 const PlayerTuning& tuning) {
    for (const RenderPlayer& player : players) {
        const Aabb2 bounds = Aabb2::from_top_left(player.position, tuning.size);
        batch.draw_rect_outline(bounds, 1.0f, player.local ? 0xFF40FF40u : 0xFF4040FFu);
        // Velocity indicator.
        const Vec2 tip{bounds.center().x + player.velocity.x * 0.05f, bounds.center().y + player.velocity.y * 0.05f};
        batch.draw_rect(Aabb2{tip - Vec2{1.0f, 1.0f}, tip + Vec2{1.0f, 1.0f}}, 0xFFFFFF00u);
    }
}

} // namespace t2d
