// Tile2D - tile definitions: flag queries. The definitions themselves are the game's data.
#include <t2d/sim/tileset.h>

namespace t2d {

void Tileset::add(const TileDef& definition) {
    // Re-adding an id replaces the previous definition: lookups stay unique and a definition never
    // depends on the order in which the tiles were registered.
    for (TileDef& existing : definitions_) {
        if (existing.id == definition.id) {
            existing = definition;
            return;
        }
    }
    definitions_.push_back(definition);
}

void Tileset::set_atlas_grid(u32 columns, u32 rows) {
    atlas_columns_ = columns > 0 ? columns : 1;
    atlas_rows_ = rows > 0 ? rows : 1;
}

const TileDef* Tileset::find(TileId id) const {
    for (const TileDef& definition : definitions_) {
        if (definition.id == id) return &definition;
    }
    return nullptr;
}

usize Tileset::count() const { return definitions_.size(); }

ConstSpan<TileDef> Tileset::definitions() const { return ConstSpan<TileDef>{definitions_}; }

bool Tileset::is_solid(TileId id) const {
    const TileDef* definition = find(id);
    return definition != nullptr && has_flag(definition->flags, TileFlag::Solid);
}

bool Tileset::is_one_way(TileId id) const {
    const TileDef* definition = find(id);
    return definition != nullptr && has_flag(definition->flags, TileFlag::OneWay);
}

bool Tileset::is_hazard(TileId id) const {
    const TileDef* definition = find(id);
    return definition != nullptr && has_flag(definition->flags, TileFlag::Hazard);
}

bool Tileset::is_decor(TileId id) const {
    const TileDef* definition = find(id);
    return definition != nullptr && has_flag(definition->flags, TileFlag::Decor);
}

bool Tileset::blocks_falling(TileId id) const {
    const TileDef* definition = find(id);
    if (definition == nullptr) return false;
    return has_flag(definition->flags, TileFlag::Solid) || has_flag(definition->flags, TileFlag::OneWay);
}

TileDef Tileset::atlas_of(TileId id) const {
    const TileDef* definition = find(id);
    // An unknown id has no atlas cell; the default TileDef reports column 0, row 0.
    return definition != nullptr ? *definition : TileDef{};
}

} // namespace t2d
