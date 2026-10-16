// Mine - things that lie in a layer and are not updated by it: a dropped item.
//
// A **drop** is an entity (types/entity.h) with two things settled. It is **not updated by the layer**:
// nothing about it changes on its own, it lies where it was put, and what moves it is the pointer. And
// it **can be dragged**: press on it, move, let go (docs/GAME_DESIGN.md section 1.25). Everything else
// an entity has, it has - a free position that is not aligned to the tile grid, identity from the
// registry (kind + id, never a name), a place in the layer's quadtree so "what is near this point"
// finds it, and the ability to be asked to interact.
//
// Why a class of its own rather than "an entity with a flag": whether the pointer may move something is
// a property of the **kind of thing** it is, the way being a floor or being ore is (types/tile.h) - so
// the type answers it, and no field in a designer's file can hand it out.
#pragma once

#include <mine/types/entity.h>

namespace mine::types {

/// Something lying in a layer that the layer does not update and the pointer can drag.
class Drop : public Entity {
public:
    /// A drop is made where it lies: content identity, and the position it was dropped at. Content that
    /// needs logic of its own derives from this and inherits the constructor.
    Drop(ContentKind kind, ContentId id, Vec2 position);

    /// The pointer may pick it up and move it. What does that is PointerDrag (pointer_drag.h), which the
    /// world view hands world coordinates - the drop itself knows nothing about pointers or pixels.
    [[nodiscard]] bool draggable() const override { return true; }
};

} // namespace mine::types
