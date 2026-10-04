// The world: a layer built out of the designer's content, the plots it is made of, the two passes over
// them, and the questions a game asks of the map.
//
// The content in here is a **test double**: names like "wall" or "dirt" are the fixture's, not the
// game's - the game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/world.h>

#include <support/test_support.h>

#include <format>
#include <string>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// A registry and the definitions that go with it: what a test's content files would have produced.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    ContentId add(ContentKind kind, std::string_view name, bool random_reverse = false) {
        const ContentId id = registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        definition.random_reverse = random_reverse;
        definitions.add(std::move(definition));
        return id;
    }

    [[nodiscard]] ContentId id_of(ContentKind kind, std::string_view name) const {
        return registry.find(kind, name);
    }
};

[[nodiscard]] PlacedContent at(ContentKind kind, std::string name, i32 tile_layer, i32 x, i32 y, i32 width = 1,
                               i32 height = 1) {
    PlacedContent where;
    where.kind = kind;
    where.name = std::move(name);
    where.layer = tile_layer;
    where.anchor = GridPos{x, y};
    where.width = width;
    where.height = height;
    return where;
}

/// The plots a query over \p cells finds, in the order the layer visits them.
[[nodiscard]] std::vector<const SceneTile*> plots_in(const MineLayer& layer, const t2d::TileRect& cells) {
    std::vector<const SceneTile*> found;
    layer.for_each_plot_in(cells, [&](const SceneTile& plot) { found.push_back(&plot); });
    return found;
}

} // namespace

T2D_TEST(a_layer_is_built_out_of_the_description_it_was_given) {
    Fixture content;
    const ContentId dirt = content.add(ContentKind::Floor, "dirt");
    const ContentId wall = content.add(ContentKind::Structure, "wall");

    MineLayer layer(0, LayerShape{8, 8, 2}, 42);
    LayerBuildReport report;
    T2D_REQUIRE(layer.place(content.registry, content.definitions, at(ContentKind::Floor, "dirt", 0, 0, 0), &report) !=
                nullptr);
    SceneTile* big = layer.place(content.registry, content.definitions,
                            at(ContentKind::Structure, "wall", 1, 1, 1, 3, 3), &report);
    T2D_REQUIRE(big != nullptr);

    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.placed, 2u);
    T2D_CHECK_EQ(report.cells, 10u);
    T2D_CHECK_EQ(report.ticking, 0u);
    T2D_CHECK_EQ(layer.plot_count(), 2u);
    T2D_CHECK_EQ(layer.ticking_count(), 0u);
    T2D_CHECK_EQ(layer.filled_cells(), 10u);
    T2D_CHECK_EQ(layer.filled_cells(0), 1u);
    T2D_CHECK_EQ(layer.filled_cells(1), 9u);
    T2D_CHECK_EQ(layer.cell_bytes(), 8u * 8u * 2u * 4u);

    // The grid says what is on a cell, and the plot index says which object that is: a 3x3 answers for
    // all nine of its cells, and the caller never has to know where its anchor is.
    const ContentRef cell = layer.ref_at(1, GridPos{2, 2});
    T2D_CHECK_EQ(cell.kind, ContentKind::Structure);
    T2D_CHECK_EQ(cell.id, wall);
    T2D_CHECK_FALSE(cell.stale);
    T2D_CHECK(layer.plot_at(1, GridPos{2, 2}) == big);
    T2D_CHECK(layer.plot_at(1, GridPos{1, 1}) == big);
    T2D_CHECK(layer.plot_at(1, GridPos{3, 3}) == big);
    T2D_CHECK(layer.plot_at(1, GridPos{4, 4}) == nullptr);
    T2D_CHECK(layer.plot_at(0, GridPos{1, 1}) == nullptr);
    T2D_CHECK_EQ(layer.ref_at(0, GridPos{0, 0}).id, dirt);
    T2D_CHECK_EQ(layer.ref_at(0, GridPos{0, 0}).kind, ContentKind::Floor);
    T2D_CHECK(layer.ref_at(0, GridPos{7, 7}).empty());

    // The topmost plot at a cell is what a pointer is over: the floor is under the structure, and the
    // structure is what is on top.
    T2D_CHECK(layer.top_plot_at(GridPos{0, 0}) == layer.plot_at(0, GridPos{0, 0}));
    T2D_CHECK(layer.top_plot_at(GridPos{2, 2}) == big);
    T2D_CHECK(layer.top_plot_at(GridPos{7, 7}) == nullptr);

    // A footprint that is 2x2, 3x3 or 2x3 is the same rectangle, not a special case.
    T2D_REQUIRE(layer.place(content.registry, content.definitions,
                            at(ContentKind::Structure, "wall", 1, 5, 5, 2, 3)) != nullptr);
    T2D_CHECK_EQ(layer.plot_count(), 3u);
    T2D_CHECK_EQ(layer.filled_cells(1), 15u);
    T2D_CHECK(layer.plot_at(1, GridPos{6, 7}) != nullptr);
    T2D_CHECK(layer.plot_at(1, GridPos{7, 7}) == nullptr);
}

