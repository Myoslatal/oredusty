// Content packs: a directory per pack, an optional header that says who it is, and a pipeline that
// puts the game's own content, the packs and the mods into one registry in a defined order.
#include <mine/content_pack.h>
#include <mine/content_loader.h>
#include <mine/registry.h>

#include <support/test_support.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace mine;
namespace fs = std::filesystem;

namespace {

/// A scratch directory of packs, removed when the test ends.
class Scratch {
public:
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / std::format("t2d_packs_{}", stamp);
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

[[nodiscard]] bool mentions(const std::vector<std::string>& lines, std::string_view needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] const ContentPack* find_pack(const ContentPipeline& pipeline, std::string_view id) {
    for (const ContentPack& pack : pipeline.packs()) {
        if (pack.id == id) return &pack;
    }
    return nullptr;
}

} // namespace

T2D_TEST(a_directory_of_packs_loads_in_dependency_order) {
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({T2D_TEST_PACKS_DIR});
    const ContentPipelineReport& report = pipeline.load(registry);

    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.packs, 3u);
    T2D_CHECK_EQ(report.pack_content, 5u);
    T2D_CHECK_EQ(report.total_content, 5u);
    T2D_REQUIRE(pipeline.packs().size() == 3u);

    // Directory order is kept wherever it is already valid, and only what must move moves:
    // "00_dependent" sorts first but requires "01_base", so it is loaded after it. That keeps ids
    // predictable for a designer who numbered their pack directories on purpose.
    T2D_CHECK_EQ(pipeline.packs()[0].id, std::string("base_pack"));
    T2D_CHECK_EQ(pipeline.packs()[1].id, std::string("anonymous"));
    T2D_CHECK_EQ(pipeline.packs()[2].id, std::string("dependent_pack"));
    {
        usize base_index = 0, dependent_index = 0;
        for (usize index = 0; index < pipeline.packs().size(); ++index) {
            if (pipeline.packs()[index].id == "base_pack") base_index = index;
            if (pipeline.packs()[index].id == "dependent_pack") dependent_index = index;
        }
        T2D_CHECK_LT(base_index, dependent_index);
    }
    for (const ContentPack& pack : pipeline.packs()) T2D_CHECK(pack.ok);

    const ContentPack* base = find_pack(pipeline, "base_pack");
    T2D_REQUIRE(base != nullptr);
    T2D_CHECK(base->declared);
    T2D_CHECK_EQ(base->name, std::string("Base pack"));
    T2D_CHECK_EQ(base->version, std::string("2.0"));
    T2D_CHECK_EQ(base->registered.size(), 2u);
    // A pack holds as many content files as it likes: two here, and both are read.
    T2D_CHECK_EQ(base->files.size(), 2u);

    // Ids follow registration order: the base pack's content first, because it is loaded first.
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "pack_ore"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "pack_floor"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "pack_tower"), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Machine, "pack_lift"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "pack_scrap"), 2u);

    // The header is a file of its own, so it is never reported as a table that is not a content kind.
    T2D_CHECK_FALSE(mentions(report.warnings, "is not a content kind"));
}

