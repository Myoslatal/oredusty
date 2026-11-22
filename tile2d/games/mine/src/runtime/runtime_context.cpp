#include <mine/runtime/runtime_context.h>

#include <mine/runtime/content/builtin.h>

#include <t2d/core/log.h>

#include <cstddef>
#include <format>

namespace mine::runtime {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

[[nodiscard]] bool valid_override_table(const void* replacement, std::string* error);

[[nodiscard]] bool valid_component_desc(const ComponentDesc& desc, std::string* error) {
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
    if (desc.abi_hash != kModComponentAbiHash) {
        set_error(error, std::format("component type {} was built for ABI {:#x}, this runtime is {:#x}",
                                     desc.type_id, desc.abi_hash, kModComponentAbiHash));
        return false;
    }
    if (desc.interface_count != 0 && desc.interfaces == nullptr) {
        set_error(error, std::format("component type {} declares {} interface(s) and no array", desc.type_id,
                                     desc.interface_count));
        return false;
    }
    for (u32 index = 0; index < desc.interface_count; ++index) {
        if (!valid_override_table(desc.interfaces[index].table, error)) return false;
    }
    return true;
}

[[nodiscard]] bool valid_override_table(const void* replacement, std::string* error) {
    if (replacement == nullptr) {
        set_error(error, "an interface override needs a replacement table");
        return false;
    }
    const auto* header = static_cast<const InterfaceTableHeader*>(replacement);
    if (header->struct_size < sizeof(InterfaceTableHeader)) {
        set_error(error, std::format("interface table is {} bytes, at least {} are needed",
                                     header->struct_size, sizeof(InterfaceTableHeader)));
        return false;
    }
    if (header->version == 0) {
        set_error(error, "interface table version 0 is not valid");
        return false;
    }
    return true;
}

} // namespace

RuntimeContext::RuntimeContext() {
    std::vector<std::string> errors;
    if (!content::register_builtin_content(components_, tile_types_, &errors)) {
        for (const std::string& error : errors)
            T2D_ERROR("runtime: built-in content: {}", error);
    }
}

RuntimeContext::~RuntimeContext() = default;

bool RuntimeContext::register_mod_component(const ComponentDesc& desc, std::string_view owner,
                                            std::string* error) {
    if (error != nullptr) error->clear();
    if (!valid_component_desc(desc, error)) return false;
    const ComponentType* existing = components_.find(desc.type_id);
    if (existing != nullptr) {
        const auto known = component_owners_.find(desc.type_id);
        if (known == component_owners_.end() || known->second != owner) {
            set_error(error, std::format("component type {} is already registered", desc.type_id));
            return false;
        }
        return components_.refresh_component(desc, error);
    }
    if (components_.find_by_name(desc.name) != nullptr) {
        set_error(error, std::format("component name '{}' is already registered", desc.name));
        return false;
    }
    if (components_.register_component(desc, error) == nullptr) return false;
    component_owners_.emplace(desc.type_id, std::string(owner));
    return true;
}

bool RuntimeContext::register_mod_tile_type(const TileTypeDesc& desc, std::string_view owner,
                                            std::string* error) {
    if (error != nullptr) error->clear();
    if (desc.struct_size < kTileTypeDescPrefixSize) {
        set_error(error, std::format("tile type descriptor is {} bytes, this runtime needs at least {}",
                                     desc.struct_size, kTileTypeDescPrefixSize));
        return false;
    }
    const u32 shared_count = desc.struct_size >= sizeof(TileTypeDesc) ? desc.shared_count : 0u;
    if (shared_count != 0 && (shared_count != desc.component_count || desc.shared == nullptr)) {
        set_error(error, std::format("tile type {} ('{}') has an unusable shared array", desc.type_id,
                                     desc.name != nullptr ? desc.name : "?"));
        return false;
    }
    if (desc.component_count != 0 && desc.components == nullptr) {
        set_error(error, std::format("tile type {} declares {} component(s) and no array", desc.type_id,
                                     desc.component_count));
        return false;
    }
    if (desc.functions != nullptr && !valid_override_table(desc.functions, error)) return false;
    const TileType* existing = tile_types_.find(desc.type_id);
    if (existing != nullptr) {
        const auto known = tile_type_owners_.find(desc.type_id);
        if (known == tile_type_owners_.end() || known->second != owner) {
            set_error(error, std::format("tile type {} is already registered", desc.type_id));
            return false;
        }
        if (!tile_types_.refresh_tile_type(components_, desc, error)) return false;
        notify_tile_type(desc.type_id);
        return true;
    }
    if (tile_types_.find_by_name(desc.name) != nullptr) {
        set_error(error, std::format("tile type name '{}' is already registered", desc.name));
        return false;
    }
    if (tile_types_.register_tile_type(components_, desc, error) == nullptr) return false;
    tile_type_owners_.emplace(desc.type_id, std::string(owner));
    notify_tile_type(desc.type_id);
    return true;
}

void RuntimeContext::add_tile_type_listener(TileTypeId type_id, TileTypeListener listener, void* owner) {
    if (type_id == kNoTileType || listener == nullptr) return;
    tile_type_listeners_.push_back(TileTypeListenerEntry{type_id, listener, owner});
}

void RuntimeContext::remove_tile_type_listener(TileTypeId type_id, TileTypeListener listener,
                                               void* owner) {
    for (usize index = 0; index < tile_type_listeners_.size(); ++index) {
        const TileTypeListenerEntry& entry = tile_type_listeners_[index];
        if (entry.type_id != type_id || entry.listener != listener || entry.owner != owner) continue;
        tile_type_listeners_.erase(tile_type_listeners_.begin() + static_cast<std::ptrdiff_t>(index));
        return;
    }
}

void RuntimeContext::notify_tile_type(TileTypeId type_id) {
    // Copy the list: a listener may outlive the call but must not mutate the vector under the loop.
    const std::vector<TileTypeListenerEntry> listeners = tile_type_listeners_;
    for (const TileTypeListenerEntry& entry : listeners) {
        if (entry.type_id == type_id && entry.listener != nullptr) entry.listener(entry.owner, type_id);
    }
}

bool RuntimeContext::override_interface(ComponentTypeId type_id, InterfaceId interface_id,
                                        u32 expected_version, const void* replacement, const void** previous,
                                        std::string* error) {
    if (error != nullptr) error->clear();
    if (previous != nullptr) *previous = nullptr;
    if (!valid_override_table(replacement, error)) return false;
    const ComponentType* type = components_.find(type_id);
    if (type == nullptr) {
        set_error(error, std::format("component type {} is not registered", type_id));
        return false;
    }
    if (type->version() != expected_version) {
        set_error(error, std::format("component type {} ('{}') is version {}, not {}", type_id, type->name(),
                                     type->version(), expected_version));
        return false;
    }
    if (!type->has_interface(interface_id)) {
        set_error(error, std::format("component type {} ('{}') has no interface {}", type_id, type->name(),
                                     interface_id));
        return false;
    }
    return components_.override_interface(type_id, interface_id, replacement, previous, error);
}

void RuntimeContext::restore_interface(ComponentTypeId type_id, InterfaceId interface_id) {
    components_.restore_interface(type_id, interface_id);
}

} // namespace mine::runtime
