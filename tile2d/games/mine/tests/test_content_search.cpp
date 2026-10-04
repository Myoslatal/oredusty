// Where a game looks for content it was not told about: the packs directory beside the executable,
// the working directory's own, and what is inside them - content packs and mod packages in one place.
#include <mine/content_pack.h>
#include <mine/content_search.h>

#include <support/test_support.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mine;

namespace {

namespace fs = std::filesystem;

/// A tree of files that exists for one test and is removed when the test is done with it. Every test
/// gets its own root, so two of them running at once cannot collide.
class TempTree {
public:
    explicit TempTree(std::string_view name) {
        root_ = fs::temp_directory_path() / ("t2d_content_search_" + std::string(name));
        std::error_code code;
        fs::remove_all(root_, code);
        fs::create_directories(root_, code);
    }
    ~TempTree() {
        std::error_code code;
        fs::remove_all(root_, code);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    [[nodiscard]] std::string path(std::string_view relative) const {
        return (root_ / relative).string();
    }
    void write(std::string_view relative, std::string_view text) const {
        const fs::path target = root_ / relative;
        std::error_code code;
        fs::create_directories(target.parent_path(), code);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file << text;
    }

private:
    fs::path root_;
};

/// A pack and a mod package, both of them the smallest thing that loads: the pack is one .ecfg file,
/// the mod is a directory with a manifest and one content file. The names are the fixture's.
constexpr std::string_view kPackText = R"(
pack::
    id:"test_pack"
    name:"Test pack"
structure::
    pack_wall::
)";
constexpr std::string_view kModManifest = R"(
id:"test_mod"
name:"Test mod"
version:"1.0"
content:["content/things.ecfg"]
)";
constexpr std::string_view kModContent = R"(
machine::
    mod_pump::
)";

} // namespace

T2D_TEST(the_packs_directory_of_a_game_is_beside_its_executable) {
    T2D_CHECK_EQ(packs_beside("/opt/game/mine_game"), std::string("/opt/game/packs"));
    T2D_CHECK_EQ(packs_beside("/opt/game/bin/mine_game"), std::string("/opt/game/bin/packs"));
    T2D_CHECK_EQ(packs_beside("/mine_game"), std::string("/packs"));
    // A bare file name names no directory of its own, so there is nothing "beside" it: the working
    // directory rule is what covers that case.
    T2D_CHECK_EQ(packs_beside("mine_game"), std::string(""));
    T2D_CHECK_EQ(packs_beside(""), std::string(""));
}

T2D_TEST(a_missing_default_is_silent_and_a_named_one_is_looked_at_once) {
    TempTree tree("defaults");
    tree.write("game/mine_game", "");
    tree.write("game/packs/mypack.ecfg", kPackText);

    // Beside the game, and nowhere else: the workspace has no packs directory.
    const std::vector<std::string> beside = default_content_directories(tree.path("game/mine_game"), tree.path("work"));
    T2D_REQUIRE(beside.size() == 1u);
    T2D_CHECK_EQ(beside[0], tree.path("game/packs"));

    // Both rules name a directory, in load order: the game's own first, the workspace's second.
    tree.write("work/packs/other.ecfg", kPackText);
    const std::vector<std::string> both = default_content_directories(tree.path("game/mine_game"), tree.path("work"));
    T2D_REQUIRE(both.size() == 2u);
    T2D_CHECK_EQ(both[0], tree.path("game/packs"));
    T2D_CHECK_EQ(both[1], tree.path("work/packs"));

    // The same directory named twice - a game run from its own directory - is looked at once, whatever
    // spelling each rule used.
    const std::vector<std::string> once = default_content_directories(tree.path("game/mine_game"), tree.path("game"));
    T2D_REQUIRE(once.size() == 1u);
    T2D_CHECK_EQ(once[0], tree.path("game/packs"));

    // Nothing anywhere is nothing to look at, and nothing to report: nobody asked for these.
    T2D_CHECK(default_content_directories(tree.path("empty/game"), tree.path("empty/work")).empty());
    T2D_CHECK(default_content_directories("", "").empty());
}

T2D_TEST(a_mod_package_in_the_packs_directory_is_not_scanned_as_packs) {
    TempTree tree("skip");
    tree.write("packs/mypack.ecfg", kPackText);
    tree.write("packs/project/notes.ecfg", kPackText);          // a pack project: a folder with its file
    tree.write("packs/mymod/mod.ecfg", kModManifest);
    tree.write("packs/mymod/content/things.ecfg", kModContent);  // the mod's content, not a pack

    const std::vector<std::string> files = ContentPack::scan_directory(tree.path("packs"));
    // The two packs, and nothing from inside the mod package: those names belong to the mod host, which
    // loads them in the order its manifest gives.
    T2D_REQUIRE(files.size() == 2u);
    T2D_CHECK_EQ(files[0], tree.path("packs/mypack.ecfg"));
    T2D_CHECK_EQ(files[1], tree.path("packs/project/notes.ecfg"));

    // A directory that *is* a mod package has no packs in it either.
    T2D_CHECK(ContentPack::scan_directory(tree.path("packs/mymod")).empty());
}

T2D_TEST(one_directory_can_hold_packs_and_mod_packages_and_both_load) {
    TempTree tree("pipeline");
    tree.write("game/mine_game", "");
    tree.write("game/packs/mypack.ecfg", kPackText);
    tree.write("game/packs/mymod/mod.ecfg", kModManifest);
    tree.write("game/packs/mymod/content/things.ecfg", kModContent);

    const std::vector<std::string> directories =
        default_content_directories(tree.path("game/mine_game"), tree.path("work"));
    T2D_REQUIRE(directories.size() == 1u);

    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_pack_directories(directories);
    pipeline.set_mod_directories(directories);
    const ContentPipelineReport& report = pipeline.load(registry);

    // The point of the rule: one directory, both kinds, no collision - the mod's content file was not
    // registered a second time as a pack.
    T2D_CHECK(report.clean());
    T2D_CHECK_EQ(report.packs, 1u);
    T2D_CHECK_EQ(report.mods, 1u);
    T2D_CHECK(registry.find(ContentKind::Structure, "pack_wall") != kNoContent);
    T2D_CHECK(registry.find(ContentKind::Machine, "mod_pump") != kNoContent);
    T2D_CHECK_EQ(registry.total_count(), 2u);

    // Both are on the list a designer reads, each with the path it came from.
    T2D_REQUIRE(report.sources.size() == 2u);
    T2D_CHECK_EQ(report.sources[0].kind, SourceKind::Pack);
    T2D_CHECK_EQ(report.sources[0].path, tree.path("game/packs/mypack.ecfg"));
    T2D_CHECK_EQ(report.sources[1].kind, SourceKind::Mod);
    T2D_CHECK_EQ(report.sources[1].path, tree.path("game/packs/mymod"));
}

T2D_TEST_MAIN
