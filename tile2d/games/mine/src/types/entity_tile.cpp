#include <mine/types/entity_tile.h>

namespace mine::types {

bool EntityTile::advance(f32 delta_seconds) {
    // A plot that is still going up does not take its turn at all, and the time it spent going up is not
    // owed to it either: the cadence is not advanced, so its first turn is a whole period after it is
    // finished rather than at once (types/update_cadence.h, docs/GAME_DESIGN.md section 1.37).
    if (under_construction()) return false;
    const std::optional<f32> accounted = cadence_.due(delta_seconds);
    if (!accounted.has_value()) return false;
    on_update(*accounted);
    return true;
}

} // namespace mine::types
