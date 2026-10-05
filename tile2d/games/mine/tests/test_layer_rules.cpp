// The story's layers, described in data: the reader that turns a layer:: entry into a rule, and the
// generator that turns the rules into the layer a world enters.
//
// The content in here is a **test double**: names like "dirt" or "rock" are the fixture's, not the
// game's - the game names no content at all (docs/GAME_DESIGN.md section 7).
#include <mine/content_pack.h>
#include <mine/layer_rules.h>

#include <support/test_support.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace mine;
using namespace mine::types;
namespace fs = std::filesystem;

namespace {

/// A scratch directory of packs, removed when the test ends. The same shape test_content_pack uses:
/// a pack is a directory, so the only way to test one is to write one.
class Scratch {
public:
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / std::format("t2d_layers_{}", stamp);
        std::error_code code;
        fs::remove_all(root_, code);
        fs::create_directories(root_, code);
    }
    ~Scratch() {
        std::error_code code;
        fs::remove_all(root_, code);
    }
    T2D_NON_COPYABLE(Scratch);

    void write(const std::string& relative, const std::string& text) const {
        const fs::path path = root_ / relative;
        std::error_code code;
        fs::create_directories(path.parent_path(), code);
        std::ofstream file(path);
        file << text;
    }
    [[nodiscard]] std::string root() const { return root_.string(); }

private:
    fs::path root_;
};

/// What reading a document's layer:: tables did.
struct Parsed {
    std::vector<LayerRule> rules;
    std::vector<std::string> errors;
};

[[nodiscard]] Parsed parse(std::string_view text) {
    t2d::EcfgError error;
    std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text, &error);
    if (!document.has_value()) return Parsed{{}, {error.describe("test")}};
    Parsed parsed;
    parsed.rules = layer_rules(*document, &parsed.errors);
    return parsed;
}

[[nodiscard]] bool mentions(const std::vector<std::string>& lines, std::string_view needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// A registry with one floor, and the definitions that go with it: what a test's content file would
/// have produced.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    void add_floor(std::string_view name, bool random_reverse = false) {
        registry.register_content(ContentKind::Floor, name);
        TileDefinition definition;
        definition.kind = ContentKind::Floor;
        definition.name = std::string(name);
        definition.random_reverse = random_reverse;
        definitions.add(std::move(definition));
    }
};

} // namespace

T2D_TEST(a_layer_is_read_from_the_data) {
    const Parsed parsed = parse(R"(
# What the layer's own fields are is the designer's; the engine reads size:: and floor:: and nothing else.
layer::
    entrance::
        name:"第一层"
        resources::
            ore:12
        size::
            width:64
            height:48
            tile_layers:2
        floor::
            layer:1
            full_flash:"dirt"
)");
    T2D_CHECK(parsed.errors.empty());
    T2D_REQUIRE(parsed.rules.size() == 1u);
    const LayerRule& rule = parsed.rules[0];
    T2D_CHECK_EQ(rule.name, std::string("entrance"));
    T2D_CHECK_EQ(rule.width, 64u);
    T2D_CHECK_EQ(rule.height, 48u);
    T2D_CHECK_EQ(rule.tile_layers, 2);
    T2D_CHECK_EQ(rule.floor.layer, 1);
    T2D_REQUIRE(rule.floor.rules.size() == 1u);
    T2D_CHECK_EQ(rule.floor.rules[0].kind, FloorRule::Kind::FullFlash);
    T2D_CHECK_EQ(rule.floor.rules[0].floor, std::string("dirt"));
}

