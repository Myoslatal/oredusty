// Mine - the half of a thing that receives dragged drops.
//
// A pointer drag ends somewhere (pointer_drag.h), and what it ends on may want what it was carrying: a
// **draginto** is that side of a thing, and it is a component of its own because a machine's input side,
// a demand's box and a warehouse all need exactly this - "would you take this", "take it", and nothing
// changed when the answer is no (docs/GAME_DESIGN.md section 1.27).
//
// It feeds a **container** (types/container.h) and knows nothing else: what a drop *is* belongs to the
// drop, what fits belongs to the container, and what happens after a drop goes in is nobody's business
// here.
#pragma once

#include <mine/types/container.h>
#include <mine/types/entity.h>

namespace mine::types {

/// What receives a dragged drop: the container it feeds, and the two questions a drag asks it.
class DragInto {
public:
    /// Takes what it receives into \p into, which has to outlive it (a warehouse owns both).
    explicit DragInto(Container& into) : into_(&into) {}

    /// Whether one more of \p kind / \p id would be taken right now.
    [[nodiscard]] bool accepts(ContentKind kind, ContentId id) const;
    /// Whether \p drop would be taken right now - what a drag asks before it lets go.
    [[nodiscard]] bool accepts(const Entity& drop) const { return accepts(drop.kind(), drop.id()); }

    /// Takes \p count of one piece of content in and returns whether it took it **whole**. When it does
    /// not, nothing changed: a drag that half emptied itself into a full box would be a lie about what
    /// the pointer did.
    bool receive(ContentKind kind, ContentId id, usize count = 1);
    /// Takes a drop in: what it carries is its content, and a drop is one of it (types/drop.h).
    bool receive(const Entity& drop) { return receive(drop.kind(), drop.id(), 1); }

    [[nodiscard]] Container& container() { return *into_; }
    [[nodiscard]] const Container& container() const { return *into_; }

private:
    Container* into_ = nullptr;
};

} // namespace mine::types
