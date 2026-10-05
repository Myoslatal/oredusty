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
using t2d::CodeAbi;
using t2d::CodeTableSection;
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
    /// What the build's own ABI is: recorded unless --no-abi, and read back by "codetab abi".
    bool no_abi = false;
    std::string headers_file;   ///< --headers: what a pack's objects were compiled against
    std::string record_file;    ///< --record: where "codetab abi" writes this build's own record
};

void usage() {
    std::cout << "codetab - pack compiled C++ into a code table, and read one back\n"
                 "\n"
                 "  codetab build <source.cpp>... -o <out.codetab> [options]   compile, then pack\n"
                 "  codetab pack <object.o>... -o <out.codetab> [options]        pack what is there\n"
                 "  codetab dump <table.codetab>                                 what is in one\n"
                 "  codetab api --surface <engine.api> <table.codetab>...        check it against the engine\n"
                 "  codetab dumphead <table.codetab> [-o <names.h>]              what it defines, as an index\n"
                 "  codetab abi <table.codetab>...                               what they were built as\n"
                 "  codetab abi --record <file> [options]                        write this build's own record\n"
                 "\n"
                 "what a table was built as (docs/ABI.md):\n"
                 "  every table carries the compiler's own answer to 'what is this ABI' - the macros, the\n"
                 "  sizes and alignments of the types two modules share, the cpu features the code needs,\n"
                 "  and a hash of every project header it was compiled against. Two tables whose records\n"
                 "  agree are one program and are merged fully; ones that differ are refused, because a\n"
                 "  module whose types are not the program's cannot call it correctly. -O level, NDEBUG,\n"
                 "  RTTI and exceptions may differ: none of them changes a layout.\n"
                 "  --no-abi            do not record it (the table then says nothing, and the load says so)\n"
                 "  --headers <file>    the headers a pack's objects were compiled against, one per line\n"
                 "                      (pack: the record is asked of the compiler with the --opt/--define/\n"
                 "                       --include given here, so pass what the objects were built with)\n"
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
                 "  --verbose           print the compile commands\n"
                 "\n"
                 "the published surface (docs/ENGINE_API.md, engine.api in the repository root):\n"
                 "  --api <file>        check what is being built against this surface before it is\n"
                 "                      written (build and pack): a symbol the surface does not list is\n"
                 "                      refused here, where the author can still do something about it\n"
                 "  --surface <file>    the surface 'codetab api' checks tables against\n"
                 "  --host <version>    pretend the engine is this version (default: what the surface\n"
                 "                      says). 'codetab api --host 1.1' asks what a move would break\n"
                 "  --engine <id>       what the host is called in a requirement (default: engine)\n";
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
        else if (argument == "--no-abi") options.no_abi = true;
        else if (argument == "--headers") { if (!value(options.headers_file)) return false; }
        else if (argument == "--record") { if (!value(options.record_file)) return false; }
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


// --- what a build's ABI is -------------------------------------------------------------------------
//
// The tool asks the compiler. A generated translation unit prints one fact per line - the macros that
// decide what a type looks like, the size and alignment of the types two modules share, and the CPU
// features the code was compiled for - and the tool runs it with the same flags, includes and defines
// the module itself was compiled with. The compiler is the only thing that knows its own ABI, so it is
// the thing asked (docs/ABI.md).

/// FNV-1a. The record is a check against accidental drift - a table is not a sandbox, and nothing here
/// is a security boundary - so a hash that is cheap and written here beats a dependency.
[[nodiscard]] std::string hash_text(std::string_view text) {
    unsigned long long hash = 1469598103934665603ull;
    for (const char letter : text) {
        hash ^= static_cast<unsigned char>(letter);
        hash *= 1099511628211ull;
    }
    return std::format("{:016x}", hash);
}

/// The probe's source. Every line is one fact; the tool decides which of them must agree.
[[nodiscard]] const char* probe_source() {
    return R"PROBE(// Generated by codetab: what this build's C++ ABI is.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#define T2D_ABI_STR(x) #x
#define T2D_ABI_TEXT(x) T2D_ABI_STR(x)
#define T2D_ABI_FLAG(name) std::printf(name "=1\n")
#define T2D_ABI_ABSENT(name) std::printf(name "=0\n")

template <class T>
static void layout(const char* name) {
    std::printf("sizeof(%s)=%zu\n", name, sizeof(T));
    std::printf("alignof(%s)=%zu\n", name, alignof(T));
}

int main() {
    // The compiler's own numbers: what a type is made of.
    std::printf("__GXX_ABI_VERSION=%s\n", T2D_ABI_TEXT(__GXX_ABI_VERSION));
    std::printf("__cplusplus=%s\n", T2D_ABI_TEXT(__cplusplus));
    std::printf("__SIZEOF_POINTER__=%s\n", T2D_ABI_TEXT(__SIZEOF_POINTER__));
    std::printf("__SIZEOF_LONG__=%s\n", T2D_ABI_TEXT(__SIZEOF_LONG__));
    std::printf("__SIZEOF_LONG_DOUBLE__=%s\n", T2D_ABI_TEXT(__SIZEOF_LONG_DOUBLE__));
    std::printf("__SIZEOF_SIZE_T__=%s\n", T2D_ABI_TEXT(__SIZEOF_SIZE_T__));
    std::printf("__CHAR_BIT__=%s\n", T2D_ABI_TEXT(__CHAR_BIT__));
    std::printf("__BYTE_ORDER__=%s\n", T2D_ABI_TEXT(__BYTE_ORDER__));
    std::printf("__ORDER_LITTLE_ENDIAN__=%s\n", T2D_ABI_TEXT(__ORDER_LITTLE_ENDIAN__));
    std::printf("__STDCPP_DEFAULT_NEW_ALIGNMENT__=%s\n", T2D_ABI_TEXT(__STDCPP_DEFAULT_NEW_ALIGNMENT__));
    std::printf("__GCC_ATOMIC_INT_LOCK_FREE=%s\n", T2D_ABI_TEXT(__GCC_ATOMIC_INT_LOCK_FREE));
    std::printf("__GCC_ATOMIC_LLONG_LOCK_FREE=%s\n", T2D_ABI_TEXT(__GCC_ATOMIC_LLONG_LOCK_FREE));
    std::printf("__SIZEOF_INT128__=%s\n", T2D_ABI_TEXT(__SIZEOF_INT128__));
#ifdef _GLIBCXX_USE_CXX11_ABI
    std::printf("_GLIBCXX_USE_CXX11_ABI=%s\n", T2D_ABI_TEXT(_GLIBCXX_USE_CXX11_ABI));
#else
    std::printf("_GLIBCXX_USE_CXX11_ABI=unset\n");
#endif
#ifdef _GLIBCXX_DEBUG
    T2D_ABI_FLAG("_GLIBCXX_DEBUG");
#else
    T2D_ABI_ABSENT("_GLIBCXX_DEBUG");
#endif
#ifdef _GLIBCXX_SANITIZE_VECTOR
    T2D_ABI_FLAG("_GLIBCXX_SANITIZE_VECTOR");
#else
    T2D_ABI_ABSENT("_GLIBCXX_SANITIZE_VECTOR");
#endif

    // The types a mod and an engine hand to each other. If one of these differs, one of the two is
    // reading the other's memory with the wrong map.
    layout<std::string>("std::string");
    layout<std::string_view>("std::string_view");
    layout<std::vector<int>>("std::vector<int>");
    layout<std::map<int, int>>("std::map<int, int>");
    layout<std::unordered_map<int, int>>("std::unordered_map<int, int>");
    layout<std::shared_ptr<int>>("std::shared_ptr<int>");
    layout<std::unique_ptr<int>>("std::unique_ptr<int>");
    layout<std::function<void()>>("std::function<void()>");
    layout<std::filesystem::path>("std::filesystem::path");
    layout<std::optional<int>>("std::optional<int>");
    layout<std::variant<int, double>>("std::variant<int, double>");

    // What may differ: none of these changes what a type looks like, and a build that differs only
    // here is still one program (docs/ABI.md).
#ifdef NDEBUG
    T2D_ABI_FLAG("NDEBUG");
#else
    T2D_ABI_ABSENT("NDEBUG");
#endif
#ifdef __EXCEPTIONS
    T2D_ABI_FLAG("__EXCEPTIONS");
#else
    T2D_ABI_ABSENT("__EXCEPTIONS");
#endif
#ifdef __GXX_RTTI
    T2D_ABI_FLAG("__GXX_RTTI");
#else
    T2D_ABI_ABSENT("__GXX_RTTI");
#endif
#ifdef _GLIBCXX_ASSERTIONS
    T2D_ABI_FLAG("_GLIBCXX_ASSERTIONS");
#else
    T2D_ABI_ABSENT("_GLIBCXX_ASSERTIONS");
#endif
#ifdef __OPTIMIZE__
    T2D_ABI_FLAG("__OPTIMIZE__");
#else
    T2D_ABI_ABSENT("__OPTIMIZE__");
#endif
#ifdef __SANITIZE_ADDRESS__
    T2D_ABI_FLAG("__SANITIZE_ADDRESS__");
#else
    T2D_ABI_ABSENT("__SANITIZE_ADDRESS__");
#endif
#ifdef __SANITIZE_THREAD__
    T2D_ABI_FLAG("__SANITIZE_THREAD__");
#else
    T2D_ABI_ABSENT("__SANITIZE_THREAD__");
#endif
#ifdef _FORTIFY_SOURCE
    std::printf("_FORTIFY_SOURCE=%s\n", T2D_ABI_TEXT(_FORTIFY_SOURCE));
#else
    std::printf("_FORTIFY_SOURCE=0\n");
#endif
    // Control-flow enforcement: code built for it starts every function with an instruction a machine
    // that enforces it requires, so two builds that disagree are not interchangeable (docs/ABI.md H12).
#ifdef __CET__
    std::printf("__CET__=%s\n", T2D_ABI_TEXT(__CET__));
#else
    std::printf("__CET__=0\n");
#endif

    // What the code needs of the machine. Absent means "not required", so this list only grows.
#ifdef __SSE4_2__
    T2D_ABI_FLAG("__SSE4_2__");
#endif
#ifdef __AVX__
    T2D_ABI_FLAG("__AVX__");
#endif
#ifdef __AVX2__
    T2D_ABI_FLAG("__AVX2__");
#endif
#ifdef __FMA__
    T2D_ABI_FLAG("__FMA__");
#endif
#ifdef __BMI__
    T2D_ABI_FLAG("__BMI__");
#endif
#ifdef __BMI2__
    T2D_ABI_FLAG("__BMI2__");
#endif
#ifdef __POPCNT__
    T2D_ABI_FLAG("__POPCNT__");
#endif
#ifdef __AES__
    T2D_ABI_FLAG("__AES__");
#endif
#ifdef __PCLMUL__
    T2D_ABI_FLAG("__PCLMUL__");
#endif
#ifdef __F16C__
    T2D_ABI_FLAG("__F16C__");
#endif
#ifdef __ADX__
    T2D_ABI_FLAG("__ADX__");
#endif
#ifdef __SHA__
    T2D_ABI_FLAG("__SHA__");
#endif
#ifdef __GFNI__
    T2D_ABI_FLAG("__GFNI__");
#endif
#ifdef __AVX512F__
    T2D_ABI_FLAG("__AVX512F__");
#endif
#ifdef __AVX512VL__
    T2D_ABI_FLAG("__AVX512VL__");
#endif
#ifdef __AVX512BW__
    T2D_ABI_FLAG("__AVX512BW__");
#endif
#ifdef __AVX512DQ__
    T2D_ABI_FLAG("__AVX512DQ__");
#endif
#ifdef __AVX512VNNI__
    T2D_ABI_FLAG("__AVX512VNNI__");
#endif
#ifdef __AVX512BF16__
    T2D_ABI_FLAG("__AVX512BF16__");
#endif
#ifdef __AVXVNNI__
    T2D_ABI_FLAG("__AVXVNNI__");
#endif
    return 0;
}
)PROBE";
}

