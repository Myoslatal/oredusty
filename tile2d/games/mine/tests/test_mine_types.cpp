// The content-logic types: what a plot is and where it is, and the two kinds of plot the mine has -
// scenery, which is brought up to date only when something asks, and functional plots, which run on a
// cadence they own.
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

/// A scene plot that counts how often it was brought up to date.
class CountingFloor final : public SceneTile {
public:
    using SceneTile::SceneTile;

    int refreshes = 0;
    bool destroy_seen = false;

protected:
    void on_refresh() override { ++refreshes; }
};

/// An entity plot that records both of the things it can be asked to do: being brought up to date
/// like scenery, and running on its cadence.
class CountingMachine final : public EntityTile {
public:
    using EntityTile::EntityTile;

    int runs = 0;
    int refreshes = 0;
    f32 accounted = 0.0f;

protected:
    void on_update(f32 elapsed) override {
        ++runs;
        accounted += elapsed;
    }
    void on_refresh() override { ++refreshes; }
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
    // Every plot is scenery, and a functional plot is scenery that also runs: there is no third kind
    // to construct, and nothing that is a plot without being scenery.
    static_assert(!std::is_constructible_v<Tile, ContentKind, ContentId, i32, GridPos, i32, i32>);
    static_assert(std::is_base_of_v<Tile, SceneTile>);
    static_assert(std::is_base_of_v<SceneTile, EntityTile>);
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

T2D_TEST(a_plot_that_is_still_going_up_does_not_update) {
    // A plot being built is on the map and is drawn, but it does nothing yet: it is not brought up to
    // date until its build is done (docs/GAME_DESIGN.md section 1.37).
    CountingFloor floor(ContentKind::Structure, 1, 0, GridPos{0, 0});
    floor.set_under_construction(true);
    T2D_CHECK(floor.under_construction());
    T2D_CHECK(floor.dirty());          // nothing has looked at it yet ...
    T2D_CHECK_FALSE(floor.refresh());  // ... and nothing does while it is going up
    T2D_CHECK_EQ(floor.refreshes, 0);
    T2D_CHECK(floor.dirty());          // the dirty flag is kept, not forgotten

    // Done: the next pass brings it up to date, once.
    floor.set_under_construction(false);
    T2D_CHECK_FALSE(floor.under_construction());
    T2D_CHECK(floor.refresh());
    T2D_CHECK_EQ(floor.refreshes, 1);
    T2D_CHECK_FALSE(floor.dirty());

    // A machine that is going up does not take its turn either, and the time it spent going up is not
    // owed to it: its first turn is a whole period after it is finished (types/entity_tile.h).
    CountingMachine machine(ContentKind::Machine, 2, 0, GridPos{0, 0});
    machine.set_period_seconds(1.0f);
    machine.set_under_construction(true);
    T2D_CHECK_FALSE(machine.advance(5.0f));
    T2D_CHECK_EQ(machine.runs, 0);
    T2D_CHECK_NEAR(machine.since_last_update(), 0.0f, 1e-6f);   // five seconds, none of them kept
    machine.set_under_construction(false);
    T2D_CHECK_FALSE(machine.advance(0.5f));
    T2D_CHECK(machine.advance(0.5f));
    T2D_CHECK_EQ(machine.runs, 1);
    T2D_CHECK_NEAR(machine.accounted, 1.0f, 1e-6f);
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
        // A machine is scenery too, so it can be reached as one: a pass over the world's scenery does
        // not walk past the machines.
        T2D_CHECK(dynamic_cast<SceneTile*>(plot.get()) != nullptr);
        T2D_CHECK(dynamic_cast<Tile*>(machine) == plot.get());
    }
    // Destroying through the base runs the derived destructor: a plot that owns something releases it.
    T2D_CHECK(destroyed);
}

T2D_TEST(a_machine_is_scenery_that_also_runs) {
    CountingMachine machine(ContentKind::Machine, 1, 0, GridPos{4, 4});
    machine.set_period_seconds(0.5f);

    // Both paths belong to the same object: the scene side brings it up to date when the world around
    // it changed, the cadence side runs it when its turn comes. Neither stands in for the other.
    T2D_CHECK(machine.dirty());
    T2D_CHECK(machine.refresh());
    T2D_CHECK_FALSE(machine.dirty());
    T2D_CHECK_EQ(machine.refreshes, 1);
    T2D_CHECK_EQ(machine.runs, 0);

    T2D_CHECK_FALSE(machine.advance(0.25f));
    T2D_CHECK(machine.advance(0.25f));
    T2D_CHECK_EQ(machine.runs, 1);
    T2D_CHECK_EQ(machine.refreshes, 1);   // running is not refreshing

    machine.mark_dirty();
    T2D_CHECK(machine.refresh());
    T2D_CHECK_EQ(machine.refreshes, 2);
    T2D_CHECK_EQ(machine.runs, 1);        // and refreshing is not running
}

