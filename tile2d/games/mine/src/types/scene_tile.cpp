#include <mine/types/scene_tile.h>

#include <mine/types/tile_definition.h>

namespace mine::types {

const Building* SceneTile::building() const {
    // A **definition** attribute: the record the plot was made from holds it, and every plot of that
    // content entry reads the same one (docs/GAME_DESIGN.md section 1.35).
    if (definition_ == nullptr || definition_->building.empty()) return nullptr;
    return &definition_->building;
}


TilePreview SceneTile::get_preview(t2d::f32 seconds) const {
    // Scenery has one picture and nothing about it moves, so its complete preview is that picture or
    // nothing: a plot made without a definition, or from an entry that names no "image", has no art to
    // draw. Which picture it is, is a **definition** attribute - one record every plot of the entry
    // reads (docs/GAME_DESIGN.md section 1.35) - and whether it is turned around is the plot's own.
    (void)seconds;
    if (definition_ == nullptr || definition_->image.empty()) return {};
    PreviewImage picture;
    picture.mirrored = mirrored();
    TilePreview preview;
    preview.images.push_back(picture);
    return preview;
}

bool SceneTile::refresh() {
    // A plot that is still going up is not brought up to date: there is nothing to bring up yet. The dirty
    // flag is **kept**, not cleared - the pass after it is finished does the work it was owed
    // (docs/GAME_DESIGN.md section 1.37).
    if (!dirty_ || under_construction_) return false;
    // Cleared before the hook runs: what the plot computes is the world as it is now, and a plot that
    // marks itself dirty from inside its own refresh - because it just changed something - has to be
    // able to say so without the flag being cleared afterwards.
    dirty_ = false;
    on_refresh();
    return true;
}

} // namespace mine::types
