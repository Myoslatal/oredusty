// Tile2D - movement tuning shared by the server simulation and the client prediction.
//
// These constants are part of the protocol: a client that predicts with different values than the
// server will be corrected every tick (and the tests that verify "prediction == server state" would
// fail), so they live in one header used by both sides.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

namespace t2d {

struct PlayerTuning {
    Vec2 size{10.0f, 14.0f};      ///< AABB of a player, in world units (pixels)
    f32 move_speed = 78.0f;       ///< px/s walking
    f32 run_multiplier = 1.55f;
    f32 ground_accel = 700.0f;
    f32 air_accel = 420.0f;
    f32 ground_friction = 900.0f;
    f32 air_drag = 90.0f;
    f32 gravity = 900.0f;
    f32 max_fall_speed = 460.0f;
    f32 jump_velocity = 245.0f;
    f32 jump_cut_multiplier = 0.45f;   ///< velocity retained when the jump button is released early
    f32 coyote_seconds = 0.1f;         ///< grace period after leaving a ledge
    f32 jump_buffer_seconds = 0.1f;    ///< grace period for pressing jump before landing
    f32 hazard_respawn_seconds = 0.6f;
    f32 step_epsilon = 0.01f;
};

/// Tick rate of the authoritative simulation. The client predicts with the same rate.
inline constexpr u32 kTickRate = 60;
inline constexpr f32 kTickSeconds = 1.0f / static_cast<f32>(kTickRate);

[[nodiscard]] constexpr u32 seconds_to_ticks(f32 seconds) {
    return static_cast<u32>(seconds * static_cast<f32>(kTickRate) + 0.5f);
}

} // namespace t2d
