// codetab - the code table toolchain.
//
// It drives the system compiler and packs what the compiler emitted into a code table. There is no
// second C++ compiler here and there never will be: the compiler is the one the project is built
// with, and this tool is the step after it.
//
//     codetab build game/*.cpp -o mine.codetab --id mine --version 1.0
//     codetab build mod.cpp -o mod.codetab --id my_mod --requires mine@1.0
//     codetab dump mod.codetab
//
// The flags it compiles with are not decoration. \c -fsemantic-interposition is what keeps a call to
// another translation unit a call *through a relocation* instead of something the compiler inlined
// away: without it, a table merged later could not replace the function, because there would be
// nothing left to replace. \c -ffunction-sections and \c -fdata-sections put every function and
// every global in a section of its own, which is what lets a merge keep one and drop another.
#include <t2d/core/code_table.h>
#include <t2d/core/object_file.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using t2d::ApiVerdict;
using t2d::ApiVersion;
using t2d::CodeRequirement;
using t2d::CodeTable;
using t2d::ObjectFile;

struct Options {
    std::vector<std::string> sources;
    std::vector<std::string> includes;
    std::vector<std::string> defines;
    std::string output;
    std::string compiler = "c++";
    std::string id;
    std::string name;
    std::string version;
    std::vector<CodeRequirement> requirements;
    std::string optimization = "-O2";
    /// The published surface, and the engine the tables are being checked against.
    std::string surface;
    std::string host_version = "1.0";
    std::string engine_id = "engine";
    bool exceptions = false;
    bool keep = false;
    bool verbose = false;
};

void usage() {
    std::cout << "codetab - pack compiled C++ into a code table, and read one back\n"
                 "\n"
                 "  codetab build <source.cpp>... -o <out.codetab> [options]   compile, then pack\n"
                 "  codetab pack <object.o>... -o <out.codetab> [options]        pack what is there\n"
                 "  codetab dump <table.codetab>                                 what is in one\n"
                 "  codetab api --surface <engine.api> <table.codetab>...        check it against the engine\n"
                 "\n"
                 "options:\n"
                 "  --compiler <path>   the compiler to drive (default: c++)\n"
                 "  --id <id>           what the module is called in the merge report\n"
                 "  --name <name>       for humans\n"
                 "  --version <v>       this module's own version\n"
                 "  --requires <id>     a module this one needs: an id, or id@version to demand that\n"
                 "                      exact version (repeatable). A requirement nothing provides, or\n"
                 "                      one whose version does not match, refuses the module at load time\n"
                 "  --include <dir>     added to the compile (repeatable)\n"
                 "  --define <X>        added to the compile (repeatable)\n"
                 "  --opt <level>       -O0 / -O1 / -O2 / -O3 (default -O2)\n"
                 "  --exceptions        compile with exceptions (unwinding through a table is not\n"
                 "                      registered with the runtime yet, so a throw is fatal)\n"
                 "  --keep              keep the objects the compiler produced\n"
                 "  --verbose           print the compile commands\n";
}

/// One argument, quoted for the shell this runs the compiler through. A path with a space in it is
/// not exotic, and a command line that silently splits it is a confusing failure much later.
[[nodiscard]] std::string quote(const std::string& text) {
    std::string out = "'";
    for (const char letter : text) {
        if (letter == '\'') out += "'\\''";
        else out += letter;
    }
    out += "'";
    return out;
}

