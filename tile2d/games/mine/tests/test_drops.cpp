// Drops: things that lie in a layer, are not updated by it, and can be dragged by the pointer - the
// class, the `drop::` definer that builds one out of the designer's data, and the drag itself.
//
// The content in here is a **test double**: names like "scrap" are the fixture's, not the game's - the
// game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/pointer_drag.h>
#include <mine/types/tile_definition.h>
#include <mine/types/warehouse.h>
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

/// A registry with the content a test's drops stand for.
struct Fixture {
    ContentRegistry registry;

    ContentId add(ContentKind kind, std::string_view name) { return registry.register_content(kind, name); }
};

/// The drop definitions a document declares, the way a load reads them.
[[nodiscard]] std::vector<DropDefinition> read_drops(std::string_view text,
                                                     std::vector<std::string>* errors = nullptr) {
    t2d::EcfgError parse_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text, &parse_error);
    if (!document.has_value()) return {};
    return drop_definitions(*document, errors);
}

} // namespace

T2D_TEST(a_drop_is_not_updated_by_the_layer_and_the_pointer_can_drag_it) {
    Fixture fixture;
    const ContentId scrap = fixture.add(ContentKind::Drop, "scrap");
    MineLayer layer(0, LayerShape{32, 24, 1}, 1);

    // A drop is an entity (types/entity.h) - a point in the layer, not a plot - and it is **not updated
    // by the layer**: that is what a drop *is*, not a field of its entry (docs/GAME_DESIGN.md 1.25).
    Drop drop(ContentKind::Drop, scrap, t2d::Vec2{4.5f, 6.25f});
    T2D_CHECK_FALSE(drop.auto_update());
    T2D_CHECK(drop.draggable());
    T2D_CHECK_EQ(drop.kind(), ContentKind::Drop);
    T2D_CHECK_EQ(drop.name(fixture.registry), std::string("scrap"));
    T2D_CHECK_FALSE(kind_is_a_tile(ContentKind::Drop));   // it is not something that occupies cells
    // A plain entity is not draggable: what the pointer may move is the kind of thing it is.
    Entity plain(ContentKind::Machine, fixture.add(ContentKind::Machine, "cart"), t2d::Vec2{});
    T2D_CHECK_FALSE(plain.draggable());

    Drop& in_layer =
        static_cast<Drop&>(layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{4.5f, 6.25f})));
    // The layer's quadtree finds it - a drop is an entity - and a pass over the layer runs nothing: what
    // the tick count reports is the things that took their turn, and a drop never does.
    T2D_CHECK_EQ(layer.entities_within(t2d::Vec2{4.5f, 6.25f}, 0.5f).size(), 1u);
    T2D_CHECK_EQ(layer.auto_updating_count(), 0u);
    T2D_CHECK_EQ(layer.tick(1.0f), 0u);
    T2D_CHECK_NEAR(in_layer.position().x, 4.5f, 1e-6f);
    T2D_CHECK_NEAR(in_layer.position().y, 6.25f, 1e-6f);
}