T2D_TEST(every_field_of_a_layer_is_optional) {
    // A layer that states nothing is a layer of the world's shape with no floor: the same "the world
    // decides" a generator that does not describe a shape gets (world.h).
    const Parsed parsed = parse(R"(
layer::
    bare::
)");
    T2D_CHECK(parsed.errors.empty());
    T2D_REQUIRE(parsed.rules.size() == 1u);
    T2D_CHECK_EQ(parsed.rules[0].width, 0u);
    T2D_CHECK_EQ(parsed.rules[0].height, 0u);
    T2D_CHECK_EQ(parsed.rules[0].tile_layers, 0);
    T2D_CHECK_EQ(parsed.rules[0].floor.layer, 0);
    T2D_CHECK(parsed.rules[0].floor.rules.empty());

    // An empty size:: says the same thing as no size:: at all.
    const Parsed empty_size = parse(R"(
layer::
    bare::
        size::
        floor::
)");
    T2D_CHECK(empty_size.errors.empty());
    T2D_REQUIRE(empty_size.rules.size() == 1u);
    T2D_CHECK_EQ(empty_size.rules[0].width, 0u);
}

T2D_TEST(a_size_that_is_not_a_size_is_refused) {
    // A field the engine reads and cannot read is refused, never read as "no size": the two are
    // different layers, and only one of them is what the author wrote.
    const Parsed array = parse(R"(
layer::
    entrance::
        size:[40,24]
)");
    T2D_CHECK(array.rules.empty());
    T2D_CHECK(mentions(array.errors, "size must be a table"));

    const Parsed typo = parse(R"(
layer::
    entrance::
        size::
            widht:40
)");
    T2D_CHECK(typo.rules.empty());
    T2D_CHECK(mentions(typo.errors, "size has no field 'widht'"));

    const Parsed text = parse(R"(
layer::
    entrance::
        size::
            width:"wide"
)");
    T2D_CHECK(text.rules.empty());
    T2D_CHECK(mentions(text.errors, "size.width must be a whole number"));

    const Parsed zero = parse(R"(
layer::
    entrance::
        size::
            height:0
)");
    T2D_CHECK(zero.rules.empty());
    T2D_CHECK(mentions(zero.errors, "1..4096"));

    const Parsed huge = parse(R"(
layer::
    entrance::
        size::
            width:4097
)");
    T2D_CHECK(huge.rules.empty());
    T2D_CHECK(mentions(huge.errors, "1..4096"));

    const Parsed layers = parse(R"(
layer::
    entrance::
        size::
            tile_layers:33
)");
    T2D_CHECK(layers.rules.empty());
    T2D_CHECK(mentions(layers.errors, "1..32"));
}

T2D_TEST(the_floor_creator_refuses_what_it_does_not_know) {
    const Parsed unknown = parse(R"(
layer::
    entrance::
        floor::
            full_flsh:"dirt"
)");
    T2D_CHECK(unknown.rules.empty());
    T2D_CHECK(mentions(unknown.errors, "no rule 'full_flsh'"));

    const Parsed number = parse(R"(
layer::
    entrance::
        floor::
            full_flash:3
)");
    T2D_CHECK(number.rules.empty());
    T2D_CHECK(mentions(number.errors, "full_flash must be the quoted name of a floor"));

    const Parsed blank = parse(R"(
layer::
    entrance::
        floor::
            full_flash:""
)");
    T2D_CHECK(blank.rules.empty());
    T2D_CHECK(mentions(blank.errors, "names no floor"));

    const Parsed negative = parse(R"(
layer::
    entrance::
        floor::
            layer:-1
            full_flash:"dirt"
)");
    T2D_CHECK(negative.rules.empty());
    T2D_CHECK(mentions(negative.errors, "0..31"));

    // A floor the map cannot hold is refused while the file is read, because the same entry says how
    // many tile layers the map has.
    const Parsed beyond = parse(R"(
layer::
    entrance::
        size::
            tile_layers:1
        floor::
            layer:1
            full_flash:"dirt"
)");
    T2D_CHECK(beyond.rules.empty());
    T2D_CHECK(mentions(beyond.errors, "works on tile layer 1, and this layer has 1"));

    const Parsed not_a_table = parse(R"(
layer::
    entrance::
        floor:0
)");
    T2D_CHECK(not_a_table.rules.empty());
    T2D_CHECK(mentions(not_a_table.errors, "floor must be a table"));

    const Parsed scalar = parse(R"(
layer::
    entrance:5
)");
    T2D_CHECK(scalar.rules.empty());
    T2D_CHECK(mentions(scalar.errors, "a layer is a table"));

    // The order the two tables appear in does not matter: the check is made once both are read.
    const Parsed floor_first = parse(R"(
layer::
    entrance::
        floor::
            layer:2
            full_flash:"dirt"
        size::
            tile_layers:2
)");
    T2D_CHECK(floor_first.rules.empty());
    T2D_CHECK(mentions(floor_first.errors, "works on tile layer 2"));
}