[[nodiscard]] bool parse(int argc, char** argv, Options& options, std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](std::string& out) {
            if (index + 1 >= argc) {
                error = std::format("{} wants a value", argument);
                return false;
            }
            out = argv[++index];
            return true;
        };
        if (argument == "--compiler") { if (!value(options.compiler)) return false; }
        else if (argument == "--id") { if (!value(options.id)) return false; }
        else if (argument == "--name") { if (!value(options.name)) return false; }
        else if (argument == "--version") { if (!value(options.version)) return false; }
        else if (argument == "--opt") { if (!value(options.optimization)) return false; }
        else if (argument == "--surface") { if (!value(options.surface)) return false; }
        else if (argument == "--host") { if (!value(options.host_version)) return false; }
        else if (argument == "--engine") { if (!value(options.engine_id)) return false; }
        else if (argument == "-o" || argument == "--output") { if (!value(options.output)) return false; }
        else if (argument == "--include" || argument == "-I") { std::string dir; if (!value(dir)) return false; options.includes.push_back(dir); }
        else if (argument == "--define" || argument == "-D") { std::string define; if (!value(define)) return false; options.defines.push_back(define); }
        else if (argument == "--requires") {
            std::string need;
            if (!value(need)) return false;
            // "id" or "id@version": the version is what the load checks, so it is part of the name.
            const std::size_t at = need.find('@');
            CodeRequirement requirement;
            requirement.id = need.substr(0, at);
            if (at != std::string::npos) requirement.version = need.substr(at + 1);
            if (requirement.id.empty()) {
                error = std::format("'{}' is not a requirement: it wants an id, or id@version", need);
                return false;
            }
            options.requirements.push_back(std::move(requirement));
        }
        else if (argument == "--exceptions") options.exceptions = true;
        else if (argument == "--keep") options.keep = true;
        else if (argument == "--verbose") options.verbose = true;
        else if (!argument.empty() && argument[0] == '-') {
            error = std::format("'{}' is not an option codetab knows", argument);
            return false;
        } else {
            options.sources.push_back(argument);
        }
    }
    return true;
}

[[nodiscard]] int build(const Options& options) {
    if (options.sources.empty()) {
        std::cerr << "codetab build: no source files were given\n";
        return 2;
    }
    if (options.output.empty()) {
        std::cerr << "codetab build: -o <out.codetab> is required\n";
        return 2;
    }
    const std::filesystem::path output(options.output);
    std::filesystem::path work = output;
    work += ".objects";
    std::error_code code;
    std::filesystem::create_directories(work, code);
    if (code) {
        std::cerr << std::format("codetab build: '{}' cannot be created: {}\n", work.string(), code.message());
        return 2;
    }

    // The flags that make a table mergeable, and the ones that keep it self contained: no stack
    // protector means no call into the running program for something a table can do itself.
    std::vector<std::string> flags = {"-std=c++20", options.optimization, "-fPIC", "-fsemantic-interposition",
                                      "-ffunction-sections", "-fdata-sections", "-fno-stack-protector"};
    if (!options.exceptions) flags.push_back("-fno-exceptions");
    for (const std::string& include : options.includes) { flags.push_back("-I"); flags.push_back(include); }
    for (const std::string& define : options.defines) { flags.push_back("-D"); flags.push_back(define); }

    std::vector<ObjectFile> objects;
    for (const std::string& source : options.sources) {
        const std::filesystem::path object = work / (std::filesystem::path(source).stem().string() + ".o");
        std::string command = quote(options.compiler);
        for (const std::string& flag : flags) command += " " + quote(flag);
        command += " -c " + quote(source) + " -o " + quote(object.string());
        if (options.verbose) std::cout << command << "\n";
        const int status = std::system(command.c_str());
        if (status != 0) {
            std::cerr << std::format("codetab build: the compiler refused '{}' (exit {})\n", source, status);
            return 1;
        }
        std::string error;
        std::optional<ObjectFile> read = ObjectFile::load(object.string(), &error);
        if (!read.has_value()) {
            std::cerr << std::format("codetab build: {}\n", error);
            return 1;
        }
        read->source = source;
        objects.push_back(std::move(*read));
    }

    std::string error;
    std::optional<CodeTable> table = CodeTable::from_objects(objects, &error);
    if (!table.has_value()) {
        std::cerr << std::format("codetab build: {}\n", error);
        return 1;
    }
    table->id = options.id.empty() ? output.stem().string() : options.id;
    table->name = options.name.empty() ? table->id : options.name;
    table->version = options.version;
    table->requirements = options.requirements;
    if (!table->save(options.output, &error)) {
        std::cerr << std::format("codetab build: {}\n", error);
        return 1;
    }
    if (!options.keep) std::filesystem::remove_all(work, code);
    std::cout << std::format("{}: {} object(s) -> {} section(s), {} symbol(s), {} relocation(s), {} overridable "
                             "name(s)\n",
                             options.output, objects.size(), table->sections.size(), table->symbols.size(),
                             table->relocations.size(), table->overridable_symbols().size());
    return 0;
}

