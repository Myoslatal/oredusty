// Drills: the machine that mines the ore it stands on - the class (an entity plot), the two numbers the
// period comes out of (the drill's digging power and the ore's hardness), and the drop it throws out.
//
// The content in here is a **test double**: names like "vein" or "rig" are the fixture's, not the game's -
// the game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/types/tile_definition.h>
#include <mine/world.h>

#include <support/test_support.h>

#include <format>
#include <memory>
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

    ContentId add_ore(std::string_view name, f32 hardness, std::string drop) {
        const ContentId id = registry.register_content(ContentKind::Ore, name);
        TileDefinition definition;
        definition.kind = ContentKind::Ore;
        definition.name = std::string(name);
        definition.hardness = hardness;
        definition.drop = std::move(drop);
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_drill(std::string_view name, f32 power) {
        const ContentId id = registry.register_content(ContentKind::Drill, name);
        TileDefinition definition;
        definition.kind = ContentKind::Drill;
        definition.name = std::string(name);
        definition.dig_power = power;
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

[[nodiscard]] PlacedContent at(ContentKind kind, std::string name, i32 tile_layer, i32 x, i32 y) {
    PlacedContent where;
    where.kind = kind;
    where.name = std::move(name);
    where.layer = tile_layer;
    where.anchor = GridPos{x, y};
    return where;
}

/// A layer with dirt under it, one cell of ore, and a drill standing on that ore.
struct Rig {
    Fixture fixture;
    MineLayer layer{0, LayerShape{24, 16, 3}, 1};
    LayerBuildReport report;
    Drill* drill = nullptr;
    ContentId chunk = kNoContent;

    Rig(f32 hardness, f32 power, std::string drop_name) {
        chunk = fixture.add_drop("chunk");
        fixture.add_floor("dirt");
        fixture.add_ore("vein", hardness, std::move(drop_name));
        fixture.add_drill("rig", power);
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 5, 5), &report);
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Ore, "vein", 1, 5, 5), &report);
        SceneTile* placed =
            layer.place(fixture.registry, fixture.definitions, at(ContentKind::Drill, "rig", 2, 5, 5), &report);
        drill = dynamic_cast<Drill*>(placed);
    }
};

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

T2D_TEST(a_drill_is_an_entity_plot_that_mines_what_is_under_it) {
    Rig rig(2.0f, 1.0f, "chunk");
    T2D_REQUIRE(rig.drill != nullptr);
    T2D_CHECK_MSG(rig.report.clean(), "{}", rig.report.errors.empty() ? std::string{} : rig.report.errors[0]);

    // It is an entity plot - scenery that also runs (types/entity_tile.h) - and the layer counts it as one
    // that ticks, which is where the "machines cost a list, not a walk" saving comes from.
    T2D_CHECK(kind_is_a_tile(ContentKind::Drill));
    T2D_CHECK(dynamic_cast<EntityTile*>(rig.drill) != nullptr);
    T2D_CHECK_EQ(rig.layer.ticking_count(), 1u);
    T2D_CHECK(rig.drill->owner() == &rig.layer);

    // What it mines is the ore **under** it (a tile layer below the one it stands on), and the period is
    // the ore's hardness over the drill's power: 2.00 / 1.00 = 2.00 s per drop (section 1.26).
    T2D_CHECK(rig.drill->ready());
    T2D_CHECK_EQ(rig.drill->job().ore_name, std::string("vein"));
    T2D_CHECK_EQ(rig.drill->job().drop_name, std::string("chunk"));
    T2D_CHECK_EQ(rig.drill->job().drop_id, rig.chunk);
    T2D_CHECK_NEAR(rig.drill->period_seconds(), 2.0f, 1e-5f);
    T2D_CHECK_NEAR(rig.drill->dig_power(), 1.0f, 1e-5f);

    // A turn that has not come yet produces nothing.
    T2D_CHECK_EQ(rig.layer.tick(1.0f), 0u);
    T2D_CHECK_EQ(rig.layer.entity_count(), 0u);

    // Its turn produces **one drop**, thrown out of its own cell.
    T2D_CHECK_EQ(rig.layer.tick(1.0f), 1u);
    T2D_REQUIRE(rig.layer.entity_count() == 1u);
    T2D_CHECK_EQ(rig.drill->produced(), 1u);
    const Entity& first = *rig.layer.entities()[0];
    T2D_CHECK_EQ(first.kind(), ContentKind::Drop);
    T2D_CHECK_EQ(first.id(), rig.chunk);
    T2D_CHECK(first.draggable());   // what it throws out is a drop: the pointer can drag it (types/drop.h)
    T2D_CHECK_NEAR(first.position().x, 6.5f, 1e-5f);   // the drill is at 5,5; the first side is +x
    T2D_CHECK_NEAR(first.position().y, 5.5f, 1e-5f);
    // And the layer's quadtree finds it where it landed.
    T2D_CHECK_EQ(rig.layer.entities_within(t2d::Vec2{6.5f, 5.5f}, 0.25f).size(), 1u);

    // The sides turn, so a drill does not bury itself in one spot: the second drop goes to the next side.
    T2D_CHECK_EQ(rig.layer.tick(2.0f), 1u);
    T2D_REQUIRE(rig.layer.entity_count() == 2u);
    T2D_CHECK_NEAR(rig.layer.entities()[1]->position().x, 5.5f, 1e-5f);
    T2D_CHECK_NEAR(rig.layer.entities()[1]->position().y, 6.5f, 1e-5f);

    // A frame that swallowed three periods is worth three drops: a hitch must not change what the mine
    // produces (types/entity_tile.h), and the cadence hands over the whole span for exactly this.
    T2D_CHECK_EQ(rig.layer.tick(6.0f), 1u);   // the drill takes its turn once, with six seconds in hand
    T2D_CHECK_EQ(rig.layer.entity_count(), 5u);
    T2D_CHECK_EQ(rig.drill->produced(), 5u);
}