T2D_TEST(a_plot_can_be_told_to_turn_itself_around) {
    // Off by default, and a plot that may not be turned around never is - however many times it is
    // asked, and without touching the generator (so turning the flag on for one plot cannot shift the
    // dice for the plots after it).
    CountingFloor plain(ContentKind::Structure, 1, 0, GridPos{0, 0});
    T2D_CHECK_FALSE(plain.random_reverse());
    T2D_CHECK_FALSE(plain.mirrored());
    t2d::Rng rng(7);
    const t2d::u64 before = rng.state_hash();
    for (int roll = 0; roll < 100; ++roll) plain.randomise_mirror(rng);
    T2D_CHECK_FALSE(plain.mirrored());
    T2D_CHECK_EQ(rng.state_hash(), before);

    // With it on, the map's own generator decides - and the same seed decides the same way twice.
    CountingFloor turnable(ContentKind::Structure, 2, 0, GridPos{0, 0});
    turnable.set_random_reverse(true);
    T2D_CHECK(turnable.random_reverse());
    t2d::Rng first(2024);
    t2d::Rng second(2024);
    usize mirrored = 0;
    std::string pattern;
    std::string repeated;
    for (int roll = 0; roll < 1000; ++roll) {
        turnable.randomise_mirror(first);
        pattern.push_back(turnable.mirrored() ? '1' : '0');
        if (turnable.mirrored()) ++mirrored;
        CountingFloor other(ContentKind::Structure, 2, 0, GridPos{0, 0});
        other.set_random_reverse(true);
        other.randomise_mirror(second);
        repeated.push_back(other.mirrored() ? '1' : '0');
    }
    // Half of them, give or take: 1000 rolls of a fair coin stay well inside this.
    T2D_CHECK_GT(mirrored, 400u);
    T2D_CHECK_LT(mirrored, 600u);
    T2D_CHECK_EQ(pattern, repeated);

    // A caller that knows better - a save that stored the answer, an editor - can set it directly.
    turnable.set_mirrored(true);
    T2D_CHECK(turnable.mirrored());
    turnable.set_mirrored(false);
    T2D_CHECK_FALSE(turnable.mirrored());
}

T2D_TEST(a_plot_previews_the_pictures_it_draws) {
    // A plot of no kind in particular has no art of its own, and neither has one whose entry names no
    // "image": the complete preview of both is no pictures at all (types/tile.cpp).
    SceneTile bare(ContentKind::Structure, 1, 0, GridPos{0, 0});
    T2D_CHECK(bare.get_preview().empty());

    TileDefinition pictureless;
    pictureless.kind = ContentKind::Structure;
    pictureless.name = "wall";
    SceneTile no_art(ContentKind::Structure, 1, 0, GridPos{0, 0});
    no_art.set_definition(&pictureless);
    T2D_CHECK(no_art.get_preview().empty());

    // Content that names a picture previews as that one picture: it is the entry's own image, so its part
    // is empty, it is not turned, and it turns about its middle when anything asks it to (types/tile.h).
    TileDefinition wall;
    wall.kind = ContentKind::Structure;
    wall.name = "wall";
    wall.image = "art/wall.png";
    SceneTile plot(ContentKind::Structure, 2, 0, GridPos{1, 1});
    plot.set_definition(&wall);
    const TilePreview preview = plot.get_preview();
    T2D_REQUIRE(preview.images.size() == 1u);
    T2D_CHECK(preview.images[0].part.empty());
    T2D_CHECK_FALSE(preview.images[0].mirrored);
    T2D_CHECK_EQ(preview.images[0].radians, 0.0f);
    const t2d::Vec2 middle{0.5f, 0.5f};
    T2D_CHECK_EQ(preview.images[0].pivot, middle);

    // Whether a plot draws mirrored is the plot's own answer, so a preview says what the map would draw.
    plot.set_mirrored(true);
    T2D_CHECK_EQ(plot.get_preview().images.size(), 1u);
    T2D_CHECK(plot.get_preview().images[0].mirrored);

    // Scenery does not turn: asking for a moment of the clock is the same picture.
    T2D_CHECK_EQ(plot.get_preview(3.5f).images.size(), 1u);
    T2D_CHECK_EQ(plot.get_preview(3.5f).images[0].radians, 0.0f);

    // What an interface holds is a plot as a Tile, so the answer has to come through the base class -
    // which is the whole point of the interface.
    const Tile& as_a_plot = plot;
    const TilePreview through_the_base = as_a_plot.get_preview();
    T2D_REQUIRE(through_the_base.images.size() == 1u);
    T2D_CHECK(through_the_base.images[0].mirrored);
    T2D_CHECK(through_the_base.images[0].part.empty());
}

