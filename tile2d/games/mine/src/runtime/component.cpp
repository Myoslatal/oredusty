#include <mine/runtime/component.h>

#include <format>
#include <utility>

namespace mine::runtime {

ComponentType::ComponentType(const ComponentDesc& desc, std::string name)
    : type_id_(desc.type_id), name_(std::move(name)), version_(desc.version),
      instance_size_(desc.instance_size), instance_align_(desc.instance_align), abi_hash_(desc.abi_hash),
      construct_(desc.construct), destruct_(desc.destruct) {
    if (desc.struct_size >= sizeof(ComponentDesc)) {
        save_ = desc.save;
        load_ = desc.load;
    }
    interfaces_.reserve(desc.interface_count);
    for (u32 index = 0; index < desc.interface_count; ++index) {
        const InterfaceBinding& binding = desc.interfaces[index];
        interfaces_.push_back(Slot{binding.interface_id, binding.table, binding.table, nullptr});
    }
}

const ComponentType::Slot* ComponentType::find_slot(InterfaceId interface_id) const {
    for (const Slot& slot : interfaces_) {
        if (slot.interface_id == interface_id) return &slot;
    }
    return nullptr;
}

bool ComponentType::has_interface(InterfaceId interface_id) const {
    return find_slot(interface_id) != nullptr;
}

const void* ComponentType::active_interface(InterfaceId interface_id) const {
    const Slot* slot = find_slot(interface_id);
    return slot != nullptr ? slot->active : nullptr;
}

const void* ComponentType::previous_interface(InterfaceId interface_id) const {
    const Slot* slot = find_slot(interface_id);
    return slot != nullptr ? slot->previous : nullptr;
}

const void* ComponentType::base_interface(InterfaceId interface_id) const {
    const Slot* slot = find_slot(interface_id);
    return slot != nullptr ? slot->base : nullptr;
}

void ComponentType::restore_interface(InterfaceId interface_id) {
    for (Slot& slot : interfaces_) {
        if (slot.interface_id != interface_id) continue;
        slot.active = slot.base;
        slot.previous = nullptr;
        return;
    }
}

bool ComponentType::refresh(const ComponentDesc& desc, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    if (error != nullptr) error->clear();
    if (desc.type_id != type_id_) {
        return fail(std::format("component type {} cannot refresh type {}", desc.type_id, type_id_));
    }
    if (desc.name == nullptr || name_ != desc.name) {
        return fail(std::format("component type {} cannot change its name", type_id_));
    }
    if (desc.version != version_) {
        return fail(std::format("component type {} ('{}') is version {}, not {}", type_id_, name_, version_,
                                desc.version));
    }
    if (desc.instance_size != instance_size_ || desc.instance_align != instance_align_) {
        return fail(std::format("component type {} ('{}') cannot change its data layout", type_id_, name_));
    }
    if (desc.interface_count != interfaces_.size()) {
        return fail(std::format("component type {} ('{}') has {} interface(s), not {}", type_id_, name_,
                                interfaces_.size(), desc.interface_count));
    }
    for (u32 index = 0; index < desc.interface_count; ++index) {
        const InterfaceBinding& binding = desc.interfaces[index];
        bool found = false;
        for (Slot& slot : interfaces_) {
            if (slot.interface_id != binding.interface_id) continue;
            if (binding.table == nullptr) {
                return fail(std::format("component type {} ('{}') binds interface {} to a null table",
                                        type_id_, name_, binding.interface_id));
            }
            slot.base = binding.table;
            slot.active = binding.table;
            slot.previous = nullptr;
            found = true;
            break;
        }
        if (!found) {
            return fail(std::format("component type {} ('{}') cannot add interface {}", type_id_, name_,
                                    binding.interface_id));
        }
    }
    const bool full_desc = desc.struct_size >= sizeof(ComponentDesc);
    if (full_desc && ((desc.save == nullptr) != (desc.load == nullptr))) {
        return fail(std::format("component type {} ('{}') must provide both save and load, or neither",
                                type_id_, name_));
    }
    abi_hash_ = desc.abi_hash;
    construct_ = desc.construct;
    destruct_ = desc.destruct;
    if (full_desc) {
        save_ = desc.save;
        load_ = desc.load;
    } else {
        save_ = nullptr;
        load_ = nullptr;
    }
    return true;
}

bool ComponentType::override_interface(InterfaceId interface_id, const void* replacement,
                                       const void** previous) {
    if (replacement == nullptr) return false;
    for (Slot& slot : interfaces_) {
        if (slot.interface_id != interface_id) continue;
        if (previous != nullptr) *previous = slot.active;
        slot.previous = slot.active;
        slot.active = replacement;
        return true;
    }
    return false;
}

} // namespace mine::runtime
