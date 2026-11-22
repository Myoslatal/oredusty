// The plot model: a plot is core data plus one runtime ObjectData, and the components its tile type
// carries decide whether it ticks, holds things, or mines. These tests cover the core data, the refresh
// flag, and what the definer builds for each content kind.
#include <mine/content_loader.h>
#include <mine/runtime/content/content_ids.h>
#include <mine/types/types.h>

#include <support/test_support.h>

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// One definition and the runtime definitions built from it. Layouts and shared records are owned by
/// the runtime library the Tile holds, so the fixture only has to live long enough to build the plot.
struct PlotFixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    /// 0 is "the size the definition declares": a test that wants a particular footprint passes it,
    /// the way a save records the footprint a plot actually had (types/tile_definition.h).
    [[nodiscard]] std::unique_ptr<Tile> make(const TileDefinition& definition, ContentId id, i32 layer,
                                             GridPos anchor, i32 width = 0, i32 height = 0) {
        definitions.add(definition);
        const TileDefinition* stored = definitions.find_tile(definition.kind, definition.name);
        if (stored == nullptr) return nullptr;
        return make_tile(definitions, *stored, registry, id, layer, anchor, width, height);
    }
};

} // namespace

T2D_TEST(a_plot_is_core_data_plus_a_runtime_component_block) {
    PlotFixture fixture;
    TileDefinition wall;
    wall.kind = ContentKind::Structure;
    wall.name = "wall";
    wall.image = "art/wall.png";
    const std::unique_ptr<Tile> plot = fixture.make(wall, 7, 2, GridPos{5, 7}, 3, 2);
    T2D_REQUIRE(plot != nullptr);

    T2D_CHECK_EQ(plot->kind(), ContentKind::Structure);
    T2D_CHECK_EQ(plot->id(), 7u);
    T2D_CHECK_EQ(plot->layer(), 2);
    T2D_CHECK_EQ(plot->anchor(), GridPos({5, 7}));
    T2D_CHECK_EQ(plot->width(), 3);
    T2D_CHECK_EQ(plot->height(), 2);
    T2D_CHECK_EQ(plot->cell_count(), 6);
    T2D_CHECK_EQ(plot->cells(), TileRect({5, 7, 3, 2}));
    T2D_CHECK(plot->valid());
    T2D_CHECK(plot->dirty()); // a new plot has not been looked at yet

    // It owns one runtime object; the layout is the tile type's and the data is the plot's own.
    T2D_REQUIRE(plot->object() != nullptr);
    T2D_CHECK(plot->tile_type() != nullptr);
    T2D_CHECK(plot->has_component(runtime::content::kPreviewComponent));
    T2D_CHECK_FALSE(plot->is_ticking());
    T2D_CHECK_FALSE(plot->has_container());
    T2D_CHECK_FALSE(plot->is_drill());
    T2D_CHECK(plot->definition() != nullptr);
    T2D_CHECK_EQ(plot->definition()->image, std::string("art/wall.png"));

    // Missing content is a flag, not a different type: the id and the place stay.
    plot->set_missing(true);
    T2D_CHECK_FALSE(plot->valid());
    T2D_CHECK(plot->missing());
    plot->set_missing(false);
    T2D_CHECK(plot->valid());

    // A 3x2 covers exactly those cells and refuses the neighbours.
    T2D_CHECK(plot->covers(GridPos{5, 7}));
    T2D_CHECK(plot->covers(GridPos{7, 8}));
    T2D_CHECK_FALSE(plot->covers(GridPos{8, 8}));
    T2D_CHECK_FALSE(plot->covers(GridPos{4, 7}));
    T2D_CHECK(plot->within(8, 9));
    T2D_CHECK_FALSE(plot->within(7, 9));
}

