// Mine - the launcher.
//
// The game is not linked into this program: it travels beside it as a **code table** (docs/TABLES.md),
// which the launcher places in memory and merges with every other table it finds - a mod's - before
// calling into the result. The engine stays here: the renderer, the fonts, the network, the file
// system, and the runtime that does the merging. This program exports its own symbols, because the
// game calls back into all of that.
#include <t2d/core/code_table.h>
#include <t2d/core/executable.h>
#include <t2d/core/log.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using t2d::CodeImage;
using t2d::CodeOverride;
using t2d::CodeTable;

constexpr const char* kGameTableName = "mine.codetab";   ///< travels with the executable
constexpr const char* kEntrySymbol = "mine_game_main";
constexpr const char* kEngineId = "engine";
constexpr const char* kEngineVersion = "0.1";

/// The game's own table first, then the ones named on the command line, then the ones beside the
/// executable: a mod drops its table into "packs" next to its content. Sorted, so the merge order does
/// not depend on what the filesystem returns first.
[[nodiscard]] std::vector<std::string> collect_tables(const std::vector<std::string>& named,
                                                      const std::string& directory) {
    std::vector<std::string> tables;
    std::error_code code;
    const fs::path game = fs::path(directory) / kGameTableName;
    if (fs::is_regular_file(game, code)) tables.push_back(game.string());
    else T2D_ERROR("table: '{}' is not there: the game's table travels with the executable", game.string());
    for (const std::string& path : named) {
        if (!fs::is_regular_file(path, code)) {
            T2D_ERROR("table: '{}' is not a table", path);
            continue;
        }
        tables.push_back(path);
    }
    std::vector<std::string> found;
    const fs::path packs = fs::path(directory) / "packs";
    if (fs::is_directory(packs, code)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(packs, code)) {
            if (entry.path().extension() == ".codetab") found.push_back(entry.path().string());
        }
    }
    std::sort(found.begin(), found.end());
    tables.insert(tables.end(), found.begin(), found.end());
    return tables;
}

} // namespace

int main(int argc, char** argv) {
    // --table belongs to the launcher, so it is taken out of the command line before the game parses
    // what is left: an option the game does not know is not a thing to hand it.
    std::vector<std::string> named;
    std::vector<std::string> kept;
    for (int index = 0; index < argc; ++index) {
        const std::string argument = argv[index];
        if (index > 0 && argument == "--table" && index + 1 < argc) {
            named.emplace_back(argv[++index]);
            continue;
        }
        kept.push_back(argument);
    }
    std::vector<char*> game_argv;
    game_argv.reserve(kept.size());
    for (std::string& argument : kept) game_argv.push_back(argument.data());

    const std::vector<std::string> paths =
        collect_tables(named, t2d::parent_directory_of(t2d::executable_path()));
    if (paths.empty()) return 1;

    // The image is deliberately never destroyed. It owns the memory the game was placed in, and the
    // C++ runtime runs the module's static destructors *after* main returns - a destroyed image would
    // have unmapped the code those destructors are made of. A program that ends in a moment is not a
    // program that needs to hand memory back.
    CodeImage& image = *new CodeImage();
    image.declare_host(kEngineId, kEngineVersion);
    for (const std::string& path : paths) {
        std::string error;
        std::optional<CodeTable> table = CodeTable::load(path, &error);
        if (!table.has_value()) {
            T2D_ERROR("table: {}", error);
            return 1;
        }
        image.add(std::move(*table));
    }

    const t2d::CodeImageReport& report = image.load();
    for (const std::string& message : report.errors) T2D_ERROR("table: {}", message);
    for (const std::string& name : report.unresolved) T2D_ERROR("table: nothing defines '{}'", name);
    // Who replaced what is the first question asked of a modded run, so it is said out loud.
    for (const CodeOverride& replaced : report.overrides) {
        T2D_WARN("table: '{}' from '{}' replaced '{}'", replaced.symbol, replaced.from, replaced.replaced);
    }
    T2D_INFO("tables: {} module(s), {} symbol(s), {} relocation(s), {} override(s), {} error(s)",
             report.modules.size(), report.symbols, report.relocations, report.overrides.size(),
             report.errors.size());
    if (!report.clean()) return 1;

    const auto entry = image.function<int(int, char**)>(kEntrySymbol);
    if (entry == nullptr) {
        T2D_ERROR("table: the game's table does not export '{}'", kEntrySymbol);
        return 1;
    }
    return entry(static_cast<int>(game_argv.size()), game_argv.data());
}
