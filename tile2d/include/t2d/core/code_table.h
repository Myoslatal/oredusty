// Tile2D - the code table: code that was not compiled into this program, in one file, mergeable.
//
// The problem this solves is not "load a library" - the system's dynamic loader already does that -
// but "**replace** what is already there". A shared library cannot do that: a call inside the program
// was bound when the program was linked, and no library loaded later can change where it goes.
//
// So the program's own code and a mod's code are both packed into **tables**, and the runtime builds
// the addresses when the tables are merged: every call, every global and every vtable entry is a
// relocation that is filled in **after** the merge, against a symbol table where a later table's
// definition of a symbol replaces an earlier one. That is what makes "a mod replaces an internal
// function of the game, and the game's own calls go to the mod" work - not a patch to machine code,
// but a link that happened one step later than usual.
//
//     source.cpp --(the system compiler)--> object.o --(codetab)--> mod.codetab
//     game.codetab + mod.codetab --(CodeImage)--> one address space, mod's symbols winning
//
// A table holds sections (the bytes), symbols (what they are called) and relocations (what has to be
// filled in). It is deliberately small: this is a container for what a compiler emitted, not an
// executable format with its own opinions.
//
// What a merge does, in the words the design asked for:
//   * **concatenate** - every module's sections are placed in one address space;
//   * **override**    - a later module's strong definition replaces an earlier one, everywhere, and is
//                       reported (find_previous() is how a mod calls what it replaced);
//   * **inject**      - a name nobody defined before is simply added;
//   * **coalesce**    - weak definitions (inline functions, templates, vtables, typeinfo) keep their
//                       first definition, which is what keeps one C++ program one program.
#pragma once

#include <t2d/core/object_file.h>
#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>


namespace t2d {

/// What a table file starts with. The version is bumped when the layout changes; a table written by
/// another version is refused, never guessed at.
inline constexpr char kCodeTableMagic[8] = {'C', 'O', 'D', 'E', 'T', 'A', 'B', 'L'};
inline constexpr u32 kCodeTableVersion = 1;
/// The only machine a table is placed for today. A table is machine code, so this is checked.
inline constexpr u32 kCodeTableArchX86_64 = 1;

/// Relocation types the runtime applies. The numbers are the object format's own; one it does not
/// know is refused with a message naming it rather than written as if it were understood.
inline constexpr u32 kRelocationAbsolute64 = 1;
inline constexpr u32 kRelocationPc32 = 2;
inline constexpr u32 kRelocationPlt32 = 4;
inline constexpr u32 kRelocationGotPcRel = 9;
inline constexpr u32 kRelocationAbsolute32 = 10;
inline constexpr u32 kRelocationAbsolute32Signed = 11;
inline constexpr u32 kRelocationGotPcRelX = 41;
inline constexpr u32 kRelocationRexGotPcRelX = 42;

struct CodeTableSection {
    std::string name;
    u32 type = kSectionProgbits;
    u32 flags = 0;
    u64 align = 1;
    std::vector<u8> data;
};

struct CodeTableSymbol {
    std::string name;
    u32 section = kInvalidId;   ///< kInvalidId: defined elsewhere, or not at all
    ObjectSymbolKind kind = ObjectSymbolKind::None;
    ObjectSymbolBinding binding = ObjectSymbolBinding::Local;
    /// See ObjectSymbol: 0 default, 1 internal, 2 hidden, 3 protected.
    u8 visibility = 0;
    /// A number rather than an address; its value is what a relocation against it is filled with.
    bool absolute = false;
    u64 value = 0;              ///< offset inside its section
    u64 size = 0;

    [[nodiscard]] bool defined() const { return section != kInvalidId; }
    /// Whether a merge is allowed to know this name at all: a local symbol belongs to its module.
    [[nodiscard]] bool shared() const { return binding != ObjectSymbolBinding::Local; }
};

struct CodeTableRelocation {
    u32 section = 0;
    u32 type = 0;
    u64 offset = 0;
    u32 symbol = 0;
    i64 addend = 0;
};

/// An engine version, written "major.minor".
struct ApiVersion {
    i64 major = 0;
    i64 minor = 0;

