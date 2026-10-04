// The content-logic types: what a plot is and where it is, and the two kinds of plot the mine has -
// scenery, which is brought up to date only when something asks, and functional plots, which run on a
// cadence they own.
#include <mine/types/types.h>

#include <support/test_support.h>

#include <memory>
#include <type_traits>

using namespace mine;
using namespace mine::types;

namespace {

/// A scene plot that counts how often it was brought up to date.
class CountingFloor final : public SceneTile {
public:
    using SceneTile::SceneTile;

    int refreshes = 0;
    bool destroy_seen = false;

protected:
    void on_refresh() override { ++refreshes; }
};

/// An entity plot that records what it was told, and marks itself when it is destroyed.
class CountingMachine final : public EntityTile {
public:
    using EntityTile::EntityTile;

    int runs = 0;
    f32 accounted = 0.0f;

protected:
    void on_update(f32 elapsed) override {
        ++runs;
        accounted += elapsed;
    }
};

/// A machine that says so when it is destroyed through the base class.
class WatchedMachine final : public EntityTile {
public:
    WatchedMachine(bool& destroyed, ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1,
                   i32 height = 1)
        : EntityTile(kind, id, layer, anchor, width, height), destroyed_(destroyed) {}
    ~WatchedMachine() override { destroyed_ = true; }

protected:
    void on_update(f32) override {}

private:
    bool& destroyed_;
};

/// A registry with one structure in it, for the name lookups.
[[nodiscard]] ContentRegistry registry_with(const char* name, ContentKind kind = ContentKind::Structure) {
    ContentRegistry registry;
    registry.register_content(kind, name);
    return registry;
}

} // namespace

T2D_TEST(a_plot_is_a_base_class_and_not_a_thing_of_its_own) {
    // Every plot is scenery or a functional plot; there is no third kind to construct.
    static_assert(!std::is_constructible_v<Tile, ContentKind, ContentId, i32, GridPos, i32, i32>);
    static_assert(std::is_base_of_v<Tile, SceneTile>);
    static_assert(std::is_base_of_v<Tile, EntityTile>);
    static_assert(std::has_virtual_destructor_v<Tile>);
    T2D_CHECK(true);
}

T2D_TEST(a_plot_knows_what_it_is_and_where_it_is) {
    const ContentRegistry registry = registry_with("wall");
    const CountingFloor wall(ContentKind::Structure, 1, 2, GridPos{5, 7});
    T2D_CHECK_EQ(wall.kind(), ContentKind::Structure);
    T2D_CHECK_EQ(wall.id(), 1u);
    T2D_CHECK_EQ(wall.layer(), 2);
    T2D_CHECK_EQ(wall.anchor(), GridPos({5, 7}));
    T2D_CHECK_EQ(wall.width(), 1);
    T2D_CHECK_EQ(wall.height(), 1);
    T2D_CHECK_EQ(wall.cell_count(), 1);
    T2D_CHECK(wall.valid());
    T2D_CHECK_FALSE(wall.missing());
    // A name is never stored in the plot: it is the registry's answer for the id, which is what keeps
    // a reload able to re-point plots by name after the ids moved.
    T2D_CHECK_EQ(wall.name(registry), std::string("wall"));

    // An empty cell is not a plot: kNoContent means there is nothing behind it.
    const CountingFloor nothing(ContentKind::Item, kNoContent, 0, GridPos{0, 0});
    T2D_CHECK_FALSE(nothing.valid());
}

T2D_TEST(a_big_structure_covers_every_cell_of_its_footprint) {
    // 3x3, 2x3 and 2x2: the large structures the game has to support are rectangles of cells.
    const CountingFloor big(ContentKind::Structure, 1, 0, GridPos{10, 20}, 3, 3);
    T2D_CHECK_EQ(big.cell_count(), 9);
    T2D_CHECK_EQ(big.width(), 3);
    T2D_CHECK_EQ(big.height(), 3);
    T2D_CHECK_EQ(big.cells(), TileRect({10, 20, 3, 3}));
    for (i32 y = 20; y < 23; ++y) {
        for (i32 x = 10; x < 13; ++x) T2D_CHECK(big.covers(GridPos{x, y}));
    }
    T2D_CHECK_FALSE(big.covers(GridPos{9, 20}));
    T2D_CHECK_FALSE(big.covers(GridPos{13, 20}));
    T2D_CHECK_FALSE(big.covers(GridPos{10, 23}));

    const CountingFloor wide(ContentKind::Structure, 2, 0, GridPos{0, 0}, 2, 3);
    T2D_CHECK_EQ(wide.cell_count(), 6);
    const CountingFloor square(ContentKind::Structure, 3, 0, GridPos{0, 0}, 2, 2);
    T2D_CHECK_EQ(square.cell_count(), 4);

    // A size below one cell is one cell: a plot is never nowhere.
    const CountingFloor tiny(ContentKind::Structure, 4, 0, GridPos{4, 4}, 0, -7);
    T2D_CHECK_EQ(tiny.cells(), TileRect({4, 4, 1, 1}));
    T2D_CHECK_EQ(footprint(GridPos{1, 2}), TileRect({1, 2, 1, 1}));
    T2D_CHECK_EQ(footprint(GridPos{1, 2}, 2, 3), TileRect({1, 2, 2, 3}));
}