/// Packs objects somebody else compiled. \c build drives the compiler itself, which is what a mod
/// author wants; a build system that already knows the include paths, the defines and the flags wants
/// this one instead, because then there is one place that decides how the code is compiled.
[[nodiscard]] int pack(const Options& options) {
    if (options.sources.empty()) {
        std::cerr << "codetab pack: no object files were given\n";
        return 2;
    }
    if (options.output.empty()) {
        std::cerr << "codetab pack: -o <out.codetab> is required\n";
        return 2;
    }
    std::vector<ObjectFile> objects;
    for (const std::string& source : options.sources) {
        std::string error;
        std::optional<ObjectFile> object = ObjectFile::load(source, &error);
        if (!object.has_value()) {
            std::cerr << std::format("codetab pack: {}\n", error);
            return 1;
        }
        objects.push_back(std::move(*object));
    }
    std::string error;
    std::optional<CodeTable> table = CodeTable::from_objects(objects, &error);
    if (!table.has_value()) {
        std::cerr << std::format("codetab pack: {}\n", error);
        return 1;
    }
    table->id = options.id.empty() ? std::filesystem::path(options.output).stem().string() : options.id;
    table->name = options.name.empty() ? table->id : options.name;
    table->version = options.version;
    table->requirements = options.requirements;
    if (!table->save(options.output, &error)) {
        std::cerr << std::format("codetab pack: {}\n", error);
        return 1;
    }
    std::cout << std::format("{}: {} object(s) -> {} section(s), {} symbol(s), {} relocation(s), {} overridable "
                             "name(s)\n",
                             options.output, objects.size(), table->sections.size(), table->symbols.size(),
                             table->relocations.size(), table->overridable_symbols().size());
    return 0;
}

/// Whether a symbol belongs to one of the engine's own namespaces. The platform's symbols - libc,
/// libstdc++, the exception machinery - are not the engine's to publish, so a table is never asked to
/// have them on the list: a mod may use the C++ library as freely as the engine does.
[[nodiscard]] bool engine_symbol(std::string_view name) {
    for (const char* space : {"3t2d", "3ore", "4mine"}) {
        const std::size_t at = name.find(space);
        if (at != std::string_view::npos && at < 12) return true;
    }
    return false;
}

/// The published surface: one symbol a line, with its tier and module after it. Comments and blank
/// lines are the file's own business, not the checker's.
[[nodiscard]] std::optional<std::unordered_set<std::string>> load_surface(const std::string& path, std::string* error) {
    std::ifstream stream(path);
    if (!stream) {
        if (error != nullptr) *error = std::format("'{}' cannot be read", path);
        return std::nullopt;
    }
    std::unordered_set<std::string> symbols;
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        std::string_view text(line);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
        if (text.empty()) continue;
        const std::size_t space = text.find(' ');
        if (space == std::string_view::npos) continue;      // a tier with no symbol is not a symbol
        std::string_view rest = text.substr(space + 1);
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        const std::size_t end = rest.find(' ');
        symbols.emplace(rest.substr(0, end));
    }
    return symbols;
}

