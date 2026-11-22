#include <mine/types/tile_runtime.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace mine::types {
namespace {

using runtime::ArchetypeComponent;
using runtime::ComponentTypeId;

const char* structure_role() {
    return "structure";
}

const char* machine_role() {
    return "machine";
}

constexpr runtime::content::BuiltinTileFunctions kStructureTileFunctions{
    {static_cast<t2d::u32>(sizeof(runtime::content::BuiltinTileFunctions)), 1}, &structure_role};
constexpr runtime::content::BuiltinTileFunctions kMachineTileFunctions{
    {static_cast<t2d::u32>(sizeof(runtime::content::BuiltinTileFunctions)), 1}, &machine_role};

[[nodiscard]] runtime::TileTypeId tile_type_for(ContentKind kind) {
    switch (kind) {
    case ContentKind::Floor:
        return runtime::content::kFloorTileType;
    case ContentKind::Ore:
        return runtime::content::kOreTileType;
    case ContentKind::Warehouse:
        return runtime::content::kWarehouseTileType;
    case ContentKind::Drill:
        return runtime::content::kDrillTileType;
    case ContentKind::Machine:
        return kMachineTileType;
    case ContentKind::Structure:
        return kStructureTileType;
    default:
        return runtime::kNoTileType;
    }
}

void register_tile(runtime::TileTypeRegistry& tiles, const runtime::ComponentRegistry& components,
                   runtime::TileTypeId type_id, const char* name,
                   std::initializer_list<ComponentTypeId> component_ids, const void* functions) {
    std::vector<ComponentTypeId> ids(component_ids);
    runtime::TileTypeDesc desc{};
    desc.struct_size = static_cast<t2d::u32>(sizeof(runtime::TileTypeDesc));
    desc.type_id = type_id;
    desc.name = name;
    desc.version = runtime::content::kBuiltinContentVersion;
    desc.components = ids.data();
    desc.component_count = static_cast<t2d::u32>(ids.size());
    desc.functions = functions;
    desc.factory = nullptr;
    std::string error;
    if (tiles.register_tile_type(components, desc, &error) == nullptr) {
        T2D_ERROR("runtime: tile type '{}' could not be registered: {}", name, error);
    }
}

} // namespace

TileRuntimeLibrary::TileRuntimeLibrary(const ContentRegistry& registry,
                                       const std::deque<TileDefinition>& definitions,
                                       std::shared_ptr<runtime::RuntimeContext> context)
    : context_(context != nullptr ? std::move(context) : std::make_shared<runtime::RuntimeContext>()) {
    ensure_generic_tile_types();
    definitions_.reserve(definitions.size());
    for (const TileDefinition& definition : definitions) {
        if (!kind_is_a_tile(definition.kind)) continue;
        definitions_.push_back(build(definition, registry));
    }
    for (const std::shared_ptr<const RuntimeTileDefinition>& definition : definitions_) {
        if (definition->tile_type != nullptr) listen_for(definition->tile_type->type_id());
    }
}

void TileRuntimeLibrary::ensure_generic_tile_types() {
    // The game maps Structure and Machine onto these two types; a mod may have registered its own, so
    // both are checked before registration. They live in the shared context and therefore survive a
    // content reload exactly like the built-in ones.
    if (context_->tile_types().find(kStructureTileType) == nullptr) {
        register_tile(context_->tile_types(), context_->components(), kStructureTileType, "structure",
                      {runtime::content::kPreviewComponent, runtime::content::kBuildingComponent},
                      &kStructureTileFunctions);
    }
    if (context_->tile_types().find(kMachineTileType) == nullptr) {
        register_tile(context_->tile_types(), context_->components(), kMachineTileType, "machine",
                      {runtime::content::kTickerComponent, runtime::content::kPreviewComponent,
                       runtime::content::kBuildingComponent},
                      &kMachineTileFunctions);
    }
}

TileRuntimeLibrary::~TileRuntimeLibrary() {
    for (const runtime::TileTypeId type_id : listener_types_) {
        context_->remove_tile_type_listener(type_id, &TileRuntimeLibrary::tile_type_refreshed, this);
    }
}

void TileRuntimeLibrary::tile_type_refreshed(void* owner, runtime::TileTypeId type_id) {
    if (owner != nullptr) static_cast<TileRuntimeLibrary*>(owner)->refresh_shared(type_id);
}

void TileRuntimeLibrary::listen_for(runtime::TileTypeId type_id) const {
    if (type_id == runtime::kNoTileType) return;
    for (const runtime::TileTypeId listed : listener_types_) {
        if (listed == type_id) return;
    }
    listener_types_.push_back(type_id);
    context_->add_tile_type_listener(type_id, &TileRuntimeLibrary::tile_type_refreshed,
                                     const_cast<TileRuntimeLibrary*>(this));
}