/// The compiler macro for a CPU feature, and the name machine_cpu_features() knows it by.
[[nodiscard]] const char* cpu_feature_of(std::string_view macro) {
    const struct {
        const char* macro;
        const char* name;
    } table[] = {{"__SSE4_2__", "sse4.2"},   {"__AVX__", "avx"},         {"__AVX2__", "avx2"},
                 {"__FMA__", "fma"},         {"__BMI__", "bmi"},         {"__BMI2__", "bmi2"},
                 {"__POPCNT__", "popcnt"},   {"__AES__", "aes"},         {"__PCLMUL__", "pclmul"},
                 {"__F16C__", "f16c"},       {"__ADX__", "adx"},         {"__SHA__", "sha"},
                 {"__GFNI__", "gfni"},       {"__AVX512F__", "avx512f"}, {"__AVX512VL__", "avx512vl"},
                 {"__AVX512BW__", "avx512bw"}, {"__AVX512DQ__", "avx512dq"},
                 {"__AVX512VNNI__", "avx512vnni"}, {"__AVX512BF16__", "avx512bf16"},
                 {"__AVXVNNI__", "avxvnni"}};
    for (const auto& entry : table) {
        if (macro == entry.macro) return entry.name;
    }
    return nullptr;
}

/// Facts that may differ without the two builds stopping being one program. None of them changes a
/// layout - measured, docs/ABI.md §2 H4 - and a module built at -O0 next to an engine built at -O3 is
/// exactly the case that has to keep working.
[[nodiscard]] bool fact_may_differ(std::string_view name) {
    for (const char* allowed : {"NDEBUG", "__EXCEPTIONS", "__GXX_RTTI", "__OPTIMIZE__", "__OPTIMIZE_SIZE__",
                                "_GLIBCXX_ASSERTIONS", "__SANITIZE_ADDRESS__", "__SANITIZE_THREAD__",
                                "_GLIBCXX_SANITIZE_VECTOR", "_FORTIFY_SOURCE"}) {
        if (name == allowed) return true;
    }
    return false;
}

