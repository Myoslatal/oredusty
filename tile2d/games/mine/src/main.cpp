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

using t2d::ApiSurface;
using t2d::ApiVerdict;
using t2d::ApiVersion;
using t2d::CodeAbi;
using t2d::CodeImage;
using t2d::CodeModuleInfo;
using t2d::CodeOverride;
using t2d::CodeRequirement;
using t2d::CodeTable;

constexpr const char* kGameTableName = "mine.codetab";   ///< travels with the executable
constexpr const char* kEntrySymbol = "mine_game_main";
constexpr const char* kEngineId = "engine";
/// The published surface travels with the executable, like the game's own table.
constexpr const char* kSurfaceName = "engine.api";
/// What the launcher itself was compiled as. It travels with the executable too, and it is what every
/// module is measured against: a table whose types are not this program's types is refused, with the
/// fact that differs named, rather than loaded into a program it cannot call (docs/ABI.md).
constexpr const char* kAbiName = "engine.abi";

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

    const std::string directory = t2d::parent_directory_of(t2d::executable_path());
    const std::vector<std::string> paths = collect_tables(named, directory);
    if (paths.empty()) return 1;

    // The engine's published surface, and the version it belongs to: a module's requirements are
    // checked against it, and so is everything the module asks the engine for.
    std::string surface_error;
    const std::optional<ApiSurface> surface = ApiSurface::load((fs::path(directory) / kSurfaceName).string(),
                                                               &surface_error);
    if (!surface.has_value()) T2D_WARN("surface: {} (no surface check this run)", surface_error);
    const ApiVersion host = surface.has_value() ? surface->version : ApiVersion{1, 0};

    // The image is deliberately never destroyed. It owns the memory the game was placed in, and the
    // C++ runtime runs the module's static destructors *after* main returns - a destroyed image would
    // have unmapped the code those destructors are made of. A program that ends in a moment is not a
    // program that needs to hand memory back.
    CodeImage& image = *new CodeImage();
    image.declare_host(kEngineId, host.text());

    // The engine's own ABI, written by the toolchain at build time. Without it the first table added is
    // the reference, which is the same answer when the two were built together - and they are, by the
    // same build, with the same toolchain (games/mine/CMakeLists.txt).
    std::string abi_error;
    if (std::optional<CodeAbi> abi = CodeAbi::load((fs::path(directory) / kAbiName).string(), &abi_error)) {
        image.declare_host_abi(std::move(*abi));
    } else {
        T2D_WARN("abi: {} (no record to measure the tables against this run)", abi_error);
    }
    std::vector<ApiVersion> built_against;
    for (const std::string& path : paths) {
        std::string error;
        std::optional<CodeTable> table = CodeTable::load(path, &error);
        if (!table.has_value()) {
            T2D_ERROR("table: {}", error);
            return 1;
        }
        // What this module was built for, before the image takes the table over.
        ApiVersion built = host;
        for (const CodeRequirement& requirement : table->requirements) {
            if (requirement.id != kEngineId || requirement.version.empty()) continue;
            built = ApiVersion::parse(requirement.version).value_or(host);
        }
        built_against.push_back(built);
        image.add(std::move(*table));
    }

    const t2d::CodeImageReport& report = image.load();
    for (const std::string& message : report.errors) T2D_ERROR("table: {}", message);
    for (const std::string& name : report.unresolved) T2D_ERROR("table: nothing defines '{}'", name);
    // What was built differently but is still one program, and which tables do not say what they were
    // built as: said out loud, because "why is this mod different" should have an answer.
    for (const std::string& message : report.warnings) T2D_WARN("table: {}", message);
    // Who replaced what is the first question asked of a modded run, so it is said out loud.
    for (const CodeOverride& replaced : report.overrides) {
        T2D_WARN("table: '{}' from '{}' replaced '{}'", replaced.symbol, replaced.from, replaced.replaced);
    }
    T2D_INFO("tables: {} module(s), {} symbol(s), {} relocation(s), {} override(s), {} error(s)",
             report.modules.size(), report.symbols, report.relocations, report.overrides.size(),
             report.errors.size());
    if (!report.clean()) return 1;

    // The surface verdict, module by module: a module that stays inside the published surface loads
    // across the whole major version and says nothing; one that reaches outside it is held to a narrow
    // range - the same version quietly, a minor version either way with a warning, further is refused.
    if (surface.has_value()) {
        bool refused = false;
        for (std::size_t index = 0; index < report.modules.size(); ++index) {
            const CodeModuleInfo& module = report.modules[index];
            if (!module.ok) continue;
            std::vector<std::string> outside;
            for (const std::string& name : module.host_symbols) {
                if (!ApiSurface::engine_symbol(name)) continue;   // the platform's, not the engine's
                if (!surface->contains(name)) outside.push_back(name);
            }
            const ApiVersion built = index < built_against.size() ? built_against[index] : host;
            const ApiVerdict verdict = api_verdict(built, host, outside.empty());
            if (verdict == ApiVerdict::Warn) {
                T2D_WARN("surface: '{}' was built for engine {} and this is {}: {} symbol(s) outside the "
                         "published surface, loading anyway",
                         module.id, built.text(), host.text(), outside.size());
            } else if (verdict == ApiVerdict::Refuse) {
                T2D_ERROR("surface: '{}' was built for engine {}, this is {}, and it uses {} symbol(s) "
                          "outside the published surface",
                          module.id, built.text(), host.text(), outside.size());
                for (const std::string& name : outside) T2D_ERROR("surface:   {}", name);
                refused = true;
            }
        }
        if (refused) return 1;
    }

    const auto entry = image.function<int(int, char**)>(kEntrySymbol);
    if (entry == nullptr) {
        T2D_ERROR("table: the game's table does not export '{}'", kEntrySymbol);
        return 1;
    }
    return entry(static_cast<int>(game_argv.size()), game_argv.data());
}
