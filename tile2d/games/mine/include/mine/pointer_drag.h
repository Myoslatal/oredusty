// Mine - the pointer picking something up and putting it down somewhere else.
//
// The game's input is a camera and a pointer (docs/GAME_DESIGN.md section 1.11), and the first thing
// that pointer *does* to the world is drag a **drop** (section 1.25): press on one, move it, let go.
// What else a pointer may do to a cell - placing, selecting, ordering - is still the action table the
// designer has not given (section 7.14), so this is deliberately **one action**, not a framework for
// actions that would have to guess at the rest.
//
// It is game logic and not screen code: it holds a layer and an entity, it moves that entity, and it
// knows nothing about pixels or buttons. The screen hands it world coordinates (app.cpp), which is what
// makes the whole thing testable without a window.
#pragma once

#include <mine/world.h>

#include <t2d/core/math2d.h>

namespace mine {

/// How far from the pointer something can be and still be picked up, in cells. A drop is drawn one cell
/// wide, so a grab anywhere inside that cell is a grab on it.
inline constexpr f32 kGrabRadius = 0.75f;

/// What letting go of a dragged thing did.
enum class ReleaseResult : u8 {
    Nothing,  ///< nothing was being held
    Left,     ///< put down where it was: nothing there takes dragged drops
    Stored,   ///< taken in by what it was dropped on (a warehouse, types/warehouse.h)
    Refused,  ///< dropped on something that takes drops, which had no room for it
};

/// What the last release did, and what it was about. The entity is **gone from the layer** once it is
/// stored, so its identity is kept here - which is what a message about it needs.
struct DragRelease {
    ReleaseResult result = ReleaseResult::Nothing;
    ContentKind kind = ContentKind::Item;
    ContentId id = kNoContent;
};

/// What the pointer is holding, and where inside it the pointer took hold.
class PointerDrag {
public:
    /// Picks up the **nearest** draggable entity within \p radius of \p point in \p layer and
    /// remembers the offset between the point and the entity, so what is picked up does not jump under
    /// the pointer. Returns whether something was picked up: a point with nothing draggable near it
    /// leaves the drag empty, which is what "the pointer is over the map" means most of the time.
    bool begin(MineLayer& layer, t2d::Vec2 point, f32 radius = kGrabRadius);

    /// Moves what is held so it follows the pointer, keeping the grab offset, **clamped to the layer's
    /// rectangle**: the pointer may leave the map, but something dragged past the edge would be somewhere
    /// nobody can see or reach. Returns whether it moved.
    bool move_to(t2d::Vec2 point);

    /// Lets go. What was held stays where it was put - unless it was let go **on something that takes
    /// dragged drops**, and then it goes in and is gone from the layer (a warehouse, types/warehouse.h).
    /// Returns what happened, so the screen can say it: a box with no room has to say so.
    ReleaseResult end();
    /// What the last end() did, and what it was about.
    [[nodiscard]] const DragRelease& last_release() const { return last_release_; }

    [[nodiscard]] bool holding() const { return held_ != nullptr; }
    [[nodiscard]] types::Entity* held() const { return held_; }
    /// Where inside the held entity the pointer took hold, in cells.
    [[nodiscard]] t2d::Vec2 grab_offset() const { return offset_; }

private:
    /// The layer the held entity is in. Kept, because letting go of it or moving it has to go through
    /// the layer - the index follows a position only when the entity tells its layer (types/entity.h).
    MineLayer* layer_ = nullptr;
    types::Entity* held_ = nullptr;
    t2d::Vec2 offset_{};
    DragRelease last_release_{};
};

} // namespace mine