T2D_TEST(a_placement_that_cannot_be_made_is_refused_and_nothing_is_half_placed) {
    Fixture content;
    content.add(ContentKind::Floor, "dirt");
    content.add(ContentKind::Structure, "wall");
    MineLayer layer(3, LayerShape{8, 8, 1}, 7);
    LayerBuildReport report;

    // Content the registry does not have.
    T2D_CHECK(layer.place(content.registry, content.definitions, at(ContentKind::Structure, "gate", 0, 0, 0),
                          &report) == nullptr);
    // A kind that does not occupy cells at all.
    T2D_CHECK(layer.place(content.registry, content.definitions, at(ContentKind::Item, "dirt", 0, 0, 0), &report) ==
              nullptr);
    // A tile layer this map does not have.
    T2D_CHECK(layer.place(content.registry, content.definitions, at(ContentKind::Floor, "dirt", 1, 0, 0), &report) ==
              nullptr);
    // A footprint that hangs over the edge.
    T2D_CHECK(layer.place(content.registry, content.definitions,
                          at(ContentKind::Structure, "wall", 0, 6, 6, 3, 3), &report) == nullptr);
    T2D_REQUIRE(report.errors.size() == 4u);
    T2D_CHECK(report.errors[0].find("gate") != std::string::npos);
    T2D_CHECK(report.errors[1].find("occupies cells") != std::string::npos);
    T2D_CHECK(report.errors[2].find("tile layer 1") != std::string::npos);
    T2D_CHECK(report.errors[3].find("does not fit") != std::string::npos);
    T2D_CHECK_EQ(report.placed, 0u);
    T2D_CHECK_EQ(layer.plot_count(), 0u);
    T2D_CHECK_EQ(layer.filled_cells(), 0u);
    T2D_CHECK_EQ(layer.cell_bytes(), 0u);   // a layer nobody placed anything on has no storage

    // Two plots on one tile layer cannot share a cell; the same footprint on another tile layer is
    // exactly what a floor and the machine standing on it are.
    T2D_REQUIRE(layer.place(content.registry, content.definitions,
                            at(ContentKind::Structure, "wall", 0, 1, 1, 2, 2)) != nullptr);
    T2D_CHECK(layer.place(content.registry, content.definitions, at(ContentKind::Structure, "wall", 0, 2, 2),
                          &report) == nullptr);
    T2D_CHECK(layer.place(content.registry, content.definitions, at(ContentKind::Structure, "wall", 0, 0, 0, 3, 3),
                          &report) == nullptr);
    T2D_CHECK_EQ(report.errors.size(), 6u);
    T2D_CHECK(report.errors[4].find("overlaps") != std::string::npos);
    T2D_CHECK_EQ(layer.plot_count(), 1u);
    T2D_CHECK_EQ(layer.filled_cells(), 4u);

    MineLayer two_layers(0, LayerShape{8, 8, 2}, 7);
    T2D_REQUIRE(two_layers.place(content.registry, content.definitions,
                                 at(ContentKind::Structure, "wall", 1, 1, 1, 2, 2)) != nullptr);
    T2D_REQUIRE(two_layers.place(content.registry, content.definitions,
                                 at(ContentKind::Floor, "dirt", 0, 1, 1, 2, 2)) != nullptr);
    T2D_CHECK_EQ(two_layers.plot_count(), 2u);
    T2D_CHECK_EQ(two_layers.filled_cells(), 8u);

    // Touching edges are not an overlap: two structures side by side are two structures.
    T2D_REQUIRE(layer.place(content.registry, content.definitions,
                            at(ContentKind::Structure, "wall", 0, 3, 1, 2, 2)) != nullptr);
    T2D_CHECK_EQ(layer.plot_count(), 2u);
}

