// Entities: what a layer holds that is not on a cell - a free position, movement, the cadence it runs
// on, the interaction hook - and the quadtree a layer indexes them with, so that "what is near this
// point" costs what the answer is rather than what the layer holds.
//
// The content in here is a **test double**: names like "cart" are the fixture's, not the game's - the
// game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/quadtree.h>
#include <mine/world.h>

#include <support/test_support.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// A registry with the content a test's entities stand for.
struct Fixture {
    ContentRegistry registry;

    ContentId add(ContentKind kind, std::string_view name) { return registry.register_content(kind, name); }
};

/// An entity that counts its turns and remembers what it was handed: the cadence, made visible.
struct Counter : Entity {
    using Entity::Entity;

    int runs = 0;
    f32 last_span = 0.0f;

    void on_update(f32 elapsed_seconds) override {
        ++runs;
        last_span = elapsed_seconds;
    }
};

/// An entity that answers an interaction and remembers who asked.
struct Talker : Entity {
    using Entity::Entity;

    bool reacts = false;
    Entity* saw = nullptr;

    [[nodiscard]] bool on_interact(Entity& other) override {
        saw = &other;
        return reacts;
    }
};

/// An entity that writes down that it ran, so the order of a pass is visible.
struct Recorder : Entity {
    using Entity::Entity;

    std::vector<u32>* order = nullptr;

    void on_update(f32) override {
        if (order != nullptr) order->push_back(serial());
    }
};

/// An entity that does something to the layer it is in while its turn runs: the reason an entity knows
/// its layer (types/entity.h).
struct Busy : Entity {
    using Entity::Entity;

    ContentId spawn_what = kNoContent;
    Entity* victim = nullptr;
    int spawned = 0;
    int destroyed = 0;
    int met = 0;

    void on_update(f32) override {
        MineLayer* world = layer();
        if (world == nullptr) return;
        if (spawn_what != kNoContent) {
            spawned += world->spawn(ContentKind::Machine, spawn_what, position()) != nullptr ? 1 : 0;
        }
        if (victim != nullptr) destroyed += world->destroy(*victim) ? 1 : 0;
        for (Entity* other : world->entities_within(position(), 1.0f)) {
            if (other == this) continue;
            met += world->interact(*this, *other) ? 1 : 0;
        }
    }

    [[nodiscard]] bool on_interact(Entity& other) override { return &other != this; }
};