T2D_TEST(overlap_and_mirroring_are_core_data) {
    PlotFixture fixture;
    TileDefinition wall;
    wall.kind = ContentKind::Structure;
    wall.name = "wall";
    wall.random_reverse = true;
    const std::unique_ptr<Tile> left = fixture.make(wall, 1, 0, GridPos{0, 0}, 2, 2);
    const std::unique_ptr<Tile> touching = fixture.make(wall, 1, 0, GridPos{2, 0}, 2, 2);
    const std::unique_ptr<Tile> corner = fixture.make(wall, 1, 0, GridPos{1, 1}, 2, 2);
    const std::unique_ptr<Tile> above = fixture.make(wall, 1, 1, GridPos{0, 0}, 2, 2);
    T2D_REQUIRE(left != nullptr && touching != nullptr && corner != nullptr && above != nullptr);
    T2D_CHECK_FALSE(left->overlaps(*touching));
    T2D_CHECK(left->overlaps(*corner));
    T2D_CHECK(corner->overlaps(*left));
    T2D_CHECK_FALSE(left->overlaps(*above));
    T2D_CHECK(left->overlaps(*left));

    // Only content that allows it is ever turned around, and the generator is what decides.
    const std::unique_ptr<Tile> turnable = fixture.make(wall, 2, 0, GridPos{4, 0});
    T2D_REQUIRE(turnable != nullptr);
    T2D_CHECK(turnable->random_reverse());
    T2D_CHECK_FALSE(turnable->mirrored());
    turnable->set_mirrored(true);
    T2D_CHECK(turnable->mirrored());
}

T2D_TEST(an_old_plot_survives_the_definitions_being_rebuilt) {
    PlotFixture fixture;
    TileDefinition wall;
    wall.kind = ContentKind::Structure;
    wall.name = "wall";
    wall.image = "art/wall.png";
    const std::unique_ptr<Tile> first = fixture.make(wall, 1, 0, GridPos{0, 0});
    T2D_REQUIRE(first != nullptr);
    const runtime::ObjectData* old_object = first->object();
    const TileDefinition* old_definition = first->definition();
    T2D_REQUIRE(old_object != nullptr && old_definition != nullptr);

    // A reload adds content and rebuilds the runtime library. The old plot keeps its own library and its
    // object: nothing that already exists is looking at the new definitions.
    TileDefinition floor;
    floor.kind = ContentKind::Floor;
    floor.name = "dirt";
    const std::unique_ptr<Tile> second = fixture.make(floor, 2, 0, GridPos{1, 0});
    T2D_REQUIRE(second != nullptr);
    T2D_CHECK(first->object() == old_object);
    T2D_CHECK(first->definition() == old_definition);
    T2D_CHECK_EQ(first->definition()->image, std::string("art/wall.png"));
    T2D_CHECK_EQ(first->kind(), ContentKind::Structure);
    T2D_CHECK_EQ(first->get_preview().images.size(), 1u);   // still answers from its own library
}

T2D_TEST(the_refresh_pass_is_a_dirty_flag_and_a_construction_state) {
    PlotFixture fixture;
    TileDefinition wall;
    wall.kind = ContentKind::Floor;
    wall.name = "dirt";
    const std::unique_ptr<Tile> plot = fixture.make(wall, 1, 0, GridPos{0, 0});
    T2D_REQUIRE(plot != nullptr);
    T2D_CHECK(plot->dirty());
    T2D_CHECK(plot->refresh());
    T2D_CHECK_FALSE(plot->dirty());
    T2D_CHECK_FALSE(plot->refresh());
    plot->mark_dirty();
    T2D_CHECK(plot->refresh());
    plot->mark_dirty();
    plot->set_under_construction(true);
    T2D_CHECK_FALSE(plot->refresh());
    T2D_CHECK(plot->dirty()); // owed after the sweep is done
    plot->set_under_construction(false);
    T2D_CHECK(plot->refresh());
}