T2D_TEST(content_that_registered_no_definition_is_still_a_plot) {
    // A name a mod registered through the C ABI has no file behind it: no picture, nothing to turn
    // around - but it is content, and it occupies its cells like everything else.
    ContentRegistry registry;
    registry.register_content(ContentKind::Machine, "mystery");
    ContentDefinitions none;
    MineLayer layer(0, LayerShape{4, 4, 1}, 1);
    const SceneTile* plot = layer.place(registry, none, at(ContentKind::Machine, "mystery", 0, 1, 1));
    T2D_REQUIRE(plot != nullptr);
    T2D_CHECK_FALSE(plot->random_reverse());
    T2D_CHECK_FALSE(plot->mirrored());
    T2D_CHECK_EQ(layer.plot_count(), 1u);
    T2D_CHECK_EQ(layer.ticking_count(), 1u);
}

T2D_TEST(the_dice_decide_how_plots_draw_themselves_and_only_once) {
    Fixture content;
    content.add(ContentKind::Floor, "dirt", /*random_reverse=*/true);
    const LayerShape shape{16, 16, 1};
    LayerSpec spec;
    spec.index = 0;
    spec.shape = shape;
    for (i32 y = 0; y < 16; ++y) {
        for (i32 x = 0; x < 16; ++x) spec.content.push_back(at(ContentKind::Floor, "dirt", 0, x, y));
    }

    MineLayer layer(0, shape, 99);
    LayerBuildReport report;
    layer.build(spec, content.registry, content.definitions, &report);
    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.placed, 256u);
    // Half of them, give or take: 256 coins is 8 of standard deviation, so this is a wide band that a
    // wrong probability still fails.
    T2D_CHECK_GT(layer.mirrored_count(), 88u);
    T2D_CHECK_LT(layer.mirrored_count(), 168u);
    T2D_CHECK_EQ(report.mirrored, layer.mirrored_count());

    // Rolled once, at build time: the flag never changes afterwards, and two layers built from the
    // same seed come out identical.
    MineLayer same(0, shape, 99);
    same.build(spec, content.registry, content.definitions, nullptr);
    T2D_CHECK_EQ(same.mirrored_count(), layer.mirrored_count());
    T2D_CHECK_EQ(same.dump_text(content.registry), layer.dump_text(content.registry));
    MineLayer other(0, shape, 100);
    other.build(spec, content.registry, content.definitions, nullptr);
    T2D_CHECK_NE(other.dump_text(content.registry), layer.dump_text(content.registry));
}

T2D_TEST(content_that_may_not_be_turned_around_does_not_touch_the_dice) {
    Fixture content;
    content.add(ContentKind::Floor, "still");
    content.add(ContentKind::Floor, "turning", /*random_reverse=*/true);

    // The same layer with one plot more in front of it: a plot that may not be turned around must not
    // shift the dice for the plots after it (docs/GAME_DESIGN.md section 1.14).
    const auto build_with = [&](bool with_still) {
        LayerSpec spec;
        spec.shape = LayerShape{4, 4, 1};
        if (with_still) spec.content.push_back(at(ContentKind::Floor, "still", 0, 0, 0));
        for (i32 x = 0; x < 4; ++x) spec.content.push_back(at(ContentKind::Floor, "turning", 0, x, 1));
        MineLayer layer(0, spec.shape, 5);
        layer.build(spec, content.registry, content.definitions, nullptr);
        std::vector<bool> mirrored;
        for (const auto& plot : layer.plots()) mirrored.push_back(plot->mirrored());
        return mirrored;
    };
    const std::vector<bool> without = build_with(false);
    const std::vector<bool> with = build_with(true);
    T2D_REQUIRE(without.size() == 4u);
    T2D_REQUIRE(with.size() == 5u);
    T2D_CHECK_FALSE(with[0]);   // the content that may not turn around never does
    for (usize index = 0; index < without.size(); ++index) T2D_CHECK_EQ(with[index + 1], without[index]);
}

