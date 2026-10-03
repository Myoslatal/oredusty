// Tile2D - tile definitions: what a tile id means for physics, and where it lives in the atlas.
#pragma once

#include <t2d/core/types.h>

#include <string_view>

namespace t2d {

using TileId = u16;

/// Reserved id: an empty cell.
inline constexpr TileId kEmptyTile = 0;

enum class TileFlag : u16 {
    None = 0,
    Solid = 1u << 0,     ///< blocks movement from every direction
    OneWay = 1u << 1,    ///< only blocks entities falling onto it (drop through with "down")
    Hazard = 1u << 2,    ///< hurts/kills on contact
    Ladder = 1u << 3,    ///< climbable (the demo movement code ignores it, the flag is honoured by queries)
    Water = 1u << 4,     ///< swimming volume
    Decor = 1u << 5,     ///< drawn but never collides
};

[[nodiscard]] constexpr TileFlag operator|(TileFlag a, TileFlag b) {
    return static_cast<TileFlag>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr TileFlag operator&(TileFlag a, TileFlag b) {
    return static_cast<TileFlag>(static_cast<u16>(a) & static_cast<u16>(b));
}
[[nodiscard]] constexpr bool has_flag(TileFlag value, TileFlag flag) {
    return (static_cast<u16>(value) & static_cast<u16>(flag)) != 0;
}
[[nodiscard]] constexpr std::string_view tile_flag_name(TileFlag flag) {
    if (has_flag(flag, TileFlag::Solid)) return "solid";
    if (has_flag(flag, TileFlag::OneWay)) return "oneway";
    if (has_flag(flag, TileFlag::Hazard)) return "hazard";
    if (has_flag(flag, TileFlag::Ladder)) return "ladder";
    if (has_flag(flag, TileFlag::Water)) return "water";
    if (has_flag(flag, TileFlag::Decor)) return "decor";
    return "none";
}

struct TileDef {
    TileId id = kEmptyTile;
    TileFlag flags = TileFlag::None;
    u8 atlas_x = 0;  ///< column in the tileset atlas
    u8 atlas_y = 0;  ///< row in the tileset atlas
};

class Tileset {
public:
    void add(const TileDef& definition);
    [[nodiscard]] const TileDef* find(TileId id) const;
    [[nodiscard]] usize count() const;
    [[nodiscard]] ConstSpan<TileDef> definitions() const;

    [[nodiscard]] bool is_solid(TileId id) const;
    [[nodiscard]] bool is_one_way(TileId id) const;
    [[nodiscard]] bool is_hazard(TileId id) const;
    [[nodiscard]] bool is_decor(TileId id) const;
    /// Solid or one-way: everything that stops a falling entity.
    [[nodiscard]] bool blocks_falling(TileId id) const;
    /// Atlas cell of a tile (0,0 when unknown).
    [[nodiscard]] TileDef atlas_of(TileId id) const;

    /// The built-in set used by the demo level and the tests. Ids are stable:
    ///   0 empty, 1 stone, 2 dirt, 3 grass top, 4 one-way platform, 5 spikes, 6 coin,
    ///   7 background brick, 8 ladder, 9 water, 10 crate
    [[nodiscard]] static const Tileset& default_platformer();

private:
    std::vector<TileDef> definitions_;
};

} // namespace t2d
