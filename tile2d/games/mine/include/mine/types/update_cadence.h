// Mine - how often something that runs wants its turn.
//
// A thing that runs owns a **cadence**: every tick by default, or a period in seconds it declares
// (docs/GAME_DESIGN.md sections 1.13, 1.24). The rule lives here once, because two kinds of thing run -
// a functional plot (types/entity_tile.h) and an entity (types/entity.h) - and a mine must not tick its
// machines on one rule and the things moving between them on another.
//
// The cadence is time, not ticks: due() is handed the frame's seconds and decides for itself whether
// its turn has come. A frame longer than a period runs the thing **once**, with the whole span in its
// hand - it cannot replay the ticks it missed, and pretending otherwise would make a hitch change what
// the mine produces. What the span means is the content's business: a drill handed five seconds can
// produce what five seconds are worth.
#pragma once

#include <t2d/core/types.h>

#include <optional>

namespace mine::types {

using t2d::f32;

/// A period, and the time that has piled up since the last run.
class UpdateCadence {
public:
    /// How often it wants its turn: 0 means every tick, anything else is a period in seconds.
    [[nodiscard]] f32 period_seconds() const { return period_; }
    /// Sets the period. A negative one is read as "every tick", which is what a period below zero
    /// would mean anyway.
    void set_period_seconds(f32 seconds) { period_ = seconds > 0.0f ? seconds : 0.0f; }

    /// The time that has piled up since the last run.
    [[nodiscard]] f32 since_last_update() const { return elapsed_; }
    /// Puts the remainder back, which is how a save restores "it had half a period to go". A negative
    /// one is read as nothing piled up, because time does not run that way.
    void set_since_last_update(f32 seconds) { elapsed_ = seconds > 0.0f ? seconds : 0.0f; }

    /// Hands it the frame's time and returns the span this run accounts for, or nothing when its turn
    /// has not come. The remainder stays in the accumulator, so a period of one second does not drift
    /// by the fractions of the frames it was fed.
    [[nodiscard]] std::optional<f32> due(f32 delta_seconds);

private:
    f32 period_ = 0.0f;
    f32 elapsed_ = 0.0f;
};

} // namespace mine::types