T2D_TEST(a_pack_without_a_header_takes_its_name_from_its_directory) {
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({std::string(T2D_TEST_PACKS_DIR) + "/anonymous"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(pipeline.packs().size() == 1u);
    T2D_CHECK_FALSE(pipeline.packs()[0].declared);
    T2D_CHECK_EQ(pipeline.packs()[0].id, std::string("anonymous"));
    T2D_CHECK_EQ(pipeline.packs()[0].name, std::string("anonymous"));
    T2D_CHECK_EQ(report.pack_content, 1u);
    T2D_CHECK(registry.find(ContentKind::Item, "pack_scrap") != kNoContent);

    // A directory that is not there is reported - a path a caller named is not a default.
    ContentRegistry other_registry;
    ContentPipeline other;
    other.set_pack_directories({"/nonexistent/packs"});
    const ContentPipelineReport& missing = other.load(other_registry);
    T2D_CHECK_FALSE(missing.clean());
    T2D_CHECK(mentions(missing.errors, "does not exist"));
}

T2D_TEST(a_pack_is_a_directory_and_a_loose_file_is_not_one) {
    // The single file pack is the shape this used to have. It is refused with the shape it has to be
    // now, because a file that is silently not loaded is content that silently disappeared.
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({std::string(T2D_TEST_PACKS_DIR) + "/01_base/items.ecfg"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(mentions(report.errors, "is a file: a content pack is a directory"));

    // The same file loose at the top of a workspace that does hold packs: reported too, and the packs
    // around it still load.
    Scratch scratch;
    scratch.write("loose.ecfg", "item::\n    loose_thing::\n");
    scratch.write("real_pack/items.ecfg", "item::\n    real_thing::\n");
    ContentRegistry mixed_registry;
    ContentPipeline mixed;
    mixed.set_pack_directories({scratch.root()});
    const ContentPipelineReport& mixed_report = mixed.load(mixed_registry);
    T2D_CHECK_FALSE(mixed_report.clean());
    T2D_CHECK(mentions(mixed_report.errors, "is a loose file"));
    T2D_CHECK_EQ(mixed_report.packs, 1u);
    T2D_CHECK(mixed_registry.find(ContentKind::Item, "real_thing") != kNoContent);
    T2D_CHECK_EQ(mixed_registry.find(ContentKind::Item, "loose_thing"), kNoContent);

    // The old header inside a content file is told where it went.
    Scratch moved;
    moved.write("old_style/pack.ecfg", "id:\"old_style\"\n");
    moved.write("old_style/items.ecfg", "pack::\n    id:\"old_style\"\nitem::\n    a::\n");
    ContentRegistry moved_registry;
    ContentPipeline moved_pipeline;
    moved_pipeline.set_pack_directories({moved.root()});
    const ContentPipelineReport& moved_report = moved_pipeline.load(moved_registry);
    T2D_CHECK_FALSE(moved_report.clean());
    T2D_CHECK(mentions(moved_report.errors, "the pack header is 'pack.ecfg' now"));

    // And the old table inside the header file itself is refused as a key, with the keys it takes.
    Scratch nested;
    nested.write("nested/pack.ecfg", "pack::\n    id:\"nested\"\n");
    nested.write("nested/items.ecfg", "item::\n    a::\n");
    ContentRegistry nested_registry;
    ContentPipeline nested_pipeline;
    nested_pipeline.set_pack_directories({nested.root()});
    const ContentPipelineReport& nested_report = nested_pipeline.load(nested_registry);
    T2D_CHECK_FALSE(nested_report.clean());
    T2D_CHECK(mentions(nested_report.errors, "'pack' is not a pack key"));
}

T2D_TEST(a_pack_that_requires_something_missing_is_refused) {
    Scratch scratch;
    scratch.write("needy/pack.ecfg", "id:\"needy\"\nrequires:[\"nope\"]\n");
    scratch.write("needy/items.ecfg", "item::\n    needy_thing::\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(mentions(report.errors, "'needy' requires 'nope', which is not loaded"));
    T2D_REQUIRE(pipeline.packs().size() == 1u);
    T2D_CHECK_FALSE(pipeline.packs()[0].ok);
    // Its content is not registered: a pack whose requirements are missing must not half load.
    T2D_CHECK_EQ(registry.total_count(), 0u);
    T2D_CHECK_EQ(report.pack_content, 0u);

    // Two packs that require each other are a cycle, and neither loads.
    Scratch cyclic;
    cyclic.write("alpha/pack.ecfg", "id:\"alpha\"\nrequires:[\"beta\"]\n");
    cyclic.write("alpha/items.ecfg", "item::\n    a::\n");
    cyclic.write("beta/pack.ecfg", "id:\"beta\"\nrequires:[\"alpha\"]\n");
    cyclic.write("beta/items.ecfg", "item::\n    b::\n");
    ContentRegistry cyclic_registry;
    ContentPipeline cyclic_pipeline;
    cyclic_pipeline.set_pack_directories({cyclic.root()});
    const ContentPipelineReport& cycle = cyclic_pipeline.load(cyclic_registry);
    T2D_CHECK(mentions(cycle.errors, "form a cycle"));
    T2D_CHECK_EQ(cycle.packs, 0u);
    T2D_CHECK_EQ(cyclic_registry.total_count(), 0u);
}

T2D_TEST(a_pack_header_that_is_wrong_is_refused) {
    const struct {
        const char* name;
        const char* header;
        const char* expected;
    } cases[] = {
        {"unknownkey", "id:\"a\"\ntier:3\n", "'tier' is not a pack key"},
        {"badid", "id:3\n", "id must be a non empty string"},
        {"emptyid", "id:\"\"\n", "id must be a non empty string"},
        {"badname", "id:\"a\"\nname:3\n", "name must be a string"},
        {"badrequires", "id:\"a\"\nrequires:3\n", "must be a name or a list of names"},
    };
    for (const auto& test_case : cases) {
        Scratch scratch;
        scratch.write("a_pack/pack.ecfg", test_case.header);
        scratch.write("a_pack/items.ecfg", "item::\n    a::\n");
        ContentRegistry registry;
        ContentPipeline pipeline;
        pipeline.set_pack_directories({scratch.root()});
        const ContentPipelineReport& report = pipeline.load(registry);
        T2D_CHECK_MSG(!report.clean(), "{} was accepted", test_case.name);
        T2D_CHECK_MSG(mentions(report.errors, test_case.expected), "{}: errors were {}", test_case.name,
                      report.first_error());
        T2D_CHECK_EQ(pipeline.packs().size(), 0u);
    }

    // A pack that declares itself and holds nothing is refused: a pack is the content in it.
    Scratch empty;
    empty.write("hollow/pack.ecfg", "id:\"hollow\"\n");
    ContentRegistry empty_registry;
    ContentPipeline empty_pipeline;
    empty_pipeline.set_pack_directories({empty.root()});
    const ContentPipelineReport& empty_report = empty_pipeline.load(empty_registry);
    T2D_CHECK_FALSE(empty_report.clean());
    T2D_CHECK(mentions(empty_report.errors, "holds no .ecfg file"));
}

T2D_TEST(two_packs_with_the_same_id_and_a_pack_that_redefines_content_are_reported) {
    {
        Scratch scratch;
        scratch.write("one/pack.ecfg", "id:\"same\"\n");
        scratch.write("one/items.ecfg", "item::\n    one_thing::\n");
        scratch.write("two/pack.ecfg", "id:\"same\"\n");
        scratch.write("two/items.ecfg", "item::\n    two_thing::\n");
        ContentRegistry registry;
        ContentPipeline pipeline;
        pipeline.set_pack_directories({scratch.root()});
        const ContentPipelineReport& report = pipeline.load(registry);
        T2D_CHECK(mentions(report.errors, "is defined twice"));
        T2D_CHECK_EQ(report.packs, 1u);
        T2D_CHECK(registry.find(ContentKind::Item, "one_thing") != kNoContent);
        T2D_CHECK_EQ(registry.find(ContentKind::Item, "two_thing"), kNoContent);
    }
    {
        // The game's own content is registered first, so a pack can never take one of its names over.
        Scratch scratch;
        scratch.write("base/content.ecfg", "item::\n    shared_thing::\n");
        scratch.write("greedy/pack.ecfg", "id:\"greedy\"\n");
        scratch.write("greedy/items.ecfg", "item::\n    shared_thing::\n    its_own::\n");
        ContentRegistry registry;
        ContentPipeline pipeline;
        pipeline.set_base_packs({scratch.root() + "/base"});
        pipeline.set_pack_directories({scratch.root() + "/greedy"});
        const ContentPipelineReport& report = pipeline.load(registry);
        T2D_CHECK_EQ(report.base_registered, 1u);
        T2D_CHECK(mentions(report.errors, "item 'shared_thing' is already registered"));
        T2D_CHECK_EQ(registry.find(ContentKind::Item, "shared_thing"), 1u);
        T2D_CHECK_EQ(registry.find(ContentKind::Item, "its_own"), 2u);
        T2D_CHECK_EQ(report.pack_content, 1u);
    }
}

T2D_TEST(a_broken_pack_is_reported_with_its_position_and_the_others_still_load) {
    Scratch scratch;
    scratch.write("good/items.ecfg", "item::\n    fine::\n");
    scratch.write("bad/items.ecfg", "item::\n    good_one::\n    bad_one:unquoted\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(mentions(report.errors, "bad/items.ecfg"));
    T2D_CHECK(mentions(report.errors, ":3:"));   // the line the reader refused
    T2D_CHECK_EQ(report.packs, 1u);              // the good one
    T2D_CHECK(registry.find(ContentKind::Item, "fine") != kNoContent);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "good_one"), kNoContent);

    // A table that is not a content kind is a warning, and the pack still loads.
    Scratch typo;
    typo.write("typo/items.ecfg", "structurs::\n    wall::\nitem::\n    ore::\n");
    ContentRegistry typo_registry;
    ContentPipeline typo_pipeline;
    typo_pipeline.set_pack_directories({typo.root()});
    const ContentPipelineReport& typo_report = typo_pipeline.load(typo_registry);
    T2D_CHECK_MSG(typo_report.clean(), "{}", typo_report.first_error());
    T2D_CHECK(mentions(typo_report.warnings, "'structurs' is not a content kind"));
    T2D_CHECK_EQ(typo_registry.count(ContentKind::Item), 1u);
}

T2D_TEST(the_pipeline_loads_the_game_first_then_packs_then_mods) {
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_packs({std::string(T2D_SOURCE_DIR) + "/games/mine/tests/data/placeholder_content"});
    pipeline.set_pack_directories({T2D_TEST_PACKS_DIR});
    pipeline.set_mod_directories({T2D_TEST_MODS_DIR});
    const ContentPipelineReport& report = pipeline.load(registry);

    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.base_packs, 1u);
    T2D_CHECK_EQ(report.base_registered, 6u);
    T2D_CHECK_EQ(report.pack_content, 5u);
    T2D_CHECK_GT(report.mods, 0u);
    T2D_CHECK_GT(report.mod_content, 0u);
    T2D_CHECK_EQ(report.total_content, report.base_registered + report.pack_content + report.mod_content);

    // The three sources keep their order: the game's content, then the packs, then the mods. Structure
    // ids therefore start with the game's, continue with the packs', and end with the mods'.
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "placeholder_wall"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "pack_floor"), 4u);
    T2D_CHECK_GT(registry.find(ContentKind::Structure, "mod_wall"), 5u);

    // A reload gives the same ids: the pipeline clears the registry and unloads the mods first.
    const usize entries = registry.total_count();
    const ContentId wall = registry.find(ContentKind::Structure, "placeholder_wall");
    const ContentId tower = registry.find(ContentKind::Structure, "pack_tower");
    const ModLoadReport before = pipeline.mods().report();
    const ContentPipelineReport& again = pipeline.load(registry);
    T2D_CHECK_MSG(again.clean(), "{}", again.first_error());
    T2D_CHECK_EQ(registry.total_count(), entries);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "placeholder_wall"), wall);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "pack_tower"), tower);
    T2D_CHECK_EQ(pipeline.mods().report().mods.size(), before.mods.size());

    pipeline.unload();
    T2D_CHECK(pipeline.packs().empty());
    T2D_CHECK(pipeline.mods().empty());
    T2D_CHECK_EQ(registry.total_count(), entries);   // unloading mods does not touch the registry
}

