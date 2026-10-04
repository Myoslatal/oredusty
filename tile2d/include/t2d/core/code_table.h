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
//     source.cpp --(the system compiler)--> object.o --(t2dtab)--> mod.t2dtab
//     game.t2dtab + mod.t2dtab --(CodeImage)--> one address space, mod's symbols winning
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
#include <vector>

namespace t2d {

/// What a table file starts with. The version is bumped when the layout changes; a table written by
/// another version is refused, never guessed at.
inline constexpr char kCodeTableMagic[8] = {'T', '2', 'D', 'T', 'A', 'B', 'L', 'E'};
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

/// One module's worth of code: its bytes, its names, and what has to be filled in.
struct CodeTable {
    std::string id;        ///< what the module is called, for the merge report
    std::string name;      ///< for humans
    std::string version;
    /// Ids of the tables this one expects to be merged with. ("requires" is a keyword in C++20, so the
    /// member carries the plain word and the file carries the key.)
    std::vector<std::string> requirements;

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
        void* address = nullptr;
        ObjectSymbolBinding binding = ObjectSymbolBinding::Global;
    };

    struct Module {
        CodeTable table;
        u8* text = nullptr;
        usize text_size = 0;
        u8* data = nullptr;
        usize data_size = 0;
        u8* got = nullptr;
        usize got_size = 0;
        std::vector<u8*> section_address;
        std::vector<u8*> symbol_address;
        usize got_count = 0;   ///< how many relocations need a slot in the global offset table
        usize got_used = 0;
        bool failed = false;
    };

    bool place(Module& module);
    void resolve_symbols();
    void relocate(Module& module);
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
    bool loaded_ = false;
};

} // namespace t2d
