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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using t2d::ApiSurface;
using t2d::ApiVerdict;
using t2d::ApiVersion;
using t2d::CodeRequirement;
using t2d::CodeTable;
using t2d::ObjectFile;
using t2d::ObjectSymbolBinding;

/// What one table asks the engine for, and what the rule says about it. \p also_defined is what the
/// other tables in the same load define, so a symbol answered inside them is not the engine's to give.
struct SurfaceCheck {
    ApiVerdict verdict = ApiVerdict::Accept;
    ApiVersion built{};
    std::vector<std::string> outside;
};

[[nodiscard]] SurfaceCheck surface_check(const CodeTable& table, const ApiSurface& surface,
                                         const std::string& engine_id,
                                         const std::unordered_set<std::string>& also_defined = {}) {
    SurfaceCheck check;
    check.built = surface.version;
    for (const CodeRequirement& requirement : table.requirements) {
        if (requirement.id != engine_id || requirement.version.empty()) continue;
        check.built = ApiVersion::parse(requirement.version).value_or(surface.version);
    }
    std::unordered_set<std::string> defined = also_defined;
    for (const t2d::CodeTableSymbol& symbol : table.symbols) {
        if (symbol.defined() && symbol.shared() && !symbol.name.empty()) defined.insert(symbol.name);
    }
    for (const t2d::CodeTableSymbol& symbol : table.symbols) {
        if (symbol.defined() || !symbol.shared() || symbol.name.empty()) continue;
        if (defined.count(symbol.name) != 0) continue;              // answered inside the tables
        if (!ApiSurface::engine_symbol(symbol.name)) continue;      // the platform's, not the engine's
        if (!surface.contains(symbol.name)) check.outside.push_back(symbol.name);
    }
    std::sort(check.outside.begin(), check.outside.end());
    check.outside.erase(std::unique(check.outside.begin(), check.outside.end()), check.outside.end());
    check.verdict = api_verdict(check.built, surface.version, check.outside.empty());
    return check;
}

[[nodiscard]] const char* verdict_word(ApiVerdict verdict) {
    switch (verdict) {
        case ApiVerdict::Warn: return "warn";
        case ApiVerdict::Refuse: return "refuse";
        case ApiVerdict::Accept: break;
    }
    return "accept";
}

/// Reports one table against the surface and returns whether it may be loaded.
[[nodiscard]] bool report_surface(const CodeTable& table, const SurfaceCheck& check, const ApiSurface& surface,
                                  const std::string& engine_id) {
    std::cout << std::format("{}: built for {} {}, engine is {}: {} ({} symbol(s) outside the surface)\n",
                             table.id, engine_id, check.built.text(), surface.version.text(),
                             verdict_word(check.verdict), check.outside.size());
    for (const std::string& name : check.outside) std::cout << std::format("  outside: {}\n", name);
    return check.verdict != ApiVerdict::Refuse;
}


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
    std::string api_surface;   ///< the surface a table being built is checked against, when given
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
                 "  codetab dumphead <table.codetab> [-o <names.h>]              what it defines, as an index\n"
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
        else if (argument == "--api") { if (!value(options.api_surface)) return false; }
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

// Defined below, where the surface check lives: a build asks it before it writes anything.
struct SurfaceCheck;
[[nodiscard]] bool check_before_writing(const CodeTable& table, const Options& options, std::string& error);

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
    if (!check_before_writing(*table, options, error)) return 1;
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
/// The surface check a build does before it writes a table: a mod that reaches outside the engine's
/// published surface is refused here, where its author can do something about it, rather than when
/// somebody tries to run it.
[[nodiscard]] bool check_before_writing(const CodeTable& table, const Options& options, std::string& error) {
    if (options.api_surface.empty()) return true;
    std::optional<ApiSurface> surface = ApiSurface::load(options.api_surface, &error);
    if (!surface.has_value()) return false;
    const SurfaceCheck check = surface_check(table, *surface, options.engine_id);
    return report_surface(table, check, *surface, options.engine_id);
}

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
    if (!check_before_writing(*table, options, error)) return 1;
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

/// Checks tables against the published surface and the version rule. What a module asks the engine for
/// is what no module in the set defines: everything else is answered inside the merged tables.
[[nodiscard]] int api_check(const Options& options) {
    if (options.surface.empty()) {
        std::cerr << "codetab api: --surface <engine.api> is required\n";
        return 2;
    }
    std::string error;
    std::optional<ApiSurface> surface = ApiSurface::load(options.surface, &error);
    if (!surface.has_value()) {
        std::cerr << std::format("codetab api: {}\n", error);
        return 2;
    }
    // --host overrides what the surface says it is, which is how "what if the engine moves" is asked.
    const std::optional<ApiVersion> host = ApiVersion::parse(options.host_version);
    if (!host.has_value()) {
        std::cerr << std::format("codetab api: '{}' is not a version (want major.minor)\n", options.host_version);
        return 2;
    }
    surface->version = *host;
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
        const SurfaceCheck check = surface_check(table, *surface, options.engine_id, defined);
        if (!report_surface(table, check, *surface, options.engine_id)) ++refused;
    }
    return refused == 0 ? 0 : 1;
}

