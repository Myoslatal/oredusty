#include <mine/types/container.h>

#include <algorithm>

namespace mine::types {

usize Container::count_of(ContentKind kind, ContentId id) const {
    for (const ContainerSlot& slot : slots_) {
        if (slot.kind == kind && slot.id == id) return slot.count;
    }
    return 0;
}

ContainerSlot* Container::find(ContentKind kind, ContentId id) {
    for (ContainerSlot& slot : slots_) {
        if (slot.kind == kind && slot.id == id) return &slot;
    }
    return nullptr;
}

usize Container::add(ContentKind kind, ContentId id, usize count) {
    if (count == 0 || id == kNoContent) return 0;
    const usize taken = std::min(count, room());
    if (taken == 0) return 0;
    if (ContainerSlot* slot = find(kind, id); slot != nullptr) {
        slot->count += taken;
    } else {
        slots_.push_back(ContainerSlot{kind, id, taken});
    }
    total_ += taken;
    return taken;
}

usize Container::take(ContentKind kind, ContentId id, usize count) {
    if (count == 0) return 0;
    for (usize index = 0; index < slots_.size(); ++index) {
        if (slots_[index].kind != kind || slots_[index].id != id) continue;
        const usize given = std::min(count, slots_[index].count);
        slots_[index].count -= given;
        total_ -= given;
        // A slot that ran out is gone: a panel that kept listing "x0" would be listing what is not there.
        if (slots_[index].count == 0) slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
        return given;
    }
    return 0;
}

void Container::clear() {
    slots_.clear();
    total_ = 0;
}

} // namespace mine::types