T2D_TEST(a_plot_is_a_plot_without_a_vtable) {
    static_assert(!std::is_polymorphic_v<Tile>);
    static_assert(!std::has_virtual_destructor_v<Tile>);
    static_assert(!std::is_copy_constructible_v<Tile>);
    static_assert(std::is_move_constructible_v<Tile>);
    static_assert(!std::is_abstract_v<Tile>);
    T2D_CHECK(true);
}

T2D_TEST(the_data_says_what_a_plot_is_and_the_definer_builds_it) {
    // Which tables describe a plot at all.
    T2D_CHECK(kind_is_a_tile(ContentKind::Floor));
    T2D_CHECK(kind_is_a_tile(ContentKind::Ore));
    T2D_CHECK(kind_is_a_tile(ContentKind::Structure));
    T2D_CHECK(kind_is_a_tile(ContentKind::Machine));
    T2D_CHECK_FALSE(kind_is_a_tile(ContentKind::Item));
    T2D_CHECK_FALSE(kind_is_a_tile(ContentKind::Recipe));
    T2D_CHECK_FALSE(kind_is_a_tile(ContentKind::Layer));
    T2D_CHECK_FALSE(kind_is_a_tile(ContentKind::Channel));

    // The two fields the engine reads, out of an entry that carries whatever else the designer likes.
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(R"(
floor::
    dirt::
        image:"art/floor_dirt.png"
        random_reverse:true
        note:"anything else here is the designer's"
machine::
    drill::
        random_reverse:false
ore::
    copper::
        image:"art/ore_copper.png"
item::
    ore::
        image:"art/ore.png"
)");
    T2D_REQUIRE(document.has_value());
    const t2d::EcfgValue* floor_table = document->find("floor");
    T2D_REQUIRE(floor_table != nullptr);
    const t2d::EcfgValue* dirt = floor_table->find("dirt");
    T2D_REQUIRE(dirt != nullptr);

    std::string error;
    const std::optional<TileDefinition> definition =
        read_tile_definition(ContentKind::Floor, "dirt", *dirt, &error);
    T2D_REQUIRE(definition.has_value());
    T2D_CHECK_EQ(definition->name, std::string("dirt"));
    T2D_CHECK_EQ(definition->image, std::string("art/floor_dirt.png"));
    T2D_CHECK(definition->random_reverse);
    T2D_CHECK(definition->is_tile());

    // A field the engine reads and cannot read is refused, never guessed at: "random_reverse:1" would
    // otherwise be a piece of content that quietly never turns around.
    const std::optional<t2d::EcfgDocument> wrong = t2d::EcfgDocument::parse(R"(
floor::
    sand::
        random_reverse:1
)");
    T2D_REQUIRE(wrong.has_value());
    const t2d::EcfgValue* sand = wrong->find("floor")->find("sand");
    T2D_REQUIRE(sand != nullptr);
    error.clear();
    T2D_CHECK_FALSE(read_tile_definition(ContentKind::Floor, "sand", *sand, &error).has_value());
    T2D_CHECK(error.find("random_reverse") != std::string::npos);

    // Every plot a document declares, and nothing else: the item table is not a plot, and an item
    // *named* "ore" is still an item - a name is not a kind.
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    T2D_REQUIRE(definitions.size() == 3u);
    T2D_CHECK(errors.empty());
    T2D_CHECK_EQ(definitions[0].kind, ContentKind::Floor);
    T2D_CHECK_EQ(definitions[0].name, std::string("dirt"));
    T2D_CHECK_EQ(definitions[1].kind, ContentKind::Machine);
    T2D_CHECK_EQ(definitions[1].name, std::string("drill"));
    T2D_CHECK_FALSE(definitions[1].random_reverse);
    T2D_CHECK_EQ(definitions[2].kind, ContentKind::Ore);
    T2D_CHECK_EQ(definitions[2].name, std::string("copper"));

    // The definer turns a definition into the plot it describes: the kind picks the tile type, and the
    // components that tile type carries decide what the plot can do. There is no class per kind.
    ContentDefinitions runtime_definitions;
    for (const TileDefinition& entry : definitions)
        runtime_definitions.add(entry);
    ContentRegistry runtime_registry;
    const std::unique_ptr<Tile> floor_plot =
        make_tile(runtime_definitions, definitions[0], runtime_registry, 7, 1, GridPos{4, 5}, 2, 3);
    T2D_REQUIRE(floor_plot != nullptr);
    T2D_CHECK(floor_plot->is_floor());
    T2D_CHECK_FALSE(floor_plot->is_ticking());
    T2D_CHECK_EQ(floor_plot->id(), 7u);
    T2D_CHECK_EQ(floor_plot->layer(), 1);
    T2D_CHECK_EQ(floor_plot->cell_count(), 6);
    T2D_CHECK(floor_plot->random_reverse());
    T2D_CHECK_FALSE(floor_plot->mirrored());

    const std::unique_ptr<Tile> machine_plot =
        make_tile(runtime_definitions, definitions[1], runtime_registry, 8, 0, GridPos{0, 0});
    T2D_REQUIRE(machine_plot != nullptr);
    T2D_CHECK(machine_plot->is_ticking());
    T2D_CHECK_FALSE(machine_plot->is_floor());

    // The kind picks the tile type, so an "ore::" entry is an ore and not scenery with a flag.
    const std::unique_ptr<Tile> ore_plot =
        make_tile(runtime_definitions, definitions[2], runtime_registry, 11, 1, GridPos{6, 7});
    T2D_REQUIRE(ore_plot != nullptr);
    T2D_CHECK(ore_plot->is_ore());
    T2D_CHECK_FALSE(ore_plot->is_floor());
    T2D_CHECK_FALSE(ore_plot->is_ticking());

    TileDefinition structure;
    structure.kind = ContentKind::Structure;
    structure.name = "wall";
    const std::unique_ptr<Tile> wall_plot =
        make_tile(runtime_definitions, structure, runtime_registry, 9, 0, GridPos{0, 0});
    T2D_REQUIRE(wall_plot != nullptr);
    T2D_CHECK_FALSE(wall_plot->is_floor());
    T2D_CHECK_FALSE(wall_plot->is_ticking());

    // Something that is not a plot at all does not become one.
    TileDefinition item;
    item.kind = ContentKind::Item;
    item.name = "ore";
    T2D_CHECK(make_tile(runtime_definitions, item, runtime_registry, 10, 0, GridPos{0, 0}) == nullptr);
}

