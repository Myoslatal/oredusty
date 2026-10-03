// Tile2D - monotonic clocks and the fixed timestep used by the server thread.
#pragma once

#include <t2d/core/types.h>

namespace t2d {

/// Monotonic milliseconds since an arbitrary epoch. Every timing decision (ticks, KCP, timeouts)
/// uses this clock.
[[nodiscard]] u64 now_ms();
[[nodiscard]] f64 now_seconds();

/// Sleeps until \p deadline_ms (absolute, from now_ms()). Returns immediately when it has passed.
void sleep_until_ms(u64 deadline_ms);
void sleep_ms(u32 milliseconds);

/// Fixed timestep accumulator with a catch-up limit, used to decouple the render/input rate from
/// the simulation rate.
class FixedTimestep {
public:
    explicit FixedTimestep(u32 ticks_per_second = 60, u32 max_catch_up_ticks = 8);

    /// Adds \p delta_seconds and returns how many simulation ticks to run this frame.
    [[nodiscard]] u32 advance(f32 delta_seconds);
    [[nodiscard]] f32 tick_seconds() const { return tick_seconds_; }
    [[nodiscard]] u32 ticks_per_second() const { return ticks_per_second_; }
    [[nodiscard]] f32 alpha() const { return accumulator_ / tick_seconds_; }
    [[nodiscard]] u64 dropped_ticks() const { return dropped_ticks_; }
    void reset();

private:
    f32 tick_seconds_ = 1.0f / 60.0f;
    f32 accumulator_ = 0.0f;
    u32 ticks_per_second_ = 60;
    u32 max_catch_up_ = 8;
    u64 dropped_ticks_ = 0;
};

/// Simple scoped timer that reports the elapsed time in the destructor (debug builds only).
class ScopedTimer {
public:
    explicit ScopedTimer(std::string_view name);
    ~ScopedTimer();

private:
    std::string_view name_;
    u64 start_ms_ = 0;
};

} // namespace t2d