T2D_TEST(a_pack_image_is_resolved_and_checked) {
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({T2D_TEST_PACKS_DIR});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.pack_images, 4u);   // two in each of the two packs that have art
    T2D_CHECK_EQ(report.images.size(), 4u);
    for (const ResolvedImage& image : report.images) {
        T2D_CHECK_MSG(image.ok, "{}: {}", image.path, image.error);
        T2D_CHECK(image.resolved.find("packs") != std::string::npos);
        T2D_CHECK(image.resolved.find("art/") != std::string::npos);
        T2D_CHECK(image.resolved.ends_with(".png"));
        // A picture knows which file declared it: that is what a source's own line counts, and it is
        // what makes a path relative to *the file that declares it* work.
        T2D_CHECK(image.source.find("packs") != std::string::npos);
        T2D_CHECK(image.source.find(".ecfg") != std::string::npos);
    }
    T2D_CHECK_EQ(report.images[0].content, std::string("pack_ore"));
    T2D_CHECK_EQ(report.images[0].kind, ContentKind::Item);
    const ContentPack* base = find_pack(pipeline, "base_pack");
    T2D_REQUIRE(base != nullptr);
    T2D_CHECK_EQ(base->registered.size(), 2u);
    T2D_CHECK_EQ(base->images, 2u);

    // A picture that is not there, an empty path, and a file the engine cannot decode are all
    // reported: a content entry that cannot be drawn must not look like one that can.
    const struct {
        const char* name;
        const char* text;
        const char* expected;
    } cases[] = {
        {"missing", "item::\n    a::\n        image:\"art/nope.png\"\n", "is not there"},
        {"empty", "item::\n    a::\n        image:\"\"\n", "the image path is empty"},
        {"notpng", "item::\n    a::\n        image:\"art/thing.jpg\"\n", "only PNG is decoded"},
    };
    for (const auto& test_case : cases) {
        Scratch scratch;
        scratch.write("a_pack/items.ecfg", test_case.text);
        scratch.write("a_pack/art/thing.jpg", "not an image");
        ContentRegistry scratch_registry;
        ContentPipeline scratch_pipeline;
        scratch_pipeline.set_pack_directories({scratch.root()});
        const ContentPipelineReport& scratch_report = scratch_pipeline.load(scratch_registry);
        T2D_CHECK_MSG(!scratch_report.clean(), "{} was accepted", test_case.name);
        T2D_CHECK_MSG(mentions(scratch_report.errors, test_case.expected), "{}: errors were {}",
                      test_case.name, scratch_report.first_error());
        T2D_CHECK_EQ(scratch_report.pack_images, 0u);
        // The content itself still loads: a missing picture is a drawing problem, not a data problem.
        T2D_CHECK_EQ(scratch_registry.count(ContentKind::Item), 1u);
    }

    // A pack that says nothing about pictures has none, and is not bothered about it.
    ContentRegistry plain_registry;
    ContentPipeline plain;
    plain.set_pack_directories({std::string(T2D_TEST_PACKS_DIR) + "/anonymous"});
    const ContentPipelineReport& plain_report = plain.load(plain_registry);
    T2D_CHECK(plain_report.clean());
    T2D_CHECK_EQ(plain_report.pack_images, 0u);
    T2D_CHECK(plain_report.images.empty());
}