T2D_TEST(only_the_plots_that_run_are_ticked_and_only_the_dirty_are_refreshed) {
    Fixture content;
    content.add(ContentKind::Floor, "dirt");
    content.add(ContentKind::Structure, "wall");
    content.add(ContentKind::Machine, "drill");
    MineLayer layer(0, LayerShape{8, 8, 1}, 3);
    LayerBuildReport report;
    (void)layer.place(content.registry, content.definitions, at(ContentKind::Floor, "dirt", 0, 0, 0), &report);
    (void)layer.place(content.registry, content.definitions, at(ContentKind::Structure, "wall", 0, 1, 0), &report);
    SceneTile* machine = layer.place(content.registry, content.definitions,
                                     at(ContentKind::Machine, "drill", 0, 2, 0), &report);
    T2D_REQUIRE(machine != nullptr);
    T2D_CHECK_EQ(report.ticking, 1u);
    T2D_CHECK_EQ(layer.ticking_count(), 1u);
    T2D_CHECK_EQ(layer.plot_count(), 3u);

    // A new plot is dirty: nobody has looked at it yet. The refresh pass walks everything and pays for
    // the dirty ones only.
    T2D_CHECK_EQ(layer.refresh(), 3u);
    T2D_CHECK_EQ(layer.refresh(), 0u);
    machine->mark_dirty();
    T2D_CHECK_EQ(layer.refresh(), 1u);

    // The tick pass walks the machines and nothing else, on the cadence each one owns.
    auto* entity = dynamic_cast<EntityTile*>(machine);
    T2D_REQUIRE(entity != nullptr);
    entity->set_period_seconds(1.0f);
    T2D_CHECK_EQ(layer.tick(0.5f), 0u);
    T2D_CHECK_EQ(layer.tick(0.5f), 1u);
    // A frame longer than a period runs it once and hands it the whole span: a plot cannot replay the
    // ticks it missed, or a hitch would change what the mine produces.
    T2D_CHECK_EQ(layer.tick(2.5f), 1u);
    T2D_CHECK_NEAR(entity->since_last_update(), 0.5f, 1e-6f);
}

T2D_TEST(a_query_visits_what_is_near_and_each_plot_exactly_once) {
    Fixture content;
    content.add(ContentKind::Structure, "wall");
    MineLayer layer(0, LayerShape{64, 8, 1}, 1);
    // A 3x3 that straddles the chunk boundary at x = 32: it is indexed in two chunks, and a query that
    // covers both of them must still see it once.
    SceneTile* straddling = layer.place(content.registry, content.definitions,
                                        at(ContentKind::Structure, "wall", 0, 31, 2, 3, 3));
    T2D_REQUIRE(straddling != nullptr);
    T2D_REQUIRE(layer.place(content.registry, content.definitions,
                            at(ContentKind::Structure, "wall", 0, 60, 0)) != nullptr);

    const std::vector<const SceneTile*> both = plots_in(layer, t2d::TileRect{30, 0, 4, 8});
    T2D_REQUIRE(both.size() == 1u);
    T2D_CHECK(both[0] == straddling);

    // A query over the middle of the plot, which is in the second chunk only.
    T2D_CHECK_EQ(plots_in(layer, t2d::TileRect{33, 4, 1, 1}).size(), 1u);
    // A query that touches neither.
    T2D_CHECK_EQ(plots_in(layer, t2d::TileRect{10, 0, 4, 4}).size(), 0u);
    // The far one is found where it is and nowhere else.
    T2D_CHECK_EQ(plots_in(layer, t2d::TileRect{0, 0, 64, 8}).size(), 2u);
    T2D_CHECK(layer.plot_at(0, GridPos{33, 4}) == straddling);
    T2D_CHECK(layer.plot_at(0, GridPos{34, 4}) == nullptr);
}