T2D_TEST(a_drill_with_nothing_to_mine_is_reported_and_idle) {
    // A drill with no ore under it: the layer says so while it is being built, and the drill produces
    // nothing - rather than a machine that quietly does nothing forever (world.h).
    {
        Fixture fixture;
        fixture.add_floor("dirt");
        fixture.add_ore("vein", 2.0f, "chunk");
        fixture.add_drill("rig", 1.0f);
        fixture.add_drop("chunk");
        MineLayer layer(0, LayerShape{16, 16, 3}, 1);
        LayerBuildReport report;
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 4, 4), &report);
        auto* drill = dynamic_cast<Drill*>(
            layer.place(fixture.registry, fixture.definitions, at(ContentKind::Drill, "rig", 2, 4, 4), &report));
        T2D_REQUIRE(drill != nullptr);
        T2D_REQUIRE(!report.clean());
        T2D_CHECK(report.errors[0].find("has no ore under it") != std::string::npos);
        T2D_CHECK_FALSE(drill->ready());
        T2D_CHECK_EQ(layer.tick(10.0f), 1u);    // it still takes its turn - it is an entity plot ...
        T2D_CHECK_EQ(layer.entity_count(), 0u); // ... and throws nothing out
        T2D_CHECK_EQ(drill->produced(), 0u);
    }

    // The four ways the data can fail to say enough, each reported with what is missing.
    const auto refuses = [](f32 hardness, std::string drop_name, f32 power, std::string_view expected) {
        Fixture fixture;
        fixture.add_floor("dirt");
        fixture.add_ore("vein", hardness, std::move(drop_name));
        fixture.add_drill("rig", power);
        if (power > 0.0f) fixture.add_drop("chunk");
        MineLayer layer(0, LayerShape{16, 16, 3}, 1);
        LayerBuildReport report;
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 4, 4), &report);
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Ore, "vein", 1, 4, 4), &report);
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Drill, "rig", 2, 4, 4), &report);
        bool found = false;
        for (const std::string& error : report.errors) {
            if (error.find(expected) != std::string::npos) found = true;
        }
        return found;
    };
    T2D_CHECK(refuses(0.0f, "chunk", 1.0f, "says nothing about its hardness"));
    T2D_CHECK(refuses(2.0f, "", 1.0f, "says nothing about what it drops"));
    T2D_CHECK(refuses(2.0f, "nothing_like_that", 1.0f, "no drop named 'nothing_like_that' is registered"));
    // A drill with no power: the ore is fine, the machine is not.
    {
        Fixture fixture;
        fixture.add_floor("dirt");
        fixture.add_ore("vein", 2.0f, "chunk");
        fixture.add_drill("rig", 0.0f);
        MineLayer layer(0, LayerShape{16, 16, 3}, 1);
        LayerBuildReport report;
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 4, 4), &report);
        (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Ore, "vein", 1, 4, 4), &report);
        auto* drill = dynamic_cast<Drill*>(
            layer.place(fixture.registry, fixture.definitions, at(ContentKind::Drill, "rig", 2, 4, 4), &report));
        T2D_REQUIRE(drill != nullptr);
        bool found = false;
        for (const std::string& error : report.errors) {
            if (error.find("says nothing about its dig_power") != std::string::npos) found = true;
        }
        T2D_CHECK(found);
        T2D_CHECK_FALSE(drill->ready());
    }
}