T2D_TEST(the_data_says_what_a_drop_is_and_the_definer_builds_it) {
    const std::vector<DropDefinition> drops = read_drops(
        "drop::\n    scrap::\n        image:\"art/scrap.png\"\n        weight:3\n    bolt::\n");
    T2D_REQUIRE(drops.size() == 2u);
    T2D_CHECK_EQ(drops[0].kind, ContentKind::Drop);
    T2D_CHECK_EQ(drops[0].name, std::string("scrap"));
    T2D_CHECK_EQ(drops[0].image, std::string("art/scrap.png"));
    // A field the engine does not read is the designer's: not read, and not an error either.
    T2D_CHECK_EQ(drops[1].name, std::string("bolt"));
    T2D_CHECK(drops[1].image.empty());

    // Another kind's table is that kind's business: only drop:: is read here.
    const std::vector<DropDefinition> mixed =
        read_drops("item::\n    stone::\n        image:\"art/x.png\"\ndrop::\n    scrap::\n");
    T2D_REQUIRE(mixed.size() == 1u);
    T2D_CHECK_EQ(mixed[0].name, std::string("scrap"));

    // A field the engine reads and cannot read is refused rather than guessed at, and the rest of the
    // file is still read - one bad entry does not lose the others.
    std::vector<std::string> errors;
    const std::vector<DropDefinition> broken = read_drops(
        "drop::\n    scrap::\n        image:7\n    bolt::\n        image:\"art/b.png\"\n", &errors);
    T2D_REQUIRE(errors.size() == 1u);
    T2D_CHECK(errors[0].find("image must be a quoted path") != std::string::npos);
    T2D_REQUIRE(broken.size() == 1u);
    T2D_CHECK_EQ(broken[0].name, std::string("bolt"));

    // An entry read out of another table is not a drop: the table an entry is written in is what it is
    // (docs/TYPES.md section 9).
    t2d::EcfgError parse_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse("ore::\n    stone::\n", &parse_error);
    T2D_REQUIRE(document.has_value());
    const t2d::EcfgValue* ore_table = document->find("ore");
    T2D_REQUIRE(ore_table != nullptr);
    T2D_REQUIRE(!ore_table->children().empty());
    std::string kind_error;
    T2D_CHECK_FALSE(read_drop_definition(ContentKind::Ore, ore_table->children()[0].key(),
                                         ore_table->children()[0], &kind_error)
                        .has_value());
    T2D_CHECK(kind_error.find("is not a drop") != std::string::npos);

    // And the definition becomes a drop: identity from the registry, the position it was dropped at, and
    // the two things a drop is.
    Fixture fixture;
    const ContentId scrap = fixture.add(ContentKind::Drop, "scrap");
    const std::unique_ptr<Drop> made = make_drop(drops[0], scrap, t2d::Vec2{2.5f, 3.5f});
    T2D_REQUIRE(made != nullptr);
    T2D_CHECK_EQ(made->kind(), ContentKind::Drop);
    T2D_CHECK_EQ(made->id(), scrap);
    T2D_CHECK_EQ(made->name(fixture.registry), std::string("scrap"));
    T2D_CHECK_NEAR(made->position().x, 2.5f, 1e-6f);
    T2D_CHECK_NEAR(made->position().y, 3.5f, 1e-6f);
    T2D_CHECK_FALSE(made->auto_update());
    T2D_CHECK(made->draggable());
}

