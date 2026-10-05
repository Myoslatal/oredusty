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

/// A registry with the content a test's files would have produced.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    void add(ContentKind kind, std::string_view name, bool random_reverse = false) {
        registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        definition.random_reverse = random_reverse;
        definitions.add(std::move(definition));
    }
    void add_floor(std::string_view name, bool random_reverse = false) {
        add(ContentKind::Floor, name, random_reverse);
    }
    void add_ore(std::string_view name) { add(ContentKind::Ore, name); }
};

/// Where the ore of \p layer sits, in the order the layer holds it: what "the same seed scatters the
/// same way" compares.
[[nodiscard]] std::vector<GridPos> ore_cells(const MineLayer& layer) {
    std::vector<GridPos> cells;
    for (const std::unique_ptr<SceneTile>& plot : layer.plots()) {
        if (plot->is_ore()) cells.push_back(plot->anchor());
    }
    return cells;
}

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

T2D_TEST(a_scatter_is_read_from_the_data) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            tile_layers:2
        floor::
            full_flash:"dirt"
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                layer:1
                density:0.02
                min:8
                max:20
                floor:"dirt"
            loose::
                kind:"structure"
                content:"rubble"
                density:0
)");
    T2D_CHECK(parsed.errors.empty());
    T2D_REQUIRE(parsed.rules.size() == 1u);
    const LayerRule& rule = parsed.rules[0];
    T2D_REQUIRE(rule.scatter.size() == 2u);

    const ScatterRule& copper = rule.scatter[0];
    T2D_CHECK_EQ(copper.name, std::string("copper"));
    T2D_CHECK_EQ(copper.kind, ContentKind::Ore);
    T2D_CHECK_EQ(copper.content, std::string("copper"));
    T2D_CHECK_EQ(copper.layer, 1);
    T2D_CHECK_EQ(copper.min, 8u);
    T2D_REQUIRE(copper.max.has_value());
    T2D_CHECK_EQ(*copper.max, 20u);
    T2D_CHECK_NEAR(copper.density, 0.02, 1e-9);
    T2D_CHECK_EQ(copper.floor, std::string("dirt"));

    // What is not written has a meaning rather than a default that hides: no layer is tile layer 0, no
    // min is no floor on the count, no max is "as many as fit", no floor is "any free cell".
    const ScatterRule& loose = rule.scatter[1];
    T2D_CHECK_EQ(loose.kind, ContentKind::Structure);
    T2D_CHECK_EQ(loose.layer, 0);
    T2D_CHECK_EQ(loose.min, 0u);
    T2D_CHECK_FALSE(loose.max.has_value());
    T2D_CHECK_EQ(loose.density, 0.0);
    T2D_CHECK(loose.floor.empty());
}