T2D_TEST(the_data_says_how_many_cells_a_plot_covers) {
    const auto read = [](std::string_view text, std::string* error = nullptr)
        -> std::optional<TileDefinition> {
        const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text);
        if (!document.has_value()) return std::nullopt;
        std::vector<std::string> errors;
        const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
        if (error != nullptr) *error = errors.empty() ? std::string{} : errors.front();
        if (!errors.empty() || definitions.size() != 1u) return std::nullopt;
        return definitions.front();
    };
    const auto refused = [&read](std::string_view text, std::string_view expected) {
        std::string error;
        const std::optional<TileDefinition> definition = read(text, &error);
        return !definition.has_value() && error.find(expected) != std::string::npos;
    };

    // "size::" is how a content entry declares its footprint (docs/MODS.md, types/tile_definition.h).
    const std::optional<TileDefinition> bench = read(R"(structure::
    bench::
        size::
            width:2
            height:1
)");
    T2D_REQUIRE(bench.has_value());
    T2D_CHECK_EQ(bench->width, 2);
    T2D_CHECK_EQ(bench->height, 1);
    // An entry that does not say is a 1x1 plot: the default is a definition attribute like any other.
    const std::optional<TileDefinition> quiet = read("structure::\n    plate::\n");
    T2D_REQUIRE(quiet.has_value());
    T2D_CHECK_EQ(quiet->width, 1);
    T2D_CHECK_EQ(quiet->height, 1);

    // The definer builds the plot the definition describes: no size passed means the entry's, and an
    // explicit one wins - a save records the footprint a plot actually had (world.h, PlacedContent).
    PlotFixture fixture;
    const std::unique_ptr<Tile> made = fixture.make(*bench, 1, 0, GridPos{0, 0});
    T2D_REQUIRE(made != nullptr);
    T2D_CHECK_EQ(made->width(), 2);
    T2D_CHECK_EQ(made->height(), 1);
    T2D_CHECK_EQ(made->cell_count(), 2);
    T2D_CHECK(made->covers(GridPos{1, 0}));
    T2D_CHECK_FALSE(made->covers(GridPos{2, 0}));
    const std::unique_ptr<Tile> wide = fixture.make(*bench, 2, 0, GridPos{2, 0}, 3, 2);
    T2D_REQUIRE(wide != nullptr);
    T2D_CHECK_EQ(wide->width(), 3);
    T2D_CHECK_EQ(wide->height(), 2);
    T2D_CHECK_EQ(wide->cell_count(), 6);

    // A size the engine reads and cannot use is refused where it is written, with the field named: a
    // footprint no map could hold must not be quietly clamped into one the designer did not ask for.
    T2D_CHECK(refused("structure::\n    bench::\n        size:2\n", "size must be a table"));
    T2D_CHECK(refused("structure::\n    bench::\n        size::\n            width:1.5\n",
                      "size.width must be a whole number"));
    T2D_CHECK(refused("structure::\n    bench::\n        size::\n            width:0\n",
                      "size.width is 0, and a plot is 1..4096 cells per side"));
    T2D_CHECK(refused("structure::\n    bench::\n        size::\n            width:-1\n",
                      "size.width is -1, and a plot is 1..4096 cells per side"));
    T2D_CHECK(refused("structure::\n    bench::\n        size::\n            width:4097\n",
                      "size.width is 4097, and a plot is 1..4096 cells per side"));
    T2D_CHECK(refused("structure::\n    bench::\n        size::\n            depth:2\n",
                      "size has no field 'depth' (width, height)"));
    std::string error;
    T2D_CHECK_FALSE(read("structure::\n    bench::\n        size::\n            height:0\n", &error)
                        .has_value());
    T2D_CHECK(error.find("structure 'bench': size.height is 0") != std::string::npos);
}

