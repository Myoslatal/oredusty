#include <mine/runtime/tile_type.h>

#include <format>
#include <utility>

namespace mine::runtime {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

} // namespace

TileType::TileType(const TileTypeDesc& desc, std::string name)
    : type_id_(desc.type_id), name_(std::move(name)), version_(desc.version), functions_(desc.functions),
      factory_(desc.factory) {
    if (desc.component_count != 0)
        components_.assign(desc.components, desc.components + desc.component_count);
    if (desc.struct_size >= sizeof(TileTypeDesc) && desc.shared_count != 0) {
        shared_.assign(desc.shared, desc.shared + desc.shared_count);
    }
}

bool TileType::refresh(const TileTypeDesc& desc, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    if (error != nullptr) error->clear();
    if (desc.type_id != type_id_) return fail("a tile type cannot change its id");
    if (desc.name == nullptr || name_ != desc.name) return fail("a tile type cannot change its name");
    if (desc.version != version_) {
        return fail(std::format("tile type {} ('{}') is version {}, not {}", type_id_, name_, version_,
                                desc.version));
    }
    if (desc.component_count != components_.size()) {
        return fail(std::format("tile type {} ('{}') cannot change its component list", type_id_, name_));
    }
    for (u32 index = 0; index < desc.component_count; ++index) {
        if (desc.components[index] != components_[index]) {
            return fail(std::format("tile type {} ('{}') cannot change its component list", type_id_, name_));
        }
    }
    const u32 shared_count =
        desc.struct_size >= sizeof(TileTypeDesc) ? desc.shared_count : 0u;
    const std::vector<const void*> shared_values =
        shared_count != 0 ? std::vector<const void*>(desc.shared, desc.shared + shared_count)
                          : std::vector<const void*>{};
    if (shared_values.size() != shared_.size()) {
        return fail("a tile type cannot change the number of shared records");
    }
    functions_ = desc.functions;
    factory_ = desc.factory;
    shared_ = shared_values;
    return true;
}

bool TileTypeRegistry::refresh_tile_type(const ComponentRegistry& components, const TileTypeDesc& desc,
                                         std::string* error) {
    if (error != nullptr) error->clear();
    if (desc.struct_size < kTileTypeDescPrefixSize) {
        if (error != nullptr) {
            *error = std::format("tile type descriptor is {} bytes, this runtime needs at least {}",
                                 desc.struct_size, kTileTypeDescPrefixSize);
        }
        return false;
    }
    const u32 shared_count = desc.struct_size >= sizeof(TileTypeDesc) ? desc.shared_count : 0u;
    if (shared_count != 0 && shared_count != desc.component_count) {
        if (error != nullptr) {
            *error = std::format("tile type {} ('{}') declares {} shared record(s) for {} component(s)",
                                 desc.type_id, desc.name, shared_count, desc.component_count);
        }
        return false;
    }
    if (shared_count != 0 && desc.shared == nullptr) {
        if (error != nullptr) *error = std::format("tile type {} has no shared array", desc.type_id);
        return false;
    }
    const TileType* type = find(desc.type_id);
    if (type == nullptr) {
        if (error != nullptr) *error = std::format("tile type {} is not registered", desc.type_id);
        return false;
    }
    if (desc.name == nullptr || type->name() != desc.name) {
        if (error != nullptr) *error = std::format("tile type {} has another name", desc.type_id);
        return false;
    }
    if (desc.component_count != 0 && desc.components == nullptr) {
        if (error != nullptr)
            *error = std::format("tile type {} ('{}') has no component array", desc.type_id, desc.name);
        return false;
    }
    for (u32 index = 0; index < desc.component_count; ++index) {
        if (components.find(desc.components[index]) == nullptr) {
            if (error != nullptr) {
                *error = std::format("tile type {} ('{}') names component type {}, which is not registered",
                                     desc.type_id, desc.name, desc.components[index]);
            }
            return false;
        }
    }
    // TileType::refresh is not const because it updates the registered object; the pointer was
    // obtained from find(), which is const, so cast the constness away only after the id matched.
    auto* mutable_type = const_cast<TileType*>(type);
    return mutable_type->refresh(desc, error);
}

const TileType* TileTypeRegistry::register_tile_type(const ComponentRegistry& components,
                                                     const TileTypeDesc& desc, std::string* error) {
    if (error != nullptr) error->clear();
    if (desc.struct_size < kTileTypeDescPrefixSize) {
        set_error(error, std::format("tile type descriptor is {} bytes, this runtime needs at least {}",
                                     desc.struct_size, kTileTypeDescPrefixSize));
        return nullptr;
    }
    const u32 shared_count = desc.struct_size >= sizeof(TileTypeDesc) ? desc.shared_count : 0u;
    if (shared_count != 0 && shared_count != desc.component_count) {
        set_error(error, std::format("tile type {} ('{}') declares {} shared record(s) for {} component(s)",
                                     desc.type_id, desc.name, shared_count, desc.component_count));
        return nullptr;
    }
    if (shared_count != 0 && desc.shared == nullptr) {
        set_error(error, std::format("tile type {} ('{}') declares shared records and no array", desc.type_id,
                                     desc.name));
        return nullptr;
    }
    if (desc.type_id == kNoTileType) {
        set_error(error, "tile type id 0 is reserved");
        return nullptr;
    }
    if (desc.name == nullptr) {
        set_error(error, std::format("tile type {} has no name", desc.type_id));
        return nullptr;
    }
    if (desc.name[0] == '\0') {
        set_error(error, std::format("tile type {} has an empty name", desc.type_id));
        return nullptr;
    }
    if (desc.version == 0) {
        set_error(error, std::format("tile type {} ('{}') has version 0", desc.type_id, desc.name));
        return nullptr;
    }
    if (desc.component_count != 0 && desc.components == nullptr) {
        set_error(error, std::format("tile type {} ('{}') declares {} component(s) and no array",
                                     desc.type_id, desc.name, desc.component_count));
        return nullptr;
    }
    if (find(desc.type_id) != nullptr) {
        set_error(error, std::format("tile type {} is already registered", desc.type_id));
        return nullptr;
    }
    if (find_by_name(desc.name) != nullptr) {
        set_error(error, std::format("tile type name '{}' is already registered", desc.name));
        return nullptr;
    }
    for (u32 index = 0; index < desc.component_count; ++index) {
        const ComponentTypeId component_id = desc.components[index];
        if (components.find(component_id) == nullptr) {
            set_error(error,
                      std::format("tile type {} ('{}') names component type {}, which is not registered",
                                  desc.type_id, desc.name, component_id));
            return nullptr;
        }
        for (u32 other = 0; other < index; ++other) {
            if (desc.components[other] != component_id) continue;
            set_error(error, std::format("tile type {} ('{}') names component type {} twice", desc.type_id,
                                         desc.name, component_id));
            return nullptr;
        }
    }

    types_.push_back(std::unique_ptr<TileType>(new TileType(desc, std::string(desc.name))));
    return types_.back().get();
}

const TileType* TileTypeRegistry::find(TileTypeId type_id) const {
    for (const std::unique_ptr<TileType>& type : types_) {
        if (type->type_id() == type_id) return type.get();
    }
    return nullptr;
}

const TileType* TileTypeRegistry::find_by_name(std::string_view name) const {
    for (const std::unique_ptr<TileType>& type : types_) {
        if (type->name() == name) return type.get();
    }
    return nullptr;
}

} // namespace mine::runtime