T2D_TEST(a_floor_is_scenery_that_says_it_is_a_floor) {
    static_assert(std::is_base_of_v<SceneTile, Floor>);
    static_assert(std::is_base_of_v<Tile, Floor>);

    const ContentRegistry registry = registry_with("dirt", ContentKind::Floor);
    Floor dirt(ContentKind::Floor, 1, 0, GridPos{2, 3}, 2, 2);
    T2D_CHECK(dirt.is_floor());
    T2D_CHECK(dirt.kind() == ContentKind::Floor);
    T2D_CHECK_EQ(dirt.cell_count(), 4);
    T2D_CHECK_EQ(dirt.name(registry), std::string("dirt"));

    // Being a floor is a kind of thing, not a property every plot has: scenery and machines are not
    // floors unless they say so, and a floor is not a machine.
    CountingFloor wall(ContentKind::Structure, 2, 0, GridPos{0, 0});
    CountingMachine drill(ContentKind::Machine, 3, 0, GridPos{0, 0});
    T2D_CHECK_FALSE(wall.is_floor());
    T2D_CHECK_FALSE(drill.is_floor());
    // A floor is not a machine, and the compiler says so: Floor and EntityTile are siblings under
    // SceneTile, so a floor can never be asked for a cadence.

    // Everything a scene plot does, a floor does: it is scenery that happens to answer yes.
    T2D_CHECK(dirt.dirty());
    T2D_CHECK(dirt.refresh());
    T2D_CHECK_FALSE(dirt.refresh());

    // What the marker is for: a question anything can ask without knowing a single floor by name.
    const std::vector<std::unique_ptr<Tile>> plots = [&] {
        std::vector<std::unique_ptr<Tile>> made;
        made.push_back(std::make_unique<Floor>(ContentKind::Floor, 1, 0, GridPos{0, 0}));
        made.push_back(std::make_unique<SceneTile>(ContentKind::Structure, 2, 0, GridPos{1, 0}));
        made.push_back(std::make_unique<EntityTile>(ContentKind::Machine, 3, 0, GridPos{2, 0}));
        return made;
    }();
    usize floors = 0;
    for (const std::unique_ptr<Tile>& plot : plots) {
        if (plot->is_floor()) ++floors;
    }
    T2D_CHECK_EQ(floors, 1u);
}

