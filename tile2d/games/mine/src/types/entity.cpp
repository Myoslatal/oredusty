#include <mine/types/entity.h>

#include <mine/world.h>

#include <format>

namespace mine::types {

Entity::Entity(ContentKind kind, ContentId id, Vec2 position) : kind_(kind), id_(id), position_(position) {}

std::string Entity::name(const ContentRegistry& registry) const {
    const ContentEntry* entry = registry.find(kind_, id_);
    if (entry != nullptr) return entry->name;
    return std::format("#{}", id_);
}

void Entity::set_position(Vec2 position) {
    if (position_ == position) return;
    const Vec2 before = position_;
    position_ = position;
    // The index is the layer's, and a position it does not know about is an entity a query cannot find:
    // so the move is what tells it, rather than a rule every caller would have to remember.
    if (layer_ != nullptr) layer_->reindex(*this, before);
}

bool Entity::advance(f32 delta_seconds) {
    if (!auto_update_ || !alive_) return false;
    const std::optional<f32> accounted = cadence_.due(delta_seconds);
    if (!accounted.has_value()) return false;
    on_update(*accounted);
    return true;
}

bool Entity::on_interact(Entity& other) {
    (void)other;
    return false;
}

} // namespace mine::types
