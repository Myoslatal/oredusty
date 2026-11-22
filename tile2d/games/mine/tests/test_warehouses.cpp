// Warehouses: scenery that holds things - the container it stores with, the draginto that takes drops
// in, the click that shows what is inside, and the drag that puts one there.
//
// The content in here is a **test double**: names like "shed" or "chunk" are the fixture's, not the
// game's - the game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/content_name.h>
#include <mine/pointer_drag.h>
#include <mine/runtime/content/content_ids.h>
#include <mine/world.h>

#include <support/test_support.h>

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// A registry and the definitions that go with it: what a test's content files would have produced.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    ContentId add_floor(std::string_view name) {
        const ContentId id = registry.register_content(ContentKind::Floor, name);
        TileDefinition definition;
        definition.kind = ContentKind::Floor;
        definition.name = std::string(name);
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_warehouse(std::string_view name, usize capacity) {
        const ContentId id = registry.register_content(ContentKind::Warehouse, name);
        TileDefinition definition;
        definition.kind = ContentKind::Warehouse;
        definition.name = std::string(name);
        definition.capacity = capacity;
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_drop(std::string_view name) {
        const ContentId id = registry.register_content(ContentKind::Drop, name);
        DropDefinition definition;
        definition.kind = ContentKind::Drop;
        definition.name = std::string(name);
        definitions.add(std::move(definition));
        return id;
    }
};

[[nodiscard]] PlacedContent at(ContentKind kind, std::string name, i32 tile_layer, i32 x, i32 y,
                               i32 width = 1, i32 height = 1) {
    PlacedContent where;
    where.kind = kind;
    where.name = std::move(name);
    where.layer = tile_layer;
    where.anchor = GridPos{x, y};
    where.width = width;
    where.height = height;
    return where;
}

/// A definition read out of a document, the way a load reads one.
[[nodiscard]] std::optional<TileDefinition> read_one(std::string_view text, ContentKind kind,
                                                     std::string* error = nullptr) {
    t2d::EcfgError parse_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text, &parse_error);
    if (!document.has_value()) return std::nullopt;
    for (const t2d::EcfgValue& table : document->root().children()) {
        if (content_kind_from_name(table.key()) != kind) continue;
        for (const t2d::EcfgValue& entry : table.children()) {
            return read_tile_definition(kind, entry.key(), entry, error);
        }
    }
    return std::nullopt;
}

} // namespace

// The standalone Container/Click/DragInto tests were the old C++ components; the runtime content suite
// (test_content_runtime) covers the function tables, and the warehouse tests below cover the same ground
// through a real plot.

T2D_TEST(two_warehouses_of_one_entry_share_their_container_definition_but_not_their_slots) {
    Fixture fixture;
    fixture.add_warehouse("shed", 4);
    MineLayer layer(0, LayerShape{8, 8, 2}, 1);
    LayerBuildReport report;
    Tile* first = layer.place(fixture.registry, fixture.definitions,
                              at(ContentKind::Warehouse, "shed", 1, 1, 1), &report);
    Tile* second = layer.place(fixture.registry, fixture.definitions,
                               at(ContentKind::Warehouse, "shed", 1, 4, 1), &report);
    T2D_REQUIRE(first != nullptr && second != nullptr);
    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? std::string{} : report.errors[0]);

    // One shared ContainerShared record per entry (capacity is content data); the slots are per object.
    T2D_CHECK(first->shared(runtime::content::kContainerComponent) ==
              second->shared(runtime::content::kContainerComponent));
    T2D_CHECK(first->object() != second->object());
    T2D_CHECK_EQ(first->container().capacity(), 4u);
    T2D_CHECK_EQ(second->container().capacity(), 4u);

    T2D_CHECK_EQ(first->container().add(ContentKind::Item, 1, 3), 3u);
    T2D_CHECK_EQ(first->container().total(), 3u);
    T2D_CHECK(second->container().empty());
    T2D_CHECK_EQ(second->container().add(ContentKind::Item, 1, 1), 1u);
    T2D_CHECK_EQ(first->container().total(), 3u);
    T2D_CHECK_EQ(second->container().total(), 1u);
}

