// Mine - plots that are part of the scenery.
//
// A **scene plot** is what the mine is made of rather than what runs in it: floors, walls, ore veins,
// decorations. It is **not** ticked, and that is the whole reason the kinds of plot are separate - a
// map of hundreds of cells per side holds orders of magnitude more scenery than machines, and scenery
// that costs nothing per frame is scenery that can be everywhere (docs/GAME_DESIGN.md section 1.13).
//
// A functional plot is a scene plot that also runs: types/entity_tile.h derives from this class, so
// everything here - the dirty flag, the refresh pass - covers machines too.
//
// A scene plot is brought up to date when something asks it to be: a machine changed the world next to
// it, a demand wants to know what is there, a save is being written. "When something asks" is what the
// dirty flag is made of - a caller walks its scene plots and only the dirty ones do any work, which is
// the difference between a frame that costs what changed and a frame that costs what exists.
#pragma once

#include <mine/types/container.h>
#include <mine/types/tile.h>

namespace mine::types {

struct Building;
struct TileDefinition;

/// A plot that is scenery: it is not ticked, and recomputes itself only when it is told to. A
/// functional plot (types/entity_tile.h) is one of these plus a cadence.
class SceneTile : public Tile {
public:
    ~SceneTile() override = default;

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

    /// True while the plot has been placed but is **still going up**: it covers its cells and is drawn
    /// like any other, but it does nothing yet - it is not brought up to date, not ticked, takes no click
    /// and no dragged drop until its build is done (docs/GAME_DESIGN.md section 1.37). It is **state of
    /// this plot** and not a definition attribute: two plots of one entry can be at different stages
    /// (docs/GAME_DESIGN.md section 1.35).
    [[nodiscard]] bool under_construction() const { return under_construction_; }
    /// Sets it, which the build animation does when the sweep starts and when it is done. Content that
    /// takes no time to build is never under construction: being finished is the default.
    void set_under_construction(bool value) { under_construction_ = value; }

    /// The container this plot holds things in, when it has one (types/container.h): it is what a double
    /// click selects, and what the panel that selection opens shows (docs/GAME_DESIGN.md section 1.31).
    /// Scenery that keeps nothing - a floor, an ore vein, a wall - holds none, and the default says so;
    /// a warehouse (types/warehouse.h) is the first that does. A machine's input side will be another.
    [[nodiscard]] virtual Container* container() { return nullptr; }
    [[nodiscard]] virtual const Container* container() const { return nullptr; }

    /// The **definition** this plot was made from - the one shared record every plot of that content
    /// entry reads its fixed attributes through (capacity, dig power, the building sub-object). A plot
    /// holds its own state (what is in its container, how far a drill has got) and nothing else
    /// (docs/GAME_DESIGN.md section 1.35). Null for a plot made without one, which only a test does.
    [[nodiscard]] const TileDefinition* definition() const { return definition_; }
    void set_definition(const TileDefinition* definition) { definition_ = definition; }

    /// The building sub-object of this plot, when its content declares one (types/building.h).
    [[nodiscard]] const Building* building() const;

    /// The complete render preview of this scene plot (types/tile.h): its own picture, drawn the way the
    /// map draws it - mirrored when this plot is (set_mirrored). A plot with no definition, or one whose
    /// entry names no "image", previews as nothing. \p seconds is not read: scenery does not turn.
    [[nodiscard]] TilePreview get_preview(t2d::f32 seconds = 0.0f) const override;

protected:
    const TileDefinition* definition_ = nullptr;
    /// What the plot recomputes when it is brought up to date. The default does nothing: content that
    /// needs no logic - a floor - is a SceneTile and nothing more.
    virtual void on_refresh() {}

private:
    bool dirty_ = true;
    /// Whether its build is still going. Off by default, and off for everything that is not being built:
    /// a plot is finished unless something says otherwise (mine/build_animation.h).
    bool under_construction_ = false;
};

} // namespace mine::types