T2D_TEST(a_scatter_that_cannot_be_read_is_refused) {
    const auto one = [](std::string_view body) {
        return parse(std::format("layer::\n    entrance::\n        scatter::\n            copper::\n{}\n", body));
    };

    const Parsed no_content = one("                kind:\"ore\"\n                density:0.1");
    T2D_CHECK(no_content.rules.empty());
    T2D_CHECK(mentions(no_content.errors, "does not say what it places"));

    const Parsed no_kind = one("                content:\"copper\"\n                density:0.1");
    T2D_CHECK(no_kind.rules.empty());
    T2D_CHECK(mentions(no_kind.errors, "does not say which kind of content"));

    // A density is what makes a scatter a scatter: without one it would place nothing, which is a typo
    // rather than a rule.
    const Parsed no_density = one("                kind:\"ore\"\n                content:\"copper\"");
    T2D_CHECK(no_density.rules.empty());
    T2D_CHECK(mentions(no_density.errors, "does not say how dense it is"));

    const Parsed not_a_kind = one("                kind:\"ores\"\n                content:\"copper\"\n"
                                  "                density:0.1");
    T2D_CHECK(not_a_kind.rules.empty());
    T2D_CHECK(mentions(not_a_kind.errors, "'ores', which is not a content kind"));

    // A scatter places plots: an item or a recipe is not something that occupies a cell.
    const Parsed not_a_tile = one("                kind:\"item\"\n                content:\"copper\"\n"
                                  "                density:0.1");
    T2D_CHECK(not_a_tile.rules.empty());
    T2D_CHECK(mentions(not_a_tile.errors, "only floors, ores, structures and machines occupy cells"));

    const Parsed bad_density = one("                kind:\"ore\"\n                content:\"copper\"\n"
                                   "                density:5");
    T2D_CHECK(bad_density.rules.empty());
    T2D_CHECK(mentions(bad_density.errors, "a density is the fraction of the eligible cells"));

    const Parsed text_density = one("                kind:\"ore\"\n                content:\"copper\"\n"
                                    "                density:\"a lot\"");
    T2D_CHECK(text_density.rules.empty());
    T2D_CHECK(mentions(text_density.errors, "takes a number for density"));

    const Parsed backwards = one("                kind:\"ore\"\n                content:\"copper\"\n"
                                 "                density:0.1\n                min:20\n                max:8");
    T2D_CHECK(backwards.rules.empty());
    T2D_CHECK(mentions(backwards.errors, "wants at least 20 and at most 8"));

    const Parsed negative = one("                kind:\"ore\"\n                content:\"copper\"\n"
                                "                density:0.1\n                min:-1");
    T2D_CHECK(negative.rules.empty());
    T2D_CHECK(mentions(negative.errors, "a count is not negative"));

    const Parsed typo = one("                kind:\"ore\"\n                content:\"copper\"\n"
                            "                density:0.1\n                densitiy:0.2");
    T2D_CHECK(typo.rules.empty());
    T2D_CHECK(mentions(typo.errors, "has no attribute 'densitiy'"));

    const Parsed not_a_table = parse(R"(
layer::
    entrance::
        scatter::
            copper:"ore"
)");
    T2D_CHECK(not_a_table.rules.empty());
    T2D_CHECK(mentions(not_a_table.errors, "is a table of what it places"));

    // A tile layer this map does not have, and a floor restriction under a layer that lays no floor:
    // both are about more than one table, so both are checked once everything is read.
    const Parsed beyond = parse(R"(
layer::
    entrance::
        size::
            tile_layers:1
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                density:0.1
                layer:1
)");
    T2D_CHECK(beyond.rules.empty());
    T2D_CHECK(mentions(beyond.errors, "the scatter 'copper' works on tile layer 1, and this layer has 1"));

    const Parsed no_floor = parse(R"(
layer::
    entrance::
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                density:0.1
                floor:"dirt"
)");
    T2D_CHECK(no_floor.rules.empty());
    T2D_CHECK(mentions(no_floor.errors, "this layer has no floor creator"));
}

T2D_TEST(a_scatter_places_one_plot_per_cell_on_its_own_tile_layer) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:8
            height:6
            tile_layers:2
        floor::
            layer:0
            full_flash:"dirt"
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                layer:1
                density:1
                floor:"dirt"
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt");
    content.add_ore("copper");
    MineWorld world(4, LayerShape{8, 6, 1});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    T2D_CHECK_MSG(world.build_report().clean(), "{}", world.build_report().errors.empty()
                                                       ? std::string{}
                                                       : world.build_report().errors.front());

    // Every cell: the floor below, one ore above it. A density of 1 takes every eligible cell, and
    // every cell is eligible because the whole map is dirt.
    T2D_CHECK_EQ(layer.plot_count(), 48u + 48u);
    T2D_CHECK_EQ(layer.filled_cells(0), 48u);
    T2D_CHECK_EQ(layer.filled_cells(1), 48u);
    for (i32 y = 0; y < 6; ++y) {
        for (i32 x = 0; x < 8; ++x) {
            const SceneTile* floor_plot = layer.plot_at(0, GridPos{x, y});
            T2D_REQUIRE(floor_plot != nullptr);
            T2D_CHECK(floor_plot->is_floor());
            const SceneTile* ore = layer.plot_at(1, GridPos{x, y});
            T2D_REQUIRE(ore != nullptr);
            T2D_CHECK(ore->is_ore());
            T2D_CHECK_EQ(ore->kind(), ContentKind::Ore);
            T2D_CHECK_EQ(ore->id(), content.registry.find(ContentKind::Ore, "copper"));
            T2D_CHECK_EQ(ore->cell_count(), 1);
        }
    }
    T2D_CHECK_EQ(ore_cells(layer).size(), 48u);
}

