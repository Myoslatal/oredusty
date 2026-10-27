// Drills: the machine that mines the ore it stands on - the class (an entity plot), the two numbers the
// period comes out of (the drill's digging power and the ore's hardness), and the drop it throws out.
//
// The content in here is a **test double**: names like "vein" or "rig" are the fixture's, not the game's -
// the game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/build_animation.h>
#include <mine/types/tile_definition.h>
#include <mine/world.h>

#include <support/test_support.h>

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

    /// What an ore drops is an **item** (docs/GAME_DESIGN.md sections 1.23, 1.25): the thing that lands on
    /// the ground is made out of it, so this registers the item and nothing else.
    ContentId add_item(std::string_view name) {
        const ContentId id = registry.register_content(ContentKind::Item, name);
        TileDefinition definition;
        definition.kind = ContentKind::Item;
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
        chunk = fixture.add_item("chunk");
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
    // The thing on the ground **is** the item its ore drops: same id, same name, same picture
    // (sections 1.23, 1.25) - which is why carrying it into a warehouse stores the item.
    T2D_CHECK_EQ(first.kind(), ContentKind::Item);
    T2D_CHECK_EQ(first.id(), rig.chunk);
    T2D_CHECK(first.draggable());   // what it throws out is a drop: the pointer can drag it (types/drop.h)
    // It lands **one cell away at a random angle**: anywhere on the circle around the drill's middle, not
    // on one of four sides (the designer asked for a random direction, and then for 360 degrees of it).
    const auto is_on_the_ring = [&](const t2d::Vec2& at) {
        const f32 dx = at.x - 5.5f;
        const f32 dy = at.y - 5.5f;
        return std::abs(std::sqrt(dx * dx + dy * dy) - 1.0f) < 1e-4f;
    };
    T2D_CHECK_MSG(is_on_the_ring(first.position()), "the first drop landed at {},{}",
                  first.position().x, first.position().y);
    // And the layer's quadtree finds it where it landed.
    T2D_CHECK_EQ(rig.layer.entities_within(first.position(), 0.25f).size(), 1u);

    // More turns, and the drops go **every which way**: with an angle that is a real number, a handful of
    // throws land on a handful of different angles - which is what 360 degrees of randomness looks like
    // from the outside, and what four sides could never do. The stream is the drill's own and seeded from
    // what it is and where it stands, so this is the same sequence every run.
    for (i32 turn = 0; turn < 15; ++turn) (void)rig.layer.tick(2.0f);
    const usize thrown = rig.drill->produced();
    std::vector<f32> angles;
    usize items = 0;
    usize piles = 0;
    for (const std::unique_ptr<Entity>& entity : rig.layer.entities()) {
        const auto* drop = dynamic_cast<const Drop*>(entity.get());
        T2D_REQUIRE(drop != nullptr);
        items += drop->count();
        if (drop->count() > 1) ++piles;
        const t2d::Vec2 at = drop->position();
        T2D_CHECK(is_on_the_ring(at));
        const f32 angle = std::atan2(at.y - 5.5f, at.x - 5.5f);
        bool seen = false;
        for (const f32 other : angles) seen = seen || std::abs(other - angle) < 1e-4f;
        if (!seen) angles.push_back(angle);
    }
    // Drops of one item that landed close together became one **pile** (section 1.43): what was mined is
    // all there (the counts add up), and there are fewer drops on the ground than throws.
    T2D_CHECK_EQ(items, thrown);
    T2D_CHECK_GT(piles, 0u);
    T2D_CHECK_LT(rig.layer.entity_count(), thrown);
    // Sixteen throws that only ever used four directions would give at most four angles; a random angle
    // gives more, and every pile still lies on the ring where its first item landed.
    T2D_CHECK_MSG(angles.size() >= 5u, "the {} throws only took {} different directions", thrown,
                  angles.size());

    // A frame that swallowed three periods is worth three drops: a hitch must not change what the mine
    // produces (types/entity_tile.h), and the cadence hands over the whole span for exactly this. Items are
    // counted again, because what the three throw may land on is a pile.
    const usize items_before = items;
    T2D_CHECK_EQ(rig.layer.tick(6.0f), 1u);   // the drill takes its turn once, with six seconds in hand
    usize items_after = 0;
    for (const std::unique_ptr<Entity>& entity : rig.layer.entities()) {
        items_after += static_cast<const Drop&>(*entity).count();
    }
    T2D_CHECK_EQ(items_after, items_before + 3u);
    T2D_CHECK_EQ(rig.drill->produced(), thrown + 3u);
}

