// The technology tree the content data declares: the research:: sub-object, the nodes, their parents,
// depth, the program layout, what is visible and what has to be researched first
// (mine/research.h, mine/types/research.h, docs/GAME_DESIGN.md section 1.50).
#include <mine/research.h>

#include <mine/types/tile_definition.h>

#include <support/test_support.h>

#include <t2d/core/ecfg.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// A registry and definitions made by hand: a research node is an ordinary content entry that declares
/// research::, so the tests do not need a content file for the graph ones.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    ContentId add_node(ContentKind kind, std::string_view name, std::vector<std::string> parents,
                       bool is_root, std::string tree = "intro", i32 min_layer = 0) {
        const ContentId id = registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        Research research;
        research.parents = std::move(parents);
        research.is_root = is_root;
        research.tree = std::move(tree);
        research.min_layer = min_layer;
        research.cost.push_back(ContentStack{"stone", 20});
        definition.research = std::move(research);
        definitions.add(std::move(definition));
        return id;
    }

    /// Content that is not a research node, so no row may appear for it.
    void add_plain(ContentKind kind, std::string_view name) {
        (void)registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        definitions.add(std::move(definition));
    }
};

} // namespace

T2D_TEST(the_research_fields_are_read_from_the_data) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(R"(
warehouse::
    stone::
        capacity:100
        research::
            cost:["stone",20,"gear",3]
            is_root:true
            tree:"intro"
            min_layer:1
    workshop::
        capacity:10
        research::
            cost:["stone",5]
            parent:["stone"]
            tree:"intro"
            min_layer:0
)");
    T2D_REQUIRE(document.has_value());
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    T2D_CHECK(errors.empty());
    T2D_REQUIRE(definitions.size() == 2u);

    const TileDefinition& root = definitions[0];
    T2D_REQUIRE(root.research.has_value());
    T2D_CHECK(root.research->is_root);
    T2D_CHECK_EQ(root.research->tree, std::string("intro"));
    T2D_CHECK_EQ(root.research->min_layer, 1);
    T2D_CHECK(root.research->parents.empty());
    T2D_REQUIRE(root.research->cost.size() == 2u);
    T2D_CHECK_EQ(root.research->cost[0].name, std::string("stone"));
    T2D_CHECK_EQ(root.research->cost[0].count, static_cast<t2d::i64>(20));
    T2D_CHECK_EQ(root.research->cost[1].name, std::string("gear"));
    T2D_CHECK_EQ(root.research->cost[1].count, static_cast<t2d::i64>(3));

    const TileDefinition& child = definitions[1];
    T2D_REQUIRE(child.research.has_value());
    T2D_CHECK_FALSE(child.research->is_root);
    T2D_REQUIRE(child.research->parents.size() == 1u);
    T2D_CHECK_EQ(child.research->parents[0], std::string("stone"));
    T2D_CHECK_EQ(child.research->min_layer, 0);
}

T2D_TEST(a_research_table_the_engine_cannot_read_is_refused) {
    struct BadResearch {
        std::string text;
        std::string_view needle;
    };
    const std::vector<BadResearch> bad = {
        {"research:1\n", "must be a table"},
        {"research::\n            cost:[\"stone\",20,\"gear\"]\n", "pairs"},
        {"research::\n            cost:[\"stone\",0]\n", "above zero"},
        {"research::\n            cost:[1,20]\n", "quoted name"},
        {"research::\n            parent:\"stone\"\n", "must be an array"},
        {"research::\n            parent:[1]\n", "quoted name"},
        {"research::\n            is_root:1\n", "is_root must be true or false"},
        {"research::\n            tree:1\n", "tree must be"},
        {"research::\n            tree:\"\"\n", "tree must be"},
        {"research::\n            min_layer:-1\n", "not negative"},
        {"research::\n            min_layer:1.5\n", "whole mine layer"},
        {"research::\n            colour:\"red\"\n", "no field 'colour'"},
    };
    for (const BadResearch& entry : bad) {
        const std::string text = "warehouse::\n    node::\n        " + entry.text;
        const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text);
        T2D_REQUIRE(document.has_value());
        std::vector<std::string> errors;
        const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
        T2D_CHECK_MSG(!errors.empty(), "no error for: {}", entry.text);
        std::string joined;
        for (const std::string& error : errors) joined += error;
        T2D_CHECK_MSG(joined.find(entry.needle) != std::string::npos, "'{}' was not in: {}", entry.needle,
                      joined);
        T2D_CHECK(definitions.empty());
    }
}

