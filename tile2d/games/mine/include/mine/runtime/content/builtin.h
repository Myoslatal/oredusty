// Mine - the built-in content: the eight component tables and the five tile types the mine itself is
// made of, plus the one operation that needs more than one component of the same object.
//
// Registration is one call. It is not part of the runtime core because it is content: a game or a mod
// picks what it wants registered, and a dedicated server registers the same eight components without a
// renderer. What the tile types mean (which components a floor, an ore, a warehouse, a drill or a task
// block has) is written down in builtin.cpp beside their descriptors.
#pragma once

#include <mine/runtime/content/components.h>
#include <mine/runtime/content/content_ids.h>

#include <mine/runtime/component_registry.h>
#include <mine/runtime/object_data.h>
#include <mine/runtime/tile_type.h>

#include <string>
#include <type_traits>
#include <vector>

namespace mine::runtime {
namespace content {

/// The function tables the descriptors bind. They are const and live for the whole program: a
/// ComponentDesc points at one, and an override (ComponentRegistry::override_interface) can replace the
/// active pointer without touching the table itself.
extern const ContainerFunctions kContainerFunctions;
extern const ClickFunctions kClickFunctions;
extern const DragFunctions kDragFunctions;
extern const TickerFunctions kTickerFunctions;
extern const PreviewFunctions kPreviewFunctions;
extern const BuildingFunctions kBuildingFunctions;
extern const DrillFunctions kDrillFunctions;
extern const TaskFunctions kTaskFunctions;

/// The opaque table a built-in tile type carries. A tile type has one table, not one per interface; this
/// is the minimal one: it names the role the layer owning tiles asked for.
struct BuiltinTileFunctions {
    InterfaceTableHeader header;
    const char* (*role)(void);
};

extern const BuiltinTileFunctions kFloorTileFunctions;
extern const BuiltinTileFunctions kOreTileFunctions;
extern const BuiltinTileFunctions kWarehouseTileFunctions;
extern const BuiltinTileFunctions kDrillTileFunctions;
extern const BuiltinTileFunctions kTaskBlockTileFunctions;

static_assert(std::is_standard_layout_v<BuiltinTileFunctions>, "the tile table must stay C compatible");
static_assert(std::is_trivially_copyable_v<BuiltinTileFunctions>, "the tile table must stay C compatible");
static_assert(!std::is_polymorphic_v<BuiltinTileFunctions>, "the runtime must not need RTTI or a vtable");

/// Registers the eight built-in components and the five built-in tile types, in that order, skipping
/// nothing on failure so a caller sees every problem. Returns true when everything registered. Each
/// refusal is appended to \p errors when it is not null (the vector is not cleared first).
[[nodiscard]] bool register_builtin_content(ComponentRegistry& components, TileTypeRegistry& tile_types,
                                            std::vector<std::string>* errors = nullptr);

/// Points the object's DragData at its own ContainerData, its ContainerShared and its active
/// ContainerFunctions, so drag can ask the container without knowing the object. Returns false (and
/// clears the link) when the object has no drag or no container. Call it after the object is built and
/// again after an override that changed the active container table.
[[nodiscard]] bool link_drag(ObjectData& object);

/// Settles the object's task chain against its own container, at \p now on the layer's clock. While the
/// current step is satisfied - a **deliver** whose item is in the container in the count it asks for, or
/// a **rate** whose deliveries within its window already reach the needed count - take exactly what the
/// step asks for and mark it done; stop at the first step that is not satisfied. Returns the number of
/// steps completed by this call. This is the one place "what is in the container" turns into task
/// progress, so every path that can put items in - a drag, a click, a placement's starting stock, a
/// load - ends with it (docs/GAME_DESIGN.md sections 1.52 and 1.53).
[[nodiscard]] u32 settle_tasks(ObjectData& object, f32 now);

/// True for the components the game itself saves in the PLOT chunk. Their state must not also be
/// written by the custom-component chunk: one writer per field, so a load never has to choose.
[[nodiscard]] bool is_builtin_component(ComponentTypeId type_id);

} // namespace content
} // namespace mine::runtime
