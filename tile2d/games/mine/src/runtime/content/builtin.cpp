#include <mine/runtime/content/builtin.h>

#include <utility>

namespace mine::runtime {
namespace content {
namespace {

template<class Data>
[[nodiscard]] ComponentDesc component_desc(ComponentTypeId type_id, const char* name,
                                           const InterfaceBinding* interfaces, u32 interface_count) {
    ComponentDesc desc{};
    desc.struct_size = static_cast<u32>(sizeof(ComponentDesc));
    desc.type_id = type_id;
    desc.name = name;
    desc.version = kBuiltinContentVersion;
    desc.instance_size = static_cast<u32>(sizeof(Data));
    desc.instance_align = static_cast<u32>(alignof(Data));
    desc.construct = nullptr;
    desc.destruct = nullptr;
    desc.interfaces = interfaces;
    desc.interface_count = interface_count;
    // The built-in components are compiled into the game itself; there is no separately built code
    // table whose ABI record this could be. A mod's components carry one (docs/ABI.md).
    desc.abi_hash = 0;
    return desc;
}

[[nodiscard]] TileTypeDesc tile_desc(TileTypeId type_id, const char* name, const ComponentTypeId* components,
                                     u32 component_count, const void* functions) {
    TileTypeDesc desc{};
    desc.struct_size = static_cast<u32>(sizeof(TileTypeDesc));
    desc.type_id = type_id;
    desc.name = name;
    desc.version = kBuiltinContentVersion;
    desc.components = components;
    desc.component_count = component_count;
    desc.functions = functions;
    desc.factory = nullptr; // the layer that owns tiles builds them from their archetype
    return desc;
}

} // namespace

bool register_builtin_content(ComponentRegistry& components, TileTypeRegistry& tile_types,
                              std::vector<std::string>* errors) {
    bool ok = true;
    const auto register_component = [&](const ComponentDesc& desc) {
        std::string reason;
        if (components.register_component(desc, &reason) == nullptr) {
            ok = false;
            if (errors != nullptr) errors->push_back(std::move(reason));
        }
    };
    const auto register_tile = [&](const TileTypeDesc& desc) {
        std::string reason;
        if (tile_types.register_tile_type(components, desc, &reason) == nullptr) {
            ok = false;
            if (errors != nullptr) errors->push_back(std::move(reason));
        }
    };

    // One binding per table; the descriptor only needs it valid for the registration call.
    const InterfaceBinding container_binding{kContainerInterface, &kContainerFunctions};
    const InterfaceBinding click_binding{kClickInterface, &kClickFunctions};
    const InterfaceBinding drag_binding{kDragInterface, &kDragFunctions};
    const InterfaceBinding ticker_binding{kTickerInterface, &kTickerFunctions};
    const InterfaceBinding preview_binding{kPreviewInterface, &kPreviewFunctions};
    const InterfaceBinding building_binding{kBuildingInterface, &kBuildingFunctions};
    const InterfaceBinding drill_binding{kDrillInterface, &kDrillFunctions};
    const InterfaceBinding task_binding{kTaskInterface, &kTaskFunctions};

    register_component(
        component_desc<ContainerData>(kContainerComponent, "container", &container_binding, 1));
    register_component(component_desc<ClickData>(kClickComponent, "click", &click_binding, 1));
    register_component(component_desc<DragData>(kDragComponent, "drag", &drag_binding, 1));
    register_component(component_desc<TickerData>(kTickerComponent, "ticker", &ticker_binding, 1));
    register_component(component_desc<PreviewData>(kPreviewComponent, "preview", &preview_binding, 1));
    register_component(component_desc<BuildingData>(kBuildingComponent, "building", &building_binding, 1));
    register_component(component_desc<DrillData>(kDrillComponent, "drill", &drill_binding, 1));
    // The task chain: its table reads the shared steps and the plot's own completed count (components.h,
    // docs/GAME_DESIGN.md section 1.52).
    register_component(component_desc<TaskData>(kTaskComponent, "task", &task_binding, 1));

    // Which components a built-in tile is made of. Floors and ore are scenery with a picture; a
    // warehouse is the three halves it acts through plus the picture; a drill adds a cadence, its
    // building half and the machine itself. The task block is a 2x1 container with a task chain in it:
    // a click opens its panel, drag brings items in, and the task half turns those items into progress.
    // The arrays only have to live for the registration call. Every plot has the building half, so the
    // layout the tile type declares and the archetype the game builds agree: a floor/ore can be placed
    // going up, a warehouse can be swept, a drill and a task block too.
    const ComponentTypeId floor_components[] = {kPreviewComponent, kBuildingComponent};
    const ComponentTypeId ore_components[] = {kPreviewComponent, kBuildingComponent};
    const ComponentTypeId warehouse_components[] = {kContainerComponent, kClickComponent, kDragComponent,
                                                    kPreviewComponent, kBuildingComponent};
    const ComponentTypeId drill_components[] = {kTickerComponent, kPreviewComponent, kBuildingComponent,
                                                kDrillComponent};
    const ComponentTypeId task_block_components[] = {kTaskComponent, kContainerComponent, kClickComponent,
                                                     kDragComponent, kPreviewComponent,
                                                     kBuildingComponent};
    register_tile(tile_desc(kFloorTileType, "floor", floor_components, 2, &kFloorTileFunctions));
    register_tile(tile_desc(kOreTileType, "ore", ore_components, 2, &kOreTileFunctions));
    register_tile(
        tile_desc(kWarehouseTileType, "warehouse", warehouse_components, 5, &kWarehouseTileFunctions));
    register_tile(tile_desc(kDrillTileType, "drill", drill_components, 4, &kDrillTileFunctions));
    register_tile(tile_desc(kTaskBlockTileType, "task_block", task_block_components, 6,
                            &kTaskBlockTileFunctions));
    return ok;
}

bool is_builtin_component(ComponentTypeId type_id) {
    return type_id == kContainerComponent || type_id == kClickComponent || type_id == kDragComponent ||
           type_id == kTickerComponent || type_id == kPreviewComponent || type_id == kBuildingComponent ||
           type_id == kDrillComponent || type_id == kTaskComponent;
}

u32 settle_tasks(ObjectData& object, f32 now) {
    void* task_data = object.instance(kTaskComponent);
    const void* task_shared = object.shared(kTaskComponent);
    const TaskFunctions* tasks =
        static_cast<const TaskFunctions*>(object.interface(kTaskComponent, kTaskInterface));
    void* container_data = object.instance(kContainerComponent);
    const void* container_shared = object.shared(kContainerComponent);
    const ContainerFunctions* container =
        static_cast<const ContainerFunctions*>(object.interface(kContainerComponent, kContainerInterface));
    // A plot without the whole pair (a task chain and a container to deliver through) settles nothing:
    // the content naming one without the other is the data's problem, not a reason to guess.
    if (task_data == nullptr || task_shared == nullptr || tasks == nullptr || container_data == nullptr ||
        container_shared == nullptr || container == nullptr) {
        return 0;
    }
    u32 settled = 0;
    const u32 steps = tasks->count(task_shared, task_data);
    for (u32 index = 0; index < steps; ++index) {
        const u32 current = tasks->current(task_shared, task_data);
        if (current >= steps) break;
        TaskStep step{};
        if (tasks->step(task_shared, task_data, current, &step) == 0) break;
        if (step.count == 0) break;
        // How the step is satisfied is its **type's** business: deliver counts what the container holds,
        // rate counts what arrived within its window at the layer's clock. Both then consume exactly
        // what the step asked for, so a surplus stays in the box (docs/GAME_DESIGN.md section 1.53).
        if (step.type == kTaskTypeDeliver) {
            const ContainerSlot* slots = container->slots(container_shared, container_data);
            const u32 used = container->count(container_shared, container_data);
            u64 held = 0;
            for (u32 slot = 0; slots != nullptr && slot < used; ++slot) {
                if (slots[slot].kind != step.kind || slots[slot].id != step.id) continue;
                held += slots[slot].count;
            }
            if (held < static_cast<u64>(step.count)) break;
        } else if (step.type == kTaskTypeRate) {
            // A rate step with a hold is not satisfied by one good window: its condition has to have
            // been continuously true for that long. The layer's tick grows the held time; the settle
            // only reads it (docs/GAME_DESIGN.md section 1.54).
            if (step.hold > 0.0f) {
                if (tasks->held(task_shared, task_data) < step.hold) break;
            } else if (tasks->window_progress(task_shared, task_data, now) < step.count) {
                break;
            }
        } else {
            // A step of a type this build does not know stops the chain where it stands: what it asks
            // for cannot be guessed.
            break;
        }
        // The step is done: its items are **consumed** by delivering them, and only then does the chain
        // move on. A take that somehow comes up short stops here rather than losing the difference.
        if (container->take(container_shared, container_data, step.kind, step.id, step.count) != step.count) {
            break;
        }
        tasks->advance(task_shared, task_data);
        ++settled;
    }
    return settled;
}

bool link_drag(ObjectData& object) {
    DragData* drag = static_cast<DragData*>(object.instance(kDragComponent));
    if (drag == nullptr) return false;
    void* container = object.instance(kContainerComponent);
    const void* container_shared = object.shared(kContainerComponent);
    const void* container_table = object.interface(kContainerComponent, kContainerInterface);
    if (container == nullptr || container_table == nullptr) {
        *drag = DragData{};
        return false;
    }
    drag->container = container;
    drag->container_shared = container_shared;
    drag->container_functions = static_cast<const ContainerFunctions*>(container_table);
    return true;
}

} // namespace content
} // namespace mine::runtime
