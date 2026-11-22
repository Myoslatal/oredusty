#include <mine/runtime/component_registry.h>

#include <format>
#include <utility>

namespace mine::runtime {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

} // namespace

const ComponentType* ComponentRegistry::find(ComponentTypeId type_id) const {
    for (const std::unique_ptr<ComponentType>& type : types_) {
        if (type->type_id() == type_id) return type.get();
    }
    return nullptr;
}

const ComponentType* ComponentRegistry::find_by_name(std::string_view name) const {
    for (const std::unique_ptr<ComponentType>& type : types_) {
        if (type->name() == name) return type.get();
    }
    return nullptr;
}

ComponentType* ComponentRegistry::find_mutable(ComponentTypeId type_id) {
    for (const std::unique_ptr<ComponentType>& type : types_) {
        if (type->type_id() == type_id) return type.get();
    }
    return nullptr;
}

const ComponentType* ComponentRegistry::register_component(const ComponentDesc& desc, std::string* error) {
    if (error != nullptr) error->clear();
    if (desc.struct_size < kComponentDescPrefixSize) {
        set_error(error, std::format("component descriptor is {} bytes, this runtime needs at least {}",
                                     desc.struct_size, kComponentDescPrefixSize));
        return nullptr;
    }
    if (desc.struct_size >= sizeof(ComponentDesc) && ((desc.save == nullptr) != (desc.load == nullptr))) {
        set_error(error, std::format("component type {} must provide both save and load, or neither",
                                     desc.type_id));
        return nullptr;
    }
    if (desc.type_id == kNoComponentType) {
        set_error(error, "component type id 0 is reserved");
        return nullptr;
    }
    if (desc.name == nullptr) {
        set_error(error, std::format("component type {} has no name", desc.type_id));
        return nullptr;
    }
    if (desc.name[0] == '\0') {
        set_error(error, std::format("component type {} has an empty name", desc.type_id));
        return nullptr;
    }
    if (desc.version == 0) {
        set_error(error, std::format("component type {} ('{}') has version 0", desc.type_id, desc.name));
        return nullptr;
    }
    if (desc.instance_size == 0) {
        set_error(error,
                  std::format("component type {} ('{}') has instance size 0", desc.type_id, desc.name));
        return nullptr;
    }
    if (!t2d::is_power_of_two(desc.instance_align)) {
        set_error(error, std::format("component type {} ('{}') has alignment {}, which is not a power of two",
                                     desc.type_id, desc.name, desc.instance_align));
        return nullptr;
    }
    if (desc.interface_count != 0 && desc.interfaces == nullptr) {
        set_error(error, std::format("component type {} ('{}') declares {} interface(s) and no array",
                                     desc.type_id, desc.name, desc.interface_count));
        return nullptr;
    }
    if (find(desc.type_id) != nullptr) {
        set_error(error, std::format("component type {} is already registered", desc.type_id));
        return nullptr;
    }
    if (find_by_name(desc.name) != nullptr) {
        set_error(error, std::format("component name '{}' is already registered", desc.name));
        return nullptr;
    }
    for (u32 index = 0; index < desc.interface_count; ++index) {
        const InterfaceBinding& binding = desc.interfaces[index];
        if (binding.interface_id == kNoInterface) {
            set_error(error,
                      std::format("component type {} ('{}') binds interface 0", desc.type_id, desc.name));
            return nullptr;
        }
        if (binding.table == nullptr) {
            set_error(error, std::format("component type {} ('{}') binds interface {} to a null table",
                                         desc.type_id, desc.name, binding.interface_id));
            return nullptr;
        }
        for (u32 other = 0; other < index; ++other) {
            if (desc.interfaces[other].interface_id != binding.interface_id) continue;
            set_error(error, std::format("component type {} ('{}') binds interface {} twice", desc.type_id,
                                         desc.name, binding.interface_id));
            return nullptr;
        }
    }

    types_.push_back(std::unique_ptr<ComponentType>(new ComponentType(desc, std::string(desc.name))));
    return types_.back().get();
}

void ComponentRegistry::restore_interface(ComponentTypeId type_id, InterfaceId interface_id) {
    if (ComponentType* type = find_mutable(type_id); type != nullptr) {
        type->restore_interface(interface_id);
    }
}

bool ComponentRegistry::refresh_component(const ComponentDesc& desc, std::string* error) {
    if (error != nullptr) error->clear();
    if (desc.struct_size < kComponentDescPrefixSize) {
        set_error(error, std::format("component descriptor is {} bytes, this runtime needs at least {}",
                                     desc.struct_size, kComponentDescPrefixSize));
        return false;
    }
    if (desc.struct_size >= sizeof(ComponentDesc) && ((desc.save == nullptr) != (desc.load == nullptr))) {
        set_error(error, std::format("component type {} must provide both save and load, or neither",
                                     desc.type_id));
        return false;
    }
    if (desc.interface_count != 0 && desc.interfaces == nullptr) {
        set_error(error, std::format("component type {} declares {} interface(s) and no array", desc.type_id,
                                     desc.interface_count));
        return false;
    }
    ComponentType* type = find_mutable(desc.type_id);
    if (type == nullptr) {
        if (error != nullptr) *error = std::format("component type {} is not registered", desc.type_id);
        return false;
    }
    if (desc.name == nullptr || type->name() != desc.name) {
        if (error != nullptr) *error = std::format("component type {} has another name", desc.type_id);
        return false;
    }
    return type->refresh(desc, error);
}

bool ComponentRegistry::override_interface(ComponentTypeId type_id, InterfaceId interface_id,
                                           const void* replacement, const void** previous,
                                           std::string* error) {
    if (error != nullptr) error->clear();
    if (previous != nullptr) *previous = nullptr;
    if (replacement == nullptr) {
        set_error(error, "an interface override needs a replacement table");
        return false;
    }
    ComponentType* type = find_mutable(type_id);
    if (type == nullptr) {
        set_error(error, std::format("component type {} is not registered", type_id));
        return false;
    }
    if (!type->has_interface(interface_id)) {
        set_error(error, std::format("component type {} ('{}') has no interface {}", type_id, type->name(),
                                     interface_id));
        return false;
    }
    return type->override_interface(interface_id, replacement, previous);
}

} // namespace mine::runtime