T2D_TEST(collision_is_a_question_about_a_layer_mask) {
    Fixture content;
    content.add(ContentKind::Floor, "dirt");
    content.add(ContentKind::Structure, "wall");
    MineLayer layer(0, LayerShape{4, 4, 2}, 1);
    (void)layer.place(content.registry, content.definitions, at(ContentKind::Floor, "dirt", 0, 0, 0));
    (void)layer.place(content.registry, content.definitions, at(ContentKind::Structure, "wall", 1, 1, 1));

    // Which tile layers block is the designer's; the layer only answers for the mask it is handed.
    T2D_CHECK(layer.blocks(0b01, GridPos{0, 0}));
    T2D_CHECK_FALSE(layer.blocks(0b10, GridPos{0, 0}));
    T2D_CHECK_FALSE(layer.blocks(0b01, GridPos{1, 1}));
    T2D_CHECK(layer.blocks(0b10, GridPos{1, 1}));
    T2D_CHECK(layer.blocks(0b11, GridPos{0, 0}));
    T2D_CHECK_FALSE(layer.blocks(0b11, GridPos{2, 2}));
    // Nothing blocks when nothing is asked about.
    T2D_CHECK_FALSE(layer.blocks(0, GridPos{0, 0}));
    // Outside the map there is nothing to route through.
    T2D_CHECK(layer.blocks(0b11, GridPos{-1, 0}));
    T2D_CHECK(layer.blocks(0b11, GridPos{4, 0}));
    T2D_CHECK(layer.blocks(0b11, GridPos{0, 4}));
}

T2D_TEST(the_same_seed_and_layer_index_build_the_same_layer_twice) {
    Fixture content;
    content.add(ContentKind::Floor, "dirt", /*random_reverse=*/true);
    content.add(ContentKind::Structure, "wall");
    const LayerShape shape{32, 24, 2};

    // The generator interface of section 4: (seed, index) in, a layer description out - and the same
    // pair always gives the same description, which is what makes a mine reproducible.
    MineWorld first(2024, shape);
    first.set_generator(debug_scatter_generator(content.registry, shape, 30));
    const MineLayer& a = first.enter(0, content.registry, content.definitions);
    T2D_CHECK_EQ(a.plot_count(), first.build_report().placed);
    T2D_CHECK_GT(a.plot_count(), 0u);

    MineWorld second(2024, shape);
    second.set_generator(debug_scatter_generator(content.registry, shape, 30));
    const MineLayer& b = second.enter(0, content.registry, content.definitions);
    T2D_CHECK_EQ(a.seed(), b.seed());
    T2D_CHECK_EQ(a.dump_text(content.registry), b.dump_text(content.registry));

    // A different layer of the same mine is a different map; the first one comes back unchanged.
    const MineLayer& other = second.enter(1, content.registry, content.definitions);
    T2D_CHECK_EQ(other.index(), 1);
    T2D_CHECK_NE(other.seed(), a.seed());
    T2D_CHECK_NE(other.dump_text(content.registry), a.dump_text(content.registry));
    const MineLayer& again = second.enter(0, content.registry, content.definitions);
    T2D_CHECK_EQ(again.dump_text(content.registry), a.dump_text(content.registry));
    T2D_CHECK_EQ(second.layer_index(), 0);
}