/// The labels a radius query finds in \p tree, sorted: the tree visits in its own order.
[[nodiscard]] std::vector<int> labels_within(const QuadTree<int>& tree, t2d::Vec2 center, f32 radius) {
    std::vector<int> found;
    tree.for_each_within(center, radius, [&](const QuadTree<int>::Entry& entry) {
        found.push_back(entry.payload);
    });
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace

T2D_TEST(an_entity_has_a_position_and_it_is_not_a_cell) {
    // An entity is not a plot: a plot *is* cells (types/tile.h), and the two are not related by
    // inheritance. What they share is identity - the kind and the id a save stores.
    static_assert(!std::is_base_of_v<Tile, Entity>, "an entity is not a plot");

    Fixture fixture;
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");

    Entity entity(ContentKind::Machine, cart, t2d::Vec2{3.5f, 2.25f});
    T2D_CHECK_EQ(entity.kind(), ContentKind::Machine);
    T2D_CHECK_EQ(entity.id(), cart);
    T2D_CHECK_EQ(entity.name(fixture.registry), std::string("cart"));
    T2D_CHECK(entity.valid());
    T2D_CHECK_FALSE(entity.missing());
    T2D_CHECK(entity.alive());
    T2D_CHECK_EQ(entity.serial(), 0u);          // it is in no layer yet
    T2D_CHECK(entity.layer() == nullptr);

    // **Not aligned to the tile grid**: nothing rounds a position, and half a cell is a place like any
    // other (docs/GAME_DESIGN.md section 1.24).
    T2D_CHECK_NEAR(entity.position().x, 3.5f, 1e-6f);
    T2D_CHECK_NEAR(entity.position().y, 2.25f, 1e-6f);
    entity.move(t2d::Vec2{0.25f, -0.5f});
    T2D_CHECK_NEAR(entity.position().x, 3.75f, 1e-6f);
    T2D_CHECK_NEAR(entity.position().y, 1.75f, 1e-6f);
    // Off the map is still a position: what the map does about it is the game's rules (section 2), and
    // nothing here refuses one.
    entity.set_position(t2d::Vec2{-2.5f, 100.125f});
    T2D_CHECK_NEAR(entity.position().x, -2.5f, 1e-6f);
    T2D_CHECK_NEAR(entity.position().y, 100.125f, 1e-6f);

    // Content that goes away leaves the entity with its id and a name that says so, the way a plot
    // keeps its id (types/tile.h).
    const ContentRegistry emptied;
    T2D_CHECK_EQ(entity.name(emptied), std::format("#{}", cart));
    entity.set_missing(true);
    T2D_CHECK_FALSE(entity.valid());
    T2D_CHECK(entity.missing());
    T2D_CHECK_EQ(entity.id(), cart);
}

T2D_TEST(an_entity_runs_every_tick_unless_it_says_otherwise) {
    Fixture fixture;
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");

    // Every tick by default: what the layer hands it is what it runs on.
    Counter every_tick(ContentKind::Machine, cart, t2d::Vec2{});
    T2D_CHECK(every_tick.auto_update());
    T2D_CHECK_NEAR(every_tick.period_seconds(), 0.0f, 1e-6f);
    T2D_CHECK(every_tick.advance(0.016f));
    T2D_CHECK(every_tick.advance(0.016f));
    T2D_CHECK_EQ(every_tick.runs, 2);
    T2D_CHECK_NEAR(every_tick.last_span, 0.016f, 1e-6f);

    // A period it declares: it runs once that much time has piled up, and it is handed the whole span.
    Counter slow(ContentKind::Machine, cart, t2d::Vec2{});
    slow.set_period_seconds(1.0f);
    T2D_CHECK_FALSE(slow.advance(0.4f));
    T2D_CHECK_FALSE(slow.advance(0.4f));
    T2D_CHECK_NEAR(slow.since_last_update(), 0.8f, 1e-5f);
    T2D_CHECK(slow.advance(0.4f));
    T2D_CHECK_EQ(slow.runs, 1);
    T2D_CHECK_NEAR(slow.last_span, 1.2f, 1e-5f);
    // What was over stays in the accumulator, so a period does not drift by the fractions of a frame.
    T2D_CHECK_NEAR(slow.since_last_update(), 0.2f, 1e-5f);
    T2D_CHECK_FALSE(slow.advance(0.7f));
    T2D_CHECK(slow.advance(0.1f));
    T2D_CHECK_EQ(slow.runs, 2);

    // A frame longer than the period runs it **once**, with the whole span in its hand: it cannot
    // replay the ticks it missed, and pretending otherwise would make a hitch change what the mine
    // produces (types/update_cadence.h).
    Counter long_frame(ContentKind::Machine, cart, t2d::Vec2{});
    long_frame.set_period_seconds(2.0f);
    T2D_CHECK(long_frame.advance(5.0f));
    T2D_CHECK_EQ(long_frame.runs, 1);
    T2D_CHECK_NEAR(long_frame.last_span, 5.0f, 1e-5f);
    T2D_CHECK_NEAR(long_frame.since_last_update(), 1.0f, 1e-5f);

    // A period of zero or less is "every tick", which is what a period below zero would mean anyway.
    Counter zero(ContentKind::Machine, cart, t2d::Vec2{});
    zero.set_period_seconds(-3.0f);
    T2D_CHECK_NEAR(zero.period_seconds(), 0.0f, 1e-6f);
    T2D_CHECK(zero.advance(0.016f));

    // And an entity that says "the layer does not update me" is never run - it is still there, still
    // moves and still interacts; what drives it is whoever holds it.
    Counter manual(ContentKind::Machine, cart, t2d::Vec2{});
    manual.set_auto_update(false);
    T2D_CHECK_FALSE(manual.auto_update());
    for (int tick = 0; tick < 100; ++tick) (void)manual.advance(0.1f);
    T2D_CHECK_EQ(manual.runs, 0);
    T2D_CHECK_NEAR(manual.since_last_update(), 0.0f, 1e-6f);
    manual.move(t2d::Vec2{1.0f, 1.0f});
    T2D_CHECK_NEAR(manual.position().x, 1.0f, 1e-6f);
}

T2D_TEST(an_interaction_is_asked_of_both_sides) {
    Fixture fixture;
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");
    MineLayer layer(0, LayerShape{32, 32, 1}, 1);

    // The default: an entity that does not react says so, and nothing happens.
    Entity plain(ContentKind::Machine, cart, t2d::Vec2{});
    T2D_CHECK_FALSE(plain.on_interact(plain));

    Talker& a = static_cast<Talker&>(
        layer.adopt(std::make_unique<Talker>(ContentKind::Machine, cart, t2d::Vec2{4.0f, 4.0f})));
    Talker& b = static_cast<Talker&>(
        layer.adopt(std::make_unique<Talker>(ContentKind::Machine, cart, t2d::Vec2{4.5f, 4.0f})));

    // Both are asked and each is handed the other: an interaction has two sides, and neither side has
    // to know what the other one is (types/entity.h).
    T2D_CHECK_FALSE(layer.interact(a, b));
    T2D_CHECK(a.saw == &b);
    T2D_CHECK(b.saw == &a);

    // Whoever reacts is what makes it an interaction that happened.
    a.reacts = true;
    T2D_CHECK(layer.interact(a, b));
    a.reacts = false;
    b.reacts = true;
    T2D_CHECK(layer.interact(a, b));

    // A dead entity is not asked anything.
    T2D_CHECK(layer.destroy(b));
    T2D_CHECK_FALSE(layer.interact(a, b));
}

T2D_TEST(a_quadtree_answers_what_is_near_a_point) {
    const t2d::Aabb2 bounds = t2d::Aabb2::from_top_left(t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{100.0f, 100.0f});
    // A small node capacity, so that a handful of points already makes the tree split.
    QuadTree<int> tree(bounds, 4, 6);

    T2D_CHECK(tree.empty());
    T2D_CHECK_EQ(tree.size(), 0u);
    T2D_CHECK_EQ(tree.node_count(), 1u);
    T2D_CHECK(tree.bounds() == bounds);

    tree.insert(1, t2d::Vec2{10.0f, 10.0f}, 100);
    tree.insert(2, t2d::Vec2{10.0f, 12.0f}, 200);
    tree.insert(3, t2d::Vec2{60.0f, 60.0f}, 300);
    tree.insert(4, t2d::Vec2{10.0f, 10.0f}, 400);   // the same place, another key: two entries
    tree.insert(5, t2d::Vec2{-5.0f, 120.0f}, 500);  // outside the root: kept, not refused
    T2D_CHECK_EQ(tree.size(), 5u);

    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{10.0f, 10.0f}, 3.0f), (std::vector<int>{100, 200, 400}));
    // A query is a closed disc: what sits exactly on the radius is inside it.
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{10.0f, 10.0f}, 2.0f), (std::vector<int>{100, 200, 400}));
    // Radius zero (and anything below it) finds what is exactly there - both of them.
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{10.0f, 10.0f}, 0.0f), (std::vector<int>{100, 400}));
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{10.0f, 10.0f}, -1.0f), (std::vector<int>{100, 400}));
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{60.0f, 60.0f}, 1.0f), (std::vector<int>{300}));
    T2D_CHECK(labels_within(tree, t2d::Vec2{40.0f, 40.0f}, 5.0f).empty());
    // What is outside the map is found where it actually is: the index does not move it.
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{-5.0f, 120.0f}, 0.5f), (std::vector<int>{500}));

    // An area query is the same walk without the distance test.
    std::vector<int> in_area;
    tree.for_each_in(t2d::Aabb2::from_top_left(t2d::Vec2{9.0f, 9.0f}, t2d::Vec2{4.0f, 4.0f}),
                     [&](const QuadTree<int>::Entry& entry) { in_area.push_back(entry.payload); });
    std::sort(in_area.begin(), in_area.end());
    T2D_CHECK_EQ(in_area, (std::vector<int>{100, 200, 400}));

    // Moving is remove plus insert: the old place stops answering and the new one starts.
    T2D_CHECK(tree.move(3, t2d::Vec2{60.0f, 60.0f}, t2d::Vec2{11.0f, 11.0f}));
    T2D_CHECK(labels_within(tree, t2d::Vec2{60.0f, 60.0f}, 1.0f).empty());
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{11.0f, 11.0f}, 0.5f), (std::vector<int>{300}));
    T2D_CHECK_FALSE(tree.move(3, t2d::Vec2{60.0f, 60.0f}, t2d::Vec2{0.0f, 0.0f}));

    T2D_CHECK(tree.remove(2, t2d::Vec2{10.0f, 12.0f}));
    T2D_CHECK_FALSE(tree.remove(2, t2d::Vec2{10.0f, 12.0f}));
    T2D_CHECK_EQ(tree.size(), 4u);
    // 300 moved to 11,11 a moment ago, so it is near 10,10 as well.
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{10.0f, 10.0f}, 3.0f), (std::vector<int>{100, 300, 400}));

    // The tree splits while a node holds more than it should, and every entry is still found.
    for (u32 key = 100; key < 200; ++key) {
        tree.insert(key, t2d::Vec2{static_cast<f32>(key % 100), static_cast<f32>((key * 7) % 100)},
                    static_cast<int>(key));
    }
    T2D_CHECK_GT(tree.node_count(), 1u);
    T2D_CHECK_EQ(tree.size(), 104u);
    // A radius of 100 from the middle of the map reaches every entry there is - including the one that
    // sits outside the map, which the root keeps.
    T2D_CHECK_EQ(labels_within(tree, t2d::Vec2{50.0f, 50.0f}, 100.0f).size(), tree.size());

    tree.clear();
    T2D_CHECK(tree.empty());
    T2D_CHECK_EQ(tree.node_count(), 1u);
    T2D_CHECK(labels_within(tree, t2d::Vec2{50.0f, 50.0f}, 100.0f).empty());
}

