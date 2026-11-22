// The built-in content runtime: eight components that are only data plus a function table, and the five
// tile types the mine itself is made of. None of it touches the old Tile/Tile classes or the app, and
// none of it knows a C++ type at run time: every function takes the shared definition and the instance's
// own bytes.
#include <mine/runtime/content/runtime_content.h>
#include <mine/runtime/runtime.h>

#include <support/test_support.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace mine::runtime;
using namespace mine::runtime::content;
using t2d::f32;
using t2d::u32;
using t2d::u64;
using t2d::u8;

namespace {

constexpr u32 kNoRoom = 0;

/// A content entry the way the next round will build one: the shared definition a tile instance reads
/// fixed attributes through, one nested struct per component that needs one.
struct WarehouseDefinition {
    ContainerShared container;
    PreviewShared preview;
};

struct DrillDefinition {
    DrillShared drill;
    PreviewShared preview;
    BuildingShared building;
};

[[nodiscard]] WarehouseDefinition warehouse_definition(u32 capacity) {
    WarehouseDefinition definition{};
    definition.container.capacity = capacity;
    definition.preview.image = "warehouse";
    definition.preview.top_image = nullptr;
    definition.preview.mirrored = 0;
    definition.preview.spin_seconds = 0.0f;
    definition.preview.spin_origin = Vec2f{0.5f, 0.5f};
    return definition;
}

[[nodiscard]] DrillDefinition drill_definition(f32 dig_power) {
    DrillDefinition definition{};
    definition.drill.dig_power = dig_power;
    definition.preview.image = "drill_base";
    definition.preview.top_image = "drill_top";
    definition.preview.mirrored = 0;
    definition.preview.spin_seconds = 2.0f;
    definition.preview.spin_origin = Vec2f{0.25f, 0.75f};
    definition.building.seconds = 5.0f;
    definition.building.material_count = 1;
    definition.building.materials[0] = BuildingMaterial{1, 7, 3};
    return definition;
}

[[nodiscard]] std::optional<Archetype> warehouse_archetype(const ComponentRegistry& components,
                                                           const WarehouseDefinition& definition) {
    return Archetype::build(components, {{kContainerComponent, &definition.container},
                                         {kClickComponent, nullptr},
                                         {kDragComponent, nullptr},
                                         {kPreviewComponent, &definition.preview}});
}

[[nodiscard]] std::optional<Archetype> drill_archetype(const ComponentRegistry& components,
                                                       const DrillDefinition& definition) {
    return Archetype::build(components, {{kTickerComponent, nullptr},
                                         {kPreviewComponent, &definition.preview},
                                         {kBuildingComponent, &definition.building},
                                         {kDrillComponent, &definition.drill}});
}

struct BuiltinFixture {
    ComponentRegistry components;
    TileTypeRegistry tiles;
};

/// Registers the built-in content once. A test that cannot get a fixture skips rather than running
/// against an empty registry.
[[nodiscard]] std::optional<BuiltinFixture> builtin_fixture() {
    BuiltinFixture fixture;
    std::vector<std::string> errors;
    if (!register_builtin_content(fixture.components, fixture.tiles, &errors)) return std::nullopt;
    return fixture;
}

template<class Table>
[[nodiscard]] const Table* content_table(const ObjectData& object, ComponentTypeId component,
                                         InterfaceId interface_id) {
    return static_cast<const Table*>(object.interface(component, interface_id));
}

u32 no_room(const void*, void*) {
    return kNoRoom;
}

} // namespace

