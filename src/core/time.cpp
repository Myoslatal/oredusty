#include <ore/core/time.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ore {

f64 Clock::now_seconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

u64 Clock::now_nanos() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

FrameTimer::FrameTimer() { reset(); }

void FrameTimer::reset() {
    history_.fill(0.0f);
    cursor_ = 0;
    count_ = 0;
    start_ = Clock::now_seconds();
    last_ = start_;
    delta_ = 0.0f;
    frame_index_ = 0;
}

f32 FrameTimer::tick() {
    const f64 now = Clock::now_seconds();
    delta_ = static_cast<f32>(now - last_);
    last_ = now;
    history_[cursor_] = delta_ * 1000.0f;
    cursor_ = (cursor_ + 1) % kHistorySize;
    count_ = std::min(count_ + 1, kHistorySize);
    ++frame_index_;
    return delta_;
}

f32 FrameTimer::average_ms() const {
    if (count_ == 0) return 0.0f;
    f32 total = 0.0f;
    for (u32 i = 0; i < count_; ++i) total += history_[i];
    return total / static_cast<f32>(count_);
}

f32 FrameTimer::max_ms() const {
    f32 value = 0.0f;
    for (u32 i = 0; i < count_; ++i) value = std::max(value, history_[i]);
    return value;
}

f32 FrameTimer::min_ms() const {
    if (count_ == 0) return 0.0f;
    f32 value = history_[0];
    for (u32 i = 0; i < count_; ++i) value = std::min(value, history_[i]);
    return value;
}

f32 FrameTimer::average_fps() const {
    const f32 avg = average_ms();
    return avg > 0.0f ? 1000.0f / avg : 0.0f;
}

f64 FrameTimer::elapsed_seconds() const { return last_ - start_; }

FixedTimestep::FixedTimestep(f32 step_seconds, u32 max_steps_per_frame)
    : step_(step_seconds), max_steps_(std::max(1u, max_steps_per_frame)) {}

void FixedTimestep::set_step(f32 seconds) {
    step_ = seconds > 0.0f ? seconds : step_;
    accumulator_ = 0.0f;
}

void FixedTimestep::reset() {
    accumulator_ = 0.0f;
    simulated_ = 0.0;
    dropped_steps_ = 0;
}

u32 FixedTimestep::advance(f32 delta_seconds) {
    if (delta_seconds > 0.0f) accumulator_ += delta_seconds;
    u32 steps = 0;
    while (accumulator_ >= step_ && steps < max_steps_) {
        accumulator_ -= step_;
        simulated_ += static_cast<f64>(step_);
        ++steps;
    }
    if (accumulator_ >= step_) {
        // The frame took longer than max_steps_ * step_: drop the backlog instead of spiralling.
        dropped_steps_ += static_cast<u32>(accumulator_ / step_);
        accumulator_ = static_cast<f32>(std::fmod(static_cast<f64>(accumulator_), static_cast<f64>(step_)));
    }
    return steps;
}

} // namespace ore
