#include <mine/types/tile.h>

#include <format>

namespace mine::types {

Tile::Tile(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width, i32 height)
    : kind_(kind), id_(id), layer_(layer), cells_(footprint(anchor, width, height)) {}

std::string Tile::name(const ContentRegistry& registry) const {
    const ContentEntry* entry = registry.find(kind_, id_);
    if (entry == nullptr) return std::format("#{}", id_);
    return entry->name;
}

bool Tile::overlaps(const Tile& other) const {
    if (layer_ != other.layer_) return false;
    return cells_.x < other.cells_.right() && other.cells_.x < cells_.right() &&
           cells_.y < other.cells_.bottom() && other.cells_.y < cells_.bottom();
}

void Tile::randomise_mirror(t2d::Rng& rng) {
    // Half of them, and only where the content allows it: a plot that may not be turned around never
    // is, and never touches the generator either.
    mirrored_ = random_reverse_ && rng.chance(0.5f);
}

bool Tile::within(i32 map_width, i32 map_height) const {
    return cells_.x >= 0 && cells_.y >= 0 && cells_.right() <= map_width && cells_.bottom() <= map_height;
}

} // namespace mine::types