/// Asks the platform's demangler what a list of symbols means. A table's names are the compiler's,
/// so the compiler's own tool is the right one to ask - and if it is not there, the names are printed
/// as they are rather than guessed at.
[[nodiscard]] std::vector<std::string> demangle(const std::vector<std::string>& names) {
    if (names.empty()) return {};
    const std::string path = (std::filesystem::temp_directory_path() / "codetab-names.txt").string();
    {
        std::ofstream file(path, std::ios::trunc);
        for (const std::string& name : names) file << name << "\n";
    }
    std::vector<std::string> out;
    const std::string command = "c++filt < '" + path + "'";
    if (FILE* pipe = popen(command.c_str(), "r"); pipe != nullptr) {
        char line[4096];
        while (std::fgets(line, sizeof(line), pipe) != nullptr) {
            std::string text(line);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            out.push_back(std::move(text));
        }
        pclose(pipe);
    }
    std::filesystem::remove(path);
    return out.size() == names.size() ? out : names;
}

/// Writes a header for a table: what it defines, grouped the way C++ groups it, so an author - and an
/// editor - can see the shape of it.
///
/// It cannot be a header of real declarations, and it says so where it is written: a mangled name
/// carries the name and the argument types but **not the return type**, and a table carries no types
/// for its data at all. So a function is a signature with the return type left out, which is a comment
/// rather than a declaration that would not compile.
[[nodiscard]] int dump_head(const Options& options) {
    if (options.sources.empty()) {
        std::cerr << "codetab dumphead: which table?\n";
        return 2;
    }
    std::string error;
    std::optional<CodeTable> table = CodeTable::load(options.sources[0], &error);
    if (!table.has_value()) {
        std::cerr << std::format("codetab dumphead: {}\n", error);
        return 1;
    }
    std::vector<std::string> names;
    for (const t2d::CodeTableSymbol& symbol : table->symbols) {
        // What this table defines *for others*: a strong, shared definition is one somebody else may
        // replace, and a weak one is one everybody already has.
        if (!symbol.defined() || !symbol.shared() || symbol.name.empty()) continue;
        if (symbol.binding != ObjectSymbolBinding::Global) continue;
        names.push_back(symbol.name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    const std::vector<std::string> readable = demangle(names);

    std::map<std::string, std::vector<std::string>> by_scope;
    for (const std::string& signature : readable) {
        const std::size_t paren = signature.find('(');
        const std::string head = paren == std::string::npos ? signature : signature.substr(0, paren);
        const std::size_t last = head.rfind("::");
        by_scope[last == std::string::npos ? std::string("(global)") : head.substr(0, last)].push_back(signature);
    }

    std::ofstream file;
    std::ostream* out = &std::cout;
    if (!options.output.empty()) {
        file.open(options.output, std::ios::trunc);
        if (!file) {
            std::cerr << std::format("codetab dumphead: '{}' cannot be written\n", options.output);
            return 1;
        }
        out = &file;
    }
    *out << "// What '" << table->id << "' defines, generated by codetab dumphead from "
         << options.sources[0] << ".\n"
         << "//\n"
         << "// What this is: an index of the names the table carries, grouped the way C++ groups them, so\n"
         << "// an editor can complete them and an author can see what is there.\n"
         << "//\n"
         << "// What it cannot be: real declarations. A mangled name carries the name and the argument types\n"
         << "// but not the return type, and a table carries no types for its data at all - so every line\n"
         << "// below is a comment. To replace one of these, declare it yourself with the same signature (the\n"
         << "// engine's headers have the real declaration and the return type); to call into the engine,\n"
         << "// include its headers. This file is what tells you which names are there.\n"
         << "//\n"
         << "// " << names.size() << " symbol(s), " << by_scope.size() << " scope(s).\n\n";
    for (const auto& [scope, signatures] : by_scope) {
        *out << "// " << scope << "\n";
        for (const std::string& signature : signatures) *out << "//   " << signature << "\n";
        *out << "\n";
    }
    if (file.is_open()) {
        std::cout << std::format("{}: {} symbol(s) in {} scope(s) written to {}\n", table->id, names.size(),
                                 by_scope.size(), options.output);
    }
    return 0;
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
    if (command == "dumphead") {
        Options options;
        std::string error;
        std::vector<char*> rest(argv + 2, argv + argc);
        std::vector<char*> with_name;
        with_name.push_back(argv[0]);
        with_name.insert(with_name.end(), rest.begin(), rest.end());
        if (!parse(static_cast<int>(with_name.size()), with_name.data(), options, error)) {
            std::cerr << std::format("codetab dumphead: {}\n", error);
            return 2;
        }
        return dump_head(options);
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