    /// Parses "1.0", "2", "3.14". A version that cannot be read is refused rather than guessed at.
    [[nodiscard]] static std::optional<ApiVersion> parse(std::string_view text);
    [[nodiscard]] std::string text() const;
    friend bool operator==(const ApiVersion&, const ApiVersion&) = default;
};

/// What a load decided about one module.
enum class ApiVerdict : u8 { Accept, Warn, Refuse };

/// How far a module that reaches outside the published surface may be from the engine it runs on.
/// One minor version: far enough for a patch or a small release, not far enough to pretend that an
/// internal symbol survived a redesign.
inline constexpr i64 kUnlistedMinorRange = 1;

/// The rule, and the whole of it:
///
///   * a **different major** version is refused, whichever surface the module used - nothing about a
///     program's internals is promised across one;
///   * a module that stays **inside** the published surface loads across the whole major version and
///     says nothing: that surface is what the engine promises;
///   * a module that reaches **outside** it is held to a narrow range - the same version loads quietly,
///     a minor version either way loads with a warning (it may well work, and the person running it
///     should know why it might not), and anything further is refused.
[[nodiscard]] ApiVerdict api_verdict(const ApiVersion& built_against, const ApiVersion& host, bool inside_surface);

/// The engine's published surface: the version it belongs to, and the symbols a table may ask it for
/// (engine.api, docs/ENGINE_API.md).
struct ApiSurface {
    ApiVersion version{};
    std::unordered_set<std::string> symbols;

    [[nodiscard]] static std::optional<ApiSurface> parse(std::string_view text, std::string* error = nullptr);
    [[nodiscard]] static std::optional<ApiSurface> load(const std::string& path, std::string* error = nullptr);

    /// Whether the engine owns this symbol at all. The platform's symbols - libc, libstdc++, the
    /// exception machinery - are not the engine's to publish, so a table is never asked to have them on
    /// the list: a mod may use the C++ library as freely as the engine does.
    [[nodiscard]] static bool engine_symbol(std::string_view name);
    [[nodiscard]] bool contains(std::string_view name) const {
        return symbols.find(std::string(name)) != symbols.end();
    }
};

/// What a module was compiled as, so that "may these two pieces of C++ share one address space?" has
/// an answer that is measured rather than assumed.
///
/// It is deliberately not a compiler name or a list of flags. Those are means; what decides the
/// question is the end - the sizes and alignments of the types two sides share, the C++ library's own
/// ABI macros, and the headers the code was compiled against. Measured: -O0 and -O3 -DNDEBUG agree on
/// every one of those, so they are one program and are merged fully. Two std::string ABIs do not, and
/// no loader can make them: the same name would have to mean two layouts at once (docs/ABI.md).
struct CodeAbi {
    /// The fingerprint over everything that must agree. Empty: the table does not say what it was
    /// built as, and the load says so out loud instead of guessing.
    std::string id;
    /// What must agree, by name: "_GLIBCXX_USE_CXX11_ABI" = "1", "sizeof(std::string)" = "32".
    std::vector<std::pair<std::string, std::string>> required;
    /// What may differ, and is reported when it does: the optimisation level, NDEBUG, RTTI, exceptions,
    /// the defines the build passed. None of them changes a layout, so none of them is a reason to
    /// refuse - and saying which ones differ is how "it was built differently" stops being a mystery.
    std::vector<std::pair<std::string, std::string>> allowed;
    /// Every header the module was compiled against, path -> content hash. Compared path by path: a
    /// header only one side included says nothing about the other.
    std::vector<std::pair<std::string, std::string>> headers;
    /// What the code needs of the machine: "avx2", "sse4.2". Checked against the machine it is loaded
    /// on, not against the other module: code built for a narrower machine runs everywhere.
    std::vector<std::string> cpu;

    [[nodiscard]] bool recorded() const { return !id.empty(); }

