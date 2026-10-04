// Mine - plots that do something.
//
// An **entity plot** is a functional plot: a machine, a belt, a drill, a demand terminal. It **is** a
// scene plot - it is scenery, it is refreshed the same way when the world around it changes - and on
// top of that it has to be updated while the game runs. How often is part of what it is: a fast
// machine runs every tick, a slow one every few seconds (docs/GAME_DESIGN.md section 1.13). That
// cadence lives here rather than in the simulation loop, so the loop is the same for every plot and a
// slow machine costs what a slow machine costs.
//
// Because an entity plot *is* a scene plot, "does this tick?" is not something to ask a plot at
// runtime: the layer knows what it created. A layer keeps the plots it ticks in their own list, which
// is where the saving is - the refresh pass can then walk everything and pay only for what is dirty,
// while the tick pass walks machines and nothing else.
//
// The cadence is time, not ticks: advance() is handed the frame's seconds and decides for itself
// whether its turn has come. A frame that took longer than a period runs the plot once, with the whole
// span in its hand - a plot cannot replay the ticks it missed, and pretending otherwise would make a
// hitch change what the mine produces. What the span means is the content's business: a drill that
// accounts for five seconds can produce what five seconds are worth.
#pragma once

#include <mine/types/scene_tile.h>

#include <t2d/core/types.h>

namespace mine::types {

using t2d::f32;

/// A plot that is updated while the game runs, on a cadence it owns. Everything a scene plot has -
/// being refreshed when the world changes, the dirty flag - it has too.
class EntityTile : public SceneTile {
public:
    ~EntityTile() override = default;

    /// A functional plot is placed where it is, like every plot. Content that needs logic of its own
    /// derives from this and inherits the constructor.
    EntityTile(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1)
        : SceneTile(kind, id, layer, anchor, width, height) {}

    /// How often it wants its turn: 0 means every tick, anything else is a period in seconds.
    [[nodiscard]] f32 period_seconds() const { return period_; }
    /// Sets the period. A negative one is read as "every tick", which is what a period below zero
    /// would mean anyway.
    void set_period_seconds(f32 seconds);

    /// Hands the plot the frame's time and runs it when its turn has come; returns whether it ran. The
    /// remainder is carried, so a period of one second does not drift by the fractions of a frame.
    bool advance(f32 delta_seconds);

    /// The time that has piled up since the last run.
    [[nodiscard]] f32 since_last_update() const { return elapsed_; }

protected:
    /// What the plot does when its turn comes. \p elapsed is how much time this run accounts for: the
    /// period, or more when the frame was long.
    virtual void on_update(f32 elapsed_seconds) { (void)elapsed_seconds; }

private:
    f32 period_ = 0.0f;
    f32 elapsed_ = 0.0f;
};

} // namespace mine::types