T2D_TEST(a_radius_query_does_not_walk_what_the_question_does_not_cover) {
    // The reason a layer indexes its entities at all (docs/GAME_DESIGN.md section 1.24): ten thousand
    // things in a 1000x1000 map, and a question about a small circle around one point.
    const t2d::Aabb2 bounds = t2d::Aabb2::from_top_left(t2d::Vec2{0.0f, 0.0f}, t2d::Vec2{1000.0f, 1000.0f});
    QuadTree<int> tree(bounds);
    t2d::Rng rng(1234);
    for (u32 key = 1; key <= 10000; ++key) {
        const t2d::Vec2 where{static_cast<f32>(rng.next_bounded(100000)) * 0.01f,
                              static_cast<f32>(rng.next_bounded(100000)) * 0.01f};
        tree.insert(key, where, static_cast<int>(key));
    }
    T2D_CHECK_EQ(tree.size(), 10000u);
    T2D_CHECK_GT(tree.node_count(), 100u);

    std::vector<int> found;
    tree.for_each_within(t2d::Vec2{500.0f, 500.0f}, 20.0f,
                         [&](const QuadTree<int>::Entry& entry) { found.push_back(entry.payload); });
    const QuadTree<int>::Cost cost = tree.last_query_cost();
    // A walk over everything would test ten thousand entries; the tree tests the nodes the circle
    // touches and what is in them. The bound is loose on purpose - what it pins is the order of
    // magnitude - and the numbers go into the log, because they are what the claim rests on.
    T2D_CHECK_LT(cost.entries, 200u);
    T2D_CHECK_LT(cost.nodes, 100u);
    T2D_CHECK_LT(found.size(), 100u);
    std::printf("quadtree: 10000 entities, a radius 20 query found %zu of them, walking %zu node(s) and "
                "testing %zu entr(ies) - a walk over the layer would have tested all 10000\n",
                found.size(), cost.nodes, cost.entries);
}

