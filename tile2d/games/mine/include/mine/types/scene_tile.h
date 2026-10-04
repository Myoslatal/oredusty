// Mine - plots that are part of the scenery.
//
// A **scene plot** is what the mine is made of rather than what runs in it: floors, walls, ore veins,
// decorations. It is **not** ticked, and that is the whole reason the two kinds of plot exist - a map
// of hundreds of cells per side holds orders of magnitude more scenery than machines, and scenery that
// costs nothing per frame is scenery that can be everywhere (docs/GAME_DESIGN.md section 1.13).
//
// A scene plot is brought up to date when something asks it to be: a machine changed the world next to
// it, a demand wants to know what is there, a save is being written. "When something asks" is what the
// dirty flag is made of - a caller walks its scene plots and only the dirty ones do any work, which is
// the difference between a frame that costs what changed and a frame that costs what exists.
#pragma once

#include <mine/types/tile.h>

namespace mine::types {

/// A plot that is scenery: it is not ticked, and recomputes itself only when it is told to.
class SceneTile : public Tile {
public:
    /// A scene plot is placed where it is, like every plot. Content that needs logic of its own
    /// derives from this and inherits the constructor.
    SceneTile(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1)
        : Tile(kind, id, layer, anchor, width, height) {}

    /// True while the plot has not been brought up to date since it was last told the world changed. A
    /// new plot starts dirty: nothing has looked at it yet.
    [[nodiscard]] bool dirty() const { return dirty_; }

    /// Says that something around this plot changed in a way that may matter to it. Cheap on purpose:
    /// this is what a machine calls on the plots it just touched, instead of the world being walked
    /// every frame.
    void mark_dirty() { dirty_ = true; }

    /// Brings the plot up to date if it needs it, and returns whether on_refresh() ran.
    bool refresh();

protected:
    /// What the plot recomputes when it is brought up to date. The default does nothing: content that
    /// needs no logic - a floor - is a SceneTile and nothing more.
    virtual void on_refresh() {}

private:
    bool dirty_ = true;
};

} // namespace mine::types
