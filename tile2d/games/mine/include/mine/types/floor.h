// Mine - floors: the plot other plots are placed on.
//
// A floor is scenery (it is a scene plot: placed once, refreshed only when something asks) with one
// thing added: it says that it is a floor. That single marker is what lets everything else ask the
// question it actually has - "is there something to stand on here?" - without knowing which content
// answers yes: a machine checks the cell under it before it is placed, pathing checks the cell it is
// about to walk into, and neither of them has to know the name of a single floor.
//
// The marker is a method rather than a flag in the data because it is a *kind of thing*, not a
// property of one: content that is a floor is a floor (types/floor.h), and content that is not does
// not become one by saying so in a file.
#pragma once

#include <mine/types/scene_tile.h>

namespace mine::types {

/// A floor: scenery that other plots are placed on.
class Floor : public SceneTile {
public:
    ~Floor() override = default;

    Floor(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1)
        : SceneTile(kind, id, layer, anchor, width, height) {}

    [[nodiscard]] bool is_floor() const override { return true; }
};

} // namespace mine::types