T2D_TEST(builtin_content_registers_eight_components_and_five_tile_types) {
    ComponentRegistry components;
    TileTypeRegistry tiles;
    std::vector<std::string> errors;
    T2D_REQUIRE(register_builtin_content(components, tiles, &errors));
    T2D_CHECK(errors.empty());
    T2D_CHECK_EQ(components.count(), 8u);
    T2D_CHECK_EQ(tiles.count(), 5u);

    struct ComponentCheck {
        ComponentTypeId type_id;
        InterfaceId interface_id;
        const void* table;
        const char* name;
        u32 size;
    };
    const ComponentCheck component_checks[] = {
        {kContainerComponent, kContainerInterface, &kContainerFunctions, "container", sizeof(ContainerData)},
        {kClickComponent, kClickInterface, &kClickFunctions, "click", sizeof(ClickData)},
        {kDragComponent, kDragInterface, &kDragFunctions, "drag", sizeof(DragData)},
        {kTickerComponent, kTickerInterface, &kTickerFunctions, "ticker", sizeof(TickerData)},
        {kPreviewComponent, kPreviewInterface, &kPreviewFunctions, "preview", sizeof(PreviewData)},
        {kBuildingComponent, kBuildingInterface, &kBuildingFunctions, "building", sizeof(BuildingData)},
        {kDrillComponent, kDrillInterface, &kDrillFunctions, "drill", sizeof(DrillData)},
        {kTaskComponent, kTaskInterface, &kTaskFunctions, "task", sizeof(TaskData)},
    };
    for (const ComponentCheck& check : component_checks) {
        const ComponentType* type = components.find(check.type_id);
        T2D_REQUIRE(type != nullptr);
        T2D_CHECK_EQ(components.find_by_name(check.name), type);
        T2D_CHECK_EQ(type->version(), kBuiltinContentVersion);
        T2D_CHECK_EQ(type->instance_size(), check.size);
        T2D_CHECK_EQ(type->active_interface(check.interface_id), check.table);
        T2D_CHECK_EQ(type->base_interface(check.interface_id), check.table);
        T2D_CHECK(type->previous_interface(check.interface_id) == nullptr);
    }
    // The task chain carries exactly one interface (count/current/step/advance, plus a rate window's
    // window_progress/record), and its state is the completed count, a reserved word and the live
    // delivery window (docs/GAME_DESIGN.md sections 1.52 and 1.53).
    const ComponentType* task = components.find(kTaskComponent);
    T2D_REQUIRE(task != nullptr);
    T2D_CHECK_EQ(components.find_by_name("task"), task);
    T2D_CHECK_EQ(task->version(), kBuiltinContentVersion);
    T2D_CHECK_EQ(task->instance_size(), static_cast<u32>(sizeof(TaskData)));
    T2D_CHECK_EQ(task->instance_size(), static_cast<u32>(sizeof(TaskData)));
    T2D_CHECK_GE(task->instance_size(), 3u * static_cast<u32>(sizeof(u32))); // completed + reserved + entries
    T2D_CHECK_EQ(task->interface_count(), 1u);

    const ComponentTypeId floor_components[] = {kPreviewComponent, kBuildingComponent};
    const ComponentTypeId ore_components[] = {kPreviewComponent, kBuildingComponent};
    const ComponentTypeId warehouse_components[] = {kContainerComponent, kClickComponent, kDragComponent,
                                                    kPreviewComponent, kBuildingComponent};
    const ComponentTypeId drill_components[] = {kTickerComponent, kPreviewComponent, kBuildingComponent,
                                                kDrillComponent};
    // A 2x1 container of items with a task chain in it, and the click that opens its panel
    // (docs/GAME_DESIGN.md section 1.52).
    const ComponentTypeId task_block_components[] = {kTaskComponent, kContainerComponent, kClickComponent,
                                                     kDragComponent, kPreviewComponent,
                                                     kBuildingComponent};
    struct TileCheck {
        TileTypeId type_id;
        const char* name;
        const void* functions;
        const ComponentTypeId* components;
        u32 component_count;
    };
    const TileCheck tile_checks[] = {
        {kFloorTileType, "floor", &kFloorTileFunctions, floor_components, 2},
        {kOreTileType, "ore", &kOreTileFunctions, ore_components, 2},
        {kWarehouseTileType, "warehouse", &kWarehouseTileFunctions, warehouse_components, 5},
        {kDrillTileType, "drill", &kDrillTileFunctions, drill_components, 4},
        {kTaskBlockTileType, "task_block", &kTaskBlockTileFunctions, task_block_components, 6},
    };
    for (const TileCheck& check : tile_checks) {
        const TileType* type = tiles.find(check.type_id);
        T2D_REQUIRE(type != nullptr);
        T2D_CHECK_EQ(tiles.find_by_name(check.name), type);
        T2D_CHECK_EQ(type->version(), kBuiltinContentVersion);
        T2D_CHECK_EQ(type->functions(), check.functions);
        T2D_CHECK(type->factory() == nullptr);
        T2D_CHECK_EQ(type->component_count(), check.component_count);
        for (u32 index = 0; index < check.component_count; ++index) {
            T2D_CHECK_EQ(type->components()[index], check.components[index]);
        }
        const auto* functions = static_cast<const BuiltinTileFunctions*>(type->functions());
        T2D_REQUIRE(functions != nullptr);
        T2D_CHECK_EQ(std::string_view(functions->role()), std::string_view(check.name));
    }

    // Registering twice reports every refusal and changes nothing.
    errors.clear();
    T2D_CHECK_FALSE(register_builtin_content(components, tiles, &errors));
    T2D_CHECK_EQ(errors.size(), 13u);
    T2D_CHECK_EQ(components.count(), 8u);
    T2D_CHECK_EQ(tiles.count(), 5u);
}

