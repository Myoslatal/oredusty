#include <mine/runtime/object_data.h>

#include <cstring>
#include <new>

namespace mine::runtime {

ObjectData::ObjectData(const Archetype& archetype) : archetype_(&archetype) {
    if (archetype_->total_size() != 0) {
        // Aligned to the archetype's alignment rather than to max_align_t: a component may ask for
        // more than the default new alignment, and every component offset is relative to this block.
        data_ = static_cast<std::byte*>(
            ::operator new(archetype_->total_size(), std::align_val_t(archetype_->alignment())));
        // Deterministic before construction: a component that reads its own bytes sees zeroes, not
        // whatever the allocator handed out.
        std::memset(data_, 0, archetype_->total_size());
    }
    for (const Archetype::ComponentLayout& component : archetype_->components()) {
        component.type->construct_instance(data_ + component.offset);
    }
}

ObjectData::~ObjectData() {
    const ConstSpan<Archetype::ComponentLayout> components = archetype_->components();
    for (usize index = components.size(); index > 0; --index) {
        const Archetype::ComponentLayout& component = components[index - 1];
        component.type->destroy_instance(data_ + component.offset);
    }
    if (data_ != nullptr) {
        ::operator delete(data_, std::align_val_t(archetype_->alignment()));
    }
}

void* ObjectData::instance(ComponentTypeId type_id) {
    const Archetype::ComponentLayout* component = archetype_->find(type_id);
    return component != nullptr ? data_ + component->offset : nullptr;
}

const void* ObjectData::instance(ComponentTypeId type_id) const {
    const Archetype::ComponentLayout* component = archetype_->find(type_id);
    return component != nullptr ? data_ + component->offset : nullptr;
}

const void* ObjectData::shared(ComponentTypeId type_id) const {
    return archetype_->shared(type_id);
}

const void* ObjectData::interface(ComponentTypeId type_id, InterfaceId interface_id) const {
    const ComponentType* type = archetype_->type(type_id);
    return type != nullptr ? type->active_interface(interface_id) : nullptr;
}

} // namespace mine::runtime