T2D_TEST(two_plots_overlap_only_on_the_same_layer_and_over_shared_cells) {
    const CountingFloor left(ContentKind::Structure, 1, 0, GridPos{0, 0}, 2, 2);
    const CountingFloor touching(ContentKind::Structure, 1, 0, GridPos{2, 0}, 2, 2);
    const CountingFloor corner(ContentKind::Structure, 1, 0, GridPos{1, 1}, 2, 2);
    const CountingFloor same_cells_other_layer(ContentKind::Structure, 1, 1, GridPos{0, 0}, 2, 2);

    T2D_CHECK_FALSE(left.overlaps(touching));            // side by side is not overlapping
    T2D_CHECK(left.overlaps(corner));                    // one shared cell is
    T2D_CHECK(corner.overlaps(left));
    T2D_CHECK_FALSE(left.overlaps(same_cells_other_layer));   // a floor and what stands on it
    T2D_CHECK(left.overlaps(left));

    // A 3x3 put down on the edge of a 10x10 map does not fit; one cell in from the edge does.
    const CountingFloor at_the_edge(ContentKind::Structure, 1, 0, GridPos{8, 0}, 3, 3);
    const CountingFloor inside(ContentKind::Structure, 1, 0, GridPos{7, 7}, 3, 3);
    const CountingFloor outside(ContentKind::Structure, 1, 0, GridPos{-1, 0}, 2, 2);
    T2D_CHECK_FALSE(at_the_edge.within(10, 10));
    T2D_CHECK(inside.within(10, 10));
    T2D_CHECK_FALSE(outside.within(10, 10));
}

T2D_TEST(a_plot_whose_content_is_gone_keeps_its_id) {
    // The sandbox does the same for a cell: the id stays, the plot is marked, and the content coming
    // back repairs it by name rather than the plot having lost its place.
    const ContentRegistry empty;
    CountingFloor vein(ContentKind::Structure, 7, 0, GridPos{3, 3});
    T2D_CHECK(vein.valid());
    T2D_CHECK_EQ(vein.name(empty), std::string("#7"));

    vein.set_missing(true);
    T2D_CHECK_FALSE(vein.valid());
    T2D_CHECK(vein.missing());
    T2D_CHECK_EQ(vein.id(), 7u);
    T2D_CHECK_EQ(vein.anchor(), GridPos({3, 3}));

    // The content comes back - a reload registered it again - and the plot is repaired by name: the id
    // is whatever the registry in force hands out, which is why the plot never stored one.
    vein.set_missing(false);
    T2D_CHECK(vein.valid());
    ContentRegistry registry = registry_with("ore_vein");
    const ContentId repaired = registry.find(ContentKind::Structure, "ore_vein");
    T2D_CHECK_NE(repaired, kNoContent);
    CountingFloor repaired_vein(ContentKind::Structure, repaired, 0, GridPos{3, 3});
    T2D_CHECK_EQ(repaired_vein.name(registry), std::string("ore_vein"));
    T2D_CHECK_EQ(repaired_vein.anchor(), vein.anchor());
    // A plot whose id the registry does not have says so with the number, the way the sandbox does.
    T2D_CHECK_EQ(vein.name(registry), std::string("#7"));
}

T2D_TEST(scenery_is_brought_up_to_date_only_when_something_asks) {
    CountingFloor floor(ContentKind::Structure, 1, 0, GridPos{0, 0});
    // A new plot has never been looked at, so it starts out needing one look.
    T2D_CHECK(floor.dirty());
    T2D_CHECK_EQ(floor.refreshes, 0);

    T2D_CHECK(floor.refresh());
    T2D_CHECK_EQ(floor.refreshes, 1);
    T2D_CHECK_FALSE(floor.dirty());

    // Nothing changed, so nothing happens: this is what a frame costs when nothing did.
    T2D_CHECK_FALSE(floor.refresh());
    T2D_CHECK_FALSE(floor.refresh());
    T2D_CHECK_EQ(floor.refreshes, 1);

    // Something next to it changed, and says so. The plot does its work once, on the next pass.
    floor.mark_dirty();
    T2D_CHECK(floor.dirty());
    T2D_CHECK(floor.refresh());
    T2D_CHECK_EQ(floor.refreshes, 2);
    T2D_CHECK_FALSE(floor.refresh());

    // Scenery has no cadence at all: a plot that was never marked dirty never runs, however many
    // frames go by.
    for (int frame = 0; frame < 100; ++frame) (void)floor.refresh();
    T2D_CHECK_EQ(floor.refreshes, 2);
}