T2D_TEST(the_shipped_stone_warehouse_is_the_root_of_the_intro_tree) {
    const std::string path = std::string(T2D_SOURCE_DIR) + "/games/mine/content/warehouses.ecfg";
    t2d::EcfgError load_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &load_error);
    if (!document.has_value()) {
        if (load_error.line == 0) T2D_SKIP("the game's content is not next to the source tree");
        T2D_CHECK_MSG(false, "{}", load_error.describe(path));
        return;
    }
    std::vector<std::string> errors;
    const std::vector<TileDefinition> definitions = tile_definitions(*document, &errors);
    T2D_CHECK(errors.empty());
    const TileDefinition* stone = nullptr;
    for (const TileDefinition& definition : definitions) {
        if (definition.kind == ContentKind::Warehouse && definition.name == "stone") stone = &definition;
    }
    T2D_REQUIRE(stone != nullptr);
    T2D_REQUIRE(stone->research.has_value());
    T2D_CHECK(stone->research->is_root);
    T2D_CHECK_EQ(stone->research->tree, std::string("intro"));
    T2D_CHECK_EQ(stone->research->min_layer, 1);
    T2D_REQUIRE(stone->research->cost.size() == 1u);
    T2D_CHECK_EQ(stone->research->cost[0].name, std::string("stone"));
    T2D_CHECK_EQ(stone->research->cost[0].count, static_cast<t2d::i64>(20));
}

T2D_TEST(the_tree_is_built_from_the_nodes_and_sits_columns_at_depth) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "intro_root", {}, true);
    (void)fixture.add_node(ContentKind::Warehouse, "intro_a", {"intro_root"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "intro_b", {"intro_a"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "intro_c", {"intro_root"}, false);

    std::vector<std::string> errors;
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, &errors);
    T2D_CHECK(errors.empty());
    T2D_REQUIRE(tree.node_count() == 4u);
    T2D_CHECK_EQ(tree.node(0).name, std::string("intro_root"));
    T2D_CHECK_EQ(tree.node(0).depth, 0);
    T2D_CHECK_EQ(tree.node(1).depth, 1);
    T2D_CHECK_EQ(tree.node(2).depth, 2);
    T2D_CHECK_EQ(tree.node(3).depth, 1);
    // Column is depth; the nodes of one column take rows in the order the data declared them.
    T2D_CHECK_EQ(tree.node(1).column, 1);
    T2D_CHECK_EQ(tree.node(1).row, 0);
    T2D_CHECK_EQ(tree.node(3).column, 1);
    T2D_CHECK_EQ(tree.node(3).row, 1);
    T2D_CHECK_EQ(tree.node(2).column, 2);
    const t2d::Vec2 position = tree.position(3);
    T2D_CHECK_NEAR(position.x, static_cast<f32>(kResearchColumnStep), 1e-6f);
    T2D_CHECK_NEAR(position.y, static_cast<f32>(kResearchRowStep), 1e-6f);

    T2D_REQUIRE(tree.edge_count() == 3u);
    T2D_CHECK_EQ(tree.edge(0).parent, 0u);
    T2D_CHECK_EQ(tree.edge(0).child, 1u);
    T2D_CHECK_EQ(tree.edge(1).parent, 1u);
    T2D_CHECK_EQ(tree.edge(1).child, 2u);
    T2D_CHECK_EQ(tree.edge(2).parent, 0u);
    T2D_CHECK_EQ(tree.edge(2).child, 3u);

    T2D_REQUIRE(tree.find("intro_b").has_value());
    T2D_CHECK_EQ(*tree.find("intro_b"), 2u);
    T2D_CHECK_FALSE(tree.find("nope").has_value());
}