T2D_TEST(the_density_sets_the_count_and_min_and_max_bound_it) {
    const auto build = [](std::string_view scatter_body, LayerShape shape = LayerShape{8, 8, 2}) {
        const Parsed parsed = parse(std::format("layer::\n    entrance::\n        size::\n"
                                                "            width:{}\n            height:{}\n"
                                                "            tile_layers:{}\n        floor::\n"
                                                "            full_flash:\"dirt\"\n        scatter::\n"
                                                "            copper::\n                kind:\"ore\"\n"
                                                "                content:\"copper\"\n{}\n",
                                                shape.width, shape.height, shape.tile_layers, scatter_body));
        Fixture content;
        content.add_floor("dirt");
        content.add_ore("copper");
        LayerRules rules;
        for (const LayerRule& rule : parsed.rules) rules.add(rule);
        MineWorld world(7, shape);
        world.set_generator(story_layer_generator(rules, world.shape()));
        const MineLayer& layer = world.enter(0, content.registry, content.definitions);
        return ore_cells(layer).size();
    };

    // Every eligible cell, then a bound on top of it.
    T2D_CHECK_EQ(build("                layer:1\n                density:1"), 64u);
    T2D_CHECK_EQ(build("                layer:1\n                density:1\n                max:7"), 7u);
    // No density at all with a minimum is "exactly that many, anywhere" - the dice pick which cells,
    // not how many.
    T2D_CHECK_EQ(build("                layer:1\n                density:0\n                min:5"), 5u);
    T2D_CHECK_EQ(build("                layer:1\n                density:0\n                min:5\n"
                       "                max:5"),
                 5u);
    // A map that cannot hold what the rule asked for wins over both bounds.
    T2D_CHECK_EQ(build("                layer:1\n                density:1\n                min:10\n"
                       "                max:200"),
                 64u);
    // In between, the density is an average: the count varies around density x cells and stays inside
    // the bounds. 4096 cells at a quarter is 1024 +- 28, so a band of 4.5 sigma is a wide one.
    const usize middling = build("                layer:1\n                density:0.25\n                min:900\n"
                                 "                max:1150",
                                 LayerShape{64, 64, 2});
    T2D_CHECK_GT(middling, 900u);
    T2D_CHECK_LT(middling, 1150u);
}

