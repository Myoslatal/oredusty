#include <mine/types/entity_tile.h>

#include <cmath>

namespace mine::types {

void EntityTile::set_period_seconds(f32 seconds) { period_ = seconds > 0.0f ? seconds : 0.0f; }

bool EntityTile::advance(f32 delta_seconds) {
    if (delta_seconds > 0.0f) elapsed_ += delta_seconds;
    // Every tick, or not yet: a period of zero is a plot that runs on every frame it is asked about.
    if (period_ > 0.0f && elapsed_ < period_) return false;

    const f32 accounted = elapsed_;
    // What is left over stays in the accumulator, so a period does not drift by the fractions of the
    // frames it was fed. It can never be a whole period: the plot was just run.
    elapsed_ = period_ > 0.0f ? std::fmod(elapsed_, period_) : 0.0f;
    on_update(accounted);
    return true;
}

} // namespace mine::types