T2D_TEST(a_layer_entry_that_cannot_be_read_does_not_hide_the_ones_after_it) {
    const Parsed parsed = parse(R"(
layer::
    broken::
        size::
            width:-4
    fine::
        size::
            width:8
        floor::
            full_flash:"dirt"
    also_fine::
)");
    T2D_REQUIRE(parsed.errors.size() == 1u);
    T2D_CHECK(mentions(parsed.errors, "size.width is -4"));
    T2D_REQUIRE(parsed.rules.size() == 2u);
    T2D_CHECK_EQ(parsed.rules[0].name, std::string("fine"));
    T2D_CHECK_EQ(parsed.rules[0].width, 8u);
    T2D_CHECK_EQ(parsed.rules[1].name, std::string("also_fine"));
}

T2D_TEST(a_layer_name_is_kept_by_its_first_declaration) {
    // The same rule the registry follows for a content name: the first declaration keeps it, so the
    // story's order and the registry's ids stay in step.
    LayerRules rules;
    LayerRule first;
    first.name = "entrance";
    first.width = 8;
    LayerRule second;
    second.name = "entrance";
    second.width = 99;
    LayerRule other;
    other.name = "shaft";
    rules.add(std::move(first));
    rules.add(std::move(second));
    rules.add(std::move(other));

    T2D_CHECK_EQ(rules.size(), 2u);
    T2D_REQUIRE(rules.find("entrance") != nullptr);
    T2D_CHECK_EQ(rules.find("entrance")->width, 8u);
    T2D_CHECK(rules.find("nothing") == nullptr);
    T2D_REQUIRE(rules.at(1) != nullptr);
    T2D_CHECK_EQ(rules.at(1)->name, std::string("shaft"));
    T2D_CHECK(rules.at(2) == nullptr);
    T2D_CHECK_EQ(rules.rules().size(), 2u);
}

T2D_TEST(the_story_order_is_the_order_the_data_declares) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:4
    shaft::
        size::
            width:6
)");
    T2D_CHECK(parsed.errors.empty());
    T2D_REQUIRE(parsed.rules.size() == 2u);
    T2D_CHECK_EQ(parsed.rules[0].name, std::string("entrance"));
    T2D_CHECK_EQ(parsed.rules[1].name, std::string("shaft"));

    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);
    const LayerGenerator generator = story_layer_generator(rules, LayerShape{16, 16, 1});
    T2D_CHECK_EQ(generator(1, 0).shape.width, 4u);
    T2D_CHECK_EQ(generator(1, 1).shape.width, 6u);
    // Past the end of the story, and before its beginning: an empty layer of the world's shape.
    T2D_CHECK_EQ(generator(1, 2).shape.width, 16u);
    T2D_CHECK_EQ(generator(1, 2).content.size(), 0u);
    T2D_CHECK_EQ(generator(1, -1).shape.width, 16u);
}

