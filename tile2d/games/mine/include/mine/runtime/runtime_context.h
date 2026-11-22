// Mine - the runtime registries one game instance owns.
//
// A component type or tile type id is stable across loads, but the registered objects are not: a mod
// reloads its library, and the function tables it registered have to point at the new code. The context
// keeps the registries for the whole process, records which mod owns which entry, and refreshes an
// entry in place when the same mod registers it again. In place matters: every archetype and every
// live object holds a ComponentType pointer, so the pointer must stay valid across a reload.
#pragma once

#include <mine/runtime/component_registry.h>
#include <mine/runtime/tile_type.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mine::runtime {

/// The two registries and the mod-ownership bookkeeping around them. Built once, when the game starts
/// (or when a test creates one); every content load uses the same instance.
class RuntimeContext {
public:
    RuntimeContext();
    ~RuntimeContext();

    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    RuntimeContext(RuntimeContext&&) = delete;
    RuntimeContext& operator=(RuntimeContext&&) = delete;

    [[nodiscard]] ComponentRegistry& components() { return components_; }
    [[nodiscard]] const ComponentRegistry& components() const { return components_; }
    [[nodiscard]] TileTypeRegistry& tile_types() { return tile_types_; }
    [[nodiscard]] const TileTypeRegistry& tile_types() const { return tile_types_; }

    /// Registers (or refreshes, when this mod registered the same id on a previous load) a component
    /// from a mod. A built-in id, or an id owned by another mod, is refused. \p error gets the reason.
    [[nodiscard]] bool register_mod_component(const ComponentDesc& desc, std::string_view owner,
                                              std::string* error = nullptr);

    /// The same for a tile type. Its component ids must already be registered.
    [[nodiscard]] bool register_mod_tile_type(const TileTypeDesc& desc, std::string_view owner,
                                              std::string* error = nullptr);

    /// Replaces the active table of a registered component interface. \p expected_version is the
    /// schema version the caller compiled against; a mismatch is refused before the table is touched.
    /// The replacement must start with an InterfaceTableHeader whose size and version are valid.
    [[nodiscard]] bool override_interface(ComponentTypeId type_id, InterfaceId interface_id,
                                          u32 expected_version, const void* replacement,
                                          const void** previous = nullptr, std::string* error = nullptr);

    /// Puts an interface back to its registration table. Used by the mod host before unloading the
    /// library that installed an override.
    void restore_interface(ComponentTypeId type_id, InterfaceId interface_id);

    /// A tile type was registered or refreshed. A TileRuntimeLibrary uses this to point the archetypes
    /// it already built at the new shared records after a module reload. The owner is not owned; the
    /// tile library removes its listener before it dies.
    using TileTypeListener = void (*)(void* owner, TileTypeId type_id);
    void add_tile_type_listener(TileTypeId type_id, TileTypeListener listener, void* owner);
    void remove_tile_type_listener(TileTypeId type_id, TileTypeListener listener, void* owner);

private:
    struct TileTypeListenerEntry {
        TileTypeId type_id = 0;
        TileTypeListener listener = nullptr;
        void* owner = nullptr;
    };

    void notify_tile_type(TileTypeId type_id);
    std::vector<TileTypeListenerEntry> tile_type_listeners_;
    ComponentRegistry components_;
    TileTypeRegistry tile_types_;
    std::unordered_map<ComponentTypeId, std::string> component_owners_;
    std::unordered_map<TileTypeId, std::string> tile_type_owners_;
};

} // namespace mine::runtime