T2D_TEST(a_warehouse_is_scenery_with_a_capacity_and_three_halves) {
    Fixture fixture;
    fixture.add_floor("dirt");
    fixture.add_warehouse("shed", 4);
    MineLayer layer(0, LayerShape{16, 12, 2}, 1);
    LayerBuildReport report;
    (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 4, 4),
                      &report);
    Tile* placed = layer.place(fixture.registry, fixture.definitions,
                               at(ContentKind::Warehouse, "shed", 1, 6, 5, 2, 2), &report);
    T2D_REQUIRE(placed != nullptr);
    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? std::string{} : report.errors[0]);
    Tile* shed = placed;

    // It is scenery: its tile type carries no Ticker, so the layer never ticks it.
    T2D_CHECK(kind_is_a_tile(ContentKind::Warehouse));
    T2D_CHECK_FALSE(placed->is_ticking());
    T2D_CHECK_EQ(layer.ticking_count(), 0u);
    T2D_CHECK_EQ(layer.tick(100.0f), 0u);

    // The capacity comes from its entry, and its three halves are its own.
    T2D_CHECK_EQ(shed->container().capacity(), 4u);
    T2D_CHECK(shed->container().empty());
    T2D_CHECK(shed->has_drag());
    T2D_CHECK(shed->has_click());
    T2D_CHECK_FALSE(shed->click().open());

    // ... and it answers for them as a plain plot. That is the question a double click asks before it
    // selects anything: does the plot at this cell hold something, and what (docs/GAME_DESIGN.md 1.31).
    T2D_CHECK(shed->has_container());
    T2D_CHECK(shed->container().total() == 0u);

    // A 2x2 warehouse covers four cells: that is what a drag and a click are measured against.
    T2D_CHECK(shed->holds(GridPos{6, 5}));
    T2D_CHECK(shed->holds(GridPos{7, 6}));
    T2D_CHECK_FALSE(shed->holds(GridPos{5, 5}));
    T2D_CHECK_FALSE(shed->holds(GridPos{8, 5}));
    T2D_CHECK_FALSE(shed->on_click(GridPos{5, 5}));
    T2D_CHECK_FALSE(shed->click().open());
    T2D_CHECK(shed->on_click(GridPos{7, 6}));
    T2D_CHECK(shed->click().open());
    T2D_CHECK_FALSE(shed->on_click(GridPos{8, 8}));
    T2D_CHECK_FALSE(shed->click().open());

    // A warehouse that is **still going up** takes no click at all: it answers as if the click had landed
    // elsewhere, which is also what closes a panel it had open before its build started
    // (docs/GAME_DESIGN.md section 1.37).
    T2D_CHECK(shed->on_click(GridPos{6, 5}));
    T2D_CHECK(shed->click().open());
    shed->set_under_construction(true);
    T2D_CHECK_FALSE(shed->on_click(GridPos{6, 5})); // on one of its own cells, and still not taken
    T2D_CHECK_FALSE(shed->click().open());
    shed->set_under_construction(false);
    T2D_CHECK(shed->on_click(GridPos{6, 5})); // done: it takes clicks again
    T2D_CHECK(shed->click().open());
}