T2D_TEST(the_size_the_data_states_is_the_size_of_the_layer) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:5
            height:3
            tile_layers:2
        floor::
            layer:1
            full_flash:"dirt"
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt");
    // The world was given a shape of its own; the layer that states one is that size instead.
    MineWorld world(7, LayerShape{16, 16, 1});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(layer.width(), 5u);
    T2D_CHECK_EQ(layer.height(), 3u);
    T2D_CHECK_EQ(layer.layer_count(), 2);
    T2D_CHECK_EQ(layer.plot_count(), 15u);
    T2D_CHECK_EQ(layer.filled_cells(0), 0u);
    T2D_CHECK_EQ(layer.filled_cells(1), 15u);

    // A layer that states no size is the world's shape, and its floor is laid on tile layer 0.
    LayerRules bare;
    LayerRule rule;
    rule.name = "bare";
    rule.floor.rules.push_back(FloorRule{FloorRule::Kind::FullFlash, "dirt"});
    bare.add(std::move(rule));
    MineWorld shaped(7, LayerShape{4, 2, 1});
    shaped.set_generator(story_layer_generator(bare, shaped.shape()));
    const MineLayer& fallback = shaped.enter(0, content.registry, content.definitions);
    T2D_CHECK(shaped.build_report().clean());
    T2D_CHECK_EQ(fallback.width(), 4u);
    T2D_CHECK_EQ(fallback.height(), 2u);
    T2D_CHECK_EQ(fallback.layer_count(), 1);
    T2D_CHECK_EQ(fallback.filled_cells(0), 8u);
}

T2D_TEST(a_floor_rule_fills_every_cell_of_the_layer) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:6
            height:4
        floor::
            full_flash:"dirt"
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt", /*random_reverse=*/true);
    MineWorld world(99, LayerShape{6, 4, 1});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    const LayerBuildReport& report = world.build_report();
    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.placed, 24u);
    T2D_CHECK_EQ(report.cells, 24u);
    T2D_CHECK_EQ(layer.plot_count(), 24u);
    T2D_CHECK_EQ(layer.filled_cells(), 24u);

    // Every cell of the map, and every plot one cell: one floor per cell is what "every cell is this
    // floor" means, and it is also what makes random_reverse mean anything (a whole-map plot would
    // have one mirroring decision for the map).
    for (i32 y = 0; y < 4; ++y) {
        for (i32 x = 0; x < 6; ++x) {
            const ContentRef ref = layer.ref_at(0, GridPos{x, y});
            T2D_CHECK_EQ(ref.kind, ContentKind::Floor);
            T2D_CHECK_EQ(ref.id, content.registry.find(ContentKind::Floor, "dirt"));
            T2D_CHECK_FALSE(ref.stale);
            const SceneTile* plot = layer.plot_at(0, GridPos{x, y});
            T2D_REQUIRE(plot != nullptr);
            T2D_CHECK(plot->is_floor());
            T2D_CHECK_EQ(plot->anchor().x, x);
            T2D_CHECK_EQ(plot->anchor().y, y);
            T2D_CHECK_EQ(plot->width(), 1);
        }
    }
    // Half of them are turned around, give or take: 24 coins is 2.4 of standard deviation.
    T2D_CHECK_GT(layer.mirrored_count(), 5u);
    T2D_CHECK_LT(layer.mirrored_count(), 19u);

    // The story is the same mine for every world: entering the layer again builds it back byte for
    // byte, and a world with another seed gets the same content - what the designer wrote does not
    // depend on the dice. The dice still decide how a copy of a floor draws itself, exactly as they do
    // in any other layer (world.h).
    const std::string first_dump = layer.dump_text(content.registry);
    MineWorld same_seed(99, LayerShape{6, 4, 1});
    same_seed.set_generator(story_layer_generator(rules, same_seed.shape()));
    T2D_CHECK_EQ(same_seed.enter(0, content.registry, content.definitions).dump_text(content.registry), first_dump);

    MineWorld other_seed(1234, LayerShape{6, 4, 1});
    other_seed.set_generator(story_layer_generator(rules, other_seed.shape()));
    const MineLayer& other = other_seed.enter(0, content.registry, content.definitions);
    T2D_CHECK(other_seed.build_report().clean());
    T2D_CHECK_EQ(other.plot_count(), layer.plot_count());
    for (i32 y = 0; y < 4; ++y) {
        for (i32 x = 0; x < 6; ++x) {
            T2D_CHECK_EQ(other.ref_at(0, GridPos{x, y}).packed(), layer.ref_at(0, GridPos{x, y}).packed());
        }
    }

    // Entering the same layer of the same world again is the same layer: a mine is not built twice.
    const MineLayer& twice = world.enter(0, content.registry, content.definitions);
    T2D_CHECK_EQ(twice.dump_text(content.registry), first_dump);
}

