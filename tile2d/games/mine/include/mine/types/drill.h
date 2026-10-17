// Mine - the machine that mines what is under it.
//
// A **drill** is an entity plot (types/entity_tile.h): it occupies the cell it was placed on and it runs
// on a cadence. It stands **on top of** the ore it mines - the ore is on a tile layer below the drill's,
// which is what tile layers are for (docs/GAME_DESIGN.md sections 1.12, 1.26) - and every time its turn
// comes it throws **one drop** of that ore's content out to a neighbouring cell (types/drop.h).
//
// The period is not a number of the drill's own: it is the ore's **hardness** divided by the drill's
// **digging power**, and both of those are the designer's data - a `drill::` entry says how strong it
// is, an `ore::` entry says how hard it is and what it drops (docs/MODS.md). A drill with no ore under
// it, or an ore that does not say how hard it is or what it drops, is **reported and idle** rather than
// given a rate nobody wrote. The layer does that reading while it places the drill, because that is the
// one moment the registry (names to ids) and the definitions (the designer's numbers) are both in hand
// (world.h).
#pragma once

#include <mine/types/entity_tile.h>

#include <string>

namespace mine::types {

/// What a drill was told to do when the layer placed it: the ore it stands on, and the drop that ore
/// yields. A drill that was told nothing - no ore under it, or an ore that says nothing - is idle.
struct DrillJob {
    ContentKind ore_kind = ContentKind::Ore;
    ContentId ore_id = kNoContent;
    std::string ore_name;   ///< what a message and the layer dump call it
    ContentId drop_id = kNoContent;
    std::string drop_name;  ///< what a message and the layer dump call it
    /// How long one drop takes: the ore's hardness divided by the drill's power.
    f32 period_seconds = 0.0f;
};

/// A machine that mines the ore it stands on and throws the drops out to its sides.
class Drill : public EntityTile {
public:
    Drill(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1);

    /// The digging power its entry declared. A drill with none (0) is one the layer reports and leaves
    /// idle: without it there is no rate to compute.
    [[nodiscard]] f32 dig_power() const { return dig_power_; }
    /// Sets the power. A value that is not positive is read as "none", which is what it would mean.
    void set_dig_power(f32 power) { dig_power_ = power > 0.0f ? power : 0.0f; }

    /// What it mines and what it makes of it. Set by the layer when it places the drill (world.h).
    [[nodiscard]] const DrillJob& job() const { return job_; }
    void set_job(DrillJob job);

    /// How long one full turn of its top takes, in seconds - the entry's `spin`. It is a **definition**
    /// attribute, read from the one record every drill of that entry shares (docs/GAME_DESIGN.md 1.35); 0
    /// when the entry does not say, which is a drill whose top does not turn (section 1.38).
    [[nodiscard]] f32 spin_seconds() const;
    /// How far its top has turned after \p seconds of the world's clock, in radians: a full turn every
    /// `spin` seconds. It is a function of the time and not a number kept per drill, so every drill of an
    /// entry turns together and a frame that is late does not slow one down.
    [[nodiscard]] f32 spin_angle(f32 seconds) const;
    /// The point of its top's picture it turns about, in that picture's own coordinates: the entry's
    /// `spin_origin`, or the middle when it does not say (section 1.38).
    [[nodiscard]] t2d::Vec2 spin_origin() const;

    /// True when it has something to mine: a drop to make, and a period to make it on.
    [[nodiscard]] bool ready() const { return job_.drop_id != kNoContent && job_.period_seconds > 0.0f; }
    /// How many drops it has thrown out so far.
    [[nodiscard]] usize produced() const { return thrown_; }

protected:
    /// Its turn: throw out what the time it was handed is worth. The cadence hands a long frame's whole
    /// span, so a hitch that swallowed three periods produces three drops rather than one - which is
    /// what "a drill that accounts for five seconds can produce what five seconds are worth" means
    /// (types/entity_tile.h).
    void on_update(f32 elapsed_seconds) override;

private:
    /// Throws one drop to the next side of the drill, one cell away, turning a quarter each time: a
    /// drill that threw everything into the same cell would bury it. The map is the bound, the way it is
    /// for anything else that moves (pointer_drag.h).
    void throw_one();

    f32 dig_power_ = 0.0f;
    DrillJob job_{};
    usize thrown_ = 0;
};

} // namespace mine::types
