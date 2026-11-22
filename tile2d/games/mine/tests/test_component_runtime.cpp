// The runtime core: components whose instances are only bytes, component types that own the function
// tables, archetypes that decide where each component sits, and objects that are one aligned block.
//
// No test here uses inheritance. A "fake entity" and a "fake tile" are two archetypes, and the same
// component table serves both because a function receives the instance's data pointer, not a C++ type.
#include <mine/runtime/runtime.h>

#include <support/test_support.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace mine::runtime;
using t2d::i32;
using t2d::u32;
using t2d::u64;
using t2d::u8;

namespace {

// Component ids are written down, not handed out: a code table must name the same component as the
// game whatever order the tables register in.
constexpr ComponentTypeId kHealth = 0x1001;
constexpr ComponentTypeId kPosition = 0x1002;
constexpr ComponentTypeId kLabel = 0x1003;
constexpr ComponentTypeId kOneByte = 0x1004;
constexpr ComponentTypeId kWord = 0x1005;
constexpr ComponentTypeId kWide = 0x1006;
constexpr ComponentTypeId kCountedFirst = 0x1007;
constexpr ComponentTypeId kCountedSecond = 0x1008;
constexpr ComponentTypeId kAbsent = 0x1FFF;

constexpr InterfaceId kHealthActive = 1;
constexpr InterfaceId kHealthPassive = 2;
constexpr InterfaceId kAbsentInterface = 99;

constexpr TileTypeId kDirtTile = 1;
constexpr TileTypeId kStoneTile = 2;
constexpr TileTypeId kEmptyTile = 3;

struct Health {
    i32 current;
    i32 maximum;
};

struct Position {
    i32 x;
    i32 y;
};

struct Label {
    char text[16];
};

struct OneByte {
    u8 value;
};

struct Word {
    u32 value;
};

struct alignas(16) Wide {
    u8 bytes[4];
};

struct HealthTable {
    InterfaceTableHeader header;
    i32 (*get)(const void* self);
    void (*damage)(void* self, i32 amount);
};

struct HealthInfoTable {
    InterfaceTableHeader header;
    const char* (*label)(const void* self);
};

i32 health_current(const void* self) {
    return static_cast<const Health*>(self)->current;
}

i32 health_current_doubled(const void* self) {
    return static_cast<const Health*>(self)->current * 2;
}

i32 health_current_tripled(const void* self) {
    return static_cast<const Health*>(self)->current * 3;
}

void health_damage(void* self, i32 amount) {
    Health* health = static_cast<Health*>(self);
    health->current = health->current > amount ? health->current - amount : 0;
}

void health_construct(void* self) {
    Health* health = static_cast<Health*>(self);
    health->current = 10;
    health->maximum = 10;
}

const char* health_label(const void*) {
    return "health";
}

constexpr HealthTable kHealthTable{
    {static_cast<u32>(sizeof(HealthTable)), 1}, &health_current, &health_damage};
constexpr HealthTable kPatchedTable{
    {static_cast<u32>(sizeof(HealthTable)), 1}, &health_current_doubled, &health_damage};
constexpr HealthInfoTable kHealthInfoTable{{static_cast<u32>(sizeof(HealthInfoTable)), 1}, &health_label};

struct HealthDefinition {
    i32 starting;
    i32 cap;
};

std::vector<ComponentTypeId> g_constructed;
std::vector<ComponentTypeId> g_destroyed;
bool g_construct_saw_zero = true;

void counted_first_construct(void* self) {
    Health* health = static_cast<Health*>(self);
    if (health->current != 0 || health->maximum != 0) g_construct_saw_zero = false;
    health->current = 11;
    health->maximum = 11;
    g_constructed.push_back(kCountedFirst);
}

void counted_first_destruct(void* self) {
    static_cast<Health*>(self)->current = 0;
    g_destroyed.push_back(kCountedFirst);
}

void counted_second_construct(void* self) {
    Position* position = static_cast<Position*>(self);
    if (position->x != 0 || position->y != 0) g_construct_saw_zero = false;
    position->x = 22;
    position->y = 22;
    g_constructed.push_back(kCountedSecond);
}

void counted_second_destruct(void* self) {
    static_cast<Position*>(self)->x = 0;
    g_destroyed.push_back(kCountedSecond);
}

void reset_counters() {
    g_constructed.clear();
    g_destroyed.clear();
    g_construct_saw_zero = true;
}

struct TileFunctions {
    InterfaceTableHeader header;
    void (*draw)(const void* self);
};

void tile_draw(const void*) {}

constexpr TileFunctions kTileFunctions{{static_cast<u32>(sizeof(TileFunctions)), 1}, &tile_draw};

void* tile_factory(const void*) {
    return nullptr;
}

u32 word_save(const void* shared, const void* data, void* out, u32 capacity) {
    if (shared == nullptr || data == nullptr) return 0;
    constexpr u32 kSize = sizeof(u32);
    if (out == nullptr || capacity < kSize) return kSize;
    *static_cast<u32*>(out) = static_cast<const Word*>(data)->value + *static_cast<const u32*>(shared);
    return kSize;
}

u32 word_load(const void* shared, void* data, const void* in, u32 size) {
    if (shared == nullptr || data == nullptr || in == nullptr || size != sizeof(u32)) return 0;
    static_cast<Word*>(data)->value = *static_cast<const u32*>(in) - *static_cast<const u32*>(shared);
    return sizeof(u32);
}

template<class T>
[[nodiscard]] ComponentDesc desc_of(ComponentTypeId type_id, const char* name,
                                    const InterfaceBinding* interfaces = nullptr, u32 interface_count = 0,
                                    void (*construct)(void* data) = nullptr,
                                    void (*destruct)(void* data) = nullptr, u32 version = 1) {
    ComponentDesc desc{};
    desc.struct_size = static_cast<u32>(sizeof(ComponentDesc));
    desc.type_id = type_id;
    desc.name = name;
    desc.version = version;
    desc.instance_size = static_cast<u32>(sizeof(T));
    desc.instance_align = static_cast<u32>(alignof(T));
    desc.construct = construct;
    desc.destruct = destruct;
    desc.interfaces = interfaces;
    desc.interface_count = interface_count;
    desc.abi_hash = 0x0123'4567'89AB'CDEFull;
    return desc;
}

} // namespace