T2D_TEST(a_floor_the_registry_does_not_have_is_refused_by_the_build) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:4
            height:4
        floor::
            full_flash:"gravel"
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt");
    MineWorld world(1, LayerShape{4, 4, 1});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    // The name is the data's, and the registry is what says whether it exists: the refusal names the
    // content, and the layer is still a layer (it can be entered, walked and drawn). All sixteen cells
    // were refused for the same reason, and the report says so once and counts the rest - the reason is
    // the same sentence for every cell, so sixteen copies of it are sixteen copies of nothing.
    const LayerBuildReport& report = world.build_report();
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK_EQ(report.refused, 16u);
    T2D_REQUIRE(report.errors.size() == 1u);
    T2D_CHECK(report.errors[0].find("gravel") != std::string::npos);
    T2D_CHECK(report.truncated());
    T2D_CHECK_EQ(layer.plot_count(), 0u);
    T2D_CHECK_EQ(layer.width(), 4u);
    T2D_CHECK_EQ(layer.filled_cells(), 0u);
}

T2D_TEST(the_last_rule_to_paint_a_cell_wins) {
    // A table cannot repeat a key, so the file cannot write two rules of one kind yet - but the model
    // is what the next rule kind leans on, so it is the model that is checked here: rules paint in
    // order, the last paint of a cell is what that cell is, and one cell is one plot (which is what
    // keeps two rules from overlapping each other into a refusal).
    LayerRules rules;
    LayerRule rule;
    rule.name = "shaft";
    rule.width = 3;
    rule.height = 2;
    rule.floor.rules.push_back(FloorRule{FloorRule::Kind::FullFlash, "rock"});
    rule.floor.rules.push_back(FloorRule{FloorRule::Kind::FullFlash, "dirt"});
    rules.add(std::move(rule));

    Fixture content;
    content.add_floor("rock");
    content.add_floor("dirt");
    MineWorld world(3, LayerShape{3, 2, 1});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(layer.plot_count(), 6u);
    for (i32 y = 0; y < 2; ++y) {
        for (i32 x = 0; x < 3; ++x) {
            T2D_CHECK_EQ(layer.ref_at(0, GridPos{x, y}).id, content.registry.find(ContentKind::Floor, "dirt"));
        }
    }
}