T2D_TEST(the_same_drill_throws_the_same_way_every_time_it_is_built) {
    // The dice are the drill's own, seeded from what it is and where it stands: two layers built from the
    // same data throw alike, so a map is reproducible even though the direction is random (section 1.21).
    // What a throw leaves behind is what is compared: drops of one item that landed close together became
    // one pile, and where those piles lie is as reproducible as where the throws went (section 1.43).
    const auto throws_of = [] {
        Rig rig(2.0f, 1.0f, "chunk");
        std::vector<std::pair<t2d::Vec2, usize>> left;
        for (i32 turn = 0; turn < 6; ++turn) (void)rig.layer.tick(2.0f);
        for (const std::unique_ptr<Entity>& entity : rig.layer.entities()) {
            const auto* drop = dynamic_cast<const Drop*>(entity.get());
            if (drop == nullptr) continue;
            left.push_back({drop->position(), drop->count()});
        }
        return left;
    };
    const auto first = throws_of();
    const auto second = throws_of();
    T2D_CHECK_GT(first.size(), 0u);
    T2D_REQUIRE(second.size() == first.size());
    for (usize index = 0; index < first.size(); ++index) {
        T2D_CHECK_NEAR(first[index].first.x, second[index].first.x, 1e-5f);
        T2D_CHECK_NEAR(first[index].first.y, second[index].first.y, 1e-5f);
        T2D_CHECK_EQ(first[index].second, second[index].second);   // and a pile is as big the second time
    }
    // And they are not all the same direction, or the seed would be doing nothing at all.
    bool differs = false;
    for (const std::pair<t2d::Vec2, usize>& at : first) {
        differs = differs || at.first.x != first.front().first.x || at.first.y != first.front().first.y;
    }
    T2D_CHECK(differs);
}

T2D_TEST(a_drill_with_nothing_to_mine_is_reported_and_idle) {
    // A drill with no ore under it: the layer says so while it is being built, and the drill produces
    // nothing - rather than a machine that quietly does nothing forever (world.h).
    {
        Fixture fixture;
        fixture.add_floor("dirt");
        fixture.add_ore("vein", 2.0f, "chunk");
        fixture.add_drill("rig", 1.0f);
        fixture.add_item("chunk");
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
        if (power > 0.0f) fixture.add_item("chunk");
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
    T2D_CHECK(refuses(2.0f, "nothing_like_that", 1.0f, "no item named 'nothing_like_that' is registered"));
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
    fixture.add_item("chunk");
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

T2D_TEST(a_drills_preview_is_its_base_and_the_turning_top_over_it) {
    const std::optional<TileDefinition> rig = read_one(R"(
drill::
    rig::
        image:"art/rig_base.png"
        top_image:"art/rig_top.png"
        dig_power:2
        spin:4
        spin_origin::
            x:0.25
            y:0.75
)",
                                                      ContentKind::Drill);
    T2D_REQUIRE(rig.has_value());
    const std::unique_ptr<SceneTile> plot = make_tile(*rig, 1, 1, GridPos{0, 0});
    T2D_REQUIRE(plot != nullptr);
    const auto* drill = dynamic_cast<const Drill*>(plot.get());
    T2D_REQUIRE(drill != nullptr);

    // Two pictures, drawn bottom up: the base, then the top that turns over it (section 1.38). The base is
    // the entry's own "image" and so has no part name; the top is the part called "top".
    const TilePreview at_rest = drill->get_preview();
    T2D_REQUIRE(at_rest.images.size() == 2u);
    T2D_CHECK(at_rest.images[0].part.empty());
    T2D_CHECK_EQ(at_rest.images[1].part, std::string("top"));
    T2D_CHECK_EQ(at_rest.images[1].radians, 0.0f);

    // The top is where the clock has turned it to, about the point of its own picture the entry named -
    // and neither is state the drill carries: the same query at another moment answers that moment.
    constexpr f32 kTau = 6.283185307179586f;
    const TilePreview turned = drill->get_preview(1.0f);
    T2D_REQUIRE(turned.images.size() == 2u);
    T2D_CHECK_EQ(turned.images[1].radians, drill->spin_angle(1.0f));
    T2D_CHECK_EQ(turned.images[1].radians, kTau * 0.25f);
    const t2d::Vec2 origin{0.25f, 0.75f};
    T2D_CHECK_EQ(turned.images[1].pivot, origin);
    T2D_CHECK_EQ(at_rest.images[1].pivot, origin);
    T2D_CHECK_EQ(turned.images[0].radians, 0.0f);   // the base does not move

    // A plot draws mirrored when it is mirrored - both pictures alike: the top turns on its base and is not
    // turned about by the mirror separately.
    plot->set_mirrored(true);
    const TilePreview mirrored = drill->get_preview(2.0f);
    T2D_REQUIRE(mirrored.images.size() == 2u);
    T2D_CHECK(mirrored.images[0].mirrored);
    T2D_CHECK(mirrored.images[1].mirrored);
    T2D_CHECK_EQ(mirrored.images[1].radians, drill->spin_angle(2.0f));

    // One picture when the entry names no top, and nothing at all when it names no base: a top with no base
    // under it is not a picture the drill draws - the world view draws the fallback colour there.
    const std::optional<TileDefinition> plain = read_one(R"(
drill::
    plain::
        image:"art/rig_base.png"
        dig_power:2
)",
                                                         ContentKind::Drill);
    T2D_REQUIRE(plain.has_value());
    const std::unique_ptr<SceneTile> plain_plot = make_tile(*plain, 2, 1, GridPos{1, 1});
    T2D_REQUIRE(plain_plot != nullptr);
    T2D_REQUIRE(dynamic_cast<const Drill*>(plain_plot.get()) != nullptr);
    T2D_CHECK_EQ(plain_plot->get_preview(3.0f).images.size(), 1u);
    T2D_CHECK(plain_plot->get_preview(3.0f).images[0].part.empty());

    const std::optional<TileDefinition> top_only = read_one(R"(
drill::
    head::
        top_image:"art/rig_top.png"
        dig_power:2
        spin:4
)",
                                                             ContentKind::Drill);
    T2D_REQUIRE(top_only.has_value());
    const std::unique_ptr<SceneTile> head_plot = make_tile(*top_only, 3, 1, GridPos{2, 2});
    T2D_REQUIRE(head_plot != nullptr);
    T2D_CHECK(head_plot->get_preview(1.0f).empty());

    // And a drill built without a definition at all - a test's own - has no art to answer with.
    Drill bare(ContentKind::Drill, 4, 1, GridPos{3, 3});
    T2D_CHECK(bare.get_preview().empty());
}