    /// The lines a table carries in its metadata - and the same lines an engine writes its own record
    /// as ("abi=<id>", "abi.require.<name>=<value>", ...). One format, so both are read the same way.
    [[nodiscard]] std::string text() const;
    /// Reads the record out of the lines a table carries - or out of a file written by
    /// "codetab abi --record". Nothing here can fail: an unknown key is not this record's business.
    [[nodiscard]] static CodeAbi parse(std::string_view text);
    [[nodiscard]] static std::optional<CodeAbi> load(const std::string& path, std::string* error = nullptr);
};

/// What a load decided about one module's ABI.
enum class AbiVerdict : u8 {
    Match,       ///< the same fingerprint: one program, merged fully
    Unrecorded,  ///< one side does not say what it was built as: loaded, and said out loud
    Refuse,      ///< different fingerprints: what the module means cannot be what the program means
};

/// The rule for two records, and the whole of it:
///
///   * neither side recorded anything -> Unrecorded: the module is loaded and the warning names it;
///   * the fingerprints agree -> Match, and the facts that are *allowed* to differ are reported;
///   * they differ -> Refuse, with \p differences saying which fact, and which header, differs.
///
/// There is no third answer, and in particular no "load it with fewer rights": a module whose types do
/// not match the program's cannot call it correctly, and a half-loaded module that silently could not
/// would be a trap rather than a limitation (docs/ABI.md).
[[nodiscard]] AbiVerdict abi_verdict(const CodeAbi& module, const CodeAbi& reference,
                                     std::vector<std::string>* differences = nullptr);

/// What the machine this program runs on has, by the names CodeAbi::cpu uses ("avx2", "sse4.2", ...).
[[nodiscard]] std::vector<std::string> machine_cpu_features();

/// What a module says it needs before it may be merged: another module's id, and optionally the exact
/// version of it.
///
/// A requirement with no version asks only that somebody provides the id. One with a version asks for
/// exactly that version - there is no "compatible range", because what makes two versions compatible
/// is not known until the ABI of the program they are loaded into is written down. Until then the
/// honest answer to "the engine changed" is to refuse the module and say so, which is what the runtime
/// does (docs/TABLES.md).
struct CodeRequirement {
    std::string id;
    std::string version;   ///< empty: any version will do
};

/// One module's worth of code: its bytes, its names, and what has to be filled in.
struct CodeTable {
    std::string id;        ///< what the module is called, for the merge report
    std::string name;      ///< for humans
    std::string version;
    /// What this module needs to be merged with, by id and optionally by version. ("requires" is a
    /// keyword in C++20, so the member carries the plain word and the file carries the key.)
    std::vector<CodeRequirement> requirements;
    /// What this module was compiled as. A module built by the toolchain carries one; a table from
    /// somewhere else may not, and then the load says so rather than assuming (docs/ABI.md).
    CodeAbi abi;

    std::vector<CodeTableSection> sections;
    std::vector<CodeTableSymbol> symbols;
    std::vector<CodeTableRelocation> relocations;

    /// Packs objects into one table. Everything a section needs to keep working is remapped: a
    /// symbol's section and a relocation's symbol both index the object they came from, and the
    /// merged table is a different list. Sections that occupy no memory when the program runs
    /// (comments, debug info, the symbol table itself) are dropped: a table is what has to be there.
    [[nodiscard]] static std::optional<CodeTable> from_objects(const std::vector<ObjectFile>& objects,
                                                              std::string* error = nullptr);

    [[nodiscard]] static std::optional<CodeTable> parse(ConstSpan<const u8> bytes, std::string* error = nullptr);
    [[nodiscard]] static std::optional<CodeTable> load(const std::string& path, std::string* error = nullptr);
    [[nodiscard]] std::vector<u8> serialize() const;
    [[nodiscard]] bool save(const std::string& path, std::string* error = nullptr) const;

    /// What the table holds, for a person looking at one: what "what is in this mod" asks.
    [[nodiscard]] std::string describe() const;

    /// The symbols a merge could replace: strong global definitions. Weak ones (inline functions,
    /// templates, vtables, typeinfo) coalesce instead, and locals are nobody else's business.
    [[nodiscard]] std::vector<std::string> overridable_symbols() const;
};

struct CodeModuleInfo {
    std::string id;
    std::string name;
    std::string version;
    usize sections = 0;
    usize symbols = 0;
    usize relocations = 0;
    /// The ABI this module was built as, empty when it does not say.
    std::string abi;
    /// Whether it was checked against what it is loaded into and agreed. False also when there was
    /// nothing to check it against - the report says which of the two it was.
    bool abi_match = false;
    /// False: the module was refused - a requirement nothing provides, a version that does not match,
    /// a different ABI, or a relocation that could not be filled in. What it defined is not in the
    /// symbol table.
    bool ok = true;
    /// What the module asks the engine for: the symbols nothing in the merged tables defines, so they
    /// were resolved from the program the tables were loaded by. This is what a surface check reads.
    std::vector<std::string> host_symbols;
};

/// One symbol a merge replaced, and who replaced it.
struct CodeOverride {
    std::string symbol;
    std::string from;       ///< the module that won
    std::string replaced;   ///< the module it took the symbol from
};

struct CodeImageReport {
    std::vector<CodeModuleInfo> modules;
    std::vector<CodeOverride> overrides;
    /// Names no module defined and the running program does not export either.
    std::vector<std::string> unresolved;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    usize symbols = 0;
    usize relocations = 0;
    usize init_calls = 0;

