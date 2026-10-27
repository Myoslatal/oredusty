// Where the game's files are: beside the executable first, the source tree only when a build tree has no
// assets copied next to it, and the names inside the assets directory being what the modules expect.
#include <mefemma/data_paths.h>

#include <support/test_support.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace mefemma;

namespace {

namespace fs = std::filesystem;

struct TempTree {
    fs::path root;

    TempTree() {
        static int next_id = 0;
        root = fs::temp_directory_path() / ("mefemma-paths-" + std::to_string(++next_id));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempTree() { fs::remove_all(root); }
};

[[nodiscard]] std::string path_of(const TempTree& tree, std::string_view relative) {
    return (tree.root / std::string(relative)).string();
}

void touch(const TempTree& tree, std::string_view relative) {
    const fs::path path = tree.root / std::string(relative);
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << "x";
}

} // namespace

T2D_TEST(the_names_inside_the_assets_directory) {
    const DataPaths paths("/game/assets");
    T2D_CHECK_EQ(paths.root(), std::string("/game/assets"));
    T2D_CHECK_EQ(paths.texture("hero"), std::string("/game/assets/textures/hero.png"));
    T2D_CHECK_EQ(paths.text(), std::string("/game/assets/text/ui.ecfg"));
    T2D_CHECK_EQ(paths.text("menu"), std::string("/game/assets/text/menu.ecfg"));
    T2D_CHECK_EQ(paths.settings(), std::string("/game/assets/config/game.ecfg"));
    T2D_CHECK_EQ(paths.font("game.ttf"), std::string("/game/assets/fonts/game.ttf"));
    T2D_CHECK_EQ(paths.file("notes/readme.txt"), std::string("/game/assets/notes/readme.txt"));
}

T2D_TEST(an_assets_directory_beside_the_executable_wins) {
    TempTree tree;
    const std::string executable = path_of(tree, "build/game");
    fs::create_directories(path_of(tree, "build/assets"));
    fs::create_directories(path_of(tree, "source/assets"));
    const DataPaths paths = DataPaths::resolve(executable, path_of(tree, "source"));
    T2D_CHECK_EQ(paths.root(), path_of(tree, "build/assets"));
}

T2D_TEST(the_source_tree_is_the_build_trees_fallback) {
    TempTree tree;
    const std::string executable = path_of(tree, "build/game");
    fs::create_directories(path_of(tree, "source/assets"));
    const DataPaths paths = DataPaths::resolve(executable, path_of(tree, "source"));
    T2D_CHECK_EQ(paths.root(), path_of(tree, "source/assets"));
}

T2D_TEST(neither_present_reports_the_place_a_package_would_have_put_them) {
    TempTree tree;
    const std::string executable = path_of(tree, "build/game");
    const DataPaths paths = DataPaths::resolve(executable, path_of(tree, "source"));
    T2D_CHECK_EQ(paths.root(), path_of(tree, "build/assets"));
}

T2D_TEST(has_file_answers_for_a_file_inside_the_root) {
    TempTree tree;
    const DataPaths paths(path_of(tree, "assets"));
    T2D_CHECK_FALSE(paths.has_file("textures/hero.png"));
    touch(tree, "assets/textures/hero.png");
    T2D_CHECK(paths.has_file("textures/hero.png"));
    T2D_CHECK_EQ(paths.texture("hero"), path_of(tree, "assets/textures/hero.png"));
}

T2D_TEST(an_executable_with_no_directory_falls_back_to_the_source_tree) {
    TempTree tree;
    fs::create_directories(path_of(tree, "source/assets"));
    const DataPaths paths = DataPaths::resolve("game", path_of(tree, "source"));
    T2D_CHECK_EQ(paths.root(), path_of(tree, "source/assets"));
}

T2D_TEST_MAIN
