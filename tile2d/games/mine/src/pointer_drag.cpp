#include <mine/pointer_drag.h>

namespace mine {

bool PointerDrag::begin(MineLayer& layer, t2d::Vec2 point, f32 radius) {
    end();
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

void PointerDrag::end() {
    layer_ = nullptr;
    held_ = nullptr;
    offset_ = t2d::Vec2{};
}

} // namespace mine