T2D_TEST(a_pack_holds_several_files_and_its_art_is_relative_to_each_of_them) {
    // The shape the designer asked for: a pack is a directory of .ecfg files and resources, not one
    // file. A path is relative to the file that declares it, so a pack can keep its art in one place
    // and its content in another.
    Scratch scratch;
    scratch.write("multi/pack.ecfg", "id:\"multi\"\nname:\"Several files\"\n");
    scratch.write("multi/items.ecfg", "item::\n    multi_ore::\n        image:\"art/ore.png\"\n");
    scratch.write("multi/art/ore.png", "pretend this is a png");
    scratch.write("multi/content/machines.ecfg",
                  "machine::\n    multi_drill::\n        image:\"../art/drill.png\"\n");
    scratch.write("multi/art/drill.png", "pretend this is a png too");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({scratch.root()});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(pipeline.packs().size() == 1u);
    const ContentPack& pack = pipeline.packs()[0];
    T2D_CHECK_EQ(pack.id, std::string("multi"));
    T2D_CHECK_EQ(pack.files.size(), 2u);
    T2D_CHECK_EQ(pack.documents.size(), 2u);
    T2D_CHECK_EQ(pack.registered.size(), 2u);
    T2D_CHECK_EQ(pack.images, 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 1u);
    T2D_CHECK_EQ(registry.count(ContentKind::Machine), 1u);
    // Both pictures resolved, each against the file that named it - the paths are relative to the
    // declaring file, so the same pack can keep its content in one directory and its art in another.
    T2D_REQUIRE(report.images.size() == 2u);
    const ResolvedImage* ore = nullptr;
    const ResolvedImage* drill = nullptr;
    for (const ResolvedImage& image : report.images) {
        if (image.content == "multi_ore") ore = &image;
        if (image.content == "multi_drill") drill = &image;
    }
    T2D_REQUIRE(ore != nullptr);
    T2D_REQUIRE(drill != nullptr);
    T2D_CHECK_MSG(ore->ok, "{}", ore->error);
    T2D_CHECK_MSG(drill->ok, "{}", drill->error);
    T2D_CHECK(ore->resolved.find("art/ore.png") != std::string::npos);
    T2D_CHECK(ore->source.find("items.ecfg") != std::string::npos);
    T2D_CHECK(drill->resolved.find("art/drill.png") != std::string::npos);
    T2D_CHECK(drill->source.find("content/machines.ecfg") != std::string::npos);
    // The definitions are kept for the world to build plots out of.
    T2D_CHECK(report.definitions.size() >= 1u);
}

