// Mine - things that are in a layer without being on a cell.
//
// A **plot** (types/tile.h) is fixed: it occupies cells, and it is placed where it is. An **entity** is
// the other half of a mine: it has a **position** - not a cell, and not aligned to the tile grid
// (docs/GAME_DESIGN.md section 1.24) - it **moves**, it can **interact**, and it is **updated while the
// game runs**: every tick by default, or on a period it declares, or never at all.
//
// An entity is not a plot and does not derive from one. A plot is cells and an entity is a point, so
// the questions asked of them differ - "what covers this cell" against "what is near this point" - and
// so do the ways they are found (a chunk index against a quadtree, world.h). What the two share is
// identity: the kind and the id a save stores, with the name coming from the registry, exactly as
// types/tile.h does it for plots.
//
// Entities are held by the layer that runs them (world.h), which also indexes them, so "what is within
// this radius" costs what the answer is rather than what the layer holds. Because that index has to
// follow, **the layer is told when an entity moves**: an entity that is in a layer knows which one, and
// set_position()/move() tell it. "Every entity of a running layer is in the index" is therefore an
// invariant rather than a rule to remember.
#pragma once

#include <mine/registry.h>
#include <mine/types/update_cadence.h>

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <string>

namespace mine {

/// The layer that holds entities (world.h). Only the name is needed here: the definition would be a
/// cycle, and an entity only ever calls back into the layer it is already in.
class MineLayer;

} // namespace mine

namespace mine::types {

using t2d::f32;
using t2d::u32;
using t2d::Vec2;

/// Something a layer holds that is not on a cell: a cart, a drill head, a walking thing - whatever the
/// designer's content turns out to be. It has a position in map units, it moves, it can be asked to
/// interact, and it runs on a cadence it owns.
class Entity {
public:
    virtual ~Entity() = default;

    /// An entity is built where it is: what content it stands for, and its position. Content that needs
    /// logic of its own derives from this and inherits the constructor.
    Entity(ContentKind kind, ContentId id, Vec2 position);

    // --- what it is -------------------------------------------------------------------------------
    [[nodiscard]] ContentKind kind() const { return kind_; }
    /// The id the table in force handed out. Kept even when the content is gone, so content that comes
    /// back repairs the entity by name instead of losing it (the same rule plots follow).
    [[nodiscard]] ContentId id() const { return id_; }
    /// The name the registry has for this entity's content, or "#<id>" when the content is gone.
    [[nodiscard]] std::string name(const ContentRegistry& registry) const;
    /// The content behind it is not in the registry any more (a reload can take it away).
    [[nodiscard]] bool missing() const { return missing_; }
    void set_missing(bool missing) { missing_ = missing; }
    /// There is content behind it and that content is here.
    [[nodiscard]] bool valid() const { return id_ != kNoContent && !missing_; }
    /// Which entity this is, in the layer that holds it: handed out at spawn, never reused, and what a
    /// query is ordered by (world.h). Zero while it is not in a layer.
    [[nodiscard]] u32 serial() const { return serial_; }
    /// False between destroy() and the end of the tick that swept it: it is out of the index, and the
    /// storage still holds it because a callback may be walking it.
    [[nodiscard]] bool alive() const { return alive_; }
    /// The layer it is in, or nullptr while it is in none. What its own logic needs in order to ask the
    /// world something from inside its turn: what is near it, whether it may make or break something,
    /// who it can interact with (world.h).
    [[nodiscard]] MineLayer* layer() const { return layer_; }

    // --- where it is ------------------------------------------------------------------------------
    /// Its position in map units - one unit is one cell - and **not aligned to the tile grid**: 3.5,
    /// 2.25 is a position like any other, and nothing here rounds it (docs/GAME_DESIGN.md section
    /// 1.24). Whether a cell blocks it is the designer's rules (section 2), not this class's.
    [[nodiscard]] Vec2 position() const { return position_; }
    /// Moves it there, and tells the layer it is in so the index follows. An entity that is not in a
    /// layer yet simply moves.
    void set_position(Vec2 position);
    void move(Vec2 delta) { set_position(position_ + delta); }

    // --- when it runs -----------------------------------------------------------------------------
    /// True while the layer updates it. **Every tick by default**; an entity that says otherwise is
    /// still there, still moves and still interacts - what drives it is whoever holds it (a script, a
    /// player action, another entity).
    [[nodiscard]] bool auto_update() const { return auto_update_; }
    void set_auto_update(bool value) { auto_update_ = value; }
    /// How often it wants its turn: 0 means every tick, anything else is a period in seconds.
    [[nodiscard]] f32 period_seconds() const { return cadence_.period_seconds(); }
    void set_period_seconds(f32 seconds) { cadence_.set_period_seconds(seconds); }
    /// The time that has piled up since the last run.
    [[nodiscard]] f32 since_last_update() const { return cadence_.since_last_update(); }

    /// Hands it the frame's time and runs it when its turn has come; returns whether it ran. Nothing
    /// runs while auto_update() is false, and nothing runs once it is dead.
    bool advance(f32 delta_seconds);

    // --- what it does -----------------------------------------------------------------------------
    /// What it does when something interacts with it. The default does nothing and says so; content
    /// that reacts returns true. Both sides of an interaction are asked - the layer does that, so that
    /// neither side has to know the other's type (world.h) - and what an interaction *is* is the
    /// designer's (docs/GAME_DESIGN.md section 7.14).
    [[nodiscard]] virtual bool on_interact(Entity& other);

    /// True for something the pointer may pick up and put down somewhere else (types/drop.h). The same
    /// shape of question as is_floor() on a plot: being draggable is a **kind of thing**, not a property
    /// a data file hands out - so a caller asks this instead of comparing content names, and no entry in
    /// a designer's file can make a machine draggable by writing a field.
    [[nodiscard]] virtual bool draggable() const { return false; }

protected:
    /// What it does when its turn comes. \p elapsed is how much time this run accounts for: the
    /// period, or more when the frame was long (types/update_cadence.h). The default does nothing.
    virtual void on_update(f32 elapsed_seconds) { (void)elapsed_seconds; }

private:
    /// The layer owns the bookkeeping: it hands out the serial, says who is alive, and is told when a
    /// position changes so the index it keeps cannot go stale (world.h). Qualified, because an
    /// unqualified friend declaration here would declare mine::types::MineLayer - a different class.
    friend class mine::MineLayer;

    ContentKind kind_ = ContentKind::Item;
    ContentId id_ = kNoContent;
    bool missing_ = false;
    Vec2 position_{};
    /// Every tick unless it declares a period (docs/GAME_DESIGN.md section 1.24).
    UpdateCadence cadence_;
    bool auto_update_ = true;
    bool alive_ = true;
    u32 serial_ = 0;
    /// The layer that holds it, or nullptr while it is in none.
    MineLayer* layer_ = nullptr;
};

} // namespace mine::types
