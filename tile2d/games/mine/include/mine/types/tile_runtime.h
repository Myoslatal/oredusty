// Mine - the runtime definition of one content entry.
//
// The definitions a content file produces are data: a name, an image, a capacity, a digging power. An
// instance is not: it is the bytes of the components its tile type is made of, and the function tables
// that act on them. This file is the bridge: for every TileDefinition it builds the per-component
// shared records (ContainerShared, PreviewShared, BuildingShared, DrillShared), the Archetype that says
// where each component sits, and picks the TileType a runtime object is made of.
//
// The library owns all of it. A Tile keeps the library alive through a shared_ptr, so a content reload
// that replaces the definitions cannot pull the archetype out from under an object that already exists.
#pragma once

#include <mine/registry.h>
#include <mine/types/tile_definition.h>

#include <mine/runtime/archetype.h>
#include <mine/runtime/component_registry.h>
#include <mine/runtime/content/runtime_content.h>
#include <mine/runtime/runtime_context.h>
#include <mine/runtime/tile_type.h>

#include <deque>
#include <memory>
#include <string_view>
#include <vector>

namespace mine::types {

/// The generic tile types: content that is a structure or a machine but is not one of the four special
/// built-ins. A structure previews; a machine previews and runs on the Ticker.
inline constexpr runtime::TileTypeId kStructureTileType = runtime::content::hash_name("mine.tile.structure");
inline constexpr runtime::TileTypeId kMachineTileType = runtime::content::hash_name("mine.tile.machine");

/// One content entry, ready to be instantiated: the definition (a copy, so a tile does not depend on the
/// content list's lifetime), the shared records its components read, and the layout it is built in.
struct RuntimeTileDefinition {
    TileDefinition definition;
    runtime::content::ContainerShared container{};
    runtime::content::PreviewShared preview{};
    runtime::content::BuildingShared building{};
    runtime::content::DrillShared drill{};
    /// The task chain this content declares, resolved against the registry at build time
    /// (docs/GAME_DESIGN.md section 1.52).
    runtime::content::TaskShared task{};
    mutable std::unique_ptr<runtime::Archetype> archetype;
    const runtime::TileType* tile_type = nullptr;
    bool ticking = false;
    bool has_container = false;
    bool has_drag = false;
    bool has_drill = false;
};

/// Owns the built-in component/tile-type registries and every runtime definition built from one content
/// load. Construct once per content load; it is immutable after that, so a Tile may hold it concurrently.
class TileRuntimeLibrary {
public:
    TileRuntimeLibrary(const ContentRegistry& registry, const std::deque<TileDefinition>& definitions,
                       std::shared_ptr<runtime::RuntimeContext> context);
    ~TileRuntimeLibrary();

    TileRuntimeLibrary(const TileRuntimeLibrary&) = delete;
    TileRuntimeLibrary& operator=(const TileRuntimeLibrary&) = delete;
    TileRuntimeLibrary(TileRuntimeLibrary&&) = delete;
    TileRuntimeLibrary& operator=(TileRuntimeLibrary&&) = delete;

    [[nodiscard]] const runtime::ComponentRegistry& components() const { return context_->components(); }
    [[nodiscard]] const runtime::TileTypeRegistry& tile_types() const { return context_->tile_types(); }
    [[nodiscard]] const std::shared_ptr<runtime::RuntimeContext>& context() const { return context_; }

    /// The definition for this kind and name, or nullptr when this load did not carry one.
    [[nodiscard]] const RuntimeTileDefinition* find(ContentKind kind, std::string_view name) const;

    /// The definition for this record, building one on demand when the content was registered without
    /// a file behind it (a mod's content, a test's fallback). The fallback is owned here.
    [[nodiscard]] std::shared_ptr<const RuntimeTileDefinition>
    definition_for(const TileDefinition& definition, const ContentRegistry& registry) const;

private:
    /// Registers the two generic tile types the game maps Structure and Machine onto, once per context.
    void ensure_generic_tile_types();
    [[nodiscard]] std::shared_ptr<RuntimeTileDefinition> build(const TileDefinition& definition,
                                                               const ContentRegistry& registry) const;
    /// Registers this library once for a tile type it has a definition of, so a later module reload
    /// refreshes the shared records of already-built objects.
    void listen_for(runtime::TileTypeId type_id) const;
    /// A tile type was refreshed after a module reload: point every archetype this library already
    /// built at that type's new shared records. Existing objects read the same Archetype, so they see
    /// the new data on their next component call.
    void refresh_shared(runtime::TileTypeId type_id) const;
    static void tile_type_refreshed(void* owner, runtime::TileTypeId type_id);

    /// The registries are owned by the game, not by one content load: a mod refresh replaces tables in
    /// place and every Tile made earlier keeps seeing the same ComponentType.
    std::shared_ptr<runtime::RuntimeContext> context_;
    std::vector<std::shared_ptr<const RuntimeTileDefinition>> definitions_;
    mutable std::vector<runtime::TileTypeId> listener_types_;
    mutable std::vector<std::shared_ptr<const RuntimeTileDefinition>> fallback_;
};

} // namespace mine::types