T2D_TEST(a_warehouse_archetype_lays_out_its_components_and_shared_definitions) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const WarehouseDefinition definition = warehouse_definition(4);
    const std::optional<Archetype> built = warehouse_archetype(fixture->components, definition);
    T2D_REQUIRE(built.has_value());
    const Archetype& archetype = *built;

    const usize click_offset = static_cast<usize>(t2d::align_up(sizeof(ContainerData), alignof(ClickData)));
    const usize drag_offset =
        static_cast<usize>(t2d::align_up(click_offset + sizeof(ClickData), alignof(DragData)));
    const usize preview_offset =
        static_cast<usize>(t2d::align_up(drag_offset + sizeof(DragData), alignof(PreviewData)));
    const usize max_align =
        std::max({alignof(ContainerData), alignof(ClickData), alignof(DragData), alignof(PreviewData)});
    const usize total = static_cast<usize>(t2d::align_up(preview_offset + sizeof(PreviewData), max_align));

    T2D_CHECK_EQ(archetype.component_count(), 4u);
    T2D_CHECK_EQ(archetype.offset(kContainerComponent), 0u);
    T2D_CHECK_EQ(archetype.offset(kClickComponent), click_offset);
    T2D_CHECK_EQ(archetype.offset(kDragComponent), drag_offset);
    T2D_CHECK_EQ(archetype.offset(kPreviewComponent), preview_offset);
    T2D_CHECK_EQ(archetype.total_size(), total);
    T2D_CHECK_EQ(archetype.alignment(), max_align);
    for (const Archetype::ComponentLayout& layout : archetype.components()) {
        T2D_CHECK_EQ(layout.offset % layout.type->instance_align(), 0u);
    }

    ObjectData object(archetype);
    T2D_CHECK(object.instance(kContainerComponent) != nullptr);
    T2D_CHECK(object.instance(kClickComponent) != nullptr);
    T2D_CHECK(object.instance(kDragComponent) != nullptr);
    T2D_CHECK(object.instance(kPreviewComponent) != nullptr);
    T2D_CHECK_EQ(object.shared(kContainerComponent), static_cast<const void*>(&definition.container));
    T2D_CHECK_EQ(object.shared(kClickComponent), nullptr);
    T2D_CHECK_EQ(object.shared(kPreviewComponent), static_cast<const void*>(&definition.preview));
    T2D_CHECK_EQ(object.interface(kContainerComponent, kContainerInterface),
                 static_cast<const void*>(&kContainerFunctions));
    T2D_CHECK_EQ(content_table<ContainerFunctions>(object, kContainerComponent, kContainerInterface),
                 &kContainerFunctions);
    const auto container_address = reinterpret_cast<std::uintptr_t>(object.instance(kContainerComponent));
    T2D_CHECK_EQ(container_address % alignof(ContainerData), 0u);

    // Drag has no data of its own but needs the container of the same object; link_drag fills it.
    T2D_REQUIRE(link_drag(object));
    const DragData* drag = static_cast<const DragData*>(object.instance(kDragComponent));
    T2D_REQUIRE(drag != nullptr);
    T2D_CHECK_EQ(drag->container, object.instance(kContainerComponent));
    T2D_CHECK_EQ(drag->container_shared, static_cast<const void*>(&definition.container));
    T2D_CHECK_EQ(drag->container_functions, &kContainerFunctions);
}

T2D_TEST(two_unrelated_shapes_share_a_container_table) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const ContainerShared small{2};
    const ContainerShared large{5};
    const std::optional<Archetype> warehouse_like =
        Archetype::build(fixture->components, {{kContainerComponent, &small}, {kDragComponent, nullptr}});
    const std::optional<Archetype> machine_like =
        Archetype::build(fixture->components, {{kContainerComponent, &large}, {kTickerComponent, nullptr}});
    T2D_REQUIRE(warehouse_like.has_value());
    T2D_REQUIRE(machine_like.has_value());

    ObjectData warehouse(*warehouse_like);
    ObjectData machine(*machine_like);
    // No inheritance, no common base: both reach the same table through their own component type.
    const ContainerFunctions* warehouse_ops =
        content_table<ContainerFunctions>(warehouse, kContainerComponent, kContainerInterface);
    const ContainerFunctions* machine_ops =
        content_table<ContainerFunctions>(machine, kContainerComponent, kContainerInterface);
    T2D_REQUIRE(warehouse_ops != nullptr);
    T2D_REQUIRE(machine_ops != nullptr);
    T2D_CHECK_EQ(warehouse_ops, machine_ops);
    T2D_CHECK_EQ(warehouse_ops->add(&small, warehouse.instance(kContainerComponent), 1, 7, 3), 2u);
    T2D_CHECK_EQ(machine_ops->add(&large, machine.instance(kContainerComponent), 1, 7, 3), 3u);
    T2D_CHECK_EQ(warehouse_ops->total(&small, warehouse.instance(kContainerComponent)), 2u);
    T2D_CHECK_EQ(warehouse_ops->total(&large, machine.instance(kContainerComponent)), 3u);
    T2D_CHECK_EQ(warehouse_ops->room(&small, warehouse.instance(kContainerComponent)), 0u);
    T2D_CHECK_EQ(machine_ops->room(&large, machine.instance(kContainerComponent)), 2u);
}

