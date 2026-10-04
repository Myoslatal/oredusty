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

#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace {

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
