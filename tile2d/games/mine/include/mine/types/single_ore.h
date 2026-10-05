// Mine - ore: one cell of something that can be dug out.
//
// An ore is scenery (it is a scene plot: placed once, refreshed only when something asks) with one
// thing added: it says that it is ore. That single marker is what lets everything else ask the question
// it actually has - "is there something to dig out here?" - without knowing which content answers yes:
// a drill checks the cell it is about to work on, a demand checks what a layer still holds, and neither
// of them has to know the name of a single ore.
//
// **Single** because one ore is one cell: a scatter places one per cell (layer_rules.h), so a cell is
// either ore or not, and what the ore yields is the content behind it (the id the registry handed out),
// never a number this class invents. An ore vein that spans cells would be a different kind of thing.
//
// The marker is a method rather than a flag in the data because it is a *kind of thing*, not a property
// of one: content that is ore is ore (types/single_ore.h), and content that is not does not become ore
// by saying so in a file. Which is the same rule the floor follows (types/floor.h).
#pragma once

#include <mine/types/scene_tile.h>

namespace mine::types {

/// An ore: scenery that something can be dug out of.
class SingleOre : public SceneTile {
public:
    ~SingleOre() override = default;

    SingleOre(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1)
        : SceneTile(kind, id, layer, anchor, width, height) {}

    [[nodiscard]] bool is_ore() const override { return true; }
};

} // namespace mine::types