T2D_TEST(the_data_says_what_an_item_is_and_the_definer_builds_it) {
    // An item is not a plot, and a name is not a kind: this document declares a floor named "stone" and
    // an item named "stone", and they are two pieces of content that happen to share a word.
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(R"(
floor::
    stone::
        image:"art/floor_stone.png"
item::
    stone::
        image:"art/item_stone.png"
        stack:64
    dust::
        note:"an item with no picture at all"
)");
    T2D_REQUIRE(document.has_value());

    // The item table is read by the item definer and by nothing else: a plot definer walking the same
    // document finds the floor, the item definer finds the two items, and neither finds the other's.
    std::vector<std::string> errors;
    const std::vector<ItemDefinition> items = item_definitions(*document, &errors);
    T2D_REQUIRE(items.size() == 2u);
    T2D_CHECK(errors.empty());
    T2D_CHECK_EQ(items[0].kind, ContentKind::Item);
    T2D_CHECK_EQ(items[0].name, std::string("stone"));
    T2D_CHECK_EQ(items[0].image, std::string("art/item_stone.png"));
    T2D_CHECK_EQ(items[1].kind, ContentKind::Item);
    T2D_CHECK_EQ(items[1].name, std::string("dust"));
    T2D_CHECK(items[1].image.empty());
    T2D_CHECK_EQ(tile_definitions(*document).size(), 1u);

    // The field the engine reads: "image". A field it reads and cannot read is refused rather than
    // guessed at - the same rule the plot definer follows, because it is the same field.
    const std::optional<t2d::EcfgDocument> wrong = t2d::EcfgDocument::parse(R"(
item::
    plate::
        image:7
)");
    T2D_REQUIRE(wrong.has_value());
    const t2d::EcfgValue* plate = wrong->find("item")->find("plate");
    T2D_REQUIRE(plate != nullptr);
    std::string error;
    T2D_CHECK_FALSE(read_item_definition(ContentKind::Item, "plate", *plate, &error).has_value());
    T2D_CHECK(error.find("image") != std::string::npos);

    // And the table an entry is written in is what it is: reading an item's entry as a mineral is
    // refused, because a definition that said it came from somewhere else would be one that lies.
    const t2d::EcfgValue* stone_entry = document->find("item")->find("stone");
    T2D_REQUIRE(stone_entry != nullptr);
    error.clear();
    T2D_CHECK_FALSE(read_item_definition(ContentKind::Ore, "stone", *stone_entry, &error).has_value());
    T2D_CHECK(error.find("is not an item") != std::string::npos);

    // The definer turns a definition into the item it describes: the id the registry handed out, and
    // the picture the data named - which is everything an item carries.
    ContentRegistry registry;
    const ContentId stone_id = registry.register_content(ContentKind::Item, "stone");
    const Item stone = make_item(items[0], stone_id);
    T2D_CHECK_EQ(stone.kind(), ContentKind::Item);
    T2D_CHECK_EQ(stone.id(), stone_id);
    T2D_CHECK_EQ(stone.image(), std::string("art/item_stone.png"));
    T2D_CHECK_EQ(stone.name(registry), std::string("stone"));
    T2D_CHECK(stone.valid(registry));

    // Content that is gone: the item keeps the id it was given and says so - the same "#<id>" a plot
    // shows (types/tile.cpp) - instead of quietly becoming whatever now holds that number.
    const Item gone(kMaxContentId, "art/item_stone.png");
    T2D_CHECK_EQ(gone.name(registry), std::format("#{}", kMaxContentId));
    T2D_CHECK_FALSE(gone.valid(registry));

    // The empty slot: an item with no content behind it and no picture.
    const Item empty;
    T2D_CHECK_EQ(empty.id(), kNoContent);
    T2D_CHECK(empty.image().empty());
    T2D_CHECK_EQ(empty.name(registry), std::string("#0"));
    T2D_CHECK_FALSE(empty.valid(registry));

    // Nothing here builds a plot: an item is not on the map.
    T2D_CHECK_FALSE(kind_is_a_tile(ContentKind::Item));
}

