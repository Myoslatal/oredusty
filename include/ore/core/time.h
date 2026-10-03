// Ore framework - clocks, frame timing statistics and fixed timestep.
#pragma once

#include <ore/core/types.h>

#include <array>

namespace ore {

/// Monotonic wall clock.
class Clock {
public:
    [[nodiscard]] static f64 now_seconds();
    [[nodiscard]] static u64 now_nanos();
};

/// Rolling frame statistics. Call tick() exactly once per frame.
class FrameTimer {
public:
    static constexpr u32 kHistorySize = 120;

    FrameTimer();

    /// Advances the timer and records the frame time. Returns the delta in seconds.
    f32 tick();

    [[nodiscard]] f32 delta_seconds() const { return delta_; }
    [[nodiscard]] f32 frame_ms() const { return delta_ * 1000.0f; }
    [[nodiscard]] f32 average_ms() const;
    [[nodiscard]] f32 max_ms() const;
    [[nodiscard]] f32 min_ms() const;
    [[nodiscard]] f32 fps() const { return delta_ > 0.0f ? 1.0f / delta_ : 0.0f; }
    [[nodiscard]] f64 elapsed_seconds() const;
    [[nodiscard]] u64 frame_index() const { return frame_index_; }
    /// Smoothed frames per second over the recorded window.
    [[nodiscard]] f32 average_fps() const;

    void reset();

private:
    std::array<f32, kHistorySize> history_{};
    u32 cursor_ = 0;
    u32 count_ = 0;
    f64 start_ = 0.0;
    f64 last_ = 0.0;
    f32 delta_ = 0.0f;
    u64 frame_index_ = 0;
};

/// Accumulator that converts a variable frame delta into a whole number of fixed simulation steps.
class FixedTimestep {
public:
    explicit FixedTimestep(f32 step_seconds = 1.0f / 60.0f, u32 max_steps_per_frame = 8);

    /// Adds p delta_seconds to the accumulator and returns how many fixed steps to run.
    u32 advance(f32 delta_seconds);

    /// Interpolation factor in [0, 1) between the previous and the current simulation state.
    [[nodiscard]] f32 alpha() const { return step_ > 0.0f ? accumulator_ / step_ : 0.0f; }
    [[nodiscard]] f32 step() const { return step_; }
    void set_step(f32 seconds);
    [[nodiscard]] f64 simulated_seconds() const { return simulated_; }
    void reset();

private:
    f32 step_;
    f32 accumulator_ = 0.0f;
    f64 simulated_ = 0.0;
    u32 max_steps_;
    u32 dropped_steps_ = 0;
};

} // namespace ore