T2D_TEST(the_games_own_content_is_a_pack_that_ships_its_own_art) {
    // The game's own content is the first stage of the load order and it is a pack like any other: a
    // directory with a header, content files and art, named relative to the file that declares it.
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_packs({std::string(T2D_SOURCE_DIR) + "/games/mine/content"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.base_packs, 1u);
    T2D_REQUIRE(report.images.size() == 1u);
    T2D_CHECK(report.images[0].ok);
    T2D_CHECK_EQ(report.images[0].kind, ContentKind::Floor);
    T2D_CHECK_EQ(report.images[0].content, std::string("dirt"));
    T2D_CHECK(report.images[0].resolved.find("floor_dirt.png") != std::string::npos);
    T2D_CHECK_EQ(report.base_images, 1u);
    T2D_CHECK_EQ(registry.count(ContentKind::Floor), 1u);
    T2D_CHECK(registry.find(ContentKind::Floor, "dirt") != kNoContent);
    // And it ships **no layer rules**: which layers a story has, how big they are and what their floors
    // are made of is the designer's data (docs/GAME_DESIGN.md sections 7.3, 7.8, 7.10), so the game's own
    // content is exactly the one floor above. A story layer that turned up here would be content this
    // repository invented.
    T2D_CHECK_EQ(report.layers.size(), 0u);
    T2D_CHECK_EQ(registry.count(ContentKind::Layer), 0u);
    // The header gives the game's own pack its name, so a content list can say what it is.
    T2D_REQUIRE(report.sources.size() == 1u);
    T2D_CHECK_EQ(report.sources[0].id, std::string("mine"));
    T2D_CHECK_EQ(report.sources[0].name, std::string("Mine"));
    T2D_CHECK(report.sources[0].ok);

    // A picture the game's own pack cannot find is reported like any other broken reference, and it is
    // the source's line in the content list that carries the message.
    Scratch scratch;
    scratch.write("mine/floor.ecfg", "floor::\n    sand::\n        image:\"art/sand.png\"\n");
    ContentRegistry missing_registry;
    ContentPipeline missing;
    missing.set_base_packs({scratch.root() + "/mine"});
    const ContentPipelineReport& missing_report = missing.load(missing_registry);
    T2D_CHECK_FALSE(missing_report.clean());
    T2D_CHECK(mentions(missing_report.errors, "is not there"));
    T2D_REQUIRE(missing_report.sources.size() == 1u);
    T2D_CHECK_FALSE(missing_report.sources[0].error.empty());
    T2D_CHECK_EQ(missing_report.sources[0].images, 0u);
    T2D_CHECK_EQ(missing_report.sources[0].images_failed, 1u);
    // The content itself still loads: a missing picture is a drawing problem, not a data problem.
    T2D_CHECK_EQ(missing_registry.count(ContentKind::Floor), 1u);

    // A base pack that is not there at all is reported as a source that failed, not as an empty game.
    ContentRegistry absent_registry;
    ContentPipeline absent;
    absent.set_base_packs({scratch.root() + "/not_here"});
    const ContentPipelineReport& absent_report = absent.load(absent_registry);
    T2D_CHECK_FALSE(absent_report.clean());
    T2D_CHECK(mentions(absent_report.errors, "is not a directory"));
    T2D_CHECK_EQ(absent_report.base_packs, 0u);
}

T2D_TEST(a_field_the_engine_reads_that_it_cannot_read_is_reported) {
    // "random_reverse:1" would otherwise be content that quietly never turns around, so the load says
    // so - the same way a picture that is not there is said rather than drawn blank.
    Scratch scratch;
    scratch.write("mine/floor.ecfg", "floor::\n    sand::\n        random_reverse:1\n");
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_packs({scratch.root() + "/mine"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(mentions(report.errors, "random_reverse must be true or false"));
    T2D_REQUIRE(report.sources.size() == 1u);
    T2D_CHECK_FALSE(report.sources[0].error.empty());
    // The content itself still loads: a field the engine cannot read is a data error, not a reason to
    // lose the name.
    T2D_CHECK_EQ(registry.count(ContentKind::Floor), 1u);

    // A value it can read is not reported, and it is what the definition carries.
    Scratch good;
    good.write("mine/floor.ecfg", "floor::\n    dirt::\n        random_reverse:true\n");
    ContentRegistry good_registry;
    ContentPipeline good_pipeline;
    good_pipeline.set_base_packs({good.root() + "/mine"});
    const ContentPipelineReport& good_report = good_pipeline.load(good_registry);
    T2D_CHECK_MSG(good_report.clean(), "{}", good_report.first_error());
}

T2D_TEST(the_pack_workspace_and_its_template_always_load) {
    // The repository ships a pack workspace (tile2d/packs) with a template project in it. The game
    // picks ./packs up on its own, so the template has to stay loadable and stay empty: a template
    // that registers content would put placeholder names in front of a designer on every run.
    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories({std::string(T2D_SOURCE_DIR) + "/packs"});
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.packs, 1u);
    T2D_REQUIRE(pipeline.packs().size() == 1u);
    T2D_CHECK_EQ(pipeline.packs()[0].id, std::string("template"));
    T2D_CHECK_EQ(pipeline.packs()[0].name, std::string("内容包模板"));
    T2D_CHECK_EQ(report.pack_content, 0u);
    T2D_CHECK_EQ(registry.total_count(), 0u);

    // The workspace is found by walking into the project directory, which is what makes a workspace of
    // pack projects work with a single --packs.
    T2D_CHECK(pipeline.packs()[0].directory.find("template") != std::string::npos);
}

T2D_TEST_MAIN