T2D_TEST(a_building_is_a_definition_attribute_every_plot_of_the_entry_shares) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(R"(
machine::
    drill::
        dig_power:2
        building::
            materials:["stone",10,"chunk",2]
            time:5
            category:"extraction"
)");
    T2D_REQUIRE(document.has_value());
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    T2D_CHECK_MSG(errors.empty(), "{}", errors.empty() ? "" : errors.front());
    T2D_REQUIRE(definitions.size() == 1u);
    const TileDefinition& definition = definitions.front();
    T2D_REQUIRE(definition.building.materials.size() == 2u);
    T2D_CHECK_EQ(definition.building.materials[0].name, std::string("stone"));
    T2D_CHECK_EQ(definition.building.materials[0].count, 10);
    T2D_CHECK_EQ(definition.building.materials[1].count, 2);
    T2D_CHECK_EQ(definition.building.seconds, 5.0f);
    T2D_CHECK(definition.building.category == BuildingCategory::Extraction);

    // Two plots of one entry read the **same** runtime record: that is the flyweight of
    // docs/GAME_DESIGN.md 1.35.
    ContentDefinitions runtime_definitions;
    runtime_definitions.add(definition);
    ContentRegistry runtime_registry;
    const std::unique_ptr<Tile> first =
        make_tile(runtime_definitions, definition, runtime_registry, 1, 0, GridPos{0, 0});
    const std::unique_ptr<Tile> second =
        make_tile(runtime_definitions, definition, runtime_registry, 2, 0, GridPos{1, 0});
    T2D_REQUIRE(first != nullptr && second != nullptr);
    T2D_CHECK(first->definition() != nullptr);
    T2D_CHECK(first->definition() == second->definition());
    T2D_REQUIRE(first->building() != nullptr);
    T2D_CHECK(first->building() == second->building()); // one building record, not one per plot
    T2D_CHECK(first->building()->category == BuildingCategory::Extraction);
    T2D_CHECK(first->building()->seconds == 5.0f);
    // `default_unlock` is a **definition** attribute that says what a **new save** starts with
    // (docs/GAME_DESIGN.md section 1.44). The data above did not say, so it starts unlocked.
    T2D_CHECK(definition.building.default_unlock);
    const auto read_building = [](std::string_view text,
                                  std::string* error = nullptr) -> std::optional<TileDefinition> {
        const std::optional<t2d::EcfgDocument> parsed = t2d::EcfgDocument::parse(text);
        if (!parsed.has_value()) return std::nullopt;
        std::vector<std::string> problems;
        const std::vector<TileDefinition> read = tile_definitions(*parsed, &problems);
        if (error != nullptr) *error = problems.empty() ? std::string{} : problems.front();
        if (!problems.empty() || read.size() != 1u) return std::nullopt;
        return read.front();
    };
    const std::optional<TileDefinition> locked = read_building(R"(
machine::
    press::
        building::
            category:"processing"
            default_unlock:false
)");
    T2D_REQUIRE(locked.has_value());
    T2D_CHECK_FALSE(locked->building.default_unlock);
    // Two plots of that entry share the one record, and the field comes with it.
    runtime_definitions.add(*locked);
    const std::unique_ptr<Tile> locked_first =
        make_tile(runtime_definitions, *locked, runtime_registry, 3, 0, GridPos{0, 0});
    const std::unique_ptr<Tile> locked_second =
        make_tile(runtime_definitions, *locked, runtime_registry, 4, 0, GridPos{1, 0});
    T2D_REQUIRE(locked_first != nullptr && locked_second != nullptr);
    T2D_REQUIRE(locked_first->building() != nullptr);
    T2D_CHECK(locked_first->building() == locked_second->building());
    T2D_CHECK_FALSE(locked_first->building()->default_unlock);

    // A value that is not true/false is refused where the file is, with the field named.
    std::string error;
    const std::optional<TileDefinition> wrong = read_building("machine::\n    press::\n        building::\n"
                                                              "            default_unlock:1\n",
                                                              &error);
    T2D_CHECK_FALSE(wrong.has_value());
    T2D_CHECK(error.find("default_unlock") != std::string::npos);
    // The list of fields the building table accepts names it, so a wrong one is answered with the choices.
    error.clear();
    const std::optional<TileDefinition> unknown = read_building("machine::\n    press::\n        building::\n"
                                                                "            unlock:false\n",
                                                                &error);
    T2D_CHECK_FALSE(unknown.has_value());
    T2D_CHECK(error.find("default_unlock") != std::string::npos);
    // Only the building table reads it: the same name at the entry's top level is the designer's, ignored.
    const std::optional<TileDefinition> top_level = read_building(R"(
machine::
    press::
        default_unlock:false
        building::
            category:"processing"
)");
    T2D_REQUIRE(top_level.has_value());
    T2D_CHECK(top_level->building.default_unlock);
}

