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

using t2d::usize;

/// Something lying in a layer that the layer does not update and the pointer can drag.
class Drop : public Entity {
public:
    /// A drop is made where it lies: content identity, the position it was dropped at, and how many of it
    /// the drop carries. One is a single thing and more is a **pile**: two drops of one item lying close
    /// together become one drop with a count (world.h, docs/GAME_DESIGN.md section 1.43). A pile is a
    /// drop in everything else - a position, identity, one picture, the same drag rules.
    Drop(ContentKind kind, ContentId id, Vec2 position, usize count = 1);

    /// The pointer may pick it up and move it. What does that is PointerDrag (pointer_drag.h), which the
    /// world view hands world coordinates - the drop itself knows nothing about pointers or pixels.
    [[nodiscard]] bool draggable() const override { return true; }

    /// How many items of its content it is: 1 for a single drop, more for a pile. Never zero - a drop that
    /// carries nothing is not a drop.
    [[nodiscard]] usize count() const { return count_; }
    /// Adds \p count of it to this drop: how a pile grows when another drop of it is merged in (world.h).
    /// Adding nothing does nothing.
    void add(usize count);
    /// Sets how many it carries, clamped to at least one: taking one item out of a pile leaves a pile
    /// behind until the last one, which is the drop itself (pointer_drag.h).
    void set_count(usize count);

private:
    usize count_ = 1;
};

} // namespace mine::types
