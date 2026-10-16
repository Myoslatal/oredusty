#include <mine/types/warehouse.h>

namespace mine::types {

Warehouse::Warehouse(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width, i32 height)
    : SceneTile(kind, id, layer, anchor, width, height) {}

} // namespace mine::types