/// Runs the probe and sorts what it says into the three lists the record carries.
[[nodiscard]] std::optional<CodeAbi> probe_abi(const Options& options, const std::vector<std::string>& flags,
                                               std::string& error) {
    const std::filesystem::path work = std::filesystem::temp_directory_path() /
                                       ("codetab-abi-" + hash_text(options.output + options.record_file + options.compiler));
    std::error_code code;
    std::filesystem::create_directories(work, code);
    const std::filesystem::path source = work / "abi_probe.cpp";
    const std::filesystem::path binary = work / "abi_probe";
    {
        std::ofstream file(source, std::ios::trunc);
        if (!file) {
            error = std::format("'{}' cannot be written", source.string());
            return std::nullopt;
        }
        file << probe_source();
    }
    // The probe is compiled the way the module was: same standard, same optimisation, same includes,
    // same defines. Anything else would be asking the compiler about a different build.
    std::string command = quote(options.compiler);
    for (const std::string& flag : flags) command += " " + quote(flag);
    for (const std::string& include : options.includes) command += " -I " + quote(include);
    for (const std::string& define : options.defines) command += " -D " + quote(define);
    command += " " + quote(source.string()) + " -o " + quote(binary.string());
    if (options.verbose) std::cout << command << "\n";
    if (std::system(command.c_str()) != 0) {
        error = "the ABI probe did not compile";
        return std::nullopt;
    }
    const std::string run = quote(binary.string());
    std::string output;
    if (FILE* pipe = popen(run.c_str(), "r"); pipe != nullptr) {
        char line[4096];
        while (std::fgets(line, sizeof(line), pipe) != nullptr) output += line;
        pclose(pipe);
    }
    std::filesystem::remove_all(work, code);
    if (output.empty()) {
        error = "the ABI probe said nothing";
        return std::nullopt;
    }

    CodeAbi abi;
    std::vector<std::pair<std::string, std::string>> required;
    while (!output.empty()) {
        const std::size_t end = output.find('\n');
        std::string line = output.substr(0, end);
        output = end == std::string::npos ? std::string{} : output.substr(end + 1);
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string name = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (const char* feature = cpu_feature_of(name); feature != nullptr) {
            abi.cpu.emplace_back(feature);
        } else if (fact_may_differ(name)) {
            abi.allowed.emplace_back(name, value);
        } else {
            required.emplace_back(name, value);
        }
    }
    // The defines the build was given are facts too: they are how a header that reads one ends up
    // meaning something else. They may differ - a mod that defines its own version number is not a
    // different ABI - but a difference is worth saying.
    for (const std::string& define : options.defines) {
        const std::size_t equals = define.find('=');
        const std::string name = equals == std::string::npos ? define : define.substr(0, equals);
        const std::string value = equals == std::string::npos ? "1" : define.substr(equals + 1);
        abi.allowed.emplace_back("define." + name, value);
    }
    std::sort(required.begin(), required.end());
    std::sort(abi.allowed.begin(), abi.allowed.end());
    std::sort(abi.cpu.begin(), abi.cpu.end());
    abi.cpu.erase(std::unique(abi.cpu.begin(), abi.cpu.end()), abi.cpu.end());

    std::string fingerprint;
    for (const auto& [name, value] : required) fingerprint += name + "=" + value + "\n";
    abi.required = std::move(required);
    abi.id = hash_text(fingerprint);
    return abi;
}