    [[nodiscard]] bool clean() const { return errors.empty(); }
    [[nodiscard]] std::string first_error() const { return errors.empty() ? std::string{} : errors.front(); }
};

/// A merged set of tables, placed in memory and ready to run.
///
/// The image owns the memory every module was copied into, so it outlives every call into them, and
/// nothing may be added once load() has run: a table that arrived later would change where an
/// already-filled relocation points.
class CodeImage {
public:
    CodeImage() = default;
    ~CodeImage();
    T2D_NON_MOVABLE(CodeImage);

    /// Declares the program the tables are loaded by - the engine's own id and version, so that a
    /// module can require it ("this mod was built for mine 1.0"). Without it, a requirement naming the
    /// host is a requirement nothing provides, and the module is refused.
    void declare_host(std::string id, std::string version);

    /// Declares what the program itself was compiled as - the engine's own record, so that a module is
    /// measured against the code it will actually call rather than against a version string. Without
    /// it, the first table added is the reference, which is the same answer when the program and its
    /// first table were built together (they are: games/mine/CMakeLists.txt).
    void declare_host_abi(CodeAbi abi);

    /// Queues a module. Tables are merged in the order they are added: the first definition of a
    /// strong symbol wins, and a later one replaces it and is reported.
    void add(CodeTable table);
    /// Places, resolves, relocates and runs the constructors. Idempotent: the second call is a no-op.
    const CodeImageReport& load();

    /// The address \p name resolves to now - the last definition, which is what every call site in
    /// every module was pointed at.
    [[nodiscard]] void* find(std::string_view name) const;
    /// The definition the winner replaced: how a mod calls the function it overrode.
    [[nodiscard]] void* find_previous(std::string_view name) const;
    template <class F>
    [[nodiscard]] F* function(std::string_view name) const {
        return reinterpret_cast<F*>(find(name));
    }

    [[nodiscard]] const CodeImageReport& report() const { return report_; }
    [[nodiscard]] usize module_count() const { return modules_.size(); }

private:
    struct Definition {
        usize module = 0;
        usize symbol = 0;   ///< which symbol of that module, so two definitions can be compared
        void* address = nullptr;
        ObjectSymbolBinding binding = ObjectSymbolBinding::Global;
    };

    struct Module {
        CodeTable table;
        /// Everything the module needs is in **one** region: its code, the stubs a call that is out of
        /// reach jumps through, and its data, a page apart. One region is what keeps a module's own
        /// references in reach - a call and a load from the global offset table are both 32 bit
        /// displacements, so code and data that belong together must not end up gigabytes apart.
        u8* base = nullptr;
        usize region_size = 0;
        u8* text = nullptr;
        usize text_size = 0;
        u8* trampolines = nullptr;
        usize trampoline_count = 0;
        usize trampoline_used = 0;
        u8* data = nullptr;
        usize data_size = 0;
        u8* got = nullptr;
        std::vector<u8*> section_address;
        std::vector<u8*> symbol_address;
        /// The frame descriptions this module registered, in the order they were registered, so that
        /// destroying the image takes them back out before the memory they describe goes away.
        std::vector<void*> frames;
        usize got_count = 0;   ///< how many relocations need a slot in the global offset table
        usize got_used = 0;
        bool failed = false;
    };

    bool place(Module& module);
    /// Whether everything \p table asks for is here, at the version it asked for.
    [[nodiscard]] bool requirements_met(const CodeTable& table, std::string* error) const;
    void resolve_symbols();
    void relocate(Module& module);
    /// Registers every frame description the module brought, so that a throw inside it can be walked
    /// back out: a table is placed by hand, and an unwinder is not told about code by itself.
    void register_frames(Module& module);
    void run_initialisers();
    /// Drops what a module that failed had defined: a symbol table that still offers it would be
    /// offering code that cannot run.
    void prune_failed();
    void release();

    std::vector<Module> modules_;
    /// Every definition of a name, the winner first. The chain is what find_previous() reads.
    std::unordered_map<std::string, std::vector<Definition>> definitions_;
    /// The module each override came from, in step with report_.overrides.
    std::vector<usize> override_sources_;
    CodeImageReport report_;
    /// The program the tables are loaded by, as declare_host() was told.
    std::string host_id_;
    std::string host_version_;
    std::optional<CodeAbi> host_abi_;
    bool loaded_ = false;
};

} // namespace t2d