void TileRuntimeLibrary::refresh_shared(runtime::TileTypeId type_id) const {
    const auto refresh = [type_id](const std::shared_ptr<const RuntimeTileDefinition>& definition) {
        if (definition->tile_type == nullptr || definition->tile_type->type_id() != type_id ||
            definition->archetype == nullptr) {
            return;
        }
        const auto shared_values = definition->tile_type->shared_values();
        usize index = 0;
        for (const runtime::ComponentTypeId component : definition->tile_type->components()) {
            if (!runtime::content::is_builtin_component(component)) {
                const void* value = index < shared_values.size() ? shared_values[index] : nullptr;
                definition->archetype->refresh_shared(component, value);
            }
            ++index;
        }
    };
    for (const std::shared_ptr<const RuntimeTileDefinition>& definition : definitions_) refresh(definition);
    for (const std::shared_ptr<const RuntimeTileDefinition>& definition : fallback_) refresh(definition);
}

const RuntimeTileDefinition* TileRuntimeLibrary::find(ContentKind kind, std::string_view name) const {
    for (const std::shared_ptr<const RuntimeTileDefinition>& definition : definitions_) {
        if (definition->definition.kind == kind && definition->definition.name == name)
            return definition.get();
    }
    return nullptr;
}

std::shared_ptr<const RuntimeTileDefinition>
TileRuntimeLibrary::definition_for(const TileDefinition& definition, const ContentRegistry& registry) const {
    for (const std::shared_ptr<const RuntimeTileDefinition>& known : definitions_) {
        if (known->definition.kind == definition.kind && known->definition.name == definition.name)
            return known;
    }
    for (const std::shared_ptr<const RuntimeTileDefinition>& known : fallback_) {
        if (known->definition.kind == definition.kind && known->definition.name == definition.name)
            return known;
    }
    // Content a mod registered without a file behind it: build the layout from what the caller has and
    // keep it, so every instance of that fallback entry still shares one archetype and one shared record.
    std::shared_ptr<RuntimeTileDefinition> built = build(definition, registry);
    if (built->tile_type != nullptr) listen_for(built->tile_type->type_id());
    fallback_.push_back(built);
    return built;
}

