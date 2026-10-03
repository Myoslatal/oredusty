#include <t2d/core/time.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace t2d {

u64 now_ms() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

f64 now_seconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

void sleep_until_ms(u64 deadline_ms) {
    const u64 current = now_ms();
    if (deadline_ms <= current) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(deadline_ms - current));
}

void sleep_ms(u32 milliseconds) {
    if (milliseconds == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

FixedTimestep::FixedTimestep(u32 ticks_per_second, u32 max_catch_up_ticks)
    : ticks_per_second_(ticks_per_second == 0 ? 60u : ticks_per_second), max_catch_up_(std::max(1u, max_catch_up_ticks)) {
    tick_seconds_ = 1.0f / static_cast<f32>(ticks_per_second_);
}

u32 FixedTimestep::advance(f32 delta_seconds) {
    if (delta_seconds > 0.0f) accumulator_ += delta_seconds;
    u32 ticks = 0;
    while (accumulator_ >= tick_seconds_ && ticks < max_catch_up_) {
        accumulator_ -= tick_seconds_;
        ++ticks;
    }
    if (accumulator_ >= tick_seconds_) {
        const f32 backlog = accumulator_;
        accumulator_ = std::fmod(accumulator_, tick_seconds_);
        dropped_ticks_ += static_cast<u64>(backlog / tick_seconds_);
    }
    return ticks;
}

void FixedTimestep::reset() {
    accumulator_ = 0.0f;
    dropped_ticks_ = 0;
}

ScopedTimer::ScopedTimer(std::string_view name) : name_(name), start_ms_(now_ms()) {}

ScopedTimer::~ScopedTimer() {
    T2D_TRACE("{} took {} ms", name_, now_ms() - start_ms_);
}

} // namespace t2d