T2D_TEST(multiple_parents_are_kept_and_depth_follows_the_deepest) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "left", {}, true);
    (void)fixture.add_node(ContentKind::Warehouse, "right", {}, true);
    (void)fixture.add_node(ContentKind::Warehouse, "join", {"left", "right"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "after", {"join"}, false);

    std::vector<std::string> errors;
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, &errors);
    T2D_CHECK(errors.empty());
    T2D_REQUIRE(tree.node_count() == 4u);
    const std::optional<usize> join = tree.find("join");
    T2D_REQUIRE(join.has_value());
    T2D_REQUIRE(tree.node(*join).parents.size() == 2u);
    T2D_CHECK_EQ(tree.node(*join).depth, 1);
    T2D_CHECK_EQ(tree.node(0).depth, 0);
    T2D_CHECK_EQ(tree.node(1).depth, 0);
    T2D_CHECK_EQ(tree.node(3).depth, 2);
}

T2D_TEST(trees_are_listed_once_in_the_order_the_nodes_appear) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "intro_root", {}, true, "intro");
    (void)fixture.add_node(ContentKind::Warehouse, "intro_a", {"intro_root"}, false, "intro");
    (void)fixture.add_node(ContentKind::Warehouse, "combat_root", {}, true, "combat");
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, nullptr);
    T2D_REQUIRE(tree.trees().size() == 2u);
    T2D_CHECK_EQ(tree.trees()[0], std::string("intro"));
    T2D_CHECK_EQ(tree.trees()[1], std::string("combat"));
}

T2D_TEST(a_parent_that_is_not_a_node_is_an_error) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "child", {"nowhere"}, false);
    std::vector<std::string> errors;
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, &errors);
    T2D_CHECK(tree.node_count() == 1u);
    T2D_REQUIRE(!errors.empty());
    T2D_CHECK(errors[0].find("not a research node") != std::string::npos);
}

T2D_TEST(a_root_with_parents_and_a_non_root_without_are_refused) {
    {
        Fixture fixture;
        (void)fixture.add_node(ContentKind::Warehouse, "root", {"other"}, true);
        std::vector<std::string> errors;
        (void)ResearchTree::build(fixture.registry, fixture.definitions, &errors);
        T2D_REQUIRE(!errors.empty());
        T2D_CHECK(errors[0].find("root") != std::string::npos ||
                  errors[0].find("parent") != std::string::npos);
    }
    {
        Fixture fixture;
        (void)fixture.add_node(ContentKind::Warehouse, "lonely", {}, false);
        std::vector<std::string> errors;
        (void)ResearchTree::build(fixture.registry, fixture.definitions, &errors);
        T2D_REQUIRE(!errors.empty());
        T2D_CHECK(errors[0].find("no parent") != std::string::npos);
    }
}

T2D_TEST(a_cycle_in_the_tree_is_refused) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "a", {"b"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "b", {"a"}, false);
    std::vector<std::string> errors;
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, &errors);
    T2D_CHECK(tree.node_count() == 2u);
    T2D_REQUIRE(!errors.empty());
    bool cycle_reported = false;
    for (const std::string& error : errors) {
        if (error.find("cycle") != std::string::npos) cycle_reported = true;
    }
    T2D_CHECK(cycle_reported);
}

T2D_TEST(a_duplicate_node_name_is_refused) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "same", {}, true);
    (void)fixture.add_node(ContentKind::Structure, "same", {}, true);
    std::vector<std::string> errors;
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, &errors);
    T2D_CHECK_EQ(tree.node_count(), 1u);
    T2D_REQUIRE(!errors.empty());
    T2D_CHECK(errors[0].find("more than once") != std::string::npos);
}