T2D_TEST(the_pointer_picks_a_drop_up_and_puts_it_down_somewhere_else) {
    Fixture fixture;
    const ContentId scrap = fixture.add(ContentKind::Drop, "scrap");
    const ContentId bolt = fixture.add(ContentKind::Drop, "bolt");
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");
    MineLayer layer(0, LayerShape{40, 30, 1}, 1);

    Drop& drop =
        static_cast<Drop&>(layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{10.0f, 10.0f})));
    // Something that is not draggable, right next to it: the pointer must not pick that up.
    layer.adopt(std::make_unique<Entity>(ContentKind::Machine, cart, t2d::Vec2{10.25f, 10.0f}));

    PointerDrag drag;
    T2D_CHECK_FALSE(drag.holding());
    // Nothing draggable within the grab radius: the press does nothing at all.
    T2D_CHECK_FALSE(drag.begin(layer, t2d::Vec2{30.0f, 20.0f}));
    T2D_CHECK_FALSE(drag.holding());
    // The nearest draggable one is picked up, and where inside it the pointer took hold is remembered:
    // press a quarter of a cell to the right of its middle and it does not jump under the pointer.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{10.25f, 10.0f}));
    T2D_CHECK(drag.holding());
    T2D_CHECK(drag.held() == &drop);
    T2D_CHECK_NEAR(drag.grab_offset().x, -0.25f, 1e-6f);
    T2D_CHECK_NEAR(drag.grab_offset().y, 0.0f, 1e-6f);

    // Moving follows the pointer, keeping the offset - and the layer's index follows the move: the old
    // place stops answering and the new one starts (types/entity.h).
    T2D_CHECK(drag.move_to(t2d::Vec2{20.25f, 15.0f}));
    T2D_CHECK_NEAR(drop.position().x, 20.0f, 1e-6f);
    T2D_CHECK_NEAR(drop.position().y, 15.0f, 1e-6f);
    const std::vector<Entity*> left_behind = layer.entities_within(t2d::Vec2{10.0f, 10.0f}, 1.0f);
    T2D_REQUIRE(left_behind.size() == 1u);   // the machine that stood next to it is still there
    T2D_CHECK(left_behind[0] != &drop);
    T2D_CHECK_EQ(layer.entities_within(t2d::Vec2{20.0f, 15.0f}, 1.0f).size(), 1u);
    // A move to where it already is moves nothing.
    T2D_CHECK_FALSE(drag.move_to(t2d::Vec2{20.25f, 15.0f}));

    // Dragged past the edge it stays on the map: the pointer may leave it, what it holds may not.
    T2D_CHECK(drag.move_to(t2d::Vec2{-50.0f, -50.0f}));
    T2D_CHECK_NEAR(drop.position().x, 0.0f, 1e-6f);
    T2D_CHECK_NEAR(drop.position().y, 0.0f, 1e-6f);
    T2D_CHECK(drag.move_to(t2d::Vec2{100.0f, 100.0f}));
    T2D_CHECK_NEAR(drop.position().x, 40.0f, 1e-6f);
    T2D_CHECK_NEAR(drop.position().y, 30.0f, 1e-6f);

    // Letting go leaves it where it was put, and the pointer holds nothing afterwards.
    drag.end();
    T2D_CHECK_FALSE(drag.holding());
    T2D_CHECK_NEAR(drop.position().x, 40.0f, 1e-6f);
    T2D_CHECK_FALSE(drag.move_to(t2d::Vec2{1.0f, 1.0f}));

    // Two of them: the nearer one is picked up, and a press exactly between them takes the lower serial,
    // so the same press picks the same drop twice. The second one is **another item**: two of one item this
    // close would be one pile (section 1.43), and this test is about which drop a press picks.
    Drop& left = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{5.0f, 5.0f})));
    Drop& right = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, bolt, t2d::Vec2{5.5f, 5.0f})));
    T2D_CHECK(drag.begin(layer, t2d::Vec2{5.4f, 5.0f}));
    T2D_CHECK(drag.held() == &right);
    drag.end();
    T2D_CHECK(drag.begin(layer, t2d::Vec2{5.25f, 5.0f}));
    T2D_CHECK(drag.held() == &left);
    drag.end();

    // Something destroyed while it is held: the drag lets go rather than moving what is not there.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{5.0f, 5.0f}));
    T2D_CHECK(layer.destroy(left));
    T2D_CHECK_FALSE(drag.move_to(t2d::Vec2{6.0f, 5.0f}));
    T2D_CHECK_FALSE(drag.holding());
}

T2D_TEST(the_debug_fill_lays_out_every_registered_drop) {
    Fixture fixture;
    fixture.add(ContentKind::Drop, "scrap");
    fixture.add(ContentKind::Drop, "bolt");
    fixture.add(ContentKind::Item, "stone");   // not a drop: the fill lays out the drop table only
    MineLayer layer(0, LayerShape{40, 30, 1}, 1);
    // No definitions: the fill builds a plain drop when the load read no file for the content, which is
    // what a name a mod registered through the C ABI looks like (types/drop_definition.h).
    const ContentDefinitions definitions;

    T2D_CHECK_EQ(lay_out_drops(layer, fixture.registry, definitions), 2u);
    T2D_CHECK_EQ(layer.entity_count(), 2u);
    // Both are drops, both are inside the map, and both can be picked up - which is what the fill is for.
    for (const std::unique_ptr<Entity>& entity : layer.entities()) {
        T2D_CHECK(entity->draggable());
        T2D_CHECK_EQ(entity->kind(), ContentKind::Drop);
        T2D_CHECK(entity->position().x >= 0.0f && entity->position().x <= 40.0f);
        T2D_CHECK(entity->position().y >= 0.0f && entity->position().y <= 30.0f);
    }
    T2D_CHECK_NE(layer.entities()[0]->position().x, layer.entities()[1]->position().x);
    // Nothing registered under the drop table: the fill places nothing, because it never invents content.
    const ContentRegistry none;
    MineLayer empty(0, LayerShape{8, 8, 1}, 1);
    T2D_CHECK_EQ(lay_out_drops(empty, none, definitions), 0u);
    T2D_CHECK_EQ(empty.entity_count(), 0u);
}