T2D_TEST(content_that_is_not_a_building_has_no_building_sub_object) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(R"(floor::
    dirt::
        image:"art/floor_dirt.png"
)");
    T2D_REQUIRE(document.has_value());
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    T2D_REQUIRE(definitions.size() == 1u);
    T2D_CHECK(definitions.front().building.empty());
    ContentDefinitions runtime_definitions;
    runtime_definitions.add(definitions.front());
    ContentRegistry runtime_registry;
    const std::unique_ptr<Tile> plot =
        make_tile(runtime_definitions, definitions.front(), runtime_registry, 1, 0, GridPos{0, 0});
    T2D_REQUIRE(plot != nullptr);
    T2D_CHECK(plot->building() == nullptr);
}

T2D_TEST(a_drills_two_pictures_and_its_speed_are_read_from_its_entry) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(R"(
drill::
    rig::
        image:"art/rig_base.png"
        top_image:"art/rig_top.png"
        spin:4
        dig_power:2
)");
    T2D_REQUIRE(document.has_value());
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    T2D_CHECK_MSG(errors.empty(), "{}", errors.empty() ? "" : errors.front());
    T2D_REQUIRE(definitions.size() == 1u);
    T2D_CHECK_EQ(definitions.front().image, std::string("art/rig_base.png"));
    T2D_CHECK_EQ(definitions.front().top_image, std::string("art/rig_top.png"));
    T2D_CHECK_EQ(definitions.front().spin, 4.0f);
}