T2D_TEST(a_layer_with_no_content_is_still_a_layer) {
    // Content is not authored yet (docs/GAME_DESIGN.md section 7): an empty layer is what a session
    // enters until the designer's layer rules arrive, and everything has to work on it.
    ContentRegistry registry;
    ContentDefinitions definitions;
    MineWorld world(11, LayerShape{16, 12, 3});
    world.set_generator(empty_layer_generator(world.shape()));
    const MineLayer& layer = world.enter(0, registry, definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(layer.plot_count(), 0u);
    T2D_CHECK_EQ(layer.filled_cells(), 0u);
    T2D_CHECK_EQ(layer.cell_bytes(), 0u);
    T2D_CHECK_EQ(layer.width(), 16u);
    T2D_CHECK_EQ(layer.height(), 12u);
    T2D_CHECK_EQ(layer.layer_count(), 3);
    T2D_CHECK_EQ(world.refresh(), 0u);
    T2D_CHECK_EQ(world.tick(0.016f), 0u);
    T2D_CHECK_FALSE(world.blocks(0b111, GridPos{4, 4}));
    T2D_CHECK(world.blocks(0b111, GridPos{16, 4}));
    T2D_CHECK_EQ(plots_in(layer, t2d::TileRect{0, 0, 16, 12}).size(), 0u);
    T2D_CHECK(layer.dump_text(registry).find("0 plot(s)") != std::string::npos);

    // A world without a layer answers the same way rather than crashing.
    MineWorld empty(1, LayerShape{4, 4, 1});
    T2D_CHECK_FALSE(empty.has_layer());
    T2D_CHECK_EQ(empty.refresh(), 0u);
    T2D_CHECK_EQ(empty.tick(1.0f), 0u);
    T2D_CHECK(empty.blocks(0b1, GridPos{0, 0}));
}

T2D_TEST(the_debug_generators_lay_out_whatever_the_registry_holds) {
    Fixture content;
    content.add(ContentKind::Floor, "dirt");
    content.add(ContentKind::Structure, "wall");
    const LayerShape shape{8, 4, 2};

    // Bands: one band per registered entry, each on its own tile layer - a view of the registry, not a
    // rule about the mine. The walk is the same one the sandbox's palette uses: by kind, then in
    // registration order, so the structures come before the floors.
    MineWorld world(1, shape);
    world.set_generator(debug_band_generator(content.registry, shape));
    const MineLayer& banded = world.enter(0, content.registry, content.definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(banded.plot_count(), 32u);
    T2D_CHECK_EQ(banded.filled_cells(0), 16u);
    T2D_CHECK_EQ(banded.filled_cells(1), 16u);
    T2D_REQUIRE(banded.plot_at(0, GridPos{0, 0}) != nullptr);
    T2D_REQUIRE(banded.plot_at(1, GridPos{0, 2}) != nullptr);
    T2D_CHECK_EQ(banded.plot_at(0, GridPos{0, 0})->kind(), ContentKind::Structure);
    T2D_CHECK_EQ(banded.plot_at(1, GridPos{0, 2})->kind(), ContentKind::Floor);
    // Everything on one tile layer still fits, because no two plots share a cell.
    MineWorld single(1, LayerShape{8, 4, 1});
    single.set_generator(debug_band_generator(content.registry, shape));
    T2D_CHECK_EQ(single.enter(0, content.registry, content.definitions).plot_count(), 32u);
    T2D_CHECK(single.build_report().clean());

    // An empty registry is an empty layer, not a failure.
    ContentRegistry nothing;
    ContentDefinitions none;
    MineWorld bare(1, shape);
    bare.set_generator(debug_band_generator(nothing, shape));
    T2D_CHECK_EQ(bare.enter(0, nothing, none).plot_count(), 0u);
    T2D_CHECK(bare.build_report().clean());
}

T2D_TEST(a_generator_that_answers_for_the_wrong_layer_is_reported) {
    ContentRegistry registry;
    ContentDefinitions definitions;
    MineWorld world(1, LayerShape{4, 4, 1});
    world.set_generator([](u64, i32 index) {
        LayerSpec spec;
        spec.index = index + 1;
        spec.shape = LayerShape{4, 4, 1};
        return spec;
    });
    (void)world.enter(0, registry, definitions);
    T2D_REQUIRE(world.build_report().errors.size() == 1u);
    T2D_CHECK(world.build_report().errors[0].find("layer 1") != std::string::npos);
    T2D_CHECK_EQ(world.layer_index(), 0);
}

T2D_TEST_MAIN