T2D_TEST(nearby_drops_of_one_item_become_one_pile) {
    Fixture fixture;
    const ContentId scrap = fixture.add(ContentKind::Drop, "scrap");
    const ContentId bolt = fixture.add(ContentKind::Drop, "bolt");
    MineLayer layer(0, LayerShape{40, 30, 1}, 1);

    // Two of one item dropped close together are **one pile**: the second joins the first, which does not
    // move, and what drop() answers with is the drop that now carries both (docs/GAME_DESIGN.md 1.43).
    Drop& first = layer.drop(ContentKind::Drop, scrap, t2d::Vec2{10.0f, 10.0f});
    Drop& second = layer.drop(ContentKind::Drop, scrap, t2d::Vec2{10.5f, 10.0f});
    T2D_CHECK(&second == &first);
    T2D_CHECK_EQ(first.count(), 2u);
    T2D_CHECK_NEAR(first.position().x, 10.0f, 1e-6f);
    T2D_CHECK_NEAR(first.position().y, 10.0f, 1e-6f);
    T2D_CHECK_EQ(layer.entity_count(), 2u);   // the merged one is dead, and swept at the end of the tick
    (void)layer.tick(0.0f);
    T2D_CHECK_EQ(layer.entity_count(), 1u);

    // A pile keeps growing, and the count is what grows.
    Drop& third = layer.drop(ContentKind::Drop, scrap, t2d::Vec2{10.25f, 10.0f});
    T2D_CHECK(&third == &first);
    T2D_CHECK_EQ(first.count(), 3u);

    // Another item, or the same name in another kind, is another drop however close it lies.
    Drop& other_item = layer.drop(ContentKind::Drop, bolt, t2d::Vec2{10.1f, 10.0f});
    T2D_CHECK(&other_item != &first);
    T2D_CHECK_EQ(other_item.count(), 1u);
    Drop& other_kind = layer.drop(ContentKind::Item, scrap, t2d::Vec2{10.1f, 10.0f});
    T2D_CHECK(&other_kind != &first);
    T2D_CHECK_EQ(other_kind.count(), 1u);
    T2D_CHECK_EQ(first.count(), 3u);

    // Out of the radius is a drop of its own; exactly on it is near enough - the rule is "within".
    Drop& far = layer.drop(ContentKind::Drop, scrap,
                           t2d::Vec2{10.0f, 10.0f + MineLayer::kDropMergeRadius + 0.01f});
    T2D_CHECK(&far != &first);
    T2D_CHECK_EQ(far.count(), 1u);
    Drop& edge = layer.drop(ContentKind::Drop, scrap, t2d::Vec2{10.0f - MineLayer::kDropMergeRadius, 10.0f});
    T2D_CHECK(&edge == &first);
    T2D_CHECK_EQ(first.count(), 4u);

    // merge_drop() is that question on its own: the **nearest** pile is the one that grows, and the drop
    // that was merged in is dead afterwards.
    Drop& near_pile = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{35.0f, 5.0f}, 2)));
    Drop& far_pile = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{35.7f, 5.0f}, 1)));
    Drop& between = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{35.4f, 5.0f}, 1)));
    T2D_CHECK(layer.merge_drop(between) == &far_pile);   // 0.3 away, against 0.4 for the other
    T2D_CHECK_EQ(far_pile.count(), 2u);
    T2D_CHECK_FALSE(between.alive());
    T2D_CHECK_EQ(near_pile.count(), 2u);

    // A dead drop is no pile to merge into, even before the sweep takes it out of the storage.
    Drop& doomed = layer.drop(ContentKind::Drop, bolt, t2d::Vec2{30.0f, 20.0f});
    T2D_CHECK(layer.destroy(doomed));
    Drop& fresh = layer.drop(ContentKind::Drop, bolt, t2d::Vec2{30.2f, 20.0f});
    T2D_CHECK(&fresh != &doomed);
    T2D_CHECK_EQ(fresh.count(), 1u);
    T2D_CHECK(layer.merge_drop(doomed) == nullptr);   // it is not this layer's any more

    // Nothing near it: a drop alone on the ground has no pile to join.
    Drop& alone = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, bolt, t2d::Vec2{5.0f, 25.0f})));
    T2D_CHECK(layer.merge_drop(alone) == nullptr);

    // The count is never zero - a drop that carries nothing is not a drop - and adding nothing does not
    // grow a pile.
    Drop& counted = layer.drop(ContentKind::Drop, bolt, t2d::Vec2{20.0f, 25.0f});
    T2D_CHECK_EQ(counted.count(), 1u);
    counted.add(0);
    T2D_CHECK_EQ(counted.count(), 1u);
    counted.add(2);
    T2D_CHECK_EQ(counted.count(), 3u);
    counted.set_count(0);
    T2D_CHECK_EQ(counted.count(), 1u);
    counted.set_count(3);
    T2D_CHECK_EQ(counted.count(), 3u);
    const Drop plain(ContentKind::Drop, scrap, t2d::Vec2{});
    T2D_CHECK_EQ(plain.count(), 1u);
    const Drop below_one(ContentKind::Drop, scrap, t2d::Vec2{}, 0);
    T2D_CHECK_EQ(below_one.count(), 1u);   // a count below one is read as one
}