T2D_TEST(the_data_says_how_hard_an_ore_is_and_what_it_drops) {
    std::string error;
    const std::optional<TileDefinition> vein =
        read_one("ore::\n    vein::\n        image:\"art/vein.png\"\n        hardness:2.5\n        drop:\"chunk\"\n",
                 ContentKind::Ore, &error);
    T2D_REQUIRE(vein.has_value());
    T2D_CHECK_EQ(vein->name, std::string("vein"));
    T2D_CHECK_EQ(vein->image, std::string("art/vein.png"));
    T2D_CHECK_NEAR(vein->hardness, 2.5f, 1e-5f);
    T2D_CHECK_EQ(vein->drop, std::string("chunk"));

    const std::optional<TileDefinition> rig =
        read_one("drill::\n    rig::\n        image:\"art/rig.png\"\n        dig_power:1.5\n", ContentKind::Drill,
                 &error);
    T2D_REQUIRE(rig.has_value());
    T2D_CHECK_NEAR(rig->dig_power, 1.5f, 1e-5f);

    // A number the engine reads and cannot use is refused where the file is, with what is wrong.
    const auto refused = [](std::string_view text, ContentKind kind, std::string_view expected) {
        std::string message;
        const std::optional<TileDefinition> definition = read_one(text, kind, &message);
        return !definition.has_value() && message.find(expected) != std::string::npos;
    };
    T2D_CHECK(refused("ore::\n    vein::\n        hardness:\"soft\"\n", ContentKind::Ore, "must be a number"));
    T2D_CHECK(refused("ore::\n    vein::\n        hardness:0\n", ContentKind::Ore, "must be greater than zero"));
    T2D_CHECK(refused("ore::\n    vein::\n        hardness:-1\n", ContentKind::Ore, "must be greater than zero"));
    T2D_CHECK(refused("ore::\n    vein::\n        drop:7\n", ContentKind::Ore, "must be the quoted name of a drop"));
    T2D_CHECK(refused("ore::\n    vein::\n        drop:\"\"\n", ContentKind::Ore, "must be the quoted name of a drop"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:\"fast\"\n", ContentKind::Drill, "must be a number"));
    T2D_CHECK(refused("drill::\n    rig::\n        dig_power:0\n", ContentKind::Drill, "must be greater than zero"));
    // A field of another kind is the designer's, not an error: a floor that writes a hardness is a floor.
    const std::optional<TileDefinition> floor =
        read_one("floor::\n    dirt::\n        hardness:2\n        drop:\"chunk\"\n", ContentKind::Floor, &error);
    T2D_REQUIRE(floor.has_value());
    T2D_CHECK_NEAR(floor->hardness, 0.0f, 1e-6f);
    T2D_CHECK(floor->drop.empty());
}

T2D_TEST(the_debug_fill_puts_a_drill_on_a_cell_that_has_ore) {
    Fixture fixture;
    fixture.add_floor("dirt");
    fixture.add_ore("vein", 2.0f, "chunk");
    fixture.add_drill("rig", 1.0f);
    fixture.add_drop("chunk");
    MineLayer layer(0, LayerShape{24, 16, 3}, 1);
    LayerBuildReport report;
    (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Floor, "dirt", 0, 5, 5), &report);
    (void)layer.place(fixture.registry, fixture.definitions, at(ContentKind::Ore, "vein", 1, 5, 5), &report);

    T2D_CHECK_EQ(lay_out_drills(layer, fixture.registry, fixture.definitions), 1u);
    T2D_REQUIRE(layer.entity_count() == 0u);   // the fill places plots, not entities
    T2D_REQUIRE(layer.plot_count() == 3u);
    const SceneTile* placed = layer.plot_at(2, GridPos{5, 5});
    T2D_REQUIRE(placed != nullptr);
    T2D_CHECK(dynamic_cast<const Drill*>(placed) != nullptr);
    T2D_CHECK(dynamic_cast<const Drill*>(placed)->ready());

    // No ore anywhere: nothing is placed (the fill never invents content, and a drill with nothing under
    // it is not worth placing).
    MineLayer bare(0, LayerShape{24, 16, 3}, 1);
    T2D_CHECK_EQ(lay_out_drills(bare, fixture.registry, fixture.definitions), 0u);
    T2D_CHECK_EQ(bare.plot_count(), 0u);
    // One tile layer: there is no layer above the ore for a drill to stand on.
    MineLayer flat(0, LayerShape{24, 16, 1}, 1);
    T2D_CHECK_EQ(lay_out_drills(flat, fixture.registry, fixture.definitions), 0u);
    // Nothing registered under the drill table.
    const ContentRegistry none;
    T2D_CHECK_EQ(lay_out_drills(bare, none, fixture.definitions), 0u);
}


