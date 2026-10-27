#include <mine/pointer_drag.h>

#include <mine/types/drop.h>
#include <mine/types/warehouse.h>

namespace mine {

namespace {

/// The warehouse at \p cell, the topmost tile layer first: what a drop let go there lands on. Topmost
/// because that is the one a viewer sees - a plot on a higher tile layer is drawn over the ones below it.
[[nodiscard]] types::Warehouse* warehouse_at(MineLayer& layer, GridPos cell) {
    for (i32 tile_layer = layer.layer_count() - 1; tile_layer >= 0; --tile_layer) {
        auto* warehouse = dynamic_cast<types::Warehouse*>(layer.plot_at(tile_layer, cell));
        // A warehouse that is still going up takes no dragged drop: the pointer looks further down the
        // tile layers, and lets the drop go on the ground when there is nothing under it
        // (types/scene_tile.h, docs/GAME_DESIGN.md section 1.37).
        if (warehouse != nullptr && !warehouse->under_construction()) return warehouse;
    }
    return nullptr;
}

/// Lets \p entity go on the ground, joining a pile of its item nearby when there is one. After this the
/// entity may be gone - what a merge destroys is dead, not just moved - so the caller must not touch it
/// again (types/drop.h, docs/GAME_DESIGN.md section 1.43).
void release_into_a_pile(MineLayer& layer, types::Entity& entity) {
    auto* drop = dynamic_cast<types::Drop*>(&entity);
    if (drop == nullptr) return;   // something draggable that is not a drop has no pile to join
    (void)layer.merge_drop(*drop);
}

} // namespace

bool PointerDrag::begin(MineLayer& layer, t2d::Vec2 point, f32 radius) {
    (void)end();   // letting go of whatever was held, before taking hold of something else
    types::Entity* nearest = nullptr;
    f32 best_distance = 0.0f;
    layer.for_each_entity_within(point, radius, [&](types::Entity& entity) {
        if (!entity.draggable() || !entity.alive()) return;
        const f32 distance = t2d::length_squared(entity.position() - point);
        // The nearest one wins, and a tie goes to the lower serial: the same press picks the same thing
        // twice, whichever order the tree happened to visit them in.
        if (nearest != nullptr &&
            (distance > best_distance || (distance == best_distance && entity.serial() >= nearest->serial()))) {
            return;
        }
        nearest = &entity;
        best_distance = distance;
    });
    if (nearest == nullptr) return false;
    layer_ = &layer;
    // A **pile** is dragged one item at a time: the pile stays where it is with one fewer, and the pointer
    // carries a single drop made at the pile's own position (types/drop.h, docs/GAME_DESIGN.md 1.43).
    auto* pile = dynamic_cast<types::Drop*>(nearest);
    if (pile != nullptr && pile->count() > 1) {
        pile->set_count(pile->count() - 1);
        // Adopted **bare**, not through MineLayer::drop(): that one merges what it is handed into a nearby
        // pile of the same item, and this drop would be merged straight back into the one it came from.
        auto& taken =
            layer.adopt(std::make_unique<types::Drop>(pile->kind(), pile->id(), pile->position(), 1));
        held_ = &taken;
        offset_ = held_->position() - point;
        return true;
    }
    held_ = nearest;
    offset_ = nearest->position() - point;
    return true;
}

bool PointerDrag::move_to(t2d::Vec2 point) {
    if (held_ == nullptr || layer_ == nullptr) return false;
    if (!held_->alive()) {
        // Something destroyed it while it was held (another entity, a reload): there is nothing left to
        // move, and holding a dead entity would be a pointer that lies about what it is doing.
        end();
        return false;
    }
    const t2d::Vec2 wanted = point + offset_;
    const t2d::Vec2 clamped{
        t2d::clamp(wanted.x, 0.0f, static_cast<f32>(layer_->width())),
        t2d::clamp(wanted.y, 0.0f, static_cast<f32>(layer_->height())),
    };
    if (clamped == held_->position()) return false;
    held_->set_position(clamped);
    return true;
}

ReleaseResult PointerDrag::end() {
    types::Entity* released = held_;
    MineLayer* layer = layer_;
    layer_ = nullptr;
    held_ = nullptr;
    offset_ = t2d::Vec2{};
    if (released == nullptr || layer == nullptr) {
        last_release_ = DragRelease{};
        return ReleaseResult::Nothing;
    }

    // What it was let go on: the cell its own position is in - "drag it onto the warehouse" means the
    // thing lands *on* the warehouse's cells, and a position is a point rather than a cell (section 1.24).
    const GridPos cell{t2d::floor_to_i32(released->position().x), t2d::floor_to_i32(released->position().y)};
    last_release_.kind = released->kind();
    last_release_.id = released->id();

    types::Warehouse* warehouse = warehouse_at(*layer, cell);
    if (warehouse == nullptr) {
        // Let go on the ground: one item let go next to a pile of its item joins it (section 1.43). After
        // this \p released may be dead, so nothing below touches it on this path.
        release_into_a_pile(*layer, *released);
        last_release_.result = ReleaseResult::Left;
        return last_release_.result;
    }
    if (!warehouse->draginto().receive(*released)) {
        // No room: the drop stays where it was let go - or joins a pile of its item right next to it - and
        // the caller is told why (types/drag_into.h).
        release_into_a_pile(*layer, *released);
        last_release_.result = ReleaseResult::Refused;
        return last_release_.result;
    }
    // It is inside the warehouse now, so the thing on the ground is gone: the layer keeps what is in a
    // container, not a picture of it (world.h).
    layer->destroy(*released);
    last_release_.result = ReleaseResult::Stored;
    return last_release_.result;
}

} // namespace mine