/// The key a header is recorded under: the path relative to the include directory it was found in.
/// Absolute paths are not portable between a build tree and the package a mod author compiles against
/// (they are different directories holding the same files), and a system header is the C++ library's
/// own business - its ABI is a fact in the record, not a file.
[[nodiscard]] std::string header_key(const std::string& path, const std::vector<std::string>& includes) {
    std::error_code code;
    const std::filesystem::path full = std::filesystem::weakly_canonical(path, code);
    if (code) return {};
    std::string best;
    for (const std::string& include : includes) {
        const std::filesystem::path directory = std::filesystem::weakly_canonical(include, code);
        if (code) continue;
        const std::filesystem::path relative = full.lexically_relative(directory);
        if (relative.empty() || relative.native().starts_with("..")) continue;
        const std::string text = relative.generic_string();
        if (text.starts_with("..")) continue;
        if (best.empty() || text.size() < best.size()) best = text;
    }
    return best;
}

/// Reads a makefile-style depfile ("object: source header header ...") and returns the headers. The
/// compiler wrote it, so the list is what was *actually* included rather than what somebody meant to.
[[nodiscard]] std::vector<std::string> headers_in_depfile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) return {};
    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::vector<std::string> headers;
    std::string current;
    for (const char letter : text) {
        if (letter == '\\') continue;              // the continuation that makes one line of many
        if (letter == '\n' || letter == ' ' || letter == '\t') {
            if (!current.empty()) headers.push_back(current);
            current.clear();
            continue;
        }
        current += letter;
    }
    if (!current.empty()) headers.push_back(current);
    // The first word is the target ("object.o:"), and the source itself is not a header.
    if (!headers.empty()) headers.erase(headers.begin());
    std::vector<std::string> out;
    for (std::string& header : headers) {
        if (!header.empty() && header.back() == ':') header.pop_back();
        if (header.empty() || header.back() == ':') continue;
        out.push_back(std::move(header));
    }
    return out;
}

