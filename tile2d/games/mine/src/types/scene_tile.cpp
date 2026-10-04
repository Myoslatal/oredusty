#include <mine/types/scene_tile.h>

namespace mine::types {

bool SceneTile::refresh() {
    if (!dirty_) return false;
    // Cleared before the hook runs: what the plot computes is the world as it is now, and a plot that
    // marks itself dirty from inside its own refresh - because it just changed something - has to be
    // able to say so without the flag being cleared afterwards.
    dirty_ = false;
    on_refresh();
    return true;
}

} // namespace mine::types
