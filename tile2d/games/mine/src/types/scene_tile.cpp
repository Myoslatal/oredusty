#include <mine/types/scene_tile.h>

#include <mine/types/tile_definition.h>

namespace mine::types {

const Building* SceneTile::building() const {
    // A **definition** attribute: the record the plot was made from holds it, and every plot of that
    // content entry reads the same one (docs/GAME_DESIGN.md section 1.35).
    if (definition_ == nullptr || definition_->building.empty()) return nullptr;
    return &definition_->building;
}


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