/// Adds the header hashes to a record: the paths a mod and an engine share, each with the hash of the
/// bytes it was compiled against.
void add_header_hashes(CodeAbi& abi, const std::vector<std::string>& headers,
                       const std::vector<std::string>& includes) {
    for (const std::string& header : headers) {
        const std::string key = header_key(header, includes);
        if (key.empty()) continue;   // a system header: not ours to pin
        std::ifstream stream(header, std::ios::binary);
        if (!stream) continue;
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const std::string hash = hash_text(bytes);
        const auto found = std::find_if(abi.headers.begin(), abi.headers.end(),
                                        [&key](const auto& entry) { return entry.first == key; });
        if (found == abi.headers.end()) abi.headers.emplace_back(key, hash);
        else if (found->second != hash) found->second = hash;   // two copies of one path: the last read wins
    }
    std::sort(abi.headers.begin(), abi.headers.end());
}


/// Reads a list of header paths: one per line, or separated by spaces.
[[nodiscard]] std::vector<std::string> headers_in_file(const std::string& path) {
    if (path.empty()) return {};
    std::ifstream stream(path);
    if (!stream) return {};
    std::vector<std::string> headers;
    std::string current;
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    for (const char letter : text) {
        if (letter == '\n' || letter == ' ' || letter == '\t' || letter == '\r') {
            if (!current.empty()) headers.push_back(current);
            current.clear();
            continue;
        }
        current += letter;
    }
    if (!current.empty()) headers.push_back(current);
    return headers;
}

/// What a summary line says about a table's record.
[[nodiscard]] std::string built_as(const CodeTable& table) {
    return table.abi.recorded() ? std::format(", built as {}", table.abi.id)
                                : std::string(", built as: not recorded");
}

