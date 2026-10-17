#include <mine/types/warehouse.h>

#include <mine/types/tile_definition.h>

namespace mine::types {

usize Warehouse::capacity() const {
    // A **definition** attribute: the one record every warehouse of this entry shares holds it, and there
    // is no setter - nothing may change it after the plot is built (docs/GAME_DESIGN.md section 1.35). A
    // plot built without a definition (a test) falls back to the container it was made with.
    return definition() != nullptr ? definition()->capacity : container_.capacity();
}


Warehouse::Warehouse(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width, i32 height)
    : SceneTile(kind, id, layer, anchor, width, height) {}

} // namespace mine::types
