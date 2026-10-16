// Drops: things that lie in a layer, are not updated by it, and can be dragged by the pointer - the
// class, the `drop::` definer that builds one out of the designer's data, and the drag itself.
//
// The content in here is a **test double**: names like "scrap" are the fixture's, not the game's - the
// game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/pointer_drag.h>
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
    // so the same press picks the same drop twice.
    Drop& left =
        static_cast<Drop&>(layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{5.0f, 5.0f})));
    Drop& right =
        static_cast<Drop&>(layer.adopt(std::make_unique<Drop>(ContentKind::Drop, scrap, t2d::Vec2{5.5f, 5.0f})));
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

    T2D_CHECK_EQ(lay_out_drops(layer, fixture.registry), 2u);
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
    T2D_CHECK_EQ(lay_out_drops(empty, none), 0u);
    T2D_CHECK_EQ(empty.entity_count(), 0u);
}

T2D_TEST_MAIN