T2D_TEST(container_adds_merges_takes_and_respects_capacity) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const ContainerShared shared{5};
    const std::optional<Archetype> built =
        Archetype::build(fixture->components, {{kContainerComponent, &shared}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    void* data = object.instance(kContainerComponent);
    const ContainerFunctions* ops =
        content_table<ContainerFunctions>(object, kContainerComponent, kContainerInterface);
    T2D_REQUIRE(ops != nullptr);
    T2D_CHECK_EQ(ops->total(&shared, data), 0u);
    T2D_CHECK_EQ(ops->room(&shared, data), 5u);
    T2D_CHECK_EQ(ops->count(&shared, data), 0u);
    T2D_REQUIRE(ops->slots(&shared, data) != nullptr);

    T2D_CHECK_EQ(ops->add(&shared, data, 1, 101, 2), 2u);
    T2D_CHECK_EQ(ops->add(&shared, data, 1, 101, 2), 2u); // merges into the same stack
    T2D_CHECK_EQ(ops->count(&shared, data), 1u);
    T2D_CHECK_EQ(ops->total(&shared, data), 4u);
    const ContainerSlot* slots = ops->slots(&shared, data);
    T2D_REQUIRE(slots != nullptr);
    T2D_CHECK_EQ(slots[0].kind, 1u);
    T2D_CHECK_EQ(slots[0].id, 101u);
    T2D_CHECK_EQ(slots[0].count, 4u);

    T2D_CHECK_EQ(ops->add(&shared, data, 2, 202, 5), 1u); // only room for one more item
    T2D_CHECK_EQ(ops->count(&shared, data), 2u);
    T2D_CHECK_EQ(ops->total(&shared, data), 5u);
    T2D_CHECK_EQ(ops->add(&shared, data, 3, 303, 1), 0u); // full
    T2D_CHECK_EQ(ops->add(&shared, data, 0, 0, 1), 0u);   // no identity
    T2D_CHECK_EQ(ops->add(&shared, data, 1, 101, 0), 0u); // no amount
    T2D_CHECK_EQ(ops->add(nullptr, data, 1, 101, 1), 0u); // no definition: capacity 0

    T2D_CHECK_EQ(ops->take(&shared, data, 1, 101, 3), 3u);
    T2D_CHECK_EQ(ops->total(&shared, data), 2u);
    T2D_CHECK_EQ(ops->count(&shared, data), 2u);
    T2D_CHECK_EQ(ops->take(&shared, data, 1, 101, 9), 1u);
    T2D_CHECK_EQ(ops->count(&shared, data), 1u); // the empty stack is gone
    T2D_CHECK_EQ(ops->take(&shared, data, 2, 202, 9), 1u);
    T2D_CHECK_EQ(ops->count(&shared, data), 0u);
    T2D_CHECK_EQ(ops->total(&shared, data), 0u);
    T2D_CHECK_EQ(ops->take(&shared, data, 2, 202, 1), 0u);

    // The same data block reads a different capacity from the shared definition it is handed.
    const ContainerShared smaller{2};
    const std::optional<Archetype> other_built =
        Archetype::build(fixture->components, {{kContainerComponent, &smaller}});
    T2D_REQUIRE(other_built.has_value());
    ObjectData other(*other_built);
    T2D_CHECK_EQ(ops->add(&smaller, other.instance(kContainerComponent), 1, 101, 3), 2u);
    T2D_CHECK_EQ(ops->room(&smaller, other.instance(kContainerComponent)), 0u);
}

T2D_TEST(click_toggles_its_panel_and_counts_only_clicks_on_it) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const std::optional<Archetype> built =
        Archetype::build(fixture->components, {{kClickComponent, nullptr}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    const ClickFunctions* ops = content_table<ClickFunctions>(object, kClickComponent, kClickInterface);
    T2D_REQUIRE(ops != nullptr);
    ClickData* data = static_cast<ClickData*>(object.instance(kClickComponent));
    T2D_REQUIRE(data != nullptr);

    T2D_CHECK_EQ(ops->open(nullptr, data), 0u);
    T2D_CHECK_EQ(ops->receive(nullptr, data, 1), 1u);
    T2D_CHECK_EQ(ops->open(nullptr, data), 1u);
    T2D_CHECK_EQ(data->clicks, 1u);
    T2D_CHECK_EQ(ops->receive(nullptr, data, 1), 1u);
    T2D_CHECK_EQ(ops->open(nullptr, data), 0u);
    T2D_CHECK_EQ(ops->receive(nullptr, data, 0), 0u); // a click elsewhere closes, does not count
    T2D_CHECK_EQ(data->clicks, 2u);
    T2D_CHECK_EQ(ops->open(nullptr, data), 0u);
    T2D_CHECK_EQ(ops->receive(nullptr, nullptr, 1), 0u);
    T2D_CHECK_EQ(ops->open(nullptr, nullptr), 0u);
}

T2D_TEST(ticker_advances_on_its_period_and_keeps_the_remainder) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const std::optional<Archetype> built =
        Archetype::build(fixture->components, {{kTickerComponent, nullptr}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    TickerData* data = static_cast<TickerData*>(object.instance(kTickerComponent));
    T2D_REQUIRE(data != nullptr);
    const TickerFunctions* ops = content_table<TickerFunctions>(object, kTickerComponent, kTickerInterface);
    T2D_REQUIRE(ops != nullptr);

    data->period = 2.0f;
    data->elapsed = 0.0f;
    T2D_CHECK_EQ(ops->period(nullptr, data), 2.0f);
    T2D_CHECK_EQ(ops->advance(nullptr, data, 1.5f), 0u);
    T2D_CHECK_NEAR(data->elapsed, 1.5f, 1e-5f);
    T2D_CHECK_EQ(ops->advance(nullptr, data, 0.75f), 1u);
    T2D_CHECK_NEAR(data->elapsed, 0.25f, 1e-5f);
    T2D_CHECK_EQ(ops->advance(nullptr, data, 5.0f), 1u); // one run, remainder carried
    T2D_CHECK_NEAR(data->elapsed, 1.25f, 1e-5f);
    T2D_CHECK_EQ(ops->advance(nullptr, data, 0.0f), 0u);

    data->period = 0.0f; // no period: every tick
    T2D_CHECK_EQ(ops->period(nullptr, data), 0.0f);
    T2D_CHECK_EQ(ops->advance(nullptr, data, 0.0f), 1u);
    T2D_CHECK_EQ(ops->advance(nullptr, nullptr, 1.0f), 0u);
}

T2D_TEST(preview_builds_base_and_turning_top_from_shared) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const PreviewShared plain{"warehouse", nullptr, 1, 0.0f, Vec2f{0.5f, 0.5f}};
    const PreviewShared turning{"drill_base", "drill_top", 0, 2.0f, Vec2f{0.25f, 0.75f}};
    const std::optional<Archetype> built =
        Archetype::build(fixture->components, {{kPreviewComponent, nullptr}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    PreviewData* data = static_cast<PreviewData*>(object.instance(kPreviewComponent));
    T2D_REQUIRE(data != nullptr);
    const PreviewFunctions* ops =
        content_table<PreviewFunctions>(object, kPreviewComponent, kPreviewInterface);
    T2D_REQUIRE(ops != nullptr);

    const TilePreview warehouse = ops->get_preview(&plain, data, 0.0f);
    T2D_CHECK_EQ(warehouse.count, 1u);
    T2D_CHECK(std::string_view(warehouse.parts[0].part) == "base");
    T2D_CHECK(std::string_view(warehouse.parts[0].image) == "warehouse");
    T2D_CHECK_EQ(warehouse.parts[0].mirrored, 1u);
    T2D_CHECK_NEAR(warehouse.parts[0].pivot.x, 0.5f, 1e-6f);
    T2D_CHECK_NEAR(warehouse.parts[0].pivot.y, 0.5f, 1e-6f);

    const TilePreview drill = ops->get_preview(&turning, data, 0.5f);
    T2D_CHECK_EQ(drill.count, 2u);
    T2D_CHECK(std::string_view(drill.parts[0].part) == "base");
    T2D_CHECK(std::string_view(drill.parts[0].image) == "drill_base");
    T2D_CHECK(std::string_view(drill.parts[1].part) == "top");
    T2D_CHECK(std::string_view(drill.parts[1].image) == "drill_top");
    T2D_CHECK_NEAR(drill.parts[1].radians, 1.5707964f, 1e-4f); // 0.5 s of a 2 s turn
    T2D_CHECK_NEAR(drill.parts[1].pivot.x, 0.25f, 1e-6f);
    T2D_CHECK_NEAR(drill.parts[1].pivot.y, 0.75f, 1e-6f);

    T2D_CHECK_EQ(ops->get_preview(nullptr, data, 0.0f).count, 0u);
    const PreviewShared empty{nullptr, nullptr, 0, 0.0f, Vec2f{0.5f, 0.5f}};
    T2D_CHECK_EQ(ops->get_preview(&empty, data, 0.0f).count, 0u);
    const PreviewShared top_only{"", "drill_top", 0, 1.0f, Vec2f{0.5f, 0.5f}};
    T2D_CHECK_EQ(ops->get_preview(&top_only, data, 0.0f).count, 0u); // a top with no base
}

T2D_TEST(building_reports_whether_it_is_still_going_up) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const BuildingShared definition{5.0f, 2, {{1, 7, 3}, {1, 9, 1}}};
    const std::optional<Archetype> built =
        Archetype::build(fixture->components, {{kBuildingComponent, &definition}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    BuildingData* data = static_cast<BuildingData*>(object.instance(kBuildingComponent));
    T2D_REQUIRE(data != nullptr);
    const BuildingFunctions* ops =
        content_table<BuildingFunctions>(object, kBuildingComponent, kBuildingInterface);
    T2D_REQUIRE(ops != nullptr);

    T2D_CHECK_EQ(ops->under_construction(&definition, data), 0u);
    data->under_construction = 1;
    data->elapsed = 2.5f;
    data->delivered = 1;
    T2D_CHECK_EQ(ops->under_construction(&definition, data), 1u);
    T2D_CHECK_EQ(ops->under_construction(&definition, nullptr), 0u);
    ops->set_under_construction(&definition, data, 1);
    T2D_CHECK_EQ(data->under_construction, 1u);
    ops->set_under_construction(&definition, data, 0);
    T2D_CHECK_EQ(data->under_construction, 0u);
    T2D_CHECK_EQ(definition.material_count, 2u);
    T2D_CHECK_EQ(definition.materials[0].count, 3u);
}

T2D_TEST(a_drill_ticks_from_its_shared_power_and_job) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const DrillDefinition definition = drill_definition(1.5f); // hardness 3 / power 1.5 = 2 s a drop
    const std::optional<Archetype> built = drill_archetype(fixture->components, definition);
    T2D_REQUIRE(built.has_value());

    ObjectData first(*built);
    ObjectData second(*built);
    DrillData* first_data = static_cast<DrillData*>(first.instance(kDrillComponent));
    DrillData* second_data = static_cast<DrillData*>(second.instance(kDrillComponent));
    T2D_REQUIRE(first_data != nullptr);
    T2D_REQUIRE(second_data != nullptr);
    const DrillJob job{1, 7, 9, 3.0f, 0.0f};
    first_data->job = job;
    second_data->job = job;
    first_data->rng = 42;
    second_data->rng = 42;

    const DrillFunctions* ops = content_table<DrillFunctions>(first, kDrillComponent, kDrillInterface);
    T2D_REQUIRE(ops != nullptr);
    T2D_CHECK_EQ(ops->produced(&definition.drill, first_data), 0u);
    T2D_CHECK_EQ(ops->tick(&definition.drill, first_data, 5.0f), 2u);
    T2D_CHECK_EQ(ops->tick(&definition.drill, second_data, 5.0f), 2u);
    T2D_CHECK_EQ(ops->produced(&definition.drill, first_data), 2u);
    T2D_CHECK_EQ(first_data->rng, second_data->rng);
    T2D_CHECK_EQ(first_data->side, second_data->side);
    T2D_CHECK(first_data->side < 4u);

    // Less than one period still produces one, the way a due cadence hands a whole span.
    T2D_CHECK_EQ(ops->tick(&definition.drill, first_data, 0.5f), 1u);
    T2D_CHECK_EQ(ops->produced(&definition.drill, first_data), 3u);
    T2D_CHECK_EQ(ops->tick(&definition.drill, first_data, 0.0f), 0u);

    // An explicit period wins over hardness / power.
    ObjectData explicitPeriod(*built);
    DrillData* explicit_data = static_cast<DrillData*>(explicitPeriod.instance(kDrillComponent));
    T2D_REQUIRE(explicit_data != nullptr);
    explicit_data->job = DrillJob{1, 7, 9, 99.0f, 4.0f};
    T2D_CHECK_EQ(ops->tick(&definition.drill, explicit_data, 8.0f), 2u);

    // No drop, or no power and no explicit period, means idle.
    ObjectData idle(*built);
    DrillData* idle_data = static_cast<DrillData*>(idle.instance(kDrillComponent));
    T2D_REQUIRE(idle_data != nullptr);
    idle_data->job = DrillJob{1, 7, 0, 3.0f, 0.0f};
    T2D_CHECK_EQ(ops->tick(&definition.drill, idle_data, 5.0f), 0u);
    idle_data->job = DrillJob{1, 7, 9, 3.0f, 0.0f};
    const DrillShared no_power{0.0f};
    T2D_CHECK_EQ(ops->tick(&no_power, idle_data, 5.0f), 0u);
    T2D_CHECK_EQ(ops->produced(&no_power, idle_data), 0u);
    T2D_CHECK_EQ(ops->tick(nullptr, idle_data, 5.0f), 0u);
}

T2D_TEST(drag_receives_into_its_container_whole_or_not_at_all) {
    const std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const WarehouseDefinition definition = warehouse_definition(3);
    const std::optional<Archetype> built = warehouse_archetype(fixture->components, definition);
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    T2D_REQUIRE(link_drag(object));

    const DragFunctions* drag = content_table<DragFunctions>(object, kDragComponent, kDragInterface);
    const ContainerFunctions* container =
        content_table<ContainerFunctions>(object, kContainerComponent, kContainerInterface);
    T2D_REQUIRE(drag != nullptr);
    T2D_REQUIRE(container != nullptr);
    void* container_data = object.instance(kContainerComponent);
    DragData* drag_data = static_cast<DragData*>(object.instance(kDragComponent));
    T2D_REQUIRE(drag_data != nullptr);
    T2D_CHECK_EQ(drag_data->container, container_data);
    T2D_CHECK_EQ(drag_data->container_functions, container);

    T2D_CHECK_EQ(drag->accepts(nullptr, drag_data, 1, 7, 2), 1u);
    T2D_CHECK_EQ(drag->receive(nullptr, object.instance(kDragComponent), 1, 7, 2), 1u);
    T2D_CHECK_EQ(container->total(&definition.container, container_data), 2u);
    T2D_CHECK_EQ(drag->accepts(nullptr, drag_data, 1, 7, 2), 0u); // only one unit of room left
    T2D_CHECK_EQ(drag->receive(nullptr, object.instance(kDragComponent), 1, 7, 2), 0u);
    T2D_CHECK_EQ(container->total(&definition.container, container_data), 2u); // nothing half went in
    T2D_CHECK_EQ(drag->receive(nullptr, object.instance(kDragComponent), 1, 7, 1), 1u);
    T2D_CHECK_EQ(container->total(&definition.container, container_data), 3u);
    T2D_CHECK_EQ(drag->accepts(nullptr, drag_data, 1, 7, 0), 0u);
    T2D_CHECK_EQ(drag->accepts(nullptr, drag_data, 1, 0, 1), 0u);

    // A drag with nothing to feed answers no, and linking says why.
    const std::optional<Archetype> drag_only =
        Archetype::build(fixture->components, {{kDragComponent, nullptr}});
    T2D_REQUIRE(drag_only.has_value());
    ObjectData lonely(*drag_only);
    T2D_CHECK_FALSE(link_drag(lonely));
    const DragFunctions* lonely_drag = content_table<DragFunctions>(lonely, kDragComponent, kDragInterface);
    T2D_REQUIRE(lonely_drag != nullptr);
    T2D_CHECK_EQ(lonely_drag->accepts(nullptr, lonely.instance(kDragComponent), 1, 7, 1), 0u);
}

T2D_TEST(an_override_redirects_container_calls_and_keeps_the_previous_table) {
    std::optional<BuiltinFixture> fixture = builtin_fixture();
    T2D_REQUIRE(fixture.has_value());
    const WarehouseDefinition definition = warehouse_definition(4);
    const std::optional<Archetype> built = warehouse_archetype(fixture->components, definition);
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);
    const ComponentType* type = fixture->components.find(kContainerComponent);
    T2D_REQUIRE(type != nullptr);
    T2D_CHECK_EQ(type->active_interface(kContainerInterface), static_cast<const void*>(&kContainerFunctions));

    ContainerFunctions patched = kContainerFunctions;
    patched.room = &no_room;
    const void* previous = nullptr;
    std::string error;
    T2D_REQUIRE(fixture->components.override_interface(kContainerComponent, kContainerInterface, &patched,
                                                       &previous, &error));
    T2D_CHECK(error.empty());
    T2D_CHECK_EQ(previous, static_cast<const void*>(&kContainerFunctions));
    T2D_CHECK_EQ(type->active_interface(kContainerInterface), static_cast<const void*>(&patched));
    T2D_CHECK_EQ(type->previous_interface(kContainerInterface),
                 static_cast<const void*>(&kContainerFunctions));
    T2D_CHECK_EQ(type->base_interface(kContainerInterface), static_cast<const void*>(&kContainerFunctions));

    // The object existed before the override and cached nothing: it calls the new table now.
    const ContainerFunctions* active =
        content_table<ContainerFunctions>(object, kContainerComponent, kContainerInterface);
    T2D_REQUIRE(active != nullptr);
    T2D_CHECK_EQ(active, &patched);
    T2D_CHECK_EQ(active->room(&definition.container, object.instance(kContainerComponent)), 0u);
    T2D_CHECK_EQ(active->total(&definition.container, object.instance(kContainerComponent)), 0u);
    // Only room() was replaced in the copy; every other entry is still the game's table.
    T2D_CHECK_EQ(active->add(&definition.container, object.instance(kContainerComponent), 1, 7, 1), 1u);
    T2D_CHECK_EQ(active->total(&definition.container, object.instance(kContainerComponent)), 1u);

    // The previous table is still callable through the pointer the override returned.
    const ContainerFunctions* original = static_cast<const ContainerFunctions*>(previous);
    T2D_REQUIRE(original != nullptr);
    T2D_CHECK_EQ(original->room(&definition.container, object.instance(kContainerComponent)), 3u);
    T2D_CHECK_EQ(original->add(&definition.container, object.instance(kContainerComponent), 1, 7, 1), 1u);

    // A drag linked after the override sees the active table (and therefore the replacement policy).
    T2D_REQUIRE(link_drag(object));
    const DragFunctions* drag = content_table<DragFunctions>(object, kDragComponent, kDragInterface);
    T2D_REQUIRE(drag != nullptr);
    T2D_CHECK_EQ(drag->accepts(nullptr, object.instance(kDragComponent), 1, 7, 1), 0u);
}

T2D_TEST(the_builtin_content_surface_is_c_compatible_and_rtti_free) {
    static_assert(std::is_standard_layout_v<ContainerFunctions>);
    static_assert(std::is_standard_layout_v<ClickFunctions>);
    static_assert(std::is_standard_layout_v<DragFunctions>);
    static_assert(std::is_standard_layout_v<TickerFunctions>);
    static_assert(std::is_standard_layout_v<PreviewFunctions>);
    static_assert(std::is_standard_layout_v<BuildingFunctions>);
    static_assert(std::is_standard_layout_v<DrillFunctions>);
    static_assert(std::is_standard_layout_v<TaskStep>);
    static_assert(std::is_standard_layout_v<TaskShared>);
    static_assert(std::is_standard_layout_v<TaskData>);
    static_assert(std::is_standard_layout_v<TaskFunctions>);
    static_assert(std::is_standard_layout_v<BuiltinTileFunctions>);
    static_assert(std::is_standard_layout_v<ContainerData>);
    static_assert(std::is_standard_layout_v<ClickData>);
    static_assert(std::is_standard_layout_v<DragData>);
    static_assert(std::is_standard_layout_v<TickerData>);
    static_assert(std::is_standard_layout_v<PreviewData>);
    static_assert(std::is_standard_layout_v<BuildingData>);
    static_assert(std::is_standard_layout_v<DrillData>);
    static_assert(std::is_trivially_copyable_v<TaskData>);
    static_assert(!std::is_polymorphic_v<ContainerData>);
    static_assert(!std::is_polymorphic_v<ClickData>);
    static_assert(!std::is_polymorphic_v<DragData>);
    static_assert(!std::is_polymorphic_v<TickerData>);
    static_assert(!std::is_polymorphic_v<PreviewData>);
    static_assert(!std::is_polymorphic_v<BuildingData>);
    static_assert(!std::is_polymorphic_v<DrillData>);
    static_assert(!std::is_polymorphic_v<TaskData>);
    static_assert(!std::is_polymorphic_v<TaskFunctions>);
    static_assert(offsetof(ContainerFunctions, header) == 0);
    static_assert(offsetof(ClickFunctions, header) == 0);
    static_assert(offsetof(DragFunctions, header) == 0);
    static_assert(offsetof(TickerFunctions, header) == 0);
    static_assert(offsetof(PreviewFunctions, header) == 0);
    static_assert(offsetof(BuildingFunctions, header) == 0);
    static_assert(offsetof(DrillFunctions, header) == 0);
    static_assert(offsetof(TaskFunctions, header) == 0);
    static_assert(offsetof(BuiltinTileFunctions, header) == 0);

    T2D_CHECK_EQ(kBuiltinContentVersion, 1u);
    T2D_CHECK_EQ(kContainerFunctions.header.struct_size, static_cast<u32>(sizeof(ContainerFunctions)));
    T2D_CHECK_EQ(kClickFunctions.header.struct_size, static_cast<u32>(sizeof(ClickFunctions)));
    T2D_CHECK_EQ(kDragFunctions.header.struct_size, static_cast<u32>(sizeof(DragFunctions)));
    T2D_CHECK_EQ(kTickerFunctions.header.struct_size, static_cast<u32>(sizeof(TickerFunctions)));
    T2D_CHECK_EQ(kPreviewFunctions.header.struct_size, static_cast<u32>(sizeof(PreviewFunctions)));
    T2D_CHECK_EQ(kBuildingFunctions.header.struct_size, static_cast<u32>(sizeof(BuildingFunctions)));
    T2D_CHECK_EQ(kDrillFunctions.header.struct_size, static_cast<u32>(sizeof(DrillFunctions)));
    T2D_CHECK_EQ(kTaskFunctions.header.struct_size, static_cast<u32>(sizeof(TaskFunctions)));

    const InterfaceTableHeader headers[] = {
        kContainerFunctions.header, kClickFunctions.header,   kDragFunctions.header,
        kTickerFunctions.header,    kPreviewFunctions.header, kBuildingFunctions.header,
        kDrillFunctions.header,     kTaskFunctions.header,
    };
    for (const InterfaceTableHeader& header : headers) {
        T2D_CHECK_GE(header.struct_size, static_cast<u32>(sizeof(InterfaceTableHeader)));
        T2D_CHECK_EQ(header.version, 1u);
    }

    const ComponentTypeId component_ids[] = {kContainerComponent, kClickComponent,   kDragComponent,
                                             kTickerComponent,    kPreviewComponent, kBuildingComponent,
                                             kDrillComponent,     kTaskComponent};
    const InterfaceId interface_ids[] = {kContainerInterface, kClickInterface,   kDragInterface,
                                         kTickerInterface,    kPreviewInterface, kBuildingInterface,
                                         kDrillInterface,     kTaskInterface};
    const TileTypeId tile_type_ids[] = {kFloorTileType, kOreTileType, kWarehouseTileType, kDrillTileType,
                                        kTaskBlockTileType};
    for (usize first = 0; first < std::size(component_ids); ++first) {
        T2D_CHECK(component_ids[first] != kNoComponentType);
        for (usize second = first + 1; second < std::size(component_ids); ++second) {
            T2D_CHECK(component_ids[first] != component_ids[second]);
        }
    }
    for (usize first = 0; first < std::size(interface_ids); ++first) {
        T2D_CHECK(interface_ids[first] != kNoInterface);
        for (usize second = first + 1; second < std::size(interface_ids); ++second) {
            T2D_CHECK(interface_ids[first] != interface_ids[second]);
        }
    }
    for (usize first = 0; first < std::size(tile_type_ids); ++first) {
        T2D_CHECK(tile_type_ids[first] != kNoTileType);
        for (usize second = first + 1; second < std::size(tile_type_ids); ++second) {
            T2D_CHECK(tile_type_ids[first] != tile_type_ids[second]);
        }
    }
}

T2D_TEST_MAIN