T2D_TEST(what_is_visible_and_what_can_be_researched_follows_the_parents) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "root", {}, true);
    (void)fixture.add_node(ContentKind::Warehouse, "child", {"root"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "grandchild", {"child"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "second_root", {}, true);
    (void)fixture.add_node(ContentKind::Warehouse, "plain", {}, true, "intro");
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, nullptr);
    ResearchState state;
    state.reset(fixture.registry, fixture.definitions);

    const std::optional<usize> root = tree.find("root");
    const std::optional<usize> child = tree.find("child");
    const std::optional<usize> grandchild = tree.find("grandchild");
    const std::optional<usize> second = tree.find("second_root");
    T2D_REQUIRE(root.has_value());
    T2D_REQUIRE(child.has_value());
    T2D_REQUIRE(grandchild.has_value());
    T2D_REQUIRE(second.has_value());

    // Roots show before anything is researched; a child behind an unresearched parent does not.
    T2D_CHECK(tree.visible(*root, state));
    T2D_CHECK(tree.visible(*second, state));
    T2D_CHECK_FALSE(tree.visible(*child, state));
    T2D_CHECK_FALSE(tree.visible(*grandchild, state));
    T2D_CHECK(tree.prereqs_met(*root, state));
    T2D_CHECK_FALSE(tree.prereqs_met(*child, state));

    T2D_CHECK(state.set_researched(tree.node(*root).kind, tree.node(*root).id, true));
    T2D_CHECK(tree.visible(*child, state));
    T2D_CHECK_FALSE(tree.visible(*grandchild, state));
    T2D_CHECK(tree.prereqs_met(*child, state));
    T2D_CHECK_FALSE(tree.prereqs_met(*grandchild, state));

    T2D_CHECK(state.set_researched(tree.node(*child).kind, tree.node(*child).id, true));
    T2D_CHECK(tree.visible(*grandchild, state));
    T2D_CHECK(tree.prereqs_met(*grandchild, state));
    // A researched node is visible for ever, even if the tree is looked at another way.
    T2D_CHECK(tree.visible(*root, state));
}