/// Asks the compiler what this build's ABI is and puts it in the table. A probe that cannot run is not
/// a failed build - it is a table that says it does not know, which the load then says out loud.
void record_abi(CodeTable& table, const Options& options, const std::vector<std::string>& flags,
                const std::vector<std::string>& headers) {
    if (options.no_abi) return;
    std::string error;
    std::optional<CodeAbi> abi = probe_abi(options, flags, error);
    if (!abi.has_value()) {
        std::cerr << std::format("codetab: no ABI record: {}\n", error);
        return;
    }
    add_header_hashes(*abi, headers, options.includes);
    table.abi = std::move(*abi);
}

/// What a table says it was built as, or - with --record - what this build is.
[[nodiscard]] int abi_command(const Options& options) {
    if (!options.record_file.empty()) {
        // The optimisation the build used, so that the facts which may differ (__OPTIMIZE__, and the
        // library assertions -O0 turns on) describe this build rather than a default one.
        const std::vector<std::string> flags = {"-std=c++20", options.optimization};
        std::string error;
        std::optional<CodeAbi> abi = probe_abi(options, flags, error);
        if (!abi.has_value()) {
            std::cerr << std::format("codetab abi: {}\n", error);
            return 1;
        }
        add_header_hashes(*abi, headers_in_file(options.headers_file), options.includes);
        std::ofstream file(options.record_file, std::ios::trunc);
        if (!file) {
            std::cerr << std::format("codetab abi: '{}' cannot be written\n", options.record_file);
            return 1;
        }
        file << "# what this build was compiled as, written by codetab (docs/ABI.md)\n" << abi->text();
        std::cout << std::format("{}: built as {} ({} fact(s) must agree, {} may differ, {} header(s), "
                                 "{} cpu feature(s))\n",
                                 options.record_file, abi->id, abi->required.size(), abi->allowed.size(),
                                 abi->headers.size(), abi->cpu.size());
        return 0;
    }
    if (options.sources.empty()) {
        std::cerr << "codetab abi: which table, or --record <file>?\n";
        return 2;
    }
    int unknown = 0;
    for (const std::string& path : options.sources) {
        std::string error;
        std::optional<CodeTable> table = CodeTable::load(path, &error);
        if (!table.has_value()) {
            // Not a table: an engine's own record is the same lines in a plain file ("engine.abi"), and
            // "what is this engine" is exactly what a mod author asks about it.
            if (std::optional<CodeAbi> abi = CodeAbi::load(path, &error); abi.has_value() && abi->recorded()) {
                std::cout << std::format("{}: built as {} ({} fact(s) must agree, {} may differ, {} header(s), "
                                         "{} cpu feature(s))\n",
                                         path, abi->id, abi->required.size(), abi->allowed.size(),
                                         abi->headers.size(), abi->cpu.size());
                for (const auto& [name, value] : abi->required) std::cout << std::format("  must agree  {}={}\n", name, value);
                for (const auto& [name, value] : abi->allowed) std::cout << std::format("  may differ  {}={}\n", name, value);
                for (const auto& [name, hash] : abi->headers) std::cout << std::format("  header      {}={}\n", name, hash);
                for (const std::string& feature : abi->cpu) std::cout << std::format("  needs       {}\n", feature);
                continue;
            }
            std::cerr << std::format("codetab abi: {}\n", error);
            return 2;
        }
        if (!table->abi.recorded()) {
            std::cout << std::format("{}: built as: not recorded\n", path);
            ++unknown;
            continue;
        }
        std::cout << std::format("{}: built as {} ({} fact(s) must agree, {} may differ, {} header(s), "
                                 "{} cpu feature(s))\n",
                                 path, table->abi.id, table->abi.required.size(), table->abi.allowed.size(),
                                 table->abi.headers.size(), table->abi.cpu.size());
        for (const auto& [name, value] : table->abi.required) {
            std::cout << std::format("  must agree  {}={}\n", name, value);
        }
        for (const auto& [name, value] : table->abi.allowed) {
            std::cout << std::format("  may differ  {}={}\n", name, value);
        }
        for (const auto& [name, hash] : table->abi.headers) {
            std::cout << std::format("  header      {}={}\n", name, hash);
        }
        for (const std::string& feature : table->abi.cpu) {
            std::cout << std::format("  needs       {}\n", feature);
        }
    }
    return unknown == 0 ? 0 : 1;
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
    const std::vector<std::string> probe_flags = flags;   // before the include/define pairs: probe_abi adds them
    for (const std::string& include : options.includes) { flags.push_back("-I"); flags.push_back(include); }
    for (const std::string& define : options.defines) { flags.push_back("-D"); flags.push_back(define); }

    std::vector<ObjectFile> objects;
    std::vector<std::string> headers;
    for (const std::string& source : options.sources) {
        const std::filesystem::path object = work / (std::filesystem::path(source).stem().string() + ".o");
        // The compiler is asked what it included, not what somebody meant it to include: that list is
        // what the ABI record pins (docs/ABI.md).
        const std::filesystem::path depfile = work / (std::filesystem::path(source).stem().string() + ".d");
        std::string command = quote(options.compiler);
        for (const std::string& flag : flags) command += " " + quote(flag);
        command += " -MMD -MF " + quote(depfile.string()) + " -c " + quote(source) + " -o " +
                   quote(object.string());
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
        for (std::string& header : headers_in_depfile(depfile.string())) headers.push_back(std::move(header));
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
    record_abi(*table, options, probe_flags, headers);
    if (!check_before_writing(*table, options, error)) {
        if (!error.empty()) std::cerr << std::format("codetab build: {}\n", error);
        return 1;
    }
    if (!table->save(options.output, &error)) {
        std::cerr << std::format("codetab build: {}\n", error);
        return 1;
    }
    if (!options.keep) std::filesystem::remove_all(work, code);
    std::cout << std::format("{}: {} object(s) -> {} section(s), {} symbol(s), {} relocation(s), {} overridable "
                             "name(s){}\n",
                             options.output, objects.size(), table->sections.size(), table->symbols.size(),
                             table->relocations.size(), table->overridable_symbols().size(), built_as(*table));
    return 0;
}

/// Packs objects somebody else compiled. \c build drives the compiler itself, which is what a mod
/// author wants; a build system that already knows the include paths, the defines and the flags wants
/// this one instead, because then there is one place that decides how the code is compiled.
/// The surface check a build does before it writes a table: a mod that reaches outside the engine's
/// published surface is refused here, where its author can do something about it, rather than when
/// somebody tries to run it.
[[nodiscard]] bool check_before_writing(const CodeTable& table, const Options& options, std::string& error) {
    // A table that carries exception machinery is fine: the runtime registers the frame descriptions a
    // table brings, so a throw inside one is walked back out to its handler (docs/ABI.md H1). What is
    // *not* fine is a table that can throw when it was built without them - and that cannot happen,
    // because the compiler refuses to compile a try block without -fexceptions.
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
    // A pack did not compile anything, so what it can say about the ABI is what the probe says when it
    // is run with the same includes and defines - which is why a build system that packs should pass
    // them, and why a table packed without them says so rather than pretending (docs/ABI.md).
    const std::vector<std::string> probe_flags = {"-std=c++20", options.optimization};
    record_abi(*table, options, probe_flags, headers_in_file(options.headers_file));
    if (!check_before_writing(*table, options, error)) {
        if (!error.empty()) std::cerr << std::format("codetab pack: {}\n", error);
        return 1;
    }
    if (!table->save(options.output, &error)) {
        std::cerr << std::format("codetab pack: {}\n", error);
        return 1;
    }
    std::cout << std::format("{}: {} object(s) -> {} section(s), {} symbol(s), {} relocation(s), {} overridable "
                             "name(s){}\n",
                             options.output, objects.size(), table->sections.size(), table->symbols.size(),
                             table->relocations.size(), table->overridable_symbols().size(), built_as(*table));
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
    if (command == "abi") {
        Options options;
        std::string error;
        std::vector<char*> rest(argv + 2, argv + argc);
        std::vector<char*> with_name;
        with_name.push_back(argv[0]);
        with_name.insert(with_name.end(), rest.begin(), rest.end());
        if (!parse(static_cast<int>(with_name.size()), with_name.data(), options, error)) {
            std::cerr << std::format("codetab abi: {}\n", error);
            return 2;
        }
        return abi_command(options);
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