/// Checks tables against the published surface and the version rule. What a module asks the engine for
/// is what no module in the set defines: everything else is answered inside the merged tables.
[[nodiscard]] int api_check(const Options& options) {
    if (options.surface.empty()) {
        std::cerr << "codetab api: --surface <engine.api> is required\n";
        return 2;
    }
    std::string error;
    std::optional<std::unordered_set<std::string>> surface = load_surface(options.surface, &error);
    if (!surface.has_value()) {
        std::cerr << std::format("codetab api: {}\n", error);
        return 2;
    }
    const std::optional<ApiVersion> host = ApiVersion::parse(options.host_version);
    if (!host.has_value()) {
        std::cerr << std::format("codetab api: '{}' is not a version (want major.minor)\n", options.host_version);
        return 2;
    }
    std::vector<CodeTable> tables;
    for (const std::string& path : options.sources) {
        std::optional<CodeTable> table = CodeTable::load(path, &error);
        if (!table.has_value()) {
            std::cerr << std::format("codetab api: {}\n", error);
            return 2;
        }
        tables.push_back(std::move(*table));
    }
    std::unordered_set<std::string> defined;
    for (const CodeTable& table : tables) {
        for (const t2d::CodeTableSymbol& symbol : table.symbols) {
            if (symbol.defined() && symbol.shared() && !symbol.name.empty()) defined.insert(symbol.name);
        }
    }

    int refused = 0;
    for (const CodeTable& table : tables) {
        std::vector<std::string> unlisted;
        for (const t2d::CodeTableSymbol& symbol : table.symbols) {
            if (symbol.defined() || !symbol.shared() || symbol.name.empty()) continue;
            if (defined.count(symbol.name) != 0) continue;          // answered inside the tables
            if (!engine_symbol(symbol.name)) continue;   // the platform's, not the engine's
            if (surface->count(symbol.name) == 0) unlisted.push_back(symbol.name);
        }
        std::sort(unlisted.begin(), unlisted.end());
        unlisted.erase(std::unique(unlisted.begin(), unlisted.end()), unlisted.end());

        ApiVersion built = *host;
        for (const CodeRequirement& requirement : table.requirements) {
            if (requirement.id != options.engine_id || requirement.version.empty()) continue;
            built = ApiVersion::parse(requirement.version).value_or(*host);
        }
        const bool inside = unlisted.empty();
        const ApiVerdict verdict = api_verdict(built, *host, inside);
        const char* word = verdict == ApiVerdict::Accept ? "accept" : (verdict == ApiVerdict::Warn ? "warn" : "refuse");
        std::cout << std::format("{}: built for {} {}, engine is {}: {} ({} symbol(s) outside the surface)\n",
                                 table.id, options.engine_id, built.text(), host->text(), word, unlisted.size());
        for (const std::string& name : unlisted) std::cout << std::format("  outside: {}\n", name);
        if (verdict == ApiVerdict::Refuse) ++refused;
    }
    return refused == 0 ? 0 : 1;
}

[[nodiscard]] int dump(const std::string& path) {
    std::string error;
    std::optional<CodeTable> table = CodeTable::load(path, &error);
    if (!table.has_value()) {
        std::cerr << std::format("codetab dump: {}\n", error);
        return 1;
    }
    std::cout << table->describe();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    if (command == "help" || command == "--help" || command == "-h") {
        usage();
        return 0;
    }
    if (command == "api") {
        Options options;
        std::string error;
        std::vector<char*> rest(argv + 2, argv + argc);
        std::vector<char*> with_name;
        with_name.push_back(argv[0]);
        with_name.insert(with_name.end(), rest.begin(), rest.end());
        if (!parse(static_cast<int>(with_name.size()), with_name.data(), options, error)) {
            std::cerr << std::format("codetab api: {}\n", error);
            return 2;
        }
        return api_check(options);
    }
    if (command == "dump") {
        if (argc < 3) {
            std::cerr << "codetab dump: which table?\n";
            return 2;
        }
        return dump(argv[2]);
    }
    if (command == "pack") {
        Options options;
        std::string error;
        std::vector<char*> rest(argv + 2, argv + argc);
        std::vector<char*> with_name;
        with_name.push_back(argv[0]);
        with_name.insert(with_name.end(), rest.begin(), rest.end());
        if (!parse(static_cast<int>(with_name.size()), with_name.data(), options, error)) {
            std::cerr << std::format("codetab pack: {}\n", error);
            return 2;
        }
        return pack(options);
    }
    if (command == "build") {
        Options options;
        std::string error;
        // argv[0] is the tool itself, argv[1] is "build": the sources start after both.
        std::vector<char*> rest(argv + 2, argv + argc);
        std::vector<char*> with_name;
        with_name.push_back(argv[0]);
        with_name.insert(with_name.end(), rest.begin(), rest.end());
        if (!parse(static_cast<int>(with_name.size()), with_name.data(), options, error)) {
            std::cerr << std::format("codetab build: {}\n", error);
            return 2;
        }
        return build(options);
    }
    std::cerr << std::format("codetab: '{}' is not a command (try build or dump)\n", command);
    return 2;
}