T2D_TEST(what_a_layer_costs_with_ten_thousand_entities) {
    // The numbers behind "an entity is cheap": ten thousand of them in one layer, indexed as they are
    // made, ticked once, and asked about a thousand times. Nothing here asserts a duration - a duration
    // belongs to the machine - so what is asserted is that the work happened; the timings are printed
    // for the record (docs/GAME_DESIGN.md section 1.24).
    Fixture fixture;
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");
    MineLayer layer(0, LayerShape{1000, 1000, 1}, 1);
    t2d::Rng rng(99);

    const auto spawn_start = std::chrono::steady_clock::now();
    for (u32 index = 0; index < 10000; ++index) {
        const t2d::Vec2 where{static_cast<f32>(rng.next_bounded(1000000)) * 0.001f,
                              static_cast<f32>(rng.next_bounded(1000000)) * 0.001f};
        if (layer.spawn(ContentKind::Machine, cart, where) == nullptr) break;
    }
    const auto spawn_end = std::chrono::steady_clock::now();
    T2D_CHECK_EQ(layer.entity_count(), 10000u);

    // One pass over all of them, every tick, because that is what an entity does by default.
    const auto tick_start = std::chrono::steady_clock::now();
    T2D_CHECK_EQ(layer.tick(0.016f), 10000u);
    const auto tick_end = std::chrono::steady_clock::now();

    // A thousand questions of the kind a pointer action or a machine asks.
    usize found = 0;
    usize nodes = 0;
    usize tested = 0;
    const auto query_start = std::chrono::steady_clock::now();
    for (u32 query = 0; query < 1000; ++query) {
        const t2d::Vec2 center{static_cast<f32>(rng.next_bounded(1000000)) * 0.001f,
                               static_cast<f32>(rng.next_bounded(1000000)) * 0.001f};
        found += layer.entities_within(center, 10.0f).size();
        nodes += layer.last_entity_query_cost().nodes;
        tested += layer.last_entity_query_cost().entries;
    }
    const auto query_end = std::chrono::steady_clock::now();

    const auto milliseconds = [](std::chrono::steady_clock::time_point from,
                                 std::chrono::steady_clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    const double query_ms = milliseconds(query_start, query_end);
    std::printf("entities: 10000 spawned in %.2f ms, one tick over all of them %.3f ms, "
                "1000 radius-10 queries %.3f ms (%.1f us each) finding %.1f entit(ies) and testing "
                "%.1f of 10000 per query (%zu node(s) per query)\n",
                milliseconds(spawn_start, spawn_end), milliseconds(tick_start, tick_end), query_ms,
                query_ms, static_cast<double>(found) / 1000.0, static_cast<double>(tested) / 1000.0,
                nodes / 1000);
}

T2D_TEST(a_layers_entities_are_indexed_and_the_index_follows_them) {
    Fixture fixture;
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");
    MineLayer layer(0, LayerShape{64, 48, 1}, 1);

    Entity* near = layer.spawn(ContentKind::Machine, cart, t2d::Vec2{10.0f, 10.0f});
    Entity* far = layer.spawn(ContentKind::Machine, cart, t2d::Vec2{40.0f, 30.0f});
    Entity* also_near = layer.spawn(ContentKind::Machine, cart, t2d::Vec2{11.5f, 9.25f});
    T2D_REQUIRE(near != nullptr);
    T2D_REQUIRE(far != nullptr);
    T2D_REQUIRE(also_near != nullptr);
    T2D_CHECK_EQ(layer.entity_count(), 3u);
    T2D_CHECK_EQ(layer.auto_updating_count(), 3u);
    // A serial is spawn order, handed out by the layer, and it is what a query is ordered by.
    T2D_CHECK_EQ(near->serial(), 1u);
    T2D_CHECK_EQ(far->serial(), 2u);
    T2D_CHECK_EQ(also_near->serial(), 3u);
    T2D_CHECK(near->layer() == &layer);

    // The question the index exists for: what is near this point.
    const std::vector<Entity*> around = layer.entities_within(t2d::Vec2{10.0f, 10.0f}, 2.0f);
    T2D_REQUIRE(around.size() == 2u);
    T2D_CHECK(around[0] == near);
    T2D_CHECK(around[1] == also_near);
    T2D_CHECK(layer.entities_within(t2d::Vec2{0.0f, 0.0f}, 5.0f).empty());

    // Moving an entity is what tells the layer, so the index cannot go stale: the old place stops
    // answering and the new one starts.
    far->set_position(t2d::Vec2{10.5f, 10.5f});
    T2D_CHECK_EQ(layer.entities_within(t2d::Vec2{10.0f, 10.0f}, 2.0f).size(), 3u);
    T2D_CHECK(layer.entities_within(t2d::Vec2{40.0f, 30.0f}, 1.0f).empty());
    // A position off the map is a position: the entity is still held, it is just not near anything on
    // the map. The map's own rules about that are the designer's (section 2).
    far->move(t2d::Vec2{-100.0f, -100.0f});
    T2D_CHECK_EQ(layer.entity_count(), 3u);
    T2D_CHECK_EQ(layer.entities_within(t2d::Vec2{10.0f, 10.0f}, 2.0f).size(), 2u);
    far->set_position(t2d::Vec2{40.0f, 30.0f});

    // What the query cost, in the same call: the index is a measurement, not a claim.
    (void)layer.entities_within(t2d::Vec2{10.0f, 10.0f}, 2.0f);
    T2D_CHECK_GE(layer.last_entity_query_cost().entries, 2u);
    T2D_CHECK_LT(layer.last_entity_query_cost().entries, 100u);

    // A destroyed entity leaves the index at once, and the storage at the end of the pass - because a
    // callback may be walking the entities while one of them is destroyed.
    T2D_CHECK(layer.destroy(*also_near));
    T2D_CHECK_FALSE(also_near->alive());
    T2D_CHECK_EQ(layer.entities_within(t2d::Vec2{10.0f, 10.0f}, 2.0f).size(), 1u);
    T2D_CHECK_EQ(layer.entity_count(), 3u);
    T2D_CHECK_FALSE(layer.destroy(*also_near));   // destroying it twice does nothing
    (void)layer.tick(0.016f);
    T2D_CHECK_EQ(layer.entity_count(), 2u);

    // Another layer's entity is not this layer's to destroy.
    MineLayer other(1, LayerShape{8, 8, 1}, 1);
    Entity* stranger = other.spawn(ContentKind::Machine, cart, t2d::Vec2{1.0f, 1.0f});
    T2D_REQUIRE(stranger != nullptr);
    T2D_CHECK_FALSE(layer.destroy(*stranger));
    // And an entity no save could name is refused: the id is what a save stores.
    T2D_CHECK(layer.spawn(ContentKind::Machine, kNoContent, t2d::Vec2{}) == nullptr);
    T2D_CHECK_EQ(layer.entity_count(), 2u);
}

T2D_TEST(the_layer_ticks_its_entities_in_spawn_order_and_they_can_reach_the_layer) {
    Fixture fixture;
    const ContentId cart = fixture.add(ContentKind::Machine, "cart");
    MineLayer layer(0, LayerShape{32, 32, 1}, 1);

    std::vector<u32> order;
    for (int index = 0; index < 3; ++index) {
        auto recorder = std::make_unique<Recorder>(ContentKind::Machine, cart,
                                                   t2d::Vec2{static_cast<f32>(index), 0.5f});
        recorder->order = &order;
        layer.adopt(std::move(recorder));
    }
    // Every tick by default, in the order the layer holds them in - which is spawn order.
    T2D_CHECK_EQ(layer.tick(0.016f), 3u);
    T2D_CHECK_EQ(order, (std::vector<u32>{1u, 2u, 3u}));

    // One that declares a period is not run until its turn has come, and one that says "not me" is
    // never run: the count the layer reports is what actually ran.
    order.clear();
    layer.entities()[0]->set_period_seconds(1.0f);
    layer.entities()[1]->set_auto_update(false);
    T2D_CHECK_EQ(layer.tick(0.016f), 1u);          // only the third one
    T2D_CHECK_EQ(order, (std::vector<u32>{3u}));
    T2D_CHECK_EQ(layer.auto_updating_count(), 2u);
    T2D_CHECK_EQ(layer.tick(1.0f), 2u);            // the slow one's turn, and the third one again
    T2D_CHECK_EQ(order, (std::vector<u32>{3u, 1u, 3u}));

    // An entity can reach the layer it is in while its own turn runs: ask what is near it, interact
    // with it, make something, break something (types/entity.h).
    MineLayer busy_layer(0, LayerShape{32, 32, 1}, 1);
    auto first = std::make_unique<Busy>(ContentKind::Machine, cart, t2d::Vec2{5.0f, 5.0f});
    first->spawn_what = cart;
    Busy& maker = static_cast<Busy&>(busy_layer.adopt(std::move(first)));
    T2D_CHECK_EQ(busy_layer.entity_count(), 1u);
    T2D_CHECK_EQ(busy_layer.tick(0.016f), 1u);
    // The one it made during its turn is there, and it runs from the next tick on - a pass does not
    // grow under itself.
    T2D_CHECK_EQ(maker.spawned, 1);
    T2D_CHECK_EQ(busy_layer.entity_count(), 2u);
    T2D_CHECK_EQ(busy_layer.tick(0.016f), 2u);
    T2D_CHECK_EQ(maker.spawned, 2);

    // Two entities close enough to meet: each asks the layer what is near, and the layer asks both
    // sides of the interaction (the default answer of the other one is "nothing happened").
    MineLayer meeting(0, LayerShape{32, 32, 1}, 1);
    Busy& left = static_cast<Busy&>(
        meeting.adopt(std::make_unique<Busy>(ContentKind::Machine, cart, t2d::Vec2{8.0f, 8.0f})));
    Busy& right = static_cast<Busy&>(
        meeting.adopt(std::make_unique<Busy>(ContentKind::Machine, cart, t2d::Vec2{8.5f, 8.0f})));
    T2D_CHECK_EQ(meeting.tick(0.016f), 2u);
    T2D_CHECK_EQ(left.met, 1);
    T2D_CHECK_EQ(right.met, 1);

    // And an entity that breaks another one during a pass: the broken one does not run in that pass,
    // and its storage is gone at the end of it.
    MineLayer breaking(0, LayerShape{32, 32, 1}, 1);
    auto killer = std::make_unique<Busy>(ContentKind::Machine, cart, t2d::Vec2{1.0f, 1.0f});
    Busy& breaks = static_cast<Busy&>(breaking.adopt(std::move(killer)));
    Entity* victim = breaking.spawn(ContentKind::Machine, cart, t2d::Vec2{20.0f, 20.0f});
    T2D_REQUIRE(victim != nullptr);
    breaks.victim = victim;
    T2D_CHECK_EQ(breaking.tick(0.016f), 1u);      // the one that ran, not the one that was broken
    T2D_CHECK_EQ(breaks.destroyed, 1);
    T2D_CHECK_EQ(breaking.entity_count(), 1u);
}

T2D_TEST_MAIN
