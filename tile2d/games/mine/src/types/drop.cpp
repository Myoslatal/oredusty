#include <mine/types/drop.h>

namespace mine::types {

Drop::Drop(ContentKind kind, ContentId id, Vec2 position, usize count)
    : Entity(kind, id, position), count_(count < 1 ? 1 : count) {
    // "Not updated by the layer" is what a drop *is*, not a field of its entry - the same reason a floor
    // has no per-frame path (types/scene_tile.h). What drives a drop is the pointer, and nothing else
    // yet (docs/GAME_DESIGN.md section 1.25).
    set_auto_update(false);
}

void Drop::add(usize count) {
    // A pile grows and never shrinks through this: what takes an item out is set_count() (pointer_drag.h).
    if (count == 0) return;
    count_ += count;
}

void Drop::set_count(usize count) { count_ = count < 1 ? 1 : count; }

} // namespace mine::types