T2D_TEST(a_drop_dragged_onto_a_warehouse_goes_in) {
    Fixture fixture;
    fixture.add_floor("dirt");
    fixture.add_warehouse("shed", 1);
    const ContentId chunk = fixture.add_drop("chunk");
    MineLayer layer(0, LayerShape{16, 12, 2}, 1);
    LayerBuildReport report;
    (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 5, 5),
                      &report);
    Tile* shed = layer.place(fixture.registry, fixture.definitions,
                             at(ContentKind::Warehouse, "shed", 1, 6, 5), &report);
    T2D_REQUIRE(shed != nullptr);

    // A **Drop**, not a plain entity: what the pointer may pick up is the kind of thing it is
    // (types/drop.h), and a plain entity is not draggable.
    auto* drop = &layer.adopt(std::make_unique<Drop>(ContentKind::Drop, chunk, t2d::Vec2{3.5f, 5.5f}));

    // Pick it up and let it go **on the warehouse**: it goes in.
    PointerDrag drag;
    T2D_CHECK(drag.begin(layer, t2d::Vec2{3.5f, 5.5f}));
    T2D_CHECK(drag.held() == drop);
    T2D_CHECK(drag.move_to(t2d::Vec2{6.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Stored);
    T2D_CHECK_EQ(drag.last_release().kind, ContentKind::Drop);
    T2D_CHECK_EQ(drag.last_release().id, chunk);
    T2D_CHECK_EQ(shed->container().total(), 1u);
    T2D_CHECK_EQ(shed->container().count_of(ContentKind::Drop, chunk), 1u);
    // It is not on the ground any more: out of the index at once, out of the storage at the next pass.
    T2D_CHECK(layer.entities_within(t2d::Vec2{6.5f, 5.5f}, 1.0f).empty());
    (void)layer.tick(0.0f);
    T2D_CHECK_EQ(layer.entity_count(), 0u);

    // The warehouse is full now (capacity 1), so the next one is **refused** and stays where it was let go.
    auto* second = &layer.adopt(std::make_unique<Drop>(ContentKind::Drop, chunk, t2d::Vec2{8.5f, 5.5f}));
    T2D_CHECK(drag.begin(layer, t2d::Vec2{8.5f, 5.5f}));
    T2D_CHECK(drag.move_to(t2d::Vec2{6.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Refused);
    T2D_CHECK_EQ(shed->container().total(), 1u);
    T2D_CHECK(second->alive());
    T2D_CHECK_NEAR(second->position().x, 6.5f, 1e-5f);

    // Let go on the open ground: nothing takes it, so it stays there.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{6.5f, 5.5f}));
    T2D_CHECK(drag.move_to(t2d::Vec2{2.5f, 2.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Left);
    T2D_CHECK(second->alive());
    T2D_CHECK_NEAR(second->position().x, 2.5f, 1e-5f);
    T2D_CHECK_EQ(shed->container().total(), 1u);

    // A warehouse that is still going up takes no dragged drop: the pointer looks further down the tile
    // layers, finds nothing, and the drop stays where it was let go - not Stored and not Refused, because
    // nothing was asked to take it (docs/GAME_DESIGN.md section 1.37).
    auto* third = &layer.adopt(std::make_unique<Drop>(ContentKind::Drop, chunk, t2d::Vec2{9.5f, 4.5f}));
    shed->set_under_construction(true);
    T2D_CHECK(drag.begin(layer, t2d::Vec2{9.5f, 4.5f}));
    T2D_CHECK(drag.held() == third);
    T2D_CHECK(drag.move_to(t2d::Vec2{6.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Left);
    T2D_CHECK(third->alive());
    T2D_CHECK_NEAR(third->position().x, 6.5f, 1e-5f);
    T2D_CHECK_EQ(shed->container().total(), 1u); // nothing went in

    // Done, and with room again (the first drop is taken back out for this): the same drop, let go on the
    // same cell, goes in.
    T2D_CHECK_EQ(shed->container().take(ContentKind::Drop, chunk), 1u);
    shed->set_under_construction(false);
    T2D_CHECK(drag.begin(layer, t2d::Vec2{6.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Stored);
    T2D_CHECK_FALSE(third->alive());
    T2D_CHECK_EQ(shed->container().total(), 1u);
    T2D_CHECK_EQ(shed->container().count_of(ContentKind::Drop, chunk), 1u);

    // Nothing held, nothing happens.
    PointerDrag empty;
    T2D_CHECK(empty.end() == ReleaseResult::Nothing);
}

T2D_TEST(the_data_says_how_much_a_warehouse_holds) {
    std::string error;
    const std::optional<TileDefinition> shed =
        read_one("warehouse::\n    shed::\n        image:\"art/shed.png\"\n        capacity:12\n",
                 ContentKind::Warehouse, &error);
    T2D_REQUIRE(shed.has_value());
    T2D_CHECK_EQ(shed->name, std::string("shed"));
    T2D_CHECK_EQ(shed->image, std::string("art/shed.png"));
    T2D_CHECK_EQ(shed->capacity, 12u);
    // An entry that says nothing about its capacity is a warehouse that can hold nothing - not one given
    // a size nobody wrote.
    const std::optional<TileDefinition> quiet =
        read_one("warehouse::\n    shed::\n", ContentKind::Warehouse, &error);
    T2D_REQUIRE(quiet.has_value());
    T2D_CHECK_EQ(quiet->capacity, 0u);

    const auto refused = [](std::string_view text, std::string_view expected) {
        std::string message;
        const std::optional<TileDefinition> definition = read_one(text, ContentKind::Warehouse, &message);
        return !definition.has_value() && message.find(expected) != std::string::npos;
    };
    T2D_CHECK(refused("warehouse::\n    shed::\n        capacity:2.5\n", "must be a whole number"));
    T2D_CHECK(refused("warehouse::\n    shed::\n        capacity:\"big\"\n", "must be a whole number"));
    T2D_CHECK(refused("warehouse::\n    shed::\n        capacity:0\n", "must be greater than zero"));
    T2D_CHECK(refused("warehouse::\n    shed::\n        capacity:-3\n", "must be greater than zero"));
    // A field another kind's reader owns is the designer's, not an error: the floor reader never looks
    // at a drill's dig_power, so a floor that says it is a floor, not a drill with a rate. (capacity is
    // read for every plot kind now - any content whose tile type has a container can declare one, which
    // is what the task block does - so it is no longer an example of an unread field here.)
    const std::optional<TileDefinition> floor =
        read_one("floor::\n    dirt::\n        dig_power:12\n", ContentKind::Floor, &error);
    T2D_REQUIRE(floor.has_value());
    T2D_CHECK_EQ(floor->dig_power, 0.0f);
}

T2D_TEST(a_content_name_comes_from_the_text_table_and_falls_back_to_its_id) {
    ContentRegistry registry;
    const ContentId shed = registry.register_content(ContentKind::Warehouse, "stone");
    const ContentId vein = registry.register_content(ContentKind::Ore, "stone");
    const ContentId chunk = registry.register_content(ContentKind::Drop, "chunk");

    // A table with nothing in it answers with the id it was asked for, which is exactly what
    // t2d::Locale::text does - and how "no label" is told apart from a label that happens to be empty.
    const TextLookup none = [](std::string_view id) { return id; };
    T2D_CHECK_EQ(content_label(registry, ContentKind::Warehouse, shed, none), std::string("stone"));
    T2D_CHECK_EQ(content_label(registry, ContentKind::Drop, chunk, none), std::string("chunk"));

    // The kind-specific key wins over the plain one, and the plain one over the id: one id can be two
    // kinds of thing, and a language may call them different things.
    const TextLookup table = [](std::string_view id) -> std::string_view {
        if (id == "content.name.warehouse.stone") return "STONE WAREHOUSE";
        if (id == "content.name.stone") return "STONE";
        return id;
    };
    T2D_CHECK_EQ(content_label(registry, ContentKind::Warehouse, shed, table),
                 std::string("STONE WAREHOUSE"));
    T2D_CHECK_EQ(content_label(registry, ContentKind::Ore, vein, table), std::string("STONE"));
    T2D_CHECK_EQ(content_label(registry, ContentKind::Drop, chunk, table), std::string("chunk"));

    // A caller that wants a fallback of its own is told whether the table had one (the warehouse panel
    // shows the kind's own word when the designer has not named the content).
    bool named = false;
    (void)content_label(registry, ContentKind::Warehouse, shed, table, &named);
    T2D_CHECK(named);
    named = true;
    (void)content_label(registry, ContentKind::Drop, chunk, table, &named);
    T2D_CHECK_FALSE(named);

    // Content that is gone has nothing but its id to show.
    T2D_CHECK_EQ(content_label(registry, ContentKind::Drop, 99u, table), std::string("#99"));
}

T2D_TEST(scenery_that_holds_nothing_has_no_container) {
    // The other half of the answer a double click needs: a plot that keeps nothing says so, so a double
    // click that lands on a floor selects nothing instead of opening an empty panel.
    Fixture fixture;
    fixture.add_floor("dirt");
    MineLayer layer(0, LayerShape{8, 4, 1}, 1);
    LayerBuildReport report;
    Tile* floor =
        layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 1, 1), &report);
    T2D_REQUIRE(floor != nullptr);
    T2D_CHECK_FALSE(floor->has_container());

    const Tile* as_const = floor;
    T2D_CHECK_FALSE(as_const->has_container());

    // And what the layer hands back for that cell is the same plot, so the question has one answer
    // whichever path it is asked through.
    T2D_REQUIRE(layer.top_plot_at(GridPos{1, 1}) == floor);
    T2D_CHECK_FALSE(layer.top_plot_at(GridPos{1, 1})->has_container());
}

T2D_TEST(a_pile_feeds_a_warehouse_one_item_at_a_time) {
    Fixture fixture;
    fixture.add_floor("dirt");
    fixture.add_warehouse("shed", 8);
    const ContentId chunk = fixture.add_drop("chunk");
    MineLayer layer(0, LayerShape{16, 12, 3}, 1);
    LayerBuildReport report;
    (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 5, 5),
                      &report);
    const PlacedContent shed_at = at(ContentKind::Warehouse, "shed", 1, 8, 5);
    Tile* shed = layer.place(fixture.registry, fixture.definitions, shed_at, &report);
    T2D_REQUIRE(shed != nullptr);

    // Three of one item lying close together: one **pile** with a count (docs/GAME_DESIGN.md section 1.43).
    auto& pile = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, chunk, t2d::Vec2{5.5f, 5.5f}, 3)));

    // Dragging it onto the warehouse takes **one item** in: the box is one fuller, the pile is one smaller,
    // and what the pointer carried went its own way when it was let go.
    PointerDrag drag;
    T2D_CHECK(drag.begin(layer, t2d::Vec2{5.5f, 5.5f}));
    T2D_CHECK(drag.held() != &pile);
    T2D_CHECK_EQ(pile.count(), 2u);
    T2D_CHECK(drag.move_to(t2d::Vec2{8.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Stored);
    T2D_CHECK_EQ(shed->container().total(), 1u);
    T2D_CHECK_EQ(pile.count(), 2u);

    T2D_CHECK(drag.begin(layer, t2d::Vec2{5.5f, 5.5f}));
    T2D_CHECK_EQ(pile.count(), 1u);
    T2D_CHECK(drag.move_to(t2d::Vec2{8.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Stored);
    T2D_CHECK_EQ(shed->container().total(), 2u);

    // One item left: what is dragged now is the drop itself, and the box takes it the same way.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{5.5f, 5.5f}));
    T2D_CHECK(drag.held() == &pile);
    T2D_CHECK(drag.move_to(t2d::Vec2{8.5f, 5.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Stored);
    T2D_CHECK_EQ(shed->container().total(), 3u);
    T2D_CHECK_FALSE(pile.alive());

    // Three items went in and nothing is left on the ground: one at a time is not losing any.
    (void)layer.tick(0.0f);
    T2D_CHECK_EQ(layer.entity_count(), 0u);
}

T2D_TEST_MAIN