T2D_TEST(a_pack_of_layer_rules_is_a_story_the_world_can_enter) {
    // End to end, the way the game does it: a pack directory on disk, the pipeline that loads it, and a
    // world that enters the layer the data describes.
    Scratch scratch;
    scratch.write("story/layers.ecfg", R"(
floor::
    dirt::

layer::
    entrance::
        size::
            width:4
            height:3
        floor::
            full_flash:"dirt"
    shaft::
        size::
            width:2
            height:2
        floor::
            full_flash:"dirt"
)");

    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.packs, 1u);
    T2D_REQUIRE(report.layers.size() == 2u);

    // The story's order and the registry's ids come from the same walk of the same file, so the nth
    // story layer is the content id n+1 - which is what a save, which stores the name -> id table it
    // was written with, needs to keep pointing at the same layer.
    T2D_CHECK_EQ(report.layers.at(0)->name, std::string("entrance"));
    T2D_CHECK_EQ(report.layers.at(1)->name, std::string("shaft"));
    T2D_CHECK_EQ(registry.find(ContentKind::Layer, "entrance"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Layer, "shaft"), 2u);

    MineWorld world(2024, LayerShape{16, 16, 1});
    world.set_generator(story_layer_generator(report.layers, world.shape()));
    const MineLayer& entrance = world.enter(0, registry, report.definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(entrance.width(), 4u);
    T2D_CHECK_EQ(entrance.height(), 3u);
    T2D_CHECK_EQ(entrance.plot_count(), 12u);
    T2D_CHECK(entrance.plot_at(0, GridPos{3, 2}) != nullptr);

    // Walking down the mine: the second layer is the second entry, and it is a different map.
    const MineLayer& shaft = world.enter(1, registry, report.definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(shaft.width(), 2u);
    T2D_CHECK_EQ(shaft.height(), 2u);
    T2D_CHECK_EQ(shaft.plot_count(), 4u);
    T2D_CHECK_EQ(shaft.index(), 1);

    // A layer the story does not describe is empty, and says so by being empty: the screen explains it
    // (app.cpp), the generator cannot.
    const MineLayer& past_the_end = world.enter(2, registry, report.definitions);
    T2D_CHECK(world.build_report().clean());
    T2D_CHECK_EQ(past_the_end.plot_count(), 0u);
    T2D_CHECK_EQ(past_the_end.width(), 16u);
}

T2D_TEST(a_layer_rule_a_pack_cannot_read_is_reported_by_that_pack) {
    // A field the engine does not know *outside* the two tables it defines is the designer's, and a
    // pack that has one is a pack that loads: this is what "the entry's own fields belong to the
    // designer" means (types/tile_definition.h does the same for image and random_reverse).
    Scratch scratch;
    scratch.write("story/layers.ecfg", R"(
layer::
    entrance::
        name:"第一层"
        resources::
            ore:12
        size::
            width:4
            height:4
        floor::
            full_flash:"dirt"
)");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.layers.size(), 1u);

    // A rule the engine *does* read and cannot read is the other case: the pack is not ok, the reason
    // names the field, and the layer is not in the story - a rule that cannot be read is not half read.
    Scratch broken;
    broken.write("story/layers.ecfg", R"(
layer::
    entrance::
        floor::
            full_flash:3
)");
    ContentRegistry other_registry;
    ContentPipeline other_pipeline;
    other_pipeline.set_pack_directories({broken.root()});
    const ContentPipelineReport& broken_report = other_pipeline.load(other_registry);
    T2D_CHECK_FALSE(broken_report.clean());
    T2D_CHECK(mentions(broken_report.errors, "full_flash must be the quoted name of a floor"));
    T2D_CHECK_EQ(broken_report.layers.size(), 0u);
    T2D_REQUIRE(broken_report.packs == 1u);
    T2D_REQUIRE(other_pipeline.packs().size() == 1u);
    // The pack loaded - its files parsed and its names are in the registry - and it lost the rule doing
    // it, which is what content_list.h calls a partial source: ok, with the message kept.
    T2D_CHECK(other_pipeline.packs()[0].ok);
    T2D_CHECK_FALSE(other_pipeline.packs()[0].error.empty());
}

T2D_TEST(the_story_fixture_the_documents_show_loads_and_is_two_layers) {
    // The one story pack that lives in the repository, and the one the documents tell a designer to run
    // (docs/MODS.md section 0): it is a **test double** - the game's own content ships no layers at all
    // (test_content_pack pins that) - so a test keeps it loadable rather than letting a screenshot go
    // stale behind it.
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_packs({std::string(T2D_SOURCE_DIR) + "/games/mine/content"});
    pipeline.set_pack_directories({std::string(T2D_SOURCE_DIR) + "/games/mine/tests/data/story_content"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.layers.size(), 2u);
    T2D_REQUIRE(report.layers.at(0) != nullptr);
    T2D_CHECK_EQ(report.layers.at(0)->name, std::string("entrance"));
    T2D_CHECK_EQ(report.layers.at(0)->width, 48u);
    T2D_CHECK_EQ(report.layers.at(0)->height, 32u);
    T2D_CHECK_EQ(report.layers.at(0)->tile_layers, 2);

    // It names the game's own floor, which is what a pack adding to a story does: add layers, not content.
    MineWorld world(1, LayerShape{40, 24, 1});
    world.set_generator(story_layer_generator(report.layers, world.shape()));
    const MineLayer& layer = world.enter(0, registry, report.definitions);
    T2D_CHECK_MSG(world.build_report().clean(), "the build refused {} placement(s)",
                  world.build_report().refused);
    T2D_CHECK_EQ(layer.plot_count(), 1536u);
    T2D_CHECK_EQ(layer.mirrored_count(), 769u);
}

T2D_TEST_MAIN