T2D_TEST(a_floor_restriction_only_takes_cells_whose_floor_is_that_one) {
    /// What one run of the rule did: how many ore cells there are, and what the build said.
    struct Built {
        usize ore = 0;
        LayerBuildReport report;
    };
    const auto build = [](std::string_view restriction) {
        const Parsed parsed = parse(std::format("layer::\n    entrance::\n        size::\n"
                                                "            width:8\n            height:8\n"
                                                "            tile_layers:2\n        floor::\n"
                                                "            full_flash:\"dirt\"\n        scatter::\n"
                                                "            copper::\n                kind:\"ore\"\n"
                                                "                content:\"copper\"\n"
                                                "                layer:1\n                density:1\n"
                                                "                min:1\n{}",
                                                restriction));
        Fixture content;
        content.add_floor("dirt");
        content.add_ore("copper");
        LayerRules rules;
        for (const LayerRule& rule : parsed.rules) rules.add(rule);
        MineWorld world(7, LayerShape{8, 8, 2});
        world.set_generator(story_layer_generator(rules, world.shape()));
        const MineLayer& layer = world.enter(0, content.registry, content.definitions);
        return Built{ore_cells(layer).size(), world.build_report()};
    };

    // The floor of the cell decides. Under a floor of dirt every cell is eligible; under a floor of
    // stone - which is what the layer is made of instead - none of them is.
    const Built dirt = build("                floor:\"dirt\"\n");
    T2D_CHECK_MSG(dirt.report.clean(), "{}", dirt.report.errors.empty() ? std::string{}
                                                                       : dirt.report.errors.front());
    T2D_CHECK_EQ(dirt.ore, 64u);

    // A restriction nothing satisfies is a rule that places nothing, and it says so rather than being
    // quietly empty: the minimum is a wish, the restriction is a rule.
    const Built stone = build("                floor:\"stone\"\n");
    T2D_CHECK_EQ(stone.ore, 0u);
    T2D_CHECK_FALSE(stone.report.clean());
    T2D_REQUIRE(!stone.report.errors.empty());
    T2D_CHECK(stone.report.errors[0].find("asked for at least 1 and 0 cell(s) were eligible") !=
              std::string::npos);
}

