#include <mine/types/update_cadence.h>

#include <cmath>

namespace mine::types {

std::optional<f32> UpdateCadence::due(f32 delta_seconds) {
    if (delta_seconds > 0.0f) elapsed_ += delta_seconds;
    // Every tick, or not yet: a period of zero is something that runs on every frame it is asked about.
    if (period_ > 0.0f && elapsed_ < period_) return std::nullopt;

    const f32 accounted = elapsed_;
    // What is left over stays in the accumulator, so a period does not drift by the fractions of the
    // frames it was fed. It can never be a whole period: the thing was just run.
    elapsed_ = period_ > 0.0f ? std::fmod(elapsed_, period_) : 0.0f;
    return accounted;
}

} // namespace mine::types