T2D_TEST(a_functional_plot_runs_on_the_cadence_it_was_given) {
    CountingMachine every_tick(ContentKind::Machine, 1, 0, GridPos{0, 0});
    T2D_CHECK_NEAR(every_tick.period_seconds(), 0.0f, 0.0001f);
    for (int frame = 0; frame < 3; ++frame) T2D_CHECK(every_tick.advance(0.016f));
    T2D_CHECK_EQ(every_tick.runs, 3);
    T2D_CHECK_NEAR(every_tick.accounted, 0.048f, 0.0001f);
    T2D_CHECK_NEAR(every_tick.since_last_update(), 0.0f, 0.0001f);

    // Twice a second: two thirds of a second is one run, and the leftover is carried.
    CountingMachine slow(ContentKind::Machine, 2, 0, GridPos{0, 0});
    slow.set_period_seconds(0.5f);
    T2D_CHECK_NEAR(slow.period_seconds(), 0.5f, 0.0001f);
    T2D_CHECK_FALSE(slow.advance(0.2f));
    T2D_CHECK_FALSE(slow.advance(0.2f));
    T2D_CHECK_EQ(slow.runs, 0);
    T2D_CHECK_NEAR(slow.since_last_update(), 0.4f, 0.0001f);
    T2D_CHECK(slow.advance(0.2f));
    T2D_CHECK_EQ(slow.runs, 1);
    T2D_CHECK_NEAR(slow.accounted, 0.6f, 0.0001f);          // it is told the whole span it accounts for
    T2D_CHECK_NEAR(slow.since_last_update(), 0.1f, 0.0001f); // and the fraction is not thrown away
    T2D_CHECK_FALSE(slow.advance(0.2f));
    T2D_CHECK(slow.advance(0.2f));
    T2D_CHECK_EQ(slow.runs, 2);
    T2D_CHECK_NEAR(slow.accounted, 1.1f, 0.0001f);

    // A period below zero is read as every tick rather than as a plot that never runs.
    CountingMachine odd(ContentKind::Machine, 3, 0, GridPos{0, 0});
    odd.set_period_seconds(-4.0f);
    T2D_CHECK_NEAR(odd.period_seconds(), 0.0f, 0.0001f);
    T2D_CHECK(odd.advance(0.016f));
}

T2D_TEST(a_long_frame_runs_a_plot_once_with_the_whole_span) {
    // A hitch must not replay the ticks it missed - that would make what the mine produces depend on
    // how the frames fell - so the plot runs once and is handed the whole time to account for.
    CountingMachine drill(ContentKind::Machine, 1, 0, GridPos{0, 0});
    drill.set_period_seconds(1.0f);
    T2D_CHECK(drill.advance(5.0f));
    T2D_CHECK_EQ(drill.runs, 1);
    T2D_CHECK_NEAR(drill.accounted, 5.0f, 0.0001f);
    T2D_CHECK_NEAR(drill.since_last_update(), 0.0f, 0.0001f);
    // Five whole periods leave nothing over, so the next second is a run again.
    T2D_CHECK_FALSE(drill.advance(0.5f));
    T2D_CHECK(drill.advance(0.5f));
    T2D_CHECK_EQ(drill.runs, 2);

    // A frame of no time at all does not run a plot that is waiting for its period.
    CountingMachine waiting(ContentKind::Machine, 2, 0, GridPos{0, 0});
    waiting.set_period_seconds(1.0f);
    T2D_CHECK_FALSE(waiting.advance(0.0f));
    T2D_CHECK_FALSE(waiting.advance(-3.0f));   // a negative delta is not time going backwards into work
    T2D_CHECK_EQ(waiting.runs, 0);
    T2D_CHECK(waiting.advance(1.0f));
}

T2D_TEST(a_plot_is_a_plot_through_a_base_pointer) {
    bool destroyed = false;
    {
        std::unique_ptr<Tile> plot =
            std::make_unique<WatchedMachine>(destroyed, ContentKind::Machine, 1, 0, GridPos{2, 2}, 2, 2);
        T2D_CHECK_EQ(plot->kind(), ContentKind::Machine);
        T2D_CHECK_EQ(plot->cell_count(), 4);
        // What the base cannot know is still reachable: the simulation keeps plots of both kinds and
        // asks the functional ones for their cadence.
        auto* machine = dynamic_cast<EntityTile*>(plot.get());
        T2D_REQUIRE(machine != nullptr);
        machine->set_period_seconds(0.25f);
        T2D_CHECK(machine->advance(0.25f));
        T2D_CHECK(dynamic_cast<SceneTile*>(plot.get()) == nullptr);
    }
    // Destroying through the base runs the derived destructor: a plot that owns something releases it.
    T2D_CHECK(destroyed);
}

T2D_TEST_MAIN
