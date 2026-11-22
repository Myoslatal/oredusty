// Mine - a tile type: the minimal registration a future custom tile needs.
//
// A tile type names the components an instance of it is made of and carries one opaque function table
// (plus an optional factory). The runtime stores the declaration and answers by id or name; what the
// functions do and how they are called belongs to the layer that owns tiles. Nothing here creates an
// instance: creation needs the per-instance shared definitions (Archetype) and the object storage
// (ObjectData), which are the same for every kind of object.
#pragma once

#include <mine/runtime/component_registry.h>

#include <t2d/core/types.h>

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mine::runtime {

using t2d::ConstSpan;
using t2d::u32;
using t2d::u64;
using t2d::usize;

/// Stable identity of a tile type, like ComponentTypeId: written down by the layer that defines it,
/// not handed out in registration order. 0 is "none".
using TileTypeId = u64;
inline constexpr TileTypeId kNoTileType = 0;

/// Optional hook the layer that owns tiles may call to create an instance. The runtime never calls it;
/// null is a type that is registered as data only.
using TileFactory = void* (*)(const void* context);

/// The C-compatible description of a tile type. A tile is made of components, so the component list is
/// validated against a ComponentRegistry at registration; it may be empty for a tile that has no data.
struct TileTypeDesc {
    /// `sizeof(TileTypeDesc)` as the producer compiled it; a descriptor at least as large as the
    /// pre-shared prefix is accepted (appended fields are then null), a smaller one is refused.
    u32 struct_size;
    TileTypeId type_id;
    const char* name;
    u32 version;
    const ComponentTypeId* components;
    u32 component_count;
    /// The opaque function table (the same contract as a component interface table).
    const void* functions;
    /// Optional; null when the type cannot be created from this declaration alone.
    TileFactory factory;

    // --- appended shared-record surface -------------------------------------------------------------
    /// One read-only shared record per component, in the same order as \p components, or null/0 when
    /// this tile type supplies none. A non-zero count must equal \p component_count. These pointers
    /// are the module's static data and must stay valid while the module is loaded.
    const void* const* shared;
    u32 shared_count;
};

/// The size of the descriptor before shared records were appended. A descriptor at least this large is
/// accepted with no shared records; a smaller one is refused.
inline constexpr usize kTileTypeDescPrefixSize = offsetof(TileTypeDesc, shared);

static_assert(std::is_standard_layout_v<TileTypeDesc>, "TileTypeDesc must stay C compatible");
static_assert(std::is_trivially_copyable_v<TileTypeDesc>, "TileTypeDesc must stay C compatible");

/// One registered tile type. Owned by TileTypeRegistry.
class TileType {
public:
    [[nodiscard]] TileTypeId type_id() const { return type_id_; }
    [[nodiscard]] std::string_view name() const { return name_; }
    [[nodiscard]] u32 version() const { return version_; }
    [[nodiscard]] ConstSpan<ComponentTypeId> components() const {
        return ConstSpan<ComponentTypeId>(components_.data(), components_.size());
    }
    [[nodiscard]] usize component_count() const { return components_.size(); }
    [[nodiscard]] const void* functions() const { return functions_; }
    [[nodiscard]] TileFactory factory() const { return factory_; }
    /// Per-component shared records, in component order; empty when the type supplies none.
    [[nodiscard]] ConstSpan<const void*> shared_values() const {
        return ConstSpan<const void*>(shared_.data(), shared_.size());
    }

    /// Refreshes the opaque table (and factory) after a mod reload. The identity, version and
    /// component list must match, so existing objects and their archetypes stay valid.
    [[nodiscard]] bool refresh(const TileTypeDesc& desc, std::string* error = nullptr);

private:
    friend class TileTypeRegistry;

    TileType(const TileTypeDesc& desc, std::string name);

    TileTypeId type_id_ = kNoTileType;
    std::string name_;
    u32 version_ = 0;
    std::vector<ComponentTypeId> components_;
    const void* functions_ = nullptr;
    TileFactory factory_ = nullptr;
    std::vector<const void*> shared_;
};

/// Registers tile types and answers by id or name. The ComponentRegistry passed to registration owns
/// the component types the tile names; only its ids are stored, so the tile type does not extend the
/// component registry's lifetime.
class TileTypeRegistry {
public:
    TileTypeRegistry() = default;
    TileTypeRegistry(const TileTypeRegistry&) = delete;
    TileTypeRegistry& operator=(const TileTypeRegistry&) = delete;
    TileTypeRegistry(TileTypeRegistry&&) noexcept = default;
    TileTypeRegistry& operator=(TileTypeRegistry&&) noexcept = default;

    /// Validates \p desc against \p components and registers it. Refused: a descriptor too small, id
    /// 0, an empty or duplicate name, version 0, a null component array with a non-zero count, a
    /// duplicate component id, and a component id the registry does not have. The functions pointer
    /// may be null; the factory may be null. \p error receives the reason on failure.
    [[nodiscard]] const TileType* register_tile_type(const ComponentRegistry& components,
                                                     const TileTypeDesc& desc, std::string* error = nullptr);
    [[nodiscard]] const TileType* find(TileTypeId type_id) const;
    [[nodiscard]] const TileType* find_by_name(std::string_view name) const;
    [[nodiscard]] usize count() const { return types_.size(); }

    /// Updates an already registered tile type in place. Same rules as ComponentRegistry's refresh:
    /// identity and components must match, only the opaque table and factory change.
    [[nodiscard]] bool refresh_tile_type(const ComponentRegistry& components, const TileTypeDesc& desc,
                                         std::string* error = nullptr);

private:
    std::vector<std::unique_ptr<TileType>> types_;
};

static_assert(!std::is_polymorphic_v<TileType>, "the runtime must not need RTTI or a vtable");
static_assert(!std::is_polymorphic_v<TileTypeRegistry>, "the runtime must not need RTTI or a vtable");

} // namespace mine::runtime
