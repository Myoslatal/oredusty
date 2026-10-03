// Tile2D - tile definitions: flag queries and the built-in platformer tileset.
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

const Tileset& Tileset::default_platformer() {
    // Built once: the atlas layout (8 columns) is part of the built-in content, ids 1..10 are laid
    // out in registration order and the empty tile is never drawn.
    static const Tileset tileset = [] {
        Tileset built;
        built.add(TileDef{kEmptyTile, TileFlag::None, 0, 0});     //  0 empty
        built.add(TileDef{1, TileFlag::Solid, 0, 0});             //  1 stone
        built.add(TileDef{2, TileFlag::Solid, 1, 0});             //  2 dirt
        built.add(TileDef{3, TileFlag::Solid, 2, 0});             //  3 grass top
        built.add(TileDef{4, TileFlag::OneWay, 3, 0});            //  4 one-way platform
        built.add(TileDef{5, TileFlag::Hazard, 4, 0});            //  5 spikes
        built.add(TileDef{6, TileFlag::Decor, 5, 0});             //  6 coin
        built.add(TileDef{7, TileFlag::Decor, 6, 0});             //  7 background brick
        built.add(TileDef{8, TileFlag::Ladder, 7, 0});            //  8 ladder
        built.add(TileDef{9, TileFlag::Water, 0, 1});             //  9 water
        built.add(TileDef{10, TileFlag::Solid, 1, 1});            // 10 crate
        return built;
    }();
    return tileset;
}

} // namespace t2d