T2D_TEST(a_multi_node_tree_reveals_one_level_at_a_time) {
    // root -> a, b, c, and a grandchild that needs both a and b: the shape the screen has to draw and
    // reveal one level at a time (docs/GAME_DESIGN.md section 1.50).
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "root", {}, true);
    (void)fixture.add_node(ContentKind::Warehouse, "a", {"root"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "b", {"root"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "c", {"root"}, false);
    (void)fixture.add_node(ContentKind::Warehouse, "join", {"a", "b"}, false);
    std::vector<std::string> errors;
    const ResearchTree tree = ResearchTree::build(fixture.registry, fixture.definitions, &errors);
    T2D_CHECK(errors.empty());
    T2D_REQUIRE(tree.node_count() == 5u);
    T2D_REQUIRE(tree.edge_count() == 5u);   // root->a, root->b, root->c, a->join, b->join

    // Depth is the vertical step, the place among a level the horizontal one: the children sit side by
    // side under the root, the grandchild under them.
    T2D_CHECK_EQ(tree.node(0).depth, 0);
    T2D_CHECK_EQ(tree.node(0).row, 0);
    T2D_CHECK_EQ(tree.node(1).depth, 1);
    T2D_CHECK_EQ(tree.node(1).row, 0);
    T2D_CHECK_EQ(tree.node(2).depth, 1);
    T2D_CHECK_EQ(tree.node(2).row, 1);
    T2D_CHECK_EQ(tree.node(3).depth, 1);
    T2D_CHECK_EQ(tree.node(3).row, 2);
    T2D_CHECK_EQ(tree.node(4).depth, 2);
    T2D_CHECK_EQ(tree.node(4).row, 0);
    const t2d::Vec2 root_position = tree.position(0);
    T2D_CHECK_NEAR(root_position.x, 0.0f, 1e-6f);
    T2D_CHECK_NEAR(root_position.y, 0.0f, 1e-6f);
    const t2d::Vec2 b_position = tree.position(2);
    T2D_CHECK_NEAR(b_position.x, static_cast<f32>(kResearchColumnStep), 1e-6f);
    T2D_CHECK_NEAR(b_position.y, static_cast<f32>(kResearchRowStep), 1e-6f);
    const t2d::Vec2 join_position = tree.position(4);
    T2D_CHECK_NEAR(join_position.x, 0.0f, 1e-6f);
    T2D_CHECK_NEAR(join_position.y, static_cast<f32>(2 * kResearchRowStep), 1e-6f);

    // Visibility: the root first, the children once it is studied, the grandchild once one of its parents
    // is - while researching it still needs both.
    ResearchState state;
    state.reset(fixture.registry, fixture.definitions);
    T2D_REQUIRE(state.size() == 5u);
    const std::optional<usize> root = tree.find("root");
    const std::optional<usize> a = tree.find("a");
    const std::optional<usize> b = tree.find("b");
    const std::optional<usize> join = tree.find("join");
    T2D_REQUIRE(root.has_value());
    T2D_REQUIRE(a.has_value());
    T2D_REQUIRE(b.has_value());
    T2D_REQUIRE(join.has_value());
    T2D_CHECK(tree.visible(*root, state));
    T2D_CHECK_FALSE(tree.visible(*a, state));
    T2D_CHECK_FALSE(tree.visible(*join, state));
    // No edge has both of its ends on screen yet.
    usize drawn = 0;
    for (usize edge = 0; edge < tree.edge_count(); ++edge) {
        if (tree.visible(tree.edge(edge).parent, state) && tree.visible(tree.edge(edge).child, state)) ++drawn;
    }
    T2D_CHECK_EQ(drawn, 0u);

    const ResearchNode& root_node = tree.node(*root);
    T2D_CHECK(state.set_researched(root_node.kind, root_node.id, true));
    T2D_CHECK(tree.visible(*a, state));
    T2D_CHECK(tree.visible(*b, state));
    T2D_CHECK_FALSE(tree.visible(*join, state));
    drawn = 0;
    for (usize edge = 0; edge < tree.edge_count(); ++edge) {
        if (tree.visible(tree.edge(edge).parent, state) && tree.visible(tree.edge(edge).child, state)) ++drawn;
    }
    T2D_CHECK_EQ(drawn, 3u);   // the root's three edges; the grandchild is not on screen yet

    const ResearchNode& a_node = tree.node(*a);
    T2D_CHECK(state.set_researched(a_node.kind, a_node.id, true));
    T2D_CHECK(tree.visible(*join, state));           // one parent is enough to show it
    T2D_CHECK_FALSE(tree.prereqs_met(*join, state)); // but both are needed to research it
    drawn = 0;
    for (usize edge = 0; edge < tree.edge_count(); ++edge) {
        if (tree.visible(tree.edge(edge).parent, state) && tree.visible(tree.edge(edge).child, state)) ++drawn;
    }
    T2D_CHECK_EQ(drawn, 5u);   // now every edge has both ends on screen

    const ResearchNode& b_node = tree.node(*b);
    T2D_CHECK(state.set_researched(b_node.kind, b_node.id, true));
    T2D_CHECK(tree.prereqs_met(*join, state));
}

T2D_TEST(the_first_layer_the_player_sees_meets_min_layer_one) {
    // The interface numbers layers from 1; the model's index is zero-based. The story opens on layer 1
    // (index 0), so min_layer 1 must already allow research there.
    T2D_CHECK(layer_reaches(1, 0));
    T2D_CHECK(layer_reaches(2, 1));
    T2D_CHECK_FALSE(layer_reaches(2, 0));
    T2D_CHECK_FALSE(layer_reaches(1, -1));
}

T2D_TEST_MAIN
