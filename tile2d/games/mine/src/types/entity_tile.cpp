#include <mine/types/entity_tile.h>

namespace mine::types {

bool EntityTile::advance(f32 delta_seconds) {
    const std::optional<f32> accounted = cadence_.due(delta_seconds);
    if (!accounted.has_value()) return false;
    on_update(*accounted);
    return true;
}

} // namespace mine::types
