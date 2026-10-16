#include <mine/types/drop.h>

namespace mine::types {

Drop::Drop(ContentKind kind, ContentId id, Vec2 position) : Entity(kind, id, position) {
    // "Not updated by the layer" is what a drop *is*, not a field of its entry - the same reason a floor
    // has no per-frame path (types/scene_tile.h). What drives a drop is the pointer, and nothing else
    // yet (docs/GAME_DESIGN.md section 1.25).
    set_auto_update(false);
}

} // namespace mine::types