std::shared_ptr<RuntimeTileDefinition> TileRuntimeLibrary::build(const TileDefinition& definition,
                                                                 const ContentRegistry& registry) const {
    auto built = std::make_shared<RuntimeTileDefinition>();
    RuntimeTileDefinition& runtime_definition = *built;
    runtime_definition.definition = definition;
    runtime_definition.container.capacity =
        static_cast<t2d::u32>(std::min<usize>(definition.capacity, std::numeric_limits<t2d::u32>::max()));
    runtime_definition.preview.image = runtime_definition.definition.image.c_str();
    runtime_definition.preview.top_image = runtime_definition.definition.top_image.c_str();
    runtime_definition.preview.mirrored = 0; // the instance's own flag is applied when it previews
    runtime_definition.preview.spin_seconds = runtime_definition.definition.spin;
    runtime_definition.preview.spin_origin = runtime::content::Vec2f{
        runtime_definition.definition.spin_origin.x, runtime_definition.definition.spin_origin.y};
    runtime_definition.building.seconds = runtime_definition.definition.building.seconds;
    runtime_definition.building.material_count = 0;
    for (const ContentStack& material : runtime_definition.definition.building.materials) {
        if (runtime_definition.building.material_count >= runtime::content::kMaxBuildingMaterials) break;
        runtime::content::BuildingMaterial& out =
            runtime_definition.building.materials[runtime_definition.building.material_count++];
        out.kind = static_cast<t2d::u32>(ContentKind::Item);
        out.id = registry.find(ContentKind::Item, material.name);
        out.count =
            static_cast<t2d::u32>(std::min<usize>(material.count, std::numeric_limits<t2d::u32>::max()));
    }
    runtime_definition.drill.dig_power = runtime_definition.definition.dig_power;
    // The task chain, resolved here, once, against the registry in force: the steps hold item ids, not
    // names, so completing one is a container lookup and never a string compare. A step whose item this
    // build does not have keeps kNoContentId - it is a step nothing can ever satisfy, and the error says
    // which content and which task asked for it (docs/GAME_DESIGN.md section 1.52).
    runtime_definition.task.count = static_cast<t2d::u32>(
        std::min<usize>(runtime_definition.definition.tasks.size(), runtime::content::kMaxTaskSteps));
    for (usize index = 0; index < runtime_definition.task.count; ++index) {
        const TaskSpec& spec = runtime_definition.definition.tasks[index];
        runtime::content::TaskStep& step = runtime_definition.task.steps[index];
        step.type = static_cast<t2d::u32>(spec.type);
        step.kind = static_cast<t2d::u32>(ContentKind::Item);
        step.id = registry.find(ContentKind::Item, spec.item);
        step.rate = step.type == runtime::content::kTaskTypeRate ? spec.rate : 0.0f;
        step.window = step.type == runtime::content::kTaskTypeRate ? spec.window : 0.0f;
        step.hold = step.type == runtime::content::kTaskTypeRate ? spec.hold : 0.0f;
        // A deliver step asks for its count; a rate step asks for what its window has to hold,
        // ceil(rate * window), and never less than one. Both are carried as count, which is what the
        // settle and the container consume (docs/GAME_DESIGN.md section 1.53).
        usize need = spec.count;
        if (step.type == runtime::content::kTaskTypeRate) {
            const t2d::f64 wanted = std::ceil(static_cast<t2d::f64>(spec.rate) *
                                              static_cast<t2d::f64>(spec.window > 0.0f ? spec.window : 1.0f));
            need = wanted >= 1.0 ? static_cast<usize>(wanted) : 1u;
        }
        step.count = static_cast<t2d::u32>(std::min<usize>(need, std::numeric_limits<t2d::u32>::max()));
        if (step.id == kNoContent) {
            T2D_ERROR("runtime: {} '{}' task '{}' delivers item '{}', which is not registered",
                      content_kind_name(runtime_definition.definition.kind),
                      runtime_definition.definition.name, spec.name, spec.item);
        }
        // No box can ever hold the whole step: it could never complete, and a player could not tell why
        // from the game. The step stays in the chain (skipping it would silently change the content) but
        // the error says which content, which task and how many.
        if (step.count > 0 && static_cast<usize>(step.count) > runtime_definition.definition.capacity) {
            T2D_ERROR("runtime: {} '{}' task '{}' asks for {} items in its container, which holds {}",
                      content_kind_name(runtime_definition.definition.kind),
                      runtime_definition.definition.name, spec.name, step.count,
                      runtime_definition.definition.capacity);
        }
    }

    // A custom tile type says which components its objects are made of. The built-in mapping is the
    // default only: a content entry that names tile_type gets exactly the list it asked for, so a mod
    // can put its own data component next to the built-in preview and building ones.
    const bool custom_tile = !definition.tile_type.empty();
    const runtime::TileType* tile_type = custom_tile
                                             ? context_->tile_types().find_by_name(definition.tile_type)
                                             : context_->tile_types().find(tile_type_for(definition.kind));
    if (tile_type == nullptr) {
        T2D_ERROR("runtime: {} '{}' names tile type '{}', which is not registered",
                  content_kind_name(definition.kind), definition.name,
                  custom_tile ? definition.tile_type : "(built-in)");
        return built;
    }
    runtime_definition.tile_type = tile_type;

    const auto builtin_shared = [&](runtime::ComponentTypeId type_id) -> const void* {
        if (type_id == runtime::content::kContainerComponent) return &runtime_definition.container;
        if (type_id == runtime::content::kPreviewComponent) return &runtime_definition.preview;
        if (type_id == runtime::content::kBuildingComponent) return &runtime_definition.building;
        if (type_id == runtime::content::kDrillComponent) return &runtime_definition.drill;
        if (type_id == runtime::content::kTaskComponent) return &runtime_definition.task;
        return nullptr;
    };
    const auto shared_for = [&](runtime::ComponentTypeId type_id, usize index) -> const void* {
        if (type_id == runtime::content::kContainerComponent ||
            type_id == runtime::content::kPreviewComponent ||
            type_id == runtime::content::kBuildingComponent ||
            type_id == runtime::content::kDrillComponent ||
            type_id == runtime::content::kTickerComponent ||
            type_id == runtime::content::kClickComponent ||
            type_id == runtime::content::kDragComponent ||
            type_id == runtime::content::kTaskComponent) {
            // Built-in components keep their per-content record; a tile type's shared array does not
            // replace what the content file says (it provides the records the game does not know).
            return builtin_shared(type_id);
        }
        const ConstSpan<const void*> shared = tile_type->shared_values();
        return index < shared.size() ? shared[index] : nullptr;
    };

    // The tile type is the single description of a layout, for built-in and custom content alike. The
    // game passes the per-content shared records for the components it knows; a custom component gets
    // the module's shared pointer from the tile type's shared array.
    std::vector<ArchetypeComponent> components;
    usize index = 0;
    for (const runtime::ComponentTypeId type_id : tile_type->components()) {
        components.push_back(ArchetypeComponent{type_id, shared_for(type_id, index++)});
    }
    const auto has = [&](runtime::ComponentTypeId type_id) {
        for (const runtime::ComponentTypeId listed : tile_type->components()) {
            if (listed == type_id) return true;
        }
        return false;
    };
    runtime_definition.ticking = has(runtime::content::kTickerComponent);
    runtime_definition.has_container = has(runtime::content::kContainerComponent);
    runtime_definition.has_drag = has(runtime::content::kDragComponent);
    runtime_definition.has_drill = has(runtime::content::kDrillComponent);
    std::string error;
    std::optional<runtime::Archetype> archetype =
        runtime::Archetype::build(context_->components(), components, &error);
    if (!archetype.has_value()) {
        T2D_ERROR("runtime: {} '{}' has no usable archetype: {}", content_kind_name(definition.kind),
                  definition.name, error);
        return built;
    }
    runtime_definition.archetype = std::make_unique<runtime::Archetype>(std::move(*archetype));
    return built;
}

} // namespace mine::types
