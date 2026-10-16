#include <mine/types/drag_into.h>

namespace mine::types {

bool DragInto::accepts(ContentKind kind, ContentId id) const {
    // Something with no content is not something to store, and a container with no room takes nothing.
    return id != kNoContent && into_->room() > 0;
}

bool DragInto::receive(ContentKind kind, ContentId id, usize count) {
    // All or nothing: the room is checked **before** anything goes in, because a container that took
    // four of the five things it was handed would have changed while the answer was still "no".
    if (!accepts(kind, id) || into_->room() < count) return false;
    return into_->add(kind, id, count) == count;
}

} // namespace mine::types