T2D_TEST(a_drill_turns_its_top_by_the_clock_and_the_speed_its_entry_gave_it) {
    Fixture content;
    // The entry says the top takes four seconds a turn, and that its picture is the second one the drill
    // has: the fixture's own add_drill() writes neither, so this one is written out here.
    const ContentId id = content.registry.register_content(ContentKind::Drill, "rig");
    {
        TileDefinition definition;
        definition.kind = ContentKind::Drill;
        definition.name = "rig";
        definition.dig_power = 2.0f;
        definition.top_image = "art/rig_top.png";
        definition.spin = 4.0f;
        content.definitions.add(std::move(definition));
    }
    const TileDefinition* found = content.definitions.find_tile(ContentKind::Drill, "rig");
    T2D_REQUIRE(found != nullptr);
    const std::unique_ptr<SceneTile> plot = make_tile(*found, id, 1, GridPos{0, 0});
    T2D_REQUIRE(plot != nullptr);
    const auto* drill = dynamic_cast<const Drill*>(plot.get());
    T2D_REQUIRE(drill != nullptr);

    // The speed is the definition's - one record every drill of the entry reads (section 1.35).
    T2D_CHECK_EQ(drill->spin_seconds(), 4.0f);
    constexpr f32 kTau = 6.283185307179586f;
    T2D_CHECK_EQ(drill->spin_angle(0.0f), 0.0f);
    T2D_CHECK_EQ(drill->spin_angle(1.0f), kTau * 0.25f);
    T2D_CHECK_EQ(drill->spin_angle(2.0f), kTau * 0.5f);
    T2D_CHECK_EQ(drill->spin_angle(3.0f), kTau * 0.75f);
    // A whole turn is back where it started, and it keeps going round rather than growing without bound.
    T2D_CHECK_EQ(drill->spin_angle(4.0f), 0.0f);
    T2D_CHECK_EQ(drill->spin_angle(5.0f), kTau * 0.25f);
    T2D_CHECK_EQ(drill->spin_angle(100.0f), 0.0f);
    // Negative time is not a turn backwards, it is nothing: the clock does not run that way.
    T2D_CHECK_EQ(drill->spin_angle(-1.0f), 0.0f);
}

T2D_TEST(a_drill_with_no_speed_in_its_entry_does_not_turn) {
    Fixture content;
    const ContentId id = content.add_drill("plain", 2.0f);   // no spin, and no top picture
    const TileDefinition* found = content.definitions.find_tile(ContentKind::Drill, "plain");
    T2D_REQUIRE(found != nullptr);
    const std::unique_ptr<SceneTile> plot = make_tile(*found, id, 1, GridPos{0, 0});
    T2D_REQUIRE(plot != nullptr);
    const auto* drill = dynamic_cast<const Drill*>(plot.get());
    T2D_REQUIRE(drill != nullptr);
    T2D_CHECK_EQ(drill->spin_seconds(), 0.0f);
    T2D_CHECK_EQ(drill->spin_angle(1.0f), 0.0f);
    T2D_CHECK_EQ(drill->spin_angle(1000.0f), 0.0f);
    // And a drill built without a definition (a test's own) has nothing to turn by either.
    Drill bare(ContentKind::Drill, id, 1, GridPos{0, 0});
    T2D_CHECK_EQ(bare.spin_seconds(), 0.0f);
    T2D_CHECK_EQ(bare.spin_angle(3.0f), 0.0f);
}

T2D_TEST_MAIN