T2D_TEST(registration_stores_the_schema_and_the_declared_tables) {
    const InterfaceBinding bindings[] = {{kHealthActive, &kHealthTable}, {kHealthPassive, &kHealthInfoTable}};
    ComponentRegistry registry;
    std::string error;
    const ComponentType* type = registry.register_component(
        desc_of<Health>(kHealth, "health", bindings, 2u, &health_construct, nullptr, 3u), &error);
    T2D_REQUIRE(type != nullptr);
    T2D_CHECK(error.empty());

    T2D_CHECK_EQ(type->type_id(), kHealth);
    T2D_CHECK_EQ(type->name(), std::string_view("health"));
    T2D_CHECK_EQ(type->version(), 3u);
    T2D_CHECK_EQ(type->instance_size(), static_cast<u32>(sizeof(Health)));
    T2D_CHECK_EQ(type->instance_align(), static_cast<u32>(alignof(Health)));
    T2D_CHECK_EQ(type->abi_hash(), 0x0123'4567'89AB'CDEFull);
    T2D_CHECK(type->has_constructor());
    T2D_CHECK_FALSE(type->has_destructor());

    T2D_CHECK_EQ(registry.count(), 1u);
    T2D_CHECK_EQ(registry.find(kHealth), type);
    T2D_CHECK_EQ(registry.find_by_name("health"), type);
    T2D_CHECK(registry.find(kAbsent) == nullptr);
    T2D_CHECK(registry.find_by_name("nothing") == nullptr);

    T2D_CHECK_EQ(type->interface_count(), 2u);
    T2D_CHECK(type->has_interface(kHealthActive));
    T2D_CHECK_FALSE(type->has_interface(kAbsentInterface));
    T2D_CHECK_EQ(type->active_interface(kHealthActive), static_cast<const void*>(&kHealthTable));
    T2D_CHECK_EQ(type->base_interface(kHealthActive), static_cast<const void*>(&kHealthTable));
    T2D_CHECK(type->previous_interface(kHealthActive) == nullptr);
    T2D_CHECK_EQ(type->active_interface(kHealthPassive), static_cast<const void*>(&kHealthInfoTable));
    T2D_CHECK(type->active_interface(kAbsentInterface) == nullptr);
    T2D_CHECK(type->previous_interface(kAbsentInterface) == nullptr);
}

T2D_TEST(malformed_and_duplicate_descriptors_are_refused) {
    const InterfaceBinding bindings[] = {{kHealthActive, &kHealthTable}};
    ComponentRegistry registry;
    std::string error;
    const ComponentDesc good = desc_of<Health>(kHealth, "health", bindings, 1u);
    T2D_REQUIRE(registry.register_component(good, &error) != nullptr);
    T2D_CHECK(error.empty());

    const auto refused = [&registry](const ComponentDesc& desc) {
        std::string reason;
        const ComponentType* type = registry.register_component(desc, &reason);
        T2D_CHECK(type == nullptr);
        T2D_CHECK(!reason.empty());
    };

    ComponentDesc bad = good;
    bad.struct_size = static_cast<u32>(kComponentDescPrefixSize) - 1u;
    refused(bad);

    bad = good;
    bad.type_id = kNoComponentType;
    refused(bad);

    bad = good;
    bad.name = nullptr;
    refused(bad);

    bad = good;
    bad.name = "";
    refused(bad);

    bad = good;
    bad.version = 0;
    refused(bad);

    bad = good;
    bad.instance_size = 0;
    refused(bad);

    bad = good;
    bad.instance_align = 0;
    refused(bad);

    bad = good;
    bad.instance_align = 3;
    refused(bad);

    // A second type under the same id, and a second type under the same name: both are refused rather
    // than letting two schemas claim one identity.
    bad = good;
    bad.name = "other";
    refused(bad);
    bad = good;
    bad.type_id = kPosition;
    refused(bad);

    const InterfaceBinding zero_interface[] = {{kNoInterface, &kHealthTable}};
    refused(desc_of<Health>(0x2001, "iface_zero", zero_interface, 1u));

    const InterfaceBinding null_table[] = {{kHealthActive, nullptr}};
    refused(desc_of<Health>(0x2002, "iface_null", null_table, 1u));

    const InterfaceBinding twice[] = {{kHealthActive, &kHealthTable}, {kHealthActive, &kHealthInfoTable}};
    refused(desc_of<Health>(0x2003, "iface_twice", twice, 2u));

    bad = desc_of<Health>(0x2004, "iface_missing");
    bad.interface_count = 1;
    refused(bad);

    std::string reason;
    ComponentDesc duplicate = good;
    duplicate.name = "other";
    T2D_CHECK(registry.register_component(duplicate, &reason) == nullptr);
    T2D_CHECK(reason.find("already registered") != std::string::npos);
    T2D_CHECK_EQ(registry.count(), 1u);
    T2D_CHECK_EQ(registry.find(kHealth)->name(), std::string_view("health"));
}

T2D_TEST(archetypes_compute_aligned_offsets_and_total_size) {
    ComponentRegistry registry;
    T2D_REQUIRE(registry.register_component(desc_of<OneByte>(kOneByte, "one_byte")) != nullptr);
    T2D_REQUIRE(registry.register_component(desc_of<Word>(kWord, "word")) != nullptr);
    T2D_REQUIRE(registry.register_component(desc_of<Wide>(kWide, "wide")) != nullptr);

    const HealthDefinition one_shared{1, 2};
    const HealthDefinition wide_shared{3, 4};
    const std::optional<Archetype> built =
        Archetype::build(registry, {{kOneByte, &one_shared}, {kWord, nullptr}, {kWide, &wide_shared}});
    T2D_REQUIRE(built.has_value());
    const Archetype& archetype = *built;

    T2D_CHECK_EQ(archetype.component_count(), 3u);
    T2D_CHECK_FALSE(archetype.empty());
    T2D_CHECK_EQ(archetype.total_size(), 32u);
    T2D_CHECK_EQ(archetype.alignment(), 16u);
    T2D_CHECK_EQ(archetype.offset(kOneByte), 0u);
    T2D_CHECK_EQ(archetype.offset(kWord), 4u);
    T2D_CHECK_EQ(archetype.offset(kWide), 16u);
    T2D_CHECK_EQ(archetype.offset(kAbsent), kNoOffset);
    T2D_CHECK(archetype.find(kAbsent) == nullptr);
    T2D_CHECK_EQ(archetype.type(kWide), registry.find(kWide));
    T2D_CHECK(archetype.type(kAbsent) == nullptr);
    T2D_CHECK_EQ(archetype.shared(kOneByte), static_cast<const void*>(&one_shared));
    T2D_CHECK(archetype.shared(kWord) == nullptr);
    T2D_CHECK(archetype.shared(kAbsent) == nullptr);

    const ConstSpan<Archetype::ComponentLayout> layout = archetype.components();
    T2D_REQUIRE(layout.size() == 3u);
    T2D_CHECK_EQ(layout[0].type->type_id(), kOneByte);
    T2D_CHECK_EQ(layout[1].type->type_id(), kWord);
    T2D_CHECK_EQ(layout[2].type->type_id(), kWide);
    T2D_CHECK_EQ(layout[0].offset, 0u);
    T2D_CHECK_EQ(layout[1].offset, 4u);
    T2D_CHECK_EQ(layout[2].offset, 16u);

    const std::optional<Archetype> empty = Archetype::build(registry, {});
    T2D_REQUIRE(empty.has_value());
    T2D_CHECK(empty->empty());
    T2D_CHECK_EQ(empty->total_size(), 0u);
    T2D_CHECK_EQ(empty->alignment(), 1u);

    std::string error;
    T2D_CHECK_FALSE(Archetype::build(registry, {{kAbsent, nullptr}}, &error).has_value());
    T2D_CHECK(!error.empty());
    error.clear();
    T2D_CHECK_FALSE(Archetype::build(registry, {{kWord, nullptr}, {kWord, nullptr}}, &error).has_value());
    T2D_CHECK(!error.empty());
}

T2D_TEST(two_unrelated_object_shapes_call_the_same_component_table) {
    const InterfaceBinding bindings[] = {{kHealthActive, &kHealthTable}};
    ComponentRegistry registry;
    const ComponentType* health_type =
        registry.register_component(desc_of<Health>(kHealth, "health", bindings, 1u, &health_construct));
    T2D_REQUIRE(health_type != nullptr);
    T2D_REQUIRE(registry.register_component(desc_of<Position>(kPosition, "position")) != nullptr);
    T2D_REQUIRE(registry.register_component(desc_of<Label>(kLabel, "label")) != nullptr);

    // An entity-like shape and a tile-like shape: no common base class, only the same component.
    const std::optional<Archetype> entity_shape =
        Archetype::build(registry, {{kHealth, nullptr}, {kPosition, nullptr}});
    const std::optional<Archetype> tile_shape =
        Archetype::build(registry, {{kHealth, nullptr}, {kLabel, nullptr}});
    T2D_REQUIRE(entity_shape.has_value());
    T2D_REQUIRE(tile_shape.has_value());

    ObjectData entity(*entity_shape);
    ObjectData tile(*tile_shape);
    T2D_CHECK_EQ(entity.archetype().offset(kPosition), 8u);
    T2D_CHECK_EQ(tile.archetype().offset(kLabel), 8u);
    T2D_CHECK(entity.instance(kHealth) != tile.instance(kHealth));
    T2D_CHECK(entity.instance(kPosition) != nullptr);
    T2D_CHECK(tile.instance(kLabel) != nullptr);
    T2D_CHECK(entity.instance(kLabel) == nullptr);
    T2D_CHECK(tile.instance(kPosition) == nullptr);

    const HealthTable* entity_table =
        static_cast<const HealthTable*>(entity.interface(kHealth, kHealthActive));
    const HealthTable* tile_table = static_cast<const HealthTable*>(tile.interface(kHealth, kHealthActive));
    T2D_REQUIRE(entity_table != nullptr);
    T2D_REQUIRE(tile_table != nullptr);
    T2D_CHECK(entity_table == tile_table);
    T2D_CHECK_EQ(entity_table->get(entity.instance(kHealth)), 10);
    entity_table->damage(entity.instance(kHealth), 4);
    entity_table->damage(tile.instance(kHealth), 7);
    T2D_CHECK_EQ(static_cast<const Health*>(entity.instance(kHealth))->current, 6);
    T2D_CHECK_EQ(static_cast<const Health*>(tile.instance(kHealth))->current, 3);

    const auto entity_address = reinterpret_cast<std::uintptr_t>(entity.instance(kHealth));
    const auto tile_address = reinterpret_cast<std::uintptr_t>(tile.instance(kHealth));
    T2D_CHECK_EQ(entity_address % alignof(Health), 0u);
    T2D_CHECK_EQ(tile_address % alignof(Health), 0u);
}

T2D_TEST(shared_definitions_are_per_archetype_and_read_only) {
    ComponentRegistry registry;
    T2D_REQUIRE(registry.register_component(desc_of<Health>(kHealth, "health")) != nullptr);
    const HealthDefinition stone{5, 20};
    const HealthDefinition copper{9, 12};

    const std::optional<Archetype> stone_shape = Archetype::build(registry, {{kHealth, &stone}});
    const std::optional<Archetype> copper_shape = Archetype::build(registry, {{kHealth, &copper}});
    const std::optional<Archetype> plain_shape = Archetype::build(registry, {{kHealth, nullptr}});
    T2D_REQUIRE(stone_shape.has_value());
    T2D_REQUIRE(copper_shape.has_value());
    T2D_REQUIRE(plain_shape.has_value());

    T2D_CHECK_EQ(stone_shape->type(kHealth), copper_shape->type(kHealth));
    T2D_CHECK_EQ(stone_shape->shared(kHealth), static_cast<const void*>(&stone));
    T2D_CHECK_EQ(copper_shape->shared(kHealth), static_cast<const void*>(&copper));
    T2D_CHECK(stone_shape->shared(kHealth) != copper_shape->shared(kHealth));
    T2D_CHECK(plain_shape->shared(kHealth) == nullptr);

    ObjectData stone_object(*stone_shape);
    ObjectData copper_object(*copper_shape);
    T2D_CHECK_EQ(stone_object.shared(kHealth), static_cast<const void*>(&stone));
    T2D_CHECK_EQ(copper_object.shared(kHealth), static_cast<const void*>(&copper));
    T2D_CHECK(stone_object.shared(kAbsent) == nullptr);

    const auto* definition = static_cast<const HealthDefinition*>(stone_object.shared(kHealth));
    T2D_REQUIRE(definition != nullptr);
    T2D_CHECK_EQ(definition->starting, 5);
    T2D_CHECK_EQ(definition->cap, 20);
    T2D_CHECK_EQ(stone.starting, 5); // the runtime holds a pointer, it never writes the definition
    T2D_CHECK_EQ(copper.starting, 9);
}

T2D_TEST(objects_construct_every_component_and_destroy_them_in_reverse) {
    reset_counters();
    ComponentRegistry registry;
    T2D_REQUIRE(registry.register_component(desc_of<Health>(kCountedFirst, "first", nullptr, 0,
                                                            &counted_first_construct,
                                                            &counted_first_destruct)) != nullptr);
    T2D_REQUIRE(registry.register_component(desc_of<Position>(kCountedSecond, "second", nullptr, 0,
                                                              &counted_second_construct,
                                                              &counted_second_destruct)) != nullptr);
    const std::optional<Archetype> built =
        Archetype::build(registry, {{kCountedFirst, nullptr}, {kCountedSecond, nullptr}});
    T2D_REQUIRE(built.has_value());
    T2D_CHECK(g_constructed.empty()); // building an archetype constructs no instance

    {
        ObjectData object(*built);
        T2D_CHECK(g_construct_saw_zero);
        T2D_REQUIRE(g_constructed.size() == 2u);
        T2D_CHECK_EQ(g_constructed[0], kCountedFirst);
        T2D_CHECK_EQ(g_constructed[1], kCountedSecond);
        T2D_CHECK(g_destroyed.empty());
        T2D_CHECK_EQ(static_cast<const Health*>(object.instance(kCountedFirst))->current, 11);
        T2D_CHECK_EQ(static_cast<const Position*>(object.instance(kCountedSecond))->x, 22);
    }

    T2D_REQUIRE(g_destroyed.size() == 2u);
    T2D_CHECK_EQ(g_destroyed[0], kCountedSecond);
    T2D_CHECK_EQ(g_destroyed[1], kCountedFirst);
}

T2D_TEST(interfaces_are_queried_through_the_component_type) {
    const InterfaceBinding bindings[] = {{kHealthActive, &kHealthTable}, {kHealthPassive, &kHealthInfoTable}};
    ComponentRegistry registry;
    std::string error;
    T2D_REQUIRE(registry.register_component(
                    desc_of<Health>(kHealth, "health", bindings, 2u, &health_construct), &error) != nullptr);
    const std::optional<Archetype> built = Archetype::build(registry, {{kHealth, nullptr}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);

    const HealthTable* active = static_cast<const HealthTable*>(object.interface(kHealth, kHealthActive));
    const HealthInfoTable* info =
        static_cast<const HealthInfoTable*>(object.interface(kHealth, kHealthPassive));
    T2D_REQUIRE(active != nullptr);
    T2D_REQUIRE(info != nullptr);
    T2D_CHECK_EQ(active, &kHealthTable);
    T2D_CHECK_EQ(info, &kHealthInfoTable);
    T2D_CHECK_EQ(active->header.struct_size, static_cast<u32>(sizeof(HealthTable)));
    T2D_CHECK_EQ(active->header.version, 1u);
    T2D_CHECK_EQ(active->get(object.instance(kHealth)), 10);
    T2D_CHECK(std::string_view(info->label(object.instance(kHealth))) == "health");

    T2D_CHECK(object.interface(kHealth, kAbsentInterface) == nullptr);
    T2D_CHECK(object.interface(kAbsent, kHealthActive) == nullptr);
    T2D_CHECK(object.interface(kPosition, kHealthActive) == nullptr);
    T2D_CHECK_FALSE(object.contains(kPosition));
    T2D_CHECK(object.contains(kHealth));
}

T2D_TEST(an_override_reaches_existing_objects_and_keeps_the_previous_table) {
    const InterfaceBinding bindings[] = {{kHealthActive, &kHealthTable}};
    ComponentRegistry registry;
    std::string error;
    T2D_REQUIRE(registry.register_component(
                    desc_of<Health>(kHealth, "health", bindings, 1u, &health_construct), &error) != nullptr);
    const std::optional<Archetype> built = Archetype::build(registry, {{kHealth, nullptr}});
    T2D_REQUIRE(built.has_value());
    ObjectData object(*built);

    const ComponentType* type = registry.find(kHealth);
    T2D_REQUIRE(type != nullptr);
    T2D_CHECK_EQ(static_cast<const HealthTable*>(object.interface(kHealth, kHealthActive)), &kHealthTable);
    T2D_CHECK_EQ(static_cast<const Health*>(object.instance(kHealth))->current, 10);

    const void* previous = nullptr;
    T2D_REQUIRE(registry.override_interface(kHealth, kHealthActive, &kPatchedTable, &previous, &error));
    T2D_CHECK(error.empty());
    T2D_CHECK_EQ(previous, static_cast<const void*>(&kHealthTable));
    T2D_CHECK_EQ(type->active_interface(kHealthActive), static_cast<const void*>(&kPatchedTable));
    T2D_CHECK_EQ(type->previous_interface(kHealthActive), static_cast<const void*>(&kHealthTable));
    T2D_CHECK_EQ(type->base_interface(kHealthActive), static_cast<const void*>(&kHealthTable));

    // The object existed before the override. It cached no table pointer, so it sees the new one.
    const HealthTable* active = static_cast<const HealthTable*>(object.interface(kHealth, kHealthActive));
    T2D_REQUIRE(active != nullptr);
    T2D_CHECK_EQ(active->get(object.instance(kHealth)), 20);

    // The table the override replaced is still callable through the pointer the override returned.
    const HealthTable* original = static_cast<const HealthTable*>(previous);
    T2D_REQUIRE(original != nullptr);
    T2D_CHECK_EQ(original->get(object.instance(kHealth)), 10);

    // A partial override: the caller copies the current table and replaces only one entry.
    HealthTable partial = kPatchedTable;
    partial.get = &health_current_tripled;
    previous = nullptr;
    error.clear();
    T2D_REQUIRE(registry.override_interface(kHealth, kHealthActive, &partial, &previous, &error));
    T2D_CHECK_EQ(previous, static_cast<const void*>(&kPatchedTable));
    T2D_CHECK_EQ(type->previous_interface(kHealthActive), static_cast<const void*>(&kPatchedTable));
    T2D_CHECK_EQ(type->base_interface(kHealthActive), static_cast<const void*>(&kHealthTable));
    T2D_CHECK_EQ(static_cast<const HealthTable*>(object.interface(kHealth, kHealthActive)),
                 static_cast<const void*>(&partial));
    T2D_CHECK_EQ(static_cast<const HealthTable*>(object.interface(kHealth, kHealthActive))
                     ->get(object.instance(kHealth)),
                 30);

    // Refusals leave the active table where it was.
    previous = &kHealthTable;
    T2D_CHECK_FALSE(registry.override_interface(kAbsent, kHealthActive, &partial, &previous, &error));
    T2D_CHECK(previous == nullptr);
    T2D_CHECK(!error.empty());
    error.clear();
    previous = &kHealthTable;
    T2D_CHECK_FALSE(registry.override_interface(kHealth, kAbsentInterface, &partial, &previous, &error));
    T2D_CHECK(previous == nullptr);
    T2D_CHECK(!error.empty());
    error.clear();
    T2D_CHECK_FALSE(registry.override_interface(kHealth, kHealthActive, nullptr, &previous, &error));
    T2D_CHECK(!error.empty());
    T2D_CHECK_EQ(type->active_interface(kHealthActive), static_cast<const void*>(&partial));
}

T2D_TEST(tile_types_register_against_their_components) {
    ComponentRegistry components;
    T2D_REQUIRE(components.register_component(desc_of<Health>(kHealth, "health")) != nullptr);
    T2D_REQUIRE(components.register_component(desc_of<Position>(kPosition, "position")) != nullptr);

    const ComponentTypeId dirt_components[] = {kHealth, kPosition};
    TileTypeRegistry tiles;
    std::string error;
    TileTypeDesc dirt{};
    dirt.struct_size = static_cast<u32>(sizeof(TileTypeDesc));
    dirt.type_id = kDirtTile;
    dirt.name = "dirt";
    dirt.version = 1;
    dirt.components = dirt_components;
    dirt.component_count = 2;
    dirt.functions = &kTileFunctions;
    dirt.factory = nullptr;
    const TileType* registered = tiles.register_tile_type(components, dirt, &error);
    T2D_REQUIRE(registered != nullptr);
    T2D_CHECK(error.empty());
    T2D_CHECK_EQ(registered->type_id(), kDirtTile);
    T2D_CHECK_EQ(registered->name(), std::string_view("dirt"));
    T2D_CHECK_EQ(registered->version(), 1u);
    T2D_CHECK_EQ(registered->component_count(), 2u);
    T2D_CHECK_EQ(registered->components()[0], kHealth);
    T2D_CHECK_EQ(registered->components()[1], kPosition);
    T2D_CHECK_EQ(registered->functions(), static_cast<const void*>(&kTileFunctions));
    T2D_CHECK(registered->factory() == nullptr);
    T2D_CHECK_EQ(tiles.count(), 1u);
    T2D_CHECK_EQ(tiles.find(kDirtTile), registered);
    T2D_CHECK_EQ(tiles.find_by_name("dirt"), registered);
    T2D_CHECK(tiles.find(kStoneTile) == nullptr);
    T2D_CHECK(tiles.find_by_name("stone") == nullptr);

    // A data-only tile type: no components, no function table, a factory. All three are legal; what
    // they mean is the layer's business.
    TileTypeDesc stone = dirt;
    stone.type_id = kStoneTile;
    stone.name = "stone";
    stone.components = nullptr;
    stone.component_count = 0;
    stone.functions = nullptr;
    stone.factory = &tile_factory;
    const TileType* second = tiles.register_tile_type(components, stone, &error);
    T2D_REQUIRE(second != nullptr);
    T2D_CHECK_EQ(second->component_count(), 0u);
    T2D_CHECK(second->functions() == nullptr);
    T2D_CHECK(second->factory() == &tile_factory);
    T2D_CHECK_EQ(tiles.count(), 2u);

    const auto refused = [&tiles, &components](const TileTypeDesc& desc) {
        std::string reason;
        T2D_CHECK(tiles.register_tile_type(components, desc, &reason) == nullptr);
        T2D_CHECK(!reason.empty());
    };

    TileTypeDesc bad = dirt;
    bad.type_id = kEmptyTile;
    refused(bad); // duplicate name "dirt"
    bad = dirt;
    bad.name = "other";
    refused(bad); // duplicate id
    bad = dirt;
    bad.struct_size = static_cast<u32>(kTileTypeDescPrefixSize) - 1u;
    bad.type_id = 0x3001;
    bad.name = "small";
    refused(bad);
    bad = dirt;
    bad.type_id = kNoTileType;
    refused(bad);
    bad = dirt;
    bad.name = "";
    refused(bad);
    bad = dirt;
    bad.version = 0;
    refused(bad);
    bad = dirt;
    bad.type_id = 0x3002;
    bad.name = "missing_array";
    bad.components = nullptr;
    refused(bad);
    const ComponentTypeId duplicate_components[] = {kHealth, kHealth};
    bad = dirt;
    bad.type_id = 0x3003;
    bad.name = "duplicate_components";
    bad.components = duplicate_components;
    bad.component_count = 2;
    refused(bad);
    const ComponentTypeId unknown_component[] = {0xDEAD};
    bad = dirt;
    bad.type_id = 0x3004;
    bad.name = "unknown_component";
    bad.components = unknown_component;
    bad.component_count = 1;
    refused(bad);

    // The component registry passed in is the authority: the same id is unknown to an empty one.
    ComponentRegistry empty_components;
    bad = dirt;
    bad.type_id = 0x3005;
    bad.name = "wrong_registry";
    std::string reason;
    T2D_CHECK(tiles.register_tile_type(empty_components, bad, &reason) == nullptr);
    T2D_CHECK(!reason.empty());

    T2D_CHECK_EQ(tiles.count(), 2u);
}

T2D_TEST(the_runtime_is_c_compatible_and_has_no_rtti) {
    static_assert(std::is_standard_layout_v<ComponentDesc>);
    static_assert(std::is_trivially_copyable_v<ComponentDesc>);
    static_assert(std::is_standard_layout_v<InterfaceBinding>);
    static_assert(std::is_trivially_copyable_v<InterfaceBinding>);
    static_assert(std::is_standard_layout_v<InterfaceTableHeader>);
    static_assert(std::is_standard_layout_v<TileTypeDesc>);
    static_assert(offsetof(ComponentDesc, struct_size) == 0);
    static_assert(!std::is_polymorphic_v<ComponentType>);
    static_assert(!std::is_polymorphic_v<ComponentRegistry>);
    static_assert(!std::is_polymorphic_v<Archetype>);
    static_assert(!std::is_polymorphic_v<ObjectData>);
    static_assert(!std::is_polymorphic_v<TileType>);
    static_assert(!std::is_polymorphic_v<TileTypeRegistry>);
    static_assert(!std::is_copy_constructible_v<ObjectData>);
    static_assert(!std::is_move_constructible_v<ObjectData>);
    static_assert(!std::is_copy_constructible_v<ComponentRegistry>);
    static_assert(!std::is_copy_constructible_v<TileTypeRegistry>);

    T2D_CHECK_EQ(sizeof(ComponentTypeId), sizeof(std::uint64_t));
    T2D_CHECK_EQ(sizeof(InterfaceId), sizeof(std::uint32_t));
    T2D_CHECK_EQ(sizeof(TileTypeId), sizeof(std::uint64_t));
    T2D_CHECK_EQ(sizeof(InterfaceTableHeader), 2u * sizeof(std::uint32_t));
    T2D_CHECK_EQ(kNoComponentType, u64{0});
    T2D_CHECK_EQ(kNoInterface, 0u);
    T2D_CHECK_EQ(kNoTileType, u64{0});
}

T2D_TEST(component_save_and_load_are_optional_and_validated) {
    ComponentRegistry registry;
    const u32 shared = 100;
    const ComponentDesc good = [] {
        ComponentDesc desc = desc_of<Word>(kWord, "word");
        desc.save = &word_save;
        desc.load = &word_load;
        return desc;
    }();
    const ComponentType* type = registry.register_component(good);
    T2D_REQUIRE(type != nullptr);
    T2D_CHECK(type->has_save());
    T2D_CHECK(type->has_load());
    Word word{7};
    T2D_CHECK_EQ(type->save_instance(&shared, &word, nullptr, 0), sizeof(u32));
    u32 bytes = 0;
    T2D_CHECK_EQ(type->save_instance(&shared, &word, &bytes, sizeof(bytes)), sizeof(u32));
    T2D_CHECK_EQ(bytes, 107u);
    Word restored{};
    T2D_CHECK_EQ(type->load_instance(&shared, &restored, &bytes, sizeof(bytes)), sizeof(u32));
    T2D_CHECK_EQ(restored.value, 7u);

    // Only one of the pair is refused; a descriptor from before the fields existed is accepted and
    // treated as having no callbacks.
    ComponentDesc half = desc_of<Word>(kWord + 10, "word.half");
    half.save = &word_save;
    T2D_CHECK(registry.register_component(half) == nullptr);
    ComponentDesc legacy = desc_of<Word>(kWord + 11, "word.legacy");
    legacy.struct_size = static_cast<u32>(kComponentDescPrefixSize);
    legacy.save = &word_save;
    legacy.load = &word_load;
    const ComponentType* old = registry.register_component(legacy);
    T2D_REQUIRE(old != nullptr);
    T2D_CHECK_FALSE(old->has_save());
    T2D_CHECK_FALSE(old->has_load());
}

T2D_TEST(tile_type_shared_records_are_stored_and_refreshed) {
    ComponentRegistry components;
    T2D_REQUIRE(components.register_component(desc_of<Word>(kWord, "word")) != nullptr);
    const ComponentTypeId ids[] = {kWord};
    const u32 first_shared = 1;
    const u32 second_shared = 2;
    const void* first[] = {&first_shared};
    TileTypeDesc desc{};
    desc.struct_size = static_cast<u32>(sizeof(TileTypeDesc));
    desc.type_id = 0x4001;
    desc.name = "shared";
    desc.version = 1;
    desc.components = ids;
    desc.component_count = 1;
    desc.shared = first;
    desc.shared_count = 1;
    TileTypeRegistry tiles;
    T2D_REQUIRE(tiles.register_tile_type(components, desc) != nullptr);
    const TileType* type = tiles.find(desc.type_id);
    T2D_REQUIRE(type != nullptr);
    T2D_REQUIRE(type->shared_values().size() == 1u);
    T2D_CHECK(type->shared_values()[0] == &first_shared);

    const void* second[] = {&second_shared};
    desc.shared = second;
    T2D_REQUIRE(tiles.refresh_tile_type(components, desc));
    T2D_CHECK(tiles.find(desc.type_id)->shared_values()[0] == &second_shared);

    TileTypeDesc bad = desc;
    bad.type_id = 0x4002;
    bad.name = "bad_shared";
    bad.shared_count = 2;   // the type has one component
    T2D_CHECK(tiles.register_tile_type(components, bad) == nullptr);

    // A descriptor from before the shared fields existed is accepted with no shared records.
    TileTypeDesc legacy = desc;
    legacy.type_id = 0x4003;
    legacy.name = "legacy_shared";
    legacy.struct_size = static_cast<u32>(kTileTypeDescPrefixSize);
    const TileType* legacy_type = tiles.register_tile_type(components, legacy);
    T2D_REQUIRE(legacy_type != nullptr);
    T2D_CHECK(legacy_type->shared_values().empty());
}

T2D_TEST_MAIN
