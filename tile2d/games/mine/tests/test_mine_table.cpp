// The game's own code in a code table: mine_core's registry is packed into a table - not linked into
// this test - merged, and called. Then a mod table replaces a function of it, and the game's own call
// follows the merge.
//
// This is the shape the game is heading for (docs/TABLES.md section 9): the game's logic arrives as a
// table, the engine stays in the program that loads it, and a mod is another table.
#include <t2d/core/code_table.h>
#include <t2d/core/object_file.h>

#include <support/test_support.h>

#include <optional>
#include <string>
#include <vector>

using namespace t2d;

/// The host half: what a table calls back into. It stands in for the engine, and it is only reachable
/// because this executable exports its symbols (ENABLE_EXPORTS in CMake).
extern "C" int host_service(int value) { return value * 3; }

namespace {

[[nodiscard]] std::string object_of(const char* name) {
    return std::format("{}/{}.o", MINE_TABLE_DIR, name);
}

/// The game's table: its real registry translation unit and the probe that drives it.
[[nodiscard]] std::optional<CodeTable> game_table(std::string* error) {
    std::vector<ObjectFile> files;
    for (const std::string& path : {object_of("registry"), object_of("game_logic")}) {
        std::optional<ObjectFile> object = ObjectFile::load(path, error);
        if (!object.has_value()) return std::nullopt;
        files.push_back(std::move(*object));
    }
    std::optional<CodeTable> table = CodeTable::from_objects(files, error);
    if (table.has_value()) {
        table->id = "mine";
        table->version = "1.0";
    }
    return table;
}

[[nodiscard]] std::optional<CodeTable> mod_table(std::string* error) {
    std::optional<ObjectFile> object = ObjectFile::load(object_of("mod_game"), error);
    if (!object.has_value()) return std::nullopt;
    std::optional<CodeTable> table = CodeTable::from_objects({std::move(*object)}, error);
    if (table.has_value()) {
        table->id = "game_mod";
        table->version = "1.0";
        table->requirements = {CodeRequirement{"mine", "1.0"}};
    }
    return table;
}

} // namespace

T2D_TEST(the_games_own_code_runs_from_a_table) {
    std::string error;
    std::optional<CodeTable> table = game_table(&error);
    T2D_REQUIRE(table.has_value());

    CodeImage image;
    image.declare_host("engine", "0.1");
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(report.modules.size() == 1u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK_EQ(report.modules[0].id, std::string("mine"));

    // The game's own class ran inside the table: two items, ids 1 and 2, registering twice changed
    // nothing. The bonus is the table's own function, and host_service is the program that loaded it.
    const auto probe = image.function<int()>("game_probe");
    T2D_REQUIRE(probe != nullptr);
    T2D_CHECK_EQ(probe(), 110);   // 1 * 100 + 2, plus 5, plus 3
}

T2D_TEST(a_mod_table_replaces_a_function_of_the_games_table) {
    std::string error;
    std::optional<CodeTable> game = game_table(&error);
    std::optional<CodeTable> mod = mod_table(&error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());

    CodeImage image;
    image.declare_host("engine", "0.1");
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK(report.modules[1].ok);

    T2D_REQUIRE(report.overrides.size() == 1u);
    T2D_CHECK_EQ(report.overrides[0].symbol, std::string("game_bonus"));
    T2D_CHECK_EQ(report.overrides[0].from, std::string("game_mod"));
    T2D_CHECK_EQ(report.overrides[0].replaced, std::string("mine"));

    // The game's own code called game_bonus(), and it landed in the mod: 7 instead of 5.
    const auto probe = image.function<int()>("game_probe");
    T2D_REQUIRE(probe != nullptr);
    T2D_CHECK_EQ(probe(), 112);
    // The mod can still reach what it replaced.
    const auto original = reinterpret_cast<int (*)()>(image.find_previous("game_bonus"));
    T2D_REQUIRE(original != nullptr);
    T2D_CHECK_EQ(original(), 5);
}

T2D_TEST(a_mod_built_for_another_engine_is_refused_before_it_is_merged) {
    std::string error;
    std::optional<CodeTable> game = game_table(&error);
    std::optional<CodeTable> mod = mod_table(&error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    mod->requirements = {CodeRequirement{"mine", "2.0"}};   // built for the next engine

    CodeImage image;
    image.declare_host("engine", "0.1");
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK_FALSE(report.modules[1].ok);
    // Refused means refused: the game's own function is still the game's.
    const auto probe = image.function<int()>("game_probe");
    T2D_REQUIRE(probe != nullptr);
    T2D_CHECK_EQ(probe(), 110);
    T2D_CHECK_EQ(report.overrides.size(), 0u);
}

T2D_TEST_MAIN
