// Tile2D - reading what a C++ compiler produced, so it can be packed into a code table.
//
// A code table (code_table.h) is Tile2D's own answer to "how does code that was not compiled into
// this program get in, and how can it *replace* what is already there". The system compiler still
// does the compiling - there is no second C++ compiler here and there never will be - so the first
// step is to read the object file it emits: sections, symbols and relocations.
//
// Only what a table needs is kept: the allocatable sections, the symbol table, and the relocations
// that patch the sections. Debug info, comments and the other bookkeeping sections are dropped.
//
// The format read is ELF64 little endian, x86-64 (the object format of the platform this was written
// on). Anything else is refused with a message rather than half understood.
#pragma once

#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <vector>

namespace t2d {

/// Section flag bits, spelled the way the object format spells them so a section can be described
/// without a translation table in between.
inline constexpr u32 kSectionWrite = 0x1;
inline constexpr u32 kSectionAlloc = 0x2;
inline constexpr u32 kSectionExec = 0x4;
inline constexpr u32 kSectionTls = 0x400;

/// Section types, same rule: the numbers are the format's.
inline constexpr u32 kSectionProgbits = 1;
inline constexpr u32 kSectionNobits = 8;
inline constexpr u32 kSectionInitArray = 14;
inline constexpr u32 kSectionFiniArray = 15;

enum class ObjectSymbolKind : u8 { None = 0, Function = 2, Data = 1, Section = 3, File = 4 };

/// Local symbols are private to their object; global and weak ones are what a merge decides between.
/// Weak is how a C++ compiler emits anything that may appear in more than one object - an inline
/// function, a template instance, a vtable, a typeinfo - and it is what makes those coalesce instead
/// of colliding.
enum class ObjectSymbolBinding : u8 { Local = 0, Global = 1, Weak = 2 };

struct ObjectSection {
    std::string name;
    u32 type = kSectionProgbits;
    u32 flags = 0;
    u64 align = 1;
    u64 size = 0;
    /// The bytes. A section that occupies no bytes in the file (.bss) has none, and its size is what
    /// matters.
    std::vector<u8> data;

    [[nodiscard]] bool is_alloc() const { return (flags & kSectionAlloc) != 0; }
    [[nodiscard]] bool is_exec() const { return (flags & kSectionExec) != 0; }
    [[nodiscard]] bool is_tls() const { return (flags & kSectionTls) != 0; }
    [[nodiscard]] bool is_nobits() const { return type == kSectionNobits; }
};

struct ObjectSymbol {
    std::string name;
    ObjectSymbolKind kind = ObjectSymbolKind::None;
    ObjectSymbolBinding binding = ObjectSymbolBinding::Local;
    /// 0 default, 1 internal, 2 hidden, 3 protected - the object format's own numbers.
    u8 visibility = 0;
    /// An absolute symbol is a number rather than an address: the linker would fold it in, so a table
    /// keeps its value instead of looking for something to place (docs/ABI.md H11).
    bool absolute = false;
    /// Index into ObjectFile::sections, or kInvalidId when the symbol is undefined here (it is
    /// something the object expects to find elsewhere).
    u32 section = kInvalidId;
    u64 value = 0;
    u64 size = 0;

    [[nodiscard]] bool defined() const { return section != kInvalidId; }
    [[nodiscard]] bool shared() const { return binding != ObjectSymbolBinding::Local; }
};

/// One patch the object asks for: "at \p offset in \p section, write something derived from
/// \p symbol". The type says what to write and is architecture specific (code_table.h lists the
/// ones the runtime understands).
struct ObjectRelocation {
    u32 section = 0;
    u64 offset = 0;
    u32 type = 0;
    u32 symbol = 0;
    i64 addend = 0;
};

/// One COMDAT group: the symbol that names it, and the sections it owns.
///
/// A C++ compiler marks the code of an inline function this way, and when the symbol itself is left in
/// a section a table does not carry (a group is bookkeeping, not something the program runs), the group
/// is the only thing that still says which section the code is in.
struct ObjectGroup {
    u32 signature = 0;          ///< index into ObjectFile::symbols
    std::vector<u32> members;   ///< section indices
};

/// One relocatable object file.
struct ObjectFile {
    std::string source;   ///< where it was read from, for messages
    std::vector<ObjectSection> sections;
    std::vector<ObjectSymbol> symbols;
    std::vector<ObjectRelocation> relocations;
    std::vector<ObjectGroup> groups;

    [[nodiscard]] static std::optional<ObjectFile> load(const std::string& path, std::string* error = nullptr);
    [[nodiscard]] static std::optional<ObjectFile> parse(ConstSpan<const u8> bytes, std::string* error = nullptr);

    /// The section \p symbol lives in, or nullptr when it is undefined here.
    [[nodiscard]] const ObjectSection* section_of(const ObjectSymbol& symbol) const;
    /// A human readable listing: what "what did the compiler actually emit" needs.
    [[nodiscard]] std::string describe() const;
};

} // namespace t2d
