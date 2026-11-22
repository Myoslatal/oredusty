#include <mine/types/container.h>

#include <algorithm>
#include <limits>

#include <mine/runtime/content/components.h>

namespace mine::types {
namespace {

[[nodiscard]] t2d::u32 as_u32(usize value) {
    return static_cast<t2d::u32>(std::min<usize>(value, std::numeric_limits<t2d::u32>::max()));
}

} // namespace

Container::Container(void* data, const void* shared, const runtime::content::ContainerFunctions* functions)
    : data_(data), shared_(shared), functions_(functions) {}

usize Container::capacity() const {
    const auto* definition = static_cast<const runtime::content::ContainerShared*>(shared_);
    return definition != nullptr ? definition->capacity : 0;
}

usize Container::total() const {
    return functions_ != nullptr ? functions_->total(shared_, data_) : 0;
}

usize Container::room() const {
    return functions_ != nullptr ? functions_->room(shared_, data_) : 0;
}

usize Container::count_of(ContentKind kind, ContentId id) const {
    if (functions_ == nullptr) return 0;
    const runtime::content::ContainerSlot* slots = functions_->slots(shared_, data_);
    const t2d::u32 count = functions_->count(shared_, data_);
    for (t2d::u32 index = 0; index < count; ++index) {
        if (slots[index].kind == static_cast<t2d::u32>(kind) && slots[index].id == id) {
            return slots[index].count;
        }
    }
    return 0;
}

std::vector<ContainerSlot> Container::slots() const {
    std::vector<ContainerSlot> result;
    if (functions_ == nullptr) return result;
    const runtime::content::ContainerSlot* slots = functions_->slots(shared_, data_);
    const t2d::u32 count = functions_->count(shared_, data_);
    result.reserve(count);
    for (t2d::u32 index = 0; index < count; ++index) {
        result.push_back(
            ContainerSlot{static_cast<ContentKind>(slots[index].kind), slots[index].id, slots[index].count});
    }
    return result;
}

usize Container::add(ContentKind kind, ContentId id, usize count) {
    if (functions_ == nullptr || count == 0) return 0;
    const t2d::u32 wanted = as_u32(count);
    return functions_->add(shared_, data_, static_cast<t2d::u32>(kind), id, wanted);
}

usize Container::take(ContentKind kind, ContentId id, usize count) {
    if (functions_ == nullptr || count == 0) return 0;
    const t2d::u32 wanted = as_u32(count);
    return functions_->take(shared_, data_, static_cast<t2d::u32>(kind), id, wanted);
}

} // namespace mine::types