T2D_TEST(a_second_picture_and_a_speed_belong_to_drills_and_are_refused_elsewhere) {
    // A floor that says top_image or spin is a typo, not a floor with a turning part (section 1.38).
    const auto refused = [](std::string_view text) {
        const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text);
        if (!document.has_value()) return false;
        std::vector<std::string> errors;
        (void)tile_definitions(*document, &errors);
        return !errors.empty();
    };
    T2D_CHECK_FALSE(refused("floor::\n    dirt::\n        top_image:\"art/x.png\"\n"));
    T2D_CHECK_FALSE(refused("floor::\n    dirt::\n        spin:4\n"));
    // A drill that says them wrongly is refused: a top that is not a path, a speed that is not a number,
    // and a turn that takes no time at all.
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:1\n        top_image:4\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:1\n        spin:\"fast\"\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:1\n        spin:0\n"));
}

T2D_TEST(a_drills_top_turns_about_the_point_of_its_picture_the_entry_names) {
    const auto read = [](std::string_view text) {
        const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text);
        if (!document.has_value()) return std::vector<TileDefinition>{};
        std::vector<std::string> errors;
        std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
        return errors.empty() ? definitions : std::vector<TileDefinition>{};
    };
    const auto refused = [&](std::string_view text) { return read(text).empty(); };

    const std::vector<TileDefinition> given = read(R"(
drill::
    rig::
        dig_power:2
        top_image:"art/rig_top.png"
        spin:4
        spin_origin::
            x:0.25
            y:0.75
)");
    T2D_REQUIRE(given.size() == 1u);
    T2D_CHECK_EQ(given.front().spin_origin.x, 0.25f);
    T2D_CHECK_EQ(given.front().spin_origin.y, 0.75f);

    // Not saying it means the middle: a part drawn to turn where it is drawn.
    const std::vector<TileDefinition> quiet = read(R"(drill::
    rig::
        dig_power:2
        spin:4
)");
    T2D_REQUIRE(quiet.size() == 1u);
    T2D_CHECK_EQ(quiet.front().spin_origin.x, 0.5f);
    T2D_CHECK_EQ(quiet.front().spin_origin.y, 0.5f);

    // And what is not a point of a picture is refused: outside it, the wrong shape, the wrong type.
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:1.5\n   "
                      "         y:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:-0.1\n  "
                      "          y:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            "
                      "x:\"half\"\n            y:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:0.5\n   "
                      "         y:0.5\n            z:0.5\n"));
    // One half is enough: what is not said is the middle of that axis.
    const std::vector<TileDefinition> half = read(R"(drill::
    rig::
        dig_power:2
        spin_origin::
            x:0.0
)");
    T2D_REQUIRE(half.size() == 1u);
    T2D_CHECK_EQ(half.front().spin_origin.x, 0.0f);
    T2D_CHECK_EQ(half.front().spin_origin.y, 0.5f);
}

T2D_TEST_MAIN