T2D_TEST(a_drill_that_is_still_going_up_does_not_mine) {
    Rig rig(2.0f, 1.0f, "chunk");
    T2D_REQUIRE(rig.drill != nullptr);

    // A drill being built is on the map, but it does not run: a machine that mined while it was still
    // going up would produce out of nothing (docs/GAME_DESIGN.md section 1.37).
    BuildAnimation sweep;
    sweep.start(rig.drill, 5.0f);
    T2D_CHECK(rig.drill->under_construction());
    T2D_CHECK_EQ(rig.layer.tick(10.0f), 0u);   // it does not even take its turn ...
    T2D_CHECK_EQ(rig.drill->produced(), 0u);   // ... and throws nothing out
    T2D_CHECK_EQ(rig.layer.entity_count(), 0u);

    // Done: the five seconds it spent going up are not owed to it, so its first turn is a whole period
    // (2 s: hardness 2 / power 1) after it is finished, not at once.
    sweep.advance(5.0f);
    T2D_CHECK_FALSE(rig.drill->under_construction());
    T2D_CHECK_EQ(rig.layer.tick(1.0f), 0u);
    T2D_CHECK_EQ(rig.drill->produced(), 0u);
    T2D_CHECK_EQ(rig.layer.tick(1.0f), 1u);
    T2D_CHECK_EQ(rig.drill->produced(), 1u);
    T2D_CHECK_EQ(rig.layer.entity_count(), 1u);
}

T2D_TEST(a_drill_piles_up_what_it_throws) {
    Rig rig(2.0f, 1.0f, "chunk");
    T2D_REQUIRE(rig.drill != nullptr);

    // A drill throws its drops into the ring around it, and one item landing near another of its item is
    // one **pile** (types/drop.h, docs/GAME_DESIGN.md section 1.43). Twenty turns is enough for the ring
    // to fill up and for throws to land on top of piles.
    for (i32 turn = 0; turn < 20; ++turn) (void)rig.layer.tick(2.0f);
    const usize thrown = rig.drill->produced();
    T2D_CHECK_EQ(thrown, 20u);

    usize items = 0;
    usize piles = 0;
    usize ground = 0;
    for (const std::unique_ptr<Entity>& entity : rig.layer.entities()) {
        const auto* drop = dynamic_cast<const Drop*>(entity.get());
        T2D_REQUIRE(drop != nullptr);
        // A pile is a drop in everything else: what it is, and how it may be dragged, is unchanged.
        T2D_CHECK_EQ(drop->kind(), ContentKind::Item);
        T2D_CHECK(drop->draggable());
        ++ground;
        items += drop->count();
        if (drop->count() > 1) ++piles;
    }
    // Nothing was lost to the merging and nothing was made up: the counts hold every item that was
    // thrown, the piles are real, and there are fewer drops on the ground than throws.
    T2D_CHECK_EQ(items, thrown);
    T2D_CHECK_GT(piles, 0u);
    T2D_CHECK_LT(ground, thrown);
}

T2D_TEST_MAIN
