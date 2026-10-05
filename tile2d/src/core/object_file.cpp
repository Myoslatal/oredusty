#include <t2d/core/object_file.h>

#include <bit>
#include <cstring>
#include <format>
#include <fstream>

namespace t2d {
namespace {

static_assert(std::endian::native == std::endian::little,
              "the object reader reads little endian fields in place; a big endian host needs the "
              "byte swaps adding rather than a silent misread");

// --- the object format's own numbers, only the ones this reader has to know ------------------------
constexpr u16 kElfMachineX86_64 = 62;
constexpr u8 kElfClass64 = 2;
constexpr u8 kElfDataLittle = 1;
constexpr u16 kShnUndef = 0;
constexpr u16 kShnAbs = 0xfff1;
constexpr u16 kShnCommon = 0xfff2;
constexpr u32 kShtSymtab = 2;
constexpr u32 kShtRela = 4;
constexpr u32 kShtGroup = 17;
constexpr u32 kShtNobits = 8;
constexpr usize kElfHeaderSize = 64;
constexpr usize kSectionHeaderSize = 64;
constexpr usize kSymbolSize = 24;
constexpr usize kRelaSize = 24;

[[nodiscard]] u16 read_u16(const u8* at) {
    u16 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
[[nodiscard]] u32 read_u32(const u8* at) {
    u32 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
[[nodiscard]] u64 read_u64(const u8* at) {
    u64 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
[[nodiscard]] i64 read_i64(const u8* at) {
    i64 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

/// A section header, as read. Kept apart from ObjectSection because the names and the bytes live in
/// other sections and are only reachable once every header is known.
struct RawSection {
    u32 name = 0;
    u32 type = 0;
    u64 flags = 0;
    u64 offset = 0;
    u64 size = 0;
    u32 link = 0;
    u32 info = 0;
    u64 align = 1;
    u64 entry_size = 0;
};

[[nodiscard]] std::string_view slice_string(ConstSpan<const u8> table, u32 offset) {
    if (offset >= table.size()) return {};
    usize end = offset;
    while (end < table.size() && table[end] != 0) ++end;
    return std::string_view(reinterpret_cast<const char*>(table.data()) + offset, end - offset);
}

[[nodiscard]] ObjectSymbolKind symbol_kind(u8 type) {
    switch (type) {
        case 1: return ObjectSymbolKind::Data;
        case 2: return ObjectSymbolKind::Function;
        case 3: return ObjectSymbolKind::Section;
        case 4: return ObjectSymbolKind::File;
        default: return ObjectSymbolKind::None;
    }
}

[[nodiscard]] ObjectSymbolBinding symbol_binding(u8 bind) {
    switch (bind) {
        case 1: return ObjectSymbolBinding::Global;
        case 2: return ObjectSymbolBinding::Weak;
        default: return ObjectSymbolBinding::Local;
    }
}

[[nodiscard]] const char* binding_name(ObjectSymbolBinding binding) {
    switch (binding) {
        case ObjectSymbolBinding::Global: return "global";
        case ObjectSymbolBinding::Weak: return "weak";
        case ObjectSymbolBinding::Local: break;
    }
    return "local";
}

[[nodiscard]] const char* kind_name(ObjectSymbolKind kind) {
    switch (kind) {
        case ObjectSymbolKind::Function: return "func";
        case ObjectSymbolKind::Data: return "data";
        case ObjectSymbolKind::Section: return "section";
        case ObjectSymbolKind::File: return "file";
        case ObjectSymbolKind::None: break;
    }
    return "none";
}

} // namespace

std::optional<ObjectFile> ObjectFile::load(const std::string& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error != nullptr) *error = std::format("'{}' cannot be read", path);
        return std::nullopt;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::optional<ObjectFile> object = parse(ConstSpan<const u8>(bytes.data(), bytes.size()), error);
    if (object.has_value()) object->source = path;
    return object;
}

std::optional<ObjectFile> ObjectFile::parse(ConstSpan<const u8> bytes, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ObjectFile> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    if (bytes.size() < kElfHeaderSize) return fail("not an object file: it is shorter than a header");
    if (bytes[0] != 0x7F || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') {
        return fail("not an object file: it does not start with the object format's magic");
    }
    if (bytes[4] != kElfClass64) return fail("only 64 bit objects are read");
    if (bytes[5] != kElfDataLittle) return fail("only little endian objects are read");
    const u16 machine = read_u16(bytes.data() + 18);
    if (machine != kElfMachineX86_64) {
        return fail(std::format("object machine {} is not x86-64 ({}), which is the only one the code "
                                "table runtime places",
                                machine, kElfMachineX86_64));
    }

    const u64 section_offset = read_u64(bytes.data() + 40);
    const u16 section_count = read_u16(bytes.data() + 60);
    const u16 section_names = read_u16(bytes.data() + 62);
    if (section_count == 0) return fail("the object has no sections");
    if (section_offset + static_cast<u64>(section_count) * kSectionHeaderSize > bytes.size()) {
        return fail("the section headers run past the end of the file");
    }
    if (section_names >= section_count) return fail("the section name table is not a section");

    std::vector<RawSection> raw(section_count);
    for (usize index = 0; index < section_count; ++index) {
        const u8* at = bytes.data() + section_offset + index * kSectionHeaderSize;
        RawSection& section = raw[index];
        section.name = read_u32(at);
        section.type = read_u32(at + 4);
        section.flags = read_u64(at + 8);
        section.offset = read_u64(at + 24);
        section.size = read_u64(at + 32);
        section.link = read_u32(at + 40);
        section.info = read_u32(at + 44);
        section.align = read_u64(at + 48);
        section.entry_size = read_u64(at + 56);
        if (section.type != kShtNobits && section.offset + section.size > bytes.size()) {
            return fail(std::format("section {} runs past the end of the file", index));
        }
    }
    const ConstSpan<const u8> names(bytes.data() + raw[section_names].offset, raw[section_names].size);

    ObjectFile object;
    object.sections.reserve(section_count);
    for (const RawSection& section : raw) {
        ObjectSection out;
        out.name = std::string(slice_string(names, section.name));
        out.type = section.type;
        out.flags = static_cast<u32>(section.flags);
        out.align = section.align != 0 ? section.align : 1;
        out.size = section.size;
        if (section.type != kShtNobits && section.size != 0) {
            out.data.assign(bytes.begin() + static_cast<isize>(section.offset),
                            bytes.begin() + static_cast<isize>(section.offset + section.size));
        }
        object.sections.push_back(std::move(out));
    }

    // The symbol table, and the strings its names live in. A relocation section names both through
    // its link field, so the two are read before anything that uses them.
    const u32 symbol_section = [&] {
        for (usize index = 0; index < raw.size(); ++index) {
            if (raw[index].type == kShtSymtab) return static_cast<u32>(index);
        }
        return kInvalidId;
    }();
    if (symbol_section != kInvalidId) {
        const RawSection& table = raw[symbol_section];
        const RawSection& strings = raw[table.link];
        const ConstSpan<const u8> text(bytes.data() + strings.offset, strings.size);
        const usize count = static_cast<usize>(table.size / kSymbolSize);
        object.symbols.reserve(count);
        for (usize index = 0; index < count; ++index) {
            const u8* at = bytes.data() + table.offset + index * kSymbolSize;
            const u8 info = *(at + 4);
            const u16 where = read_u16(at + 6);
            ObjectSymbol symbol;
            symbol.name = std::string(slice_string(text, read_u32(at)));
            symbol.kind = symbol_kind(static_cast<u8>(info & 0x0F));
            symbol.binding = symbol_binding(static_cast<u8>(info >> 4));
            symbol.value = read_u64(at + 8);
            symbol.size = read_u64(at + 16);
            symbol.visibility = static_cast<u8>(*(at + 5) & 0x3);
            // An absolute symbol is a number, not an address, and a common one is a tentative
            // definition the linker would place: neither is a section here.
            symbol.section = (where == kShnUndef || where == kShnAbs || where == kShnCommon ||
                              where >= section_count)
                                 ? kInvalidId
                                 : where;
            // An absolute symbol is a number rather than something to place - except a file symbol,
            // whose whole job is to carry the name of the source it came from. The number is kept: a
            // relocation that only needs a value can be filled with it (docs/ABI.md H11).
            if (where == kShnAbs && symbol.kind != ObjectSymbolKind::File) {
                symbol.kind = ObjectSymbolKind::None;
                symbol.absolute = true;
            }
            object.symbols.push_back(std::move(symbol));
        }
    }

    // The COMDAT groups: a group's signature is the symbol its info field names, and its members are
    // the sections it lists after the flags word.
    for (const RawSection& section : raw) {
        if (section.type != kShtGroup) continue;
        const u32 words = static_cast<u32>(section.size / 4);
        ObjectGroup group;
        group.signature = section.info;
        for (u32 entry = 1; entry < words; ++entry) {
            const u32 member = read_u32(bytes.data() + section.offset + entry * 4);
            if (member < section_count) group.members.push_back(member);
        }
        object.groups.push_back(std::move(group));
    }

    for (usize index = 0; index < raw.size(); ++index) {
        const RawSection& section = raw[index];
        if (section.type != kShtRela) continue;
        if (section.info >= section_count) {
            return fail(std::format("relocation section '{}' patches section {} of {}", section.name,
                                    section.info, section_count));
        }
        const usize count = static_cast<usize>(section.size / kRelaSize);
        for (usize entry = 0; entry < count; ++entry) {
            const u8* at = bytes.data() + section.offset + entry * kRelaSize;
            const u64 info = read_u64(at + 8);
            ObjectRelocation relocation;
            relocation.section = section.info;
            relocation.offset = read_u64(at);
            relocation.type = static_cast<u32>(info & 0xFFFF'FFFFu);
            relocation.symbol = static_cast<u32>(info >> 32);
            relocation.addend = read_i64(at + 16);
            object.relocations.push_back(relocation);
        }
    }
    return object;
}

const ObjectSection* ObjectFile::section_of(const ObjectSymbol& symbol) const {
    if (!symbol.defined() || symbol.section >= sections.size()) return nullptr;
    return &sections[symbol.section];
}

std::string ObjectFile::describe() const {
    std::string text = std::format("{}: {} section(s), {} symbol(s), {} relocation(s)\n",
                                   source.empty() ? "(object)" : source, sections.size(), symbols.size(),
                                   relocations.size());
    for (const ObjectSection& section : sections) {
        text += std::format("  section {:<24} {:>8} bytes  align {:<4} {}{}{}\n", section.name, section.size,
                            section.align, section.is_alloc() ? "alloc " : "", section.is_exec() ? "exec " : "",
                            section.is_tls() ? "tls" : "");
    }
    for (const ObjectSymbol& symbol : symbols) {
        if (symbol.kind == ObjectSymbolKind::File || symbol.kind == ObjectSymbolKind::Section) continue;
        text += std::format("  symbol  {:<24} {:>6} {:<6} {}\n", symbol.name, kind_name(symbol.kind),
                            binding_name(symbol.binding),
                            symbol.defined() ? std::format("section {} + {:#x}", symbol.section, symbol.value)
                                             : std::string("undefined"));
    }
    for (const ObjectRelocation& relocation : relocations) {
        const std::string_view name =
            relocation.symbol < symbols.size() ? std::string_view(symbols[relocation.symbol].name) : "?";
        text += std::format("  reloc   section {:>3} + {:#06x}  type {:>3}  {}{:+d}\n", relocation.section,
                            relocation.offset, relocation.type, name, relocation.addend);
    }
    return text;
}

} // namespace t2d