T2D_TEST(nothing_lands_where_the_description_already_has_something) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:8
            height:8
            tile_layers:2
        floor::
            layer:0
            full_flash:"dirt"
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                layer:1
                density:1
                max:10
            coal::
                kind:"ore"
                content:"coal"
                layer:1
                density:1
                max:10
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt");
    content.add_ore("copper");
    content.add_ore("coal");
    MineWorld world(4, LayerShape{8, 8, 2});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    T2D_CHECK_MSG(world.build_report().clean(), "{}", world.build_report().errors.empty()
                                                       ? std::string{}
                                                       : world.build_report().errors.front());

    // Ten of each, and no cell carries two: a scatter never lands on a cell of its own tile layer that
    // the same description already holds.
    T2D_CHECK_EQ(layer.plot_count(), 64u + 20u);
    T2D_CHECK_EQ(layer.filled_cells(1), 20u);
    std::vector<GridPos> cells = ore_cells(layer);
    T2D_REQUIRE(cells.size() == 20u);
    std::sort(cells.begin(), cells.end(), [](GridPos a, GridPos b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    for (usize index = 1; index < cells.size(); ++index) {
        T2D_CHECK_FALSE(cells[index] == cells[index - 1]);
    }

    // A scatter on the floor's own tile layer finds every cell taken, so a rule that asks for three of
    // them gets none and says so.
    const Parsed on_the_floor = parse(R"(
layer::
    entrance::
        size::
            width:4
            height:4
        floor::
            layer:0
            full_flash:"dirt"
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                layer:0
                density:1
                min:3
)");
    T2D_CHECK(on_the_floor.errors.empty());
    LayerRules on_floor_rules;
    for (const LayerRule& rule : on_the_floor.rules) on_floor_rules.add(rule);
    MineWorld other(4, LayerShape{4, 4, 1});
    other.set_generator(story_layer_generator(on_floor_rules, other.shape()));
    const MineLayer& floored = other.enter(0, content.registry, content.definitions);
    T2D_CHECK_EQ(floored.plot_count(), 16u);
    T2D_CHECK_EQ(floored.filled_cells(0), 16u);
    T2D_REQUIRE(!other.build_report().errors.empty());
    T2D_CHECK(other.build_report().errors[0].find("asked for at least 3 and 0 cell(s) were eligible") !=
              std::string::npos);
}

T2D_TEST(the_same_seed_scatters_the_same_way_and_another_seed_moves_it) {
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:16
            height:16
            tile_layers:2
        floor::
            full_flash:"dirt"
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                layer:1
                density:0.1
                min:4
                max:40
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt");
    content.add_ore("copper");
    const auto scatter_of = [&](u64 seed) {
        MineWorld world(seed, LayerShape{16, 16, 2});
        world.set_generator(story_layer_generator(rules, world.shape()));
        const MineLayer& layer = world.enter(0, content.registry, content.definitions);
        T2D_CHECK_MSG(world.build_report().clean(), "{}", world.build_report().errors.empty()
                                                           ? std::string{}
                                                           : world.build_report().errors.front());
        return ore_cells(layer);
    };

    const std::vector<GridPos> first = scatter_of(99);
    T2D_CHECK_GT(first.size(), 3u);
    T2D_CHECK_LT(first.size(), 41u);
    // The same seed gives the same cells, in the same order: a mine is reproducible.
    T2D_CHECK(scatter_of(99) == first);
    // Another seed gives the same kind of layer and puts the ore somewhere else: the rules are the
    // designer's, the dice are the layer's (world.h).
    const std::vector<GridPos> other = scatter_of(1234);
    T2D_CHECK_NE(other, first);
    T2D_CHECK_GT(other.size(), 3u);
    T2D_CHECK_LT(other.size(), 41u);
}

T2D_TEST(a_scatter_on_a_tile_layer_the_map_does_not_have_says_so) {
    // The data does not state how many tile layers the map has, so the world's shape decides - and a
    // rule that names a tile layer the map does not have is about the description, not about a
    // placement: it is reported once, by the generator, and nothing is placed on it.
    const Parsed parsed = parse(R"(
layer::
    entrance::
        size::
            width:6
            height:6
        floor::
            layer:1
            full_flash:"dirt"
        scatter::
            copper::
                kind:"ore"
                content:"copper"
                layer:2
                density:1
)");
    T2D_CHECK(parsed.errors.empty());
    LayerRules rules;
    for (const LayerRule& rule : parsed.rules) rules.add(rule);

    Fixture content;
    content.add_floor("dirt");
    content.add_ore("copper");
    MineWorld world(1, LayerShape{6, 6, 1});
    world.set_generator(story_layer_generator(rules, world.shape()));
    const MineLayer& layer = world.enter(0, content.registry, content.definitions);
    const LayerBuildReport& report = world.build_report();
    T2D_CHECK_EQ(layer.plot_count(), 0u);
    T2D_CHECK_EQ(report.refused, 2u);
    T2D_REQUIRE(report.errors.size() == 2u);
    T2D_CHECK(report.errors[0].find("the floor creator works on tile layer 1, and this layer has 1") !=
              std::string::npos);
    T2D_CHECK(report.errors[1].find("the scatter 'copper' works on tile layer 2, and this layer has 1") !=
              std::string::npos);
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
    T2D_CHECK_EQ(layer.mirrored_count(), 769u);

    // The floor it filled, and the ore the scatter put on the tile layer above it: a density of 0.01
    // over 1536 cells is around fifteen of them, bounded by the rule's own min and max.
    const std::vector<GridPos> ore = ore_cells(layer);
    T2D_CHECK_EQ(layer.plot_count(), 1536u + ore.size());
    T2D_CHECK_GT(ore.size(), 5u);
    T2D_CHECK_LT(ore.size(), 41u);
    for (const GridPos cell : ore) {
        const SceneTile* plot = layer.plot_at(1, cell);
        T2D_REQUIRE(plot != nullptr);
        T2D_CHECK(plot->is_ore());
        T2D_CHECK_EQ(plot->id(), registry.find(ContentKind::Ore, "copper"));
        // What the restriction is for: every ore sits on the game's own dirt, because that is the only
        // floor the layer has.
        const SceneTile* below = layer.plot_at(0, cell);
        T2D_REQUIRE(below != nullptr);
        T2D_CHECK(below->is_floor());
    }
}

T2D_TEST_MAIN