T2D_TEST(a_drop_let_go_near_a_pile_joins_it) {
    Fixture fixture;
    const ContentId scrap = fixture.add(ContentKind::Drop, "scrap");
    const ContentId bolt = fixture.add(ContentKind::Drop, "bolt");
    MineLayer layer(0, LayerShape{40, 30, 1}, 1);

    Drop& pile = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{10.0f, 10.0f}, 2)));
    Drop& lone = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{20.0f, 10.0f})));
    Drop& other = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, bolt, t2d::Vec2{10.2f, 10.0f})));

    // A single drop is picked up itself - there is no pile to take one out of - and let go next to the
    // pile it joins it: that is how a pile grows (docs/GAME_DESIGN.md section 1.43).
    PointerDrag drag;
    T2D_CHECK(drag.begin(layer, t2d::Vec2{20.0f, 10.0f}));
    T2D_CHECK(drag.held() == &lone);
    T2D_CHECK(drag.move_to(t2d::Vec2{10.5f, 10.0f}));
    T2D_CHECK_NEAR(lone.position().x, 10.5f, 1e-6f);
    T2D_CHECK(drag.end() == ReleaseResult::Left);
    T2D_CHECK_EQ(pile.count(), 3u);
    T2D_CHECK_FALSE(lone.alive());
    (void)layer.tick(0.0f);
    T2D_CHECK_EQ(layer.entity_count(), 2u);   // the pile and the other item

    // One of **another** item, let go on the same spot, does not join it: a pile is of one item.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{10.2f, 10.0f}));
    T2D_CHECK(drag.held() == &other);
    T2D_CHECK(drag.end() == ReleaseResult::Left);   // let go where it lies, next to the pile
    T2D_CHECK(other.alive());
    T2D_CHECK_EQ(pile.count(), 3u);
    T2D_CHECK_EQ(other.count(), 1u);
}