T2D_TEST(an_ore_is_scenery_that_says_it_is_ore) {
    static_assert(std::is_base_of_v<SceneTile, SingleOre>);
    static_assert(std::is_base_of_v<Tile, SingleOre>);

    const ContentRegistry registry = registry_with("copper", ContentKind::Ore);
    SingleOre copper(ContentKind::Ore, 1, 1, GridPos{2, 3});
    T2D_CHECK(copper.is_ore());
    T2D_CHECK_FALSE(copper.is_floor());
    T2D_CHECK(copper.kind() == ContentKind::Ore);
    T2D_CHECK_EQ(copper.cell_count(), 1);
    T2D_CHECK_EQ(copper.name(registry), std::string("copper"));

    // Being ore is a kind of thing, not a property every plot has - the same rule the floor follows.
    CountingFloor wall(ContentKind::Structure, 2, 0, GridPos{0, 0});
    CountingMachine drill(ContentKind::Machine, 3, 0, GridPos{0, 0});
    Floor dirt(ContentKind::Floor, 4, 0, GridPos{0, 0});
    T2D_CHECK_FALSE(wall.is_ore());
    T2D_CHECK_FALSE(drill.is_ore());
    T2D_CHECK_FALSE(dirt.is_ore());

    // An ore is scenery: it is never ticked, and it is brought up to date only when something asks.
    T2D_CHECK(copper.dirty());
    T2D_CHECK(copper.refresh());
    T2D_CHECK_FALSE(copper.refresh());
    const SceneTile& as_scenery = copper;
    T2D_CHECK(dynamic_cast<const EntityTile*>(&as_scenery) == nullptr);

    // What the marker is for: a question anything can ask without knowing a single ore by name. A cell
    // can answer yes to both - a floor with ore in it is two plots on two tile layers.
    const std::vector<std::unique_ptr<Tile>> plots = [&] {
        std::vector<std::unique_ptr<Tile>> made;
        made.push_back(std::make_unique<SingleOre>(ContentKind::Ore, 1, 1, GridPos{0, 0}));
        made.push_back(std::make_unique<Floor>(ContentKind::Floor, 2, 0, GridPos{0, 0}));
        made.push_back(std::make_unique<SceneTile>(ContentKind::Structure, 3, 1, GridPos{1, 0}));
        return made;
    }();
    usize ores = 0;
    for (const std::unique_ptr<Tile>& plot : plots) {
        if (plot->is_ore()) ++ores;
    }
    T2D_CHECK_EQ(ores, 1u);
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

    // The definer turns a definition into the plot it describes: the kind decides the class, and the
    // data decides the rest.
    const std::unique_ptr<SceneTile> floor_plot = make_tile(definitions[0], 7, 1, GridPos{4, 5}, 2, 3);
    T2D_REQUIRE(floor_plot != nullptr);
    T2D_CHECK(floor_plot->is_floor());
    T2D_CHECK(dynamic_cast<Floor*>(floor_plot.get()) != nullptr);
    T2D_CHECK_EQ(floor_plot->id(), 7u);
    T2D_CHECK_EQ(floor_plot->layer(), 1);
    T2D_CHECK_EQ(floor_plot->cell_count(), 6);
    T2D_CHECK(floor_plot->random_reverse());
    T2D_CHECK_FALSE(floor_plot->mirrored());

    const std::unique_ptr<SceneTile> machine_plot = make_tile(definitions[1], 8, 0, GridPos{0, 0});
    T2D_REQUIRE(machine_plot != nullptr);
    T2D_CHECK(dynamic_cast<EntityTile*>(machine_plot.get()) != nullptr);
    T2D_CHECK_FALSE(machine_plot->is_floor());

    // The kind decides the class, so an "ore::" entry becomes an ore and not scenery with a flag.
    const std::unique_ptr<SceneTile> ore_plot = make_tile(definitions[2], 11, 1, GridPos{6, 7});
    T2D_REQUIRE(ore_plot != nullptr);
    T2D_CHECK(dynamic_cast<SingleOre*>(ore_plot.get()) != nullptr);
    T2D_CHECK(ore_plot->is_ore());
    T2D_CHECK_FALSE(ore_plot->is_floor());
    T2D_CHECK_FALSE(dynamic_cast<EntityTile*>(ore_plot.get()) != nullptr);

    TileDefinition structure;
    structure.kind = ContentKind::Structure;
    structure.name = "wall";
    const std::unique_ptr<SceneTile> wall_plot = make_tile(structure, 9, 0, GridPos{0, 0});
    T2D_REQUIRE(wall_plot != nullptr);
    T2D_CHECK(dynamic_cast<SceneTile*>(wall_plot.get()) != nullptr);
    T2D_CHECK(dynamic_cast<EntityTile*>(wall_plot.get()) == nullptr);

    // Something that is not a plot at all does not become one.
    TileDefinition item;
    item.kind = ContentKind::Item;
    item.name = "ore";
    T2D_CHECK(make_tile(item, 10, 0, GridPos{0, 0}) == nullptr);
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

    // Two plots of one entry read the **same** record: that is the flyweight of docs/GAME_DESIGN.md 1.35.
    const std::unique_ptr<SceneTile> first = make_tile(definition, 1, 0, GridPos{0, 0});
    const std::unique_ptr<SceneTile> second = make_tile(definition, 2, 0, GridPos{1, 0});
    T2D_REQUIRE(first != nullptr && second != nullptr);
    T2D_CHECK(first->definition() == &definition);
    T2D_CHECK(first->definition() == second->definition());
    T2D_REQUIRE(first->building() != nullptr);
    T2D_CHECK(first->building() == second->building());   // one building record, not one per plot
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
    const std::unique_ptr<SceneTile> locked_first = make_tile(*locked, 3, 0, GridPos{0, 0});
    const std::unique_ptr<SceneTile> locked_second = make_tile(*locked, 4, 0, GridPos{1, 0});
    T2D_REQUIRE(locked_first != nullptr && locked_second != nullptr);
    T2D_REQUIRE(locked_first->building() != nullptr);
    T2D_CHECK(locked_first->building() == locked_second->building());
    T2D_CHECK_FALSE(locked_first->building()->default_unlock);

    // A value that is not true/false is refused where the file is, with the field named.
    std::string error;
    const std::optional<TileDefinition> wrong = read_building(
        "machine::\n    press::\n        building::\n" "            default_unlock:1\n", &error);
    T2D_CHECK_FALSE(wrong.has_value());
    T2D_CHECK(error.find("default_unlock") != std::string::npos);
    // The list of fields the building table accepts names it, so a wrong one is answered with the choices.
    error.clear();
    const std::optional<TileDefinition> unknown = read_building(
        "machine::\n    press::\n        building::\n" "            unlock:false\n", &error);
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
    const std::unique_ptr<SceneTile> plot = make_tile(definitions.front(), 1, 0, GridPos{0, 0});
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
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:1.5\n            y:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:-0.1\n            y:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:\"half\"\n            y:0.5\n"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:2\n        spin_origin::\n            x:0.5\n            y:0.5\n            z:0.5\n"));
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
