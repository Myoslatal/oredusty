#include <mine/types/item.h>

#include <format>
#include <utility>

namespace mine::types {

Item::Item(ContentId id, std::string image) : id_(id), image_(std::move(image)) {}

std::string Item::name(const ContentRegistry& registry) const {
    const ContentEntry* entry = registry.find(ContentKind::Item, id_);
    if (entry == nullptr) return std::format("#{}", id_);
    return entry->name;
}

bool Item::valid(const ContentRegistry& registry) const {
    return registry.find(ContentKind::Item, id_) != nullptr;
}

} // namespace mine::types
