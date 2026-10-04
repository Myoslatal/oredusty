// Where a game looks for content it was not told about: the packs directory beside the executable,
// the working directory's own, the game's own content pack beside it too, and what is inside them -
// content packs and mod packages in one place.
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

/// A pack and a mod package, both of them the smallest thing that loads: the pack is a directory with
/// one content file in it, the mod is a directory with a manifest and one content file. The names are
/// the fixture's.
constexpr std::string_view kPackContent = "structure::\n    pack_wall::\n";
constexpr std::string_view kModManifest = "id:\"test_mod\"\nname:\"Test mod\"\nversion:\"1.0\"\n"
                                          "content:[\"content/things.ecfg\"]\n";
constexpr std::string_view kModContent = "machine::\n    mod_pump::\n";

} // namespace

T2D_TEST(the_packs_directory_of_a_game_is_beside_its_executable) {
    T2D_CHECK_EQ(packs_beside("/opt/game/mine_game"), std::string("/opt/game/packs"));
    T2D_CHECK_EQ(packs_beside("/opt/game/bin/mine_game"), std::string("/opt/game/bin/packs"));
    T2D_CHECK_EQ(packs_beside("/mine_game"), std::string("/packs"));
    // A bare file name names no directory of its own, so there is nothing "beside" it: the working
    // directory rule is what covers that case.
    T2D_CHECK_EQ(packs_beside("mine_game"), std::string(""));
    T2D_CHECK_EQ(packs_beside(""), std::string(""));
    // The game's own content follows the same rule, one directory over.
    T2D_CHECK_EQ(content_beside("/opt/game/mine_game"), std::string("/opt/game/content"));
    T2D_CHECK_EQ(content_beside("mine_game"), std::string(""));
}

T2D_TEST(a_missing_default_is_silent_and_a_named_one_is_looked_at_once) {
    TempTree tree("defaults");
    tree.write("game/mine_game", "");
    tree.write("game/packs/mypack/items.ecfg", kPackContent);

    // Beside the game, and nowhere else: the workspace has no packs directory.
    const std::vector<std::string> beside = default_content_directories(tree.path("game/mine_game"), tree.path("work"));
    T2D_REQUIRE(beside.size() == 1u);
    T2D_CHECK_EQ(beside[0], tree.path("game/packs"));

    // Both rules name a directory, in load order: the game's own first, the workspace's second.
    tree.write("work/packs/other/items.ecfg", kPackContent);
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

T2D_TEST(a_pack_is_a_directory_and_a_mod_package_is_not_one) {
    TempTree tree("find");
    tree.write("packs/mypack/pack.ecfg", "id:\"my_pack\"\n");
    tree.write("packs/mypack/items.ecfg", kPackContent);
    tree.write("packs/mypack/art/wall.png", "pretend this is a png");
    tree.write("packs/anonymous/items.ecfg", kPackContent);      // no header: the directory names it
    tree.write("packs/mymod/mod.ecfg", kModManifest);
    tree.write("packs/mymod/content/things.ecfg", kModContent);  // the mod's content, not a pack
    tree.write("packs/loose.ecfg", kPackContent);                // the old single file shape

    std::vector<std::string> errors;
    const std::vector<std::string> packs = ContentPack::find_packs(tree.path("packs"), &errors);
    // The two packs, and nothing from inside the mod package: those names belong to the mod host, which
    // loads them in the order its manifest gives.
    T2D_REQUIRE(packs.size() == 2u);
    T2D_CHECK_EQ(packs[0], tree.path("packs/anonymous"));
    T2D_CHECK_EQ(packs[1], tree.path("packs/mypack"));
    // The loose file is reported: a file that is silently not loaded is content that disappeared.
    T2D_REQUIRE(errors.size() == 1u);
    T2D_CHECK(errors[0].find("is a loose file") != std::string::npos);
    T2D_CHECK(errors[0].find("loose.ecfg") != std::string::npos);

    // A directory that *is* a mod package has no packs in it either, and a directory that *is* a pack
    // is one pack.
    T2D_CHECK(ContentPack::find_packs(tree.path("packs/mymod")).empty());
    T2D_CHECK_EQ(ContentPack::find_packs(tree.path("packs/mypack")).size(), 1u);
    T2D_CHECK(ContentPack::is_pack(tree.path("packs/mypack")));
    T2D_CHECK_FALSE(ContentPack::is_pack(tree.path("packs/mymod")));
    T2D_CHECK_FALSE(ContentPack::is_pack(tree.path("packs/mypack/art")));
}

T2D_TEST(one_directory_can_hold_packs_and_mod_packages_and_both_load) {
    TempTree tree("pipeline");
    tree.write("game/mine_game", "");
    tree.write("game/packs/mypack/items.ecfg", kPackContent);
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
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.packs, 1u);
    T2D_CHECK_EQ(report.mods, 1u);
    T2D_CHECK(registry.find(ContentKind::Structure, "pack_wall") != kNoContent);
    T2D_CHECK(registry.find(ContentKind::Machine, "mod_pump") != kNoContent);
    T2D_CHECK_EQ(registry.total_count(), 2u);

    // Both are on the list a designer reads, each with the path it came from.
    T2D_REQUIRE(report.sources.size() == 2u);
    T2D_CHECK_EQ(report.sources[0].kind, SourceKind::Pack);
    T2D_CHECK_EQ(report.sources[0].path, tree.path("game/packs/mypack"));
    T2D_CHECK_EQ(report.sources[1].kind, SourceKind::Mod);
    T2D_CHECK_EQ(report.sources[1].path, tree.path("game/packs/mymod"));
}

T2D_TEST(the_games_own_pack_is_found_beside_the_game) {
    TempTree tree("vanilla");
    tree.write("game/mine_game", "");
    tree.write("game/content/pack.ecfg", "id:\"vanilla\"\nname:\"The game's own content\"\n");
    tree.write("game/content/floors.ecfg", "floor::\n    dirt::\n");
    tree.write("game/packs/extra/items.ecfg", kPackContent);

    // The rule the game's own content follows: beside the executable, the same way the packs directory
    // is found - and it is loaded before the packs, so its ids never move.
    const std::string vanilla = content_beside(tree.path("game/mine_game"));
    T2D_CHECK_EQ(vanilla, tree.path("game/content"));
    T2D_REQUIRE(fs::is_directory(vanilla));

    ContentRegistry registry;
    ContentPipeline pipeline;
    pipeline.set_base_packs({vanilla});
    pipeline.set_pack_directories(default_content_directories(tree.path("game/mine_game"), tree.path("work")));
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.base_packs, 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Floor, "dirt"), 1u);       // the game's own first
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "pack_wall"), 1u);
    T2D_REQUIRE(report.sources.size() == 2u);
    T2D_CHECK_EQ(report.sources[0].kind, SourceKind::File);
    T2D_CHECK_EQ(report.sources[0].id, std::string("vanilla"));
    T2D_CHECK_EQ(report.sources[0].path, vanilla);
    T2D_CHECK_EQ(report.sources[1].kind, SourceKind::Pack);
}

T2D_TEST_MAIN