T2D_TEST(dragging_from_a_pile_takes_one_item) {
    Fixture fixture;
    const ContentId scrap = fixture.add(ContentKind::Drop, "scrap");
    MineLayer layer(0, LayerShape{40, 30, 1}, 1);

    // A pile of three, as three throws of one item that landed close together would have made it.
    auto& pile = static_cast<Drop&>(
        layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{10.0f, 10.0f}, 3)));
    T2D_CHECK_EQ(pile.count(), 3u);
    const auto items_on_the_ground = [&] {
        usize total = 0;
        for (const std::unique_ptr<Entity>& entity : layer.entities()) {
            if (entity->alive()) total += static_cast<const Drop&>(*entity).count();
        }
        return total;
    };
    T2D_CHECK_EQ(items_on_the_ground(), 3u);

    // Picking it up takes **one item** out of it: the pointer holds a drop of its own, and the pile stays
    // where it is with one fewer (docs/GAME_DESIGN.md section 1.43).
    PointerDrag drag;
    T2D_CHECK(drag.begin(layer, t2d::Vec2{10.0f, 10.0f}));
    T2D_REQUIRE(drag.held() != nullptr);
    T2D_CHECK(drag.held() != &pile);
    T2D_CHECK_EQ(static_cast<Drop&>(*drag.held()).count(), 1u);
    T2D_CHECK_EQ(pile.count(), 2u);
    T2D_CHECK_NEAR(drag.held()->position().x, 10.0f, 1e-6f);

    // Let go where it was picked up: it joins the pile again, and the pile is three again.
    T2D_CHECK(drag.end() == ReleaseResult::Left);
    T2D_CHECK_EQ(pile.count(), 3u);
    T2D_CHECK(pile.alive());
    (void)layer.tick(0.0f);
    T2D_CHECK_EQ(layer.entity_count(), 1u);   // the pile, and nothing else: the taken drop went back in

    // Carried away and let go on the open ground: the pile keeps two and the item that was carried lies on
    // its own - an item is never lost, only moved.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{10.0f, 10.0f}));
    T2D_CHECK_EQ(pile.count(), 2u);
    T2D_CHECK(drag.move_to(t2d::Vec2{14.0f, 14.0f}));
    T2D_CHECK(drag.end() == ReleaseResult::Left);
    T2D_CHECK_EQ(pile.count(), 2u);
    T2D_CHECK_EQ(items_on_the_ground(), 3u);   // two in the pile and one where it was let go

    // A warehouse beside it: dragging from the pile into the warehouse stores **one** item, not the pile.
    (void)fixture.add(ContentKind::Warehouse, "bin");
    ContentDefinitions definitions;
    TileDefinition definition;
    definition.kind = ContentKind::Warehouse;
    definition.name = "bin";
    definition.capacity = 8;
    definitions.add(std::move(definition));
    PlacedContent where;
    where.kind = ContentKind::Warehouse;
    where.name = "bin";
    where.layer = 0;
    where.anchor = GridPos{14, 14};
    LayerBuildReport report;
    auto* warehouse = dynamic_cast<Warehouse*>(layer.place(fixture.registry, definitions, where, &report));
    T2D_REQUIRE(warehouse != nullptr);
    T2D_CHECK(drag.begin(layer, t2d::Vec2{10.0f, 10.0f}));
    T2D_CHECK_EQ(pile.count(), 1u);
    T2D_CHECK(drag.move_to(t2d::Vec2{14.5f, 14.5f}));
    T2D_CHECK(drag.end() == ReleaseResult::Stored);
    T2D_CHECK_EQ(warehouse->container()->total(), 1u);
    T2D_CHECK_EQ(pile.count(), 1u);
    T2D_CHECK_EQ(items_on_the_ground(), 2u);   // one still in the pile, one in the box

    // The pile is down to one: what is left **is** the drop, and dragging it takes the drop itself.
    T2D_CHECK(drag.begin(layer, t2d::Vec2{10.0f, 10.0f}));
    T2D_CHECK(drag.held() == &pile);
    T2D_CHECK(drag.move_to(t2d::Vec2{20.0f, 20.0f}));
    T2D_CHECK(drag.end() == ReleaseResult::Left);
    T2D_CHECK(pile.alive());
    T2D_CHECK_NEAR(pile.position().x, 20.0f, 1e-6f);
    T2D_CHECK_EQ(pile.count(), 1u);
    T2D_CHECK_EQ(items_on_the_ground(), 2u);
}

T2D_TEST_MAIN
