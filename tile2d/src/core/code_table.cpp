#include <t2d/core/code_table.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>

#if !defined(_WIN32)
#  include <dlfcn.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace t2d {
namespace {

// --- the file layout ------------------------------------------------------------------------------
// A header of fixed size, then five blocks in a fixed order. Every offset is written down rather than
// computed from the entry sizes, so a reader never has to agree with the writer about anything except
// the header itself.
constexpr u64 kHeaderSize = 104;
/// The room one stub takes: six bytes of instruction, two of padding, then the address it jumps to.
constexpr u64 kTrampolineSize = 16;
constexpr u64 kDefaultPageSize = 4096;
constexpr u64 kSectionEntrySize = 32;
constexpr u64 kSymbolEntrySize = 32;
constexpr u64 kRelocationEntrySize = 32;

void append_u32(std::vector<u8>& out, u32 value) {
    for (int byte = 0; byte < 4; ++byte) out.push_back(static_cast<u8>(value >> (8 * byte)));
}
void append_u64(std::vector<u8>& out, u64 value) {
    for (int byte = 0; byte < 8; ++byte) out.push_back(static_cast<u8>(value >> (8 * byte)));
}
void append_bytes(std::vector<u8>& out, const void* data, usize size) {
    const u8* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}
u32 read_u32(const u8* at) {
    u32 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
u64 read_u64(const u8* at) {
    u64 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
i64 read_i64(const u8* at) {
    i64 value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}
void write_u32(u8* at, u32 value) { std::memcpy(at, &value, sizeof(value)); }
void write_u64(u8* at, u64 value) { std::memcpy(at, &value, sizeof(value)); }

/// "jmp qword ptr [rip + 2]" followed by the address: a call that is out of reach points here, and
/// this points anywhere. It is what a linker's PLT entry is, and it is needed because the engine a
/// table calls lives wherever the program that loaded it put it.
void write_trampoline(u8* stub, u64 target) {
    const u8 code[6] = {0xFF, 0x25, 0x02, 0x00, 0x00, 0x00};
    std::memcpy(stub, code, sizeof(code));
    std::memcpy(stub + 8, &target, sizeof(target));
}

[[nodiscard]] u64 page_size() {
#if !defined(_WIN32)
    const long size = sysconf(_SC_PAGESIZE);
    if (size > 0) return static_cast<u64>(size);
#endif
    return kDefaultPageSize;
}

[[nodiscard]] bool is_got_relocation(u32 type) {
    return type == kRelocationGotPcRel || type == kRelocationGotPcRelX || type == kRelocationRexGotPcRelX;
}

} // namespace

// --- the table itself -----------------------------------------------------------------------------

std::optional<CodeTable> CodeTable::from_objects(const std::vector<ObjectFile>& objects, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<CodeTable> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    if (objects.empty()) return fail("no object files were given");

    CodeTable table;
    for (const ObjectFile& object : objects) {
        const std::string label = object.source.empty() ? std::string("(object)") : object.source;
        // Which of the object's sections the table carries. A section that occupies no memory when
        // the program runs - a comment, debug info, the symbol table itself - has nothing to place,
        // and a relocation that patched one of those goes with it.
        std::vector<u32> section_map(object.sections.size(), kInvalidId);
        for (usize index = 0; index < object.sections.size(); ++index) {
            const ObjectSection& section = object.sections[index];
            if (!section.is_alloc()) continue;
            if (section.is_tls()) {
                return fail(std::format("{}: '{}' is thread local storage, which a code table does not "
                                        "place: keep per-module state in the module's own data instead",
                                        label, section.name));
            }
            section_map[index] = static_cast<u32>(table.sections.size());
            CodeTableSection packed;
            packed.name = section.name;
            packed.type = section.type;
            packed.flags = section.flags;
            packed.align = section.align != 0 ? section.align : 1;
            packed.data = section.data;
            // A section that takes up room but has no bytes in the file (.bss) is zeros: that is
            // exactly what the linker would give it.
            if (packed.data.size() < section.size) packed.data.resize(section.size, 0);
            table.sections.push_back(std::move(packed));
        }

        // What the sections of this object are called, so a symbol left behind in a section the table
        // does not carry can still be found (see below).
        std::map<std::string, u32> section_by_name;
        for (usize index = 0; index < object.sections.size(); ++index) {
            if (section_map[index] == kInvalidId) continue;
            section_by_name.emplace(object.sections[index].name, section_map[index]);
        }

        // Which section each COMDAT group's code is in, by the symbol index of its signature: the
        // group is what says where the code of an inline function lives, and it does not depend on the
        // symbol's name matching a section's.
        std::map<u32, u32> group_section;
        for (const ObjectGroup& group : object.groups) {
            for (const u32 member : group.members) {
                if (member >= section_map.size() || section_map[member] == kInvalidId) continue;
                group_section.emplace(group.signature, section_map[member]);
                break;
            }
        }

        // Every symbol is kept, including the ones the table does not define and the empty symbol at
        // index zero: a relocation names its symbol by index, so dropping one would move every index
        // after it.
        const u32 symbol_base = static_cast<u32>(table.symbols.size());
        for (usize symbol_index = 0; symbol_index < object.symbols.size(); ++symbol_index) {
            const ObjectSymbol& symbol = object.symbols[symbol_index];
            CodeTableSymbol packed;
            packed.name = symbol.name;
            packed.kind = symbol.kind;
            packed.binding = symbol.binding;
            packed.value = symbol.value;
            packed.size = symbol.size;
            packed.section = symbol.defined() && symbol.section < section_map.size() ? section_map[symbol.section]
                                                                                    : kInvalidId;
            // A C++ compiler marks the code of an inline function with the *signature of its COMDAT
            // group*, and that signature lives in a section the table does not carry (a group is
            // bookkeeping, not something the program runs). The code itself is here, and with
            // -ffunction-sections its section is named after the symbol - so it is found by name and
            // made weak, which is what a COMDAT definition is: two modules carrying it coalesce instead
            // of colliding. Without this, every inline function a table calls - and the C++ library is
            // full of them - would be reported as something nobody defines.
            if (packed.section == kInvalidId && packed.binding == ObjectSymbolBinding::Local &&
                packed.kind == ObjectSymbolKind::None && packed.size == 0 && !packed.name.empty()) {
                const auto by_group = group_section.find(static_cast<u32>(symbol_index));
                if (by_group != group_section.end()) {
                    packed.section = by_group->second;
                    packed.binding = ObjectSymbolBinding::Weak;
                    packed.kind = (object.sections.empty() ? ObjectSymbolKind::Function
                                                           : ObjectSymbolKind::Function);
                    table.symbols.push_back(std::move(packed));
                    continue;
                }
                for (const char* prefix : {".text.", ".rodata.", ".data."}) {
                    const auto found = section_by_name.find(std::string(prefix) + packed.name);
                    if (found == section_by_name.end()) continue;
                    packed.section = found->second;
                    packed.binding = ObjectSymbolBinding::Weak;
                    if (std::string_view(prefix) == ".text.") packed.kind = ObjectSymbolKind::Function;
                    break;
                }
            }
            table.symbols.push_back(std::move(packed));
        }
        for (const ObjectRelocation& relocation : object.relocations) {
            if (relocation.section >= section_map.size()) continue;
            const u32 section = section_map[relocation.section];
            if (section == kInvalidId) continue;   // a section the table does not carry
            if (relocation.symbol >= object.symbols.size()) {
                return fail(std::format("{}: a relocation names symbol {} of {}", label, relocation.symbol,
                                        object.symbols.size()));
            }
            CodeTableRelocation packed;
            packed.section = section;
            packed.type = relocation.type;
            packed.offset = relocation.offset;
            packed.symbol = symbol_base + relocation.symbol;
            packed.addend = relocation.addend;
            table.relocations.push_back(packed);
        }
    }
    return table;
}

std::vector<u8> CodeTable::serialize() const {
    std::vector<u8> out;
    out.reserve(static_cast<usize>(kHeaderSize) + sections.size() * kSectionEntrySize +
                symbols.size() * kSymbolEntrySize + relocations.size() * kRelocationEntrySize);

    // The names, in one block, each one ended by a zero. Offsets are into this block.
    std::vector<u8> strings;
    const auto intern = [&strings](const std::string& text) {
        const u32 offset = static_cast<u32>(strings.size());
        append_bytes(strings, text.data(), text.size());
        strings.push_back(0);
        return offset;
    };
    std::vector<u32> section_names(sections.size());
    for (usize index = 0; index < sections.size(); ++index) section_names[index] = intern(sections[index].name);
    std::vector<u32> symbol_names(symbols.size());
    for (usize index = 0; index < symbols.size(); ++index) symbol_names[index] = intern(symbols[index].name);
    const u32 meta_names[4] = {intern("id"), intern("name"), intern("version"), intern("requires")};

    // The bytes of every section, each one at the alignment it asked for.
    std::vector<u8> blob;
    std::vector<u64> section_offset(sections.size(), 0);
    for (usize index = 0; index < sections.size(); ++index) {
        const CodeTableSection& section = sections[index];
        const u64 align = std::max<u64>(section.align, 1);
        while (blob.size() % align != 0) blob.push_back(0);
        section_offset[index] = blob.size();
        append_bytes(blob, section.data.data(), section.data.size());
    }

    std::vector<u8> meta;
    std::string requires_text;
    for (const CodeRequirement& requirement : requirements) {
        if (!requires_text.empty()) requires_text += ",";
        requires_text += requirement.id;
        if (!requirement.version.empty()) requires_text += "@" + requirement.version;
    }
    const std::string meta_text = std::format("id={}\nname={}\nversion={}\nrequires={}\n", id, name, version,
                                              requires_text);
    append_bytes(meta, meta_text.data(), meta_text.size());

    const u64 strings_offset = kHeaderSize;
    const u64 sections_offset = strings_offset + strings.size();
    const u64 symbols_offset = sections_offset + sections.size() * kSectionEntrySize;
    const u64 relocations_offset = symbols_offset + symbols.size() * kSymbolEntrySize;
    const u64 blob_offset = relocations_offset + relocations.size() * kRelocationEntrySize;
    const u64 meta_offset = blob_offset + blob.size();

    append_bytes(out, kCodeTableMagic, sizeof(kCodeTableMagic));
    append_u32(out, kCodeTableVersion);
    append_u32(out, kCodeTableArchX86_64);
    append_u32(out, static_cast<u32>(sections.size()));
    append_u32(out, static_cast<u32>(symbols.size()));
    append_u32(out, static_cast<u32>(relocations.size()));
    append_u32(out, meta_names[0]);   // reserved for a while: the first metadata key, kept for readers
    append_u64(out, strings_offset);
    append_u64(out, strings.size());
    append_u64(out, sections_offset);
    append_u64(out, symbols_offset);
    append_u64(out, relocations_offset);
    append_u64(out, blob_offset);
    append_u64(out, blob.size());
    append_u64(out, meta_offset);
    append_u64(out, meta.size());

    append_bytes(out, strings.data(), strings.size());
    for (usize index = 0; index < sections.size(); ++index) {
        append_u32(out, section_names[index]);
        append_u32(out, sections[index].type);
        append_u32(out, sections[index].flags);
        append_u32(out, static_cast<u32>(sections[index].align));
        append_u64(out, section_offset[index]);
        append_u64(out, sections[index].data.size());
    }
    for (usize index = 0; index < symbols.size(); ++index) {
        append_u32(out, symbol_names[index]);
        append_u32(out, symbols[index].section);
        append_u32(out, static_cast<u32>(symbols[index].kind));
        append_u32(out, static_cast<u32>(symbols[index].binding));
        append_u64(out, symbols[index].value);
        append_u64(out, symbols[index].size);
    }
    for (const CodeTableRelocation& relocation : relocations) {
        append_u32(out, relocation.section);
        append_u32(out, relocation.type);
        append_u64(out, relocation.offset);
        append_u32(out, relocation.symbol);
        append_u32(out, 0);
        append_u64(out, static_cast<u64>(relocation.addend));
    }
    append_bytes(out, blob.data(), blob.size());
    append_bytes(out, meta.data(), meta.size());
    return out;
}

std::optional<CodeTable> CodeTable::parse(ConstSpan<const u8> bytes, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<CodeTable> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    if (bytes.size() < kHeaderSize) return fail("not a code table: it is shorter than a header");
    if (std::memcmp(bytes.data(), kCodeTableMagic, sizeof(kCodeTableMagic)) != 0) {
        return fail("not a code table: it does not start with the table magic");
    }
    const u32 version = read_u32(bytes.data() + 8);
    if (version != kCodeTableVersion) {
        return fail(std::format("code table version {} was read by a runtime that writes {}", version,
                                kCodeTableVersion));
    }
    const u32 arch = read_u32(bytes.data() + 12);
    if (arch != kCodeTableArchX86_64) {
        return fail(std::format("the table was built for machine {}, this runtime places machine {}", arch,
                                kCodeTableArchX86_64));
    }
    const u32 section_count = read_u32(bytes.data() + 16);
    const u32 symbol_count = read_u32(bytes.data() + 20);
    const u32 relocation_count = read_u32(bytes.data() + 24);
    const u64 strings_offset = read_u64(bytes.data() + 32);
    const u64 strings_size = read_u64(bytes.data() + 40);
    const u64 sections_offset = read_u64(bytes.data() + 48);
    const u64 symbols_offset = read_u64(bytes.data() + 56);
    const u64 relocations_offset = read_u64(bytes.data() + 64);
    const u64 blob_offset = read_u64(bytes.data() + 72);
    const u64 blob_size = read_u64(bytes.data() + 80);
    const u64 meta_offset = read_u64(bytes.data() + 88);
    const u64 meta_size = read_u64(bytes.data() + 96);

    const auto fits = [&](u64 offset, u64 size) { return offset <= bytes.size() && size <= bytes.size() - offset; };
    if (!fits(strings_offset, strings_size) || !fits(sections_offset, section_count * kSectionEntrySize) ||
        !fits(symbols_offset, symbol_count * kSymbolEntrySize) ||
        !fits(relocations_offset, relocation_count * kRelocationEntrySize) || !fits(blob_offset, blob_size) ||
        !fits(meta_offset, meta_size)) {
        return fail("the code table's blocks run past the end of the file");
    }

    const ConstSpan<const u8> strings(bytes.data() + strings_offset, static_cast<usize>(strings_size));
    const auto text_at = [&strings](u32 offset) {
        if (offset >= strings.size()) return std::string{};
        usize end = offset;
        while (end < strings.size() && strings[end] != 0) ++end;
        return std::string(reinterpret_cast<const char*>(strings.data()) + offset, end - offset);
    };

    CodeTable table;
    table.sections.reserve(section_count);
    for (u32 index = 0; index < section_count; ++index) {
        const u8* at = bytes.data() + sections_offset + index * kSectionEntrySize;
        CodeTableSection section;
        section.name = text_at(read_u32(at));
        section.type = read_u32(at + 4);
        section.flags = read_u32(at + 8);
        section.align = read_u32(at + 12) != 0 ? read_u32(at + 12) : 1;
        const u64 offset = read_u64(at + 16);
        const u64 size = read_u64(at + 24);
        if (offset > blob_size || size > blob_size - offset) {
            return fail(std::format("section '{}' runs past the end of the table's bytes", section.name));
        }
        section.data.assign(bytes.begin() + static_cast<isize>(blob_offset + offset),
                            bytes.begin() + static_cast<isize>(blob_offset + offset + size));
        table.sections.push_back(std::move(section));
    }
    table.symbols.reserve(symbol_count);
    for (u32 index = 0; index < symbol_count; ++index) {
        const u8* at = bytes.data() + symbols_offset + index * kSymbolEntrySize;
        CodeTableSymbol symbol;
        symbol.name = text_at(read_u32(at));
        symbol.section = read_u32(at + 4);
        symbol.kind = static_cast<ObjectSymbolKind>(read_u32(at + 8));
        symbol.binding = static_cast<ObjectSymbolBinding>(read_u32(at + 12));
        symbol.value = read_u64(at + 16);
        symbol.size = read_u64(at + 24);
        if (symbol.defined() && symbol.section >= table.sections.size()) {
            return fail(std::format("symbol '{}' is in section {}, which the table does not have", symbol.name,
                                    symbol.section));
        }
        table.symbols.push_back(std::move(symbol));
    }
    table.relocations.reserve(relocation_count);
    for (u32 index = 0; index < relocation_count; ++index) {
        const u8* at = bytes.data() + relocations_offset + index * kRelocationEntrySize;
        CodeTableRelocation relocation;
        relocation.section = read_u32(at);
        relocation.type = read_u32(at + 4);
        relocation.offset = read_u64(at + 8);
        relocation.symbol = read_u32(at + 16);
        relocation.addend = read_i64(at + 24);
        if (relocation.section >= table.sections.size() || relocation.symbol >= table.symbols.size()) {
            return fail(std::format("a relocation in the table names section {} symbol {}", relocation.section,
                                    relocation.symbol));
        }
        table.relocations.push_back(relocation);
    }

    // The metadata, as the lines it was written as. An unknown key is kept out of the table rather
    // than refused: the metadata is what the toolchain said about the module, not part of the code.
    std::string_view meta(reinterpret_cast<const char*>(bytes.data() + meta_offset), static_cast<usize>(meta_size));
    while (!meta.empty()) {
        const usize end = meta.find('\n');
        const std::string_view line = meta.substr(0, end);
        const usize equals = line.find('=');
        if (equals != std::string_view::npos) {
            const std::string_view key = line.substr(0, equals);
            const std::string value(line.substr(equals + 1));
            if (key == "id") table.id = value;
            else if (key == "name") table.name = value;
            else if (key == "version") table.version = value;
            else if (key == "requires") {
                // "id" or "id@version", separated by commas.
                usize start = 0;
                while (start <= value.size() && !value.empty()) {
                    const usize comma = value.find(',', start);
                    const std::string part = value.substr(start, comma == std::string::npos ? comma : comma - start);
                    if (!part.empty()) {
                        const usize at = part.find('@');
                        CodeRequirement requirement;
                        requirement.id = part.substr(0, at);
                        if (at != std::string::npos) requirement.version = part.substr(at + 1);
                        if (!requirement.id.empty()) table.requirements.push_back(std::move(requirement));
                    }
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
        }
        if (end == std::string_view::npos) break;
        meta.remove_prefix(end + 1);
    }
    return table;
}

std::optional<CodeTable> CodeTable::load(const std::string& path, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error != nullptr) *error = std::format("'{}' cannot be read", path);
        return std::nullopt;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::optional<CodeTable> table = parse(ConstSpan<const u8>(bytes.data(), bytes.size()), error);
    if (table.has_value() && table->id.empty()) table->id = path;
    return table;
}

bool CodeTable::save(const std::string& path, std::string* error) const {
    const std::vector<u8> bytes = serialize();
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error != nullptr) *error = std::format("'{}' cannot be written", path);
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        if (error != nullptr) *error = std::format("'{}' was not written completely", path);
        return false;
    }
    return true;
}

std::vector<std::string> CodeTable::overridable_symbols() const {
    std::vector<std::string> names;
    for (const CodeTableSymbol& symbol : symbols) {
        if (!symbol.defined() || symbol.binding != ObjectSymbolBinding::Global || symbol.name.empty()) continue;
        names.push_back(symbol.name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string CodeTable::describe() const {
    std::string text = std::format("code table '{}'{}: {} section(s), {} symbol(s), {} relocation(s)\n", id,
                                   version.empty() ? "" : std::format(" version {}", version), sections.size(),
                                   symbols.size(), relocations.size());
    if (!requirements.empty()) {
        std::string list;
        for (const CodeRequirement& requirement : requirements) {
            if (!list.empty()) list += ", ";
            list += requirement.id;
            if (!requirement.version.empty()) list += "@" + requirement.version;
        }
        text += std::format("  requires: {}\n", list);
    }
    for (const CodeTableSection& section : sections) {
        text += std::format("  section {:<24} {:>8} bytes  align {:<4} {}{}\n", section.name, section.data.size(),
                            section.align, (section.flags & kSectionAlloc) != 0 ? "alloc " : "",
                            (section.flags & kSectionExec) != 0 ? "exec" : "");
    }
    for (const CodeTableSymbol& symbol : symbols) {
        if (symbol.kind == ObjectSymbolKind::File || symbol.kind == ObjectSymbolKind::Section) continue;
        const char* binding = symbol.binding == ObjectSymbolBinding::Weak      ? "weak"
                              : symbol.binding == ObjectSymbolBinding::Global  ? "global"
                                                                               : "local";
        text += std::format("  symbol  {:<32} {:<8} {}\n", symbol.name, binding,
                            symbol.defined() ? std::format("section {} + {:#x}", symbol.section, symbol.value)
                                             : std::string("undefined"));
    }
    for (const CodeTableRelocation& relocation : relocations) {
        const std::string& target =
            relocation.symbol < symbols.size() ? symbols[relocation.symbol].name : std::string("?");
        text += std::format("  reloc   section {:>3} + {:#06x}  type {:>3}  {}{:+d}\n", relocation.section,
                            relocation.offset, relocation.type, target, relocation.addend);
    }
    return text;
}

// --- the merged image -----------------------------------------------------------------------------

CodeImage::~CodeImage() { release(); }

void CodeImage::declare_host(std::string id, std::string version) {
    host_id_ = std::move(id);
    host_version_ = std::move(version);
}

bool CodeImage::requirements_met(const CodeTable& table, std::string* error) const {
    for (const CodeRequirement& requirement : table.requirements) {
        std::string version;
        bool found = false;
        if (!host_id_.empty() && requirement.id == host_id_) {
            found = true;
            version = host_version_;
        } else {
            for (const Module& module : modules_) {
                if (&module.table == &table) continue;   // a module does not provide for itself
                if (module.table.id != requirement.id) continue;
                found = true;
                version = module.table.version;
                break;
            }
        }
        if (!found) {
            if (error != nullptr) {
                *error = std::format("requires '{}', which is not loaded", requirement.id);
            }
            return false;
        }
        if (!requirement.version.empty() && requirement.version != version) {
            if (error != nullptr) {
                *error = std::format("was built for '{}' version {}, and {} is here: rebuild it against this one",
                                     requirement.id, requirement.version,
                                     version.empty() ? "an unversioned build" : version.c_str());
            }
            return false;
        }
    }
    return true;
}

void CodeImage::add(CodeTable table) {
    if (loaded_) {
        report_.errors.push_back("a code table was added after the image was loaded: the relocations "
                                 "that are already filled in would point at the wrong place");
        return;
    }
    if (table.id.empty()) table.id = std::format("module {}", modules_.size() + 1);
    modules_.push_back(Module{});
    modules_.back().table = std::move(table);
}

void CodeImage::release() {
#if !defined(_WIN32)
    for (Module& module : modules_) {
        if (module.base != nullptr) munmap(module.base, module.region_size);
        module.base = nullptr;
        module.text = nullptr;
        module.data = nullptr;
    }
#endif
}

bool CodeImage::place(Module& module) {
#if defined(_WIN32)
    (void)module;
    report_.errors.push_back("the code table runtime places machine code for ELF x86-64 only");
    return false;
#else
    // One region, code first and data a page later: a call and a load from the global offset table are
    // both 32 bit displacements, so what belongs together must not end up gigabytes apart.
    const u64 page = page_size();
    u64 text_size = 0;
    u64 data_size = 0;
    for (const CodeTableSection& section : module.table.sections) {
        const u64 align = std::max<u64>(section.align, 1);
        if ((section.flags & kSectionExec) != 0) {
            text_size = align_up(text_size, align) + section.data.size();
        } else {
            data_size = align_up(data_size, align) + section.data.size();
        }
    }
    module.got_count = 0;
    module.trampoline_count = 0;
    for (const CodeTableRelocation& relocation : module.table.relocations) {
        if (is_got_relocation(relocation.type)) ++module.got_count;
        // Every call gets room for a stub, because any of them may turn out to be out of reach.
        if (relocation.type == kRelocationPlt32 || relocation.type == kRelocationPc32) ++module.trampoline_count;
    }
    const u64 stubs_at = align_up(text_size, 16);
    const u64 text_total = stubs_at + module.trampoline_count * kTrampolineSize;
    const u64 data_start = align_up(text_total, page);
    data_size = align_up(data_size, 8) + module.got_count * 8;
    const u64 total = data_start + data_size;

    if (total != 0) {
        void* memory = mmap(nullptr, static_cast<usize>(total), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            report_.errors.push_back(
                std::format("module '{}': {} bytes could not be placed", module.table.id, total));
            return false;
        }
        module.base = static_cast<u8*>(memory);
    }
    module.region_size = static_cast<usize>(total);
    module.text = module.base;
    module.text_size = static_cast<usize>(text_total);
    module.trampolines = module.base != nullptr ? module.base + stubs_at : nullptr;
    module.data = module.base != nullptr ? module.base + data_start : nullptr;
    module.data_size = static_cast<usize>(data_size);

    module.section_address.assign(module.table.sections.size(), nullptr);
    u64 text_at = 0;
    u64 data_at = 0;
    for (usize index = 0; index < module.table.sections.size(); ++index) {
        const CodeTableSection& section = module.table.sections[index];
        const u64 align = std::max<u64>(section.align, 1);
        u8* at = nullptr;
        if ((section.flags & kSectionExec) != 0) {
            text_at = align_up(text_at, align);
            at = module.text + text_at;
            text_at += section.data.size();
        } else {
            data_at = align_up(data_at, align);
            at = module.data + data_at;
            data_at += section.data.size();
        }
        module.section_address[index] = at;
        if (!section.data.empty()) std::memcpy(at, section.data.data(), section.data.size());
    }
    module.got = module.data != nullptr ? module.data + align_up(data_at, 8) : nullptr;
    module.symbol_address.assign(module.table.symbols.size(), nullptr);
    return true;
#endif
}

void CodeImage::resolve_symbols() {
    // First pass: who defines what. A strong definition replaces whatever was there - that is the
    // merge, and it is why the game's own call to a function can end up in a mod.
    for (usize index = 0; index < modules_.size(); ++index) {
        Module& module = modules_[index];
        if (module.failed) continue;
        for (const CodeTableSymbol& symbol : module.table.symbols) {
            if (!symbol.defined() || symbol.section >= module.section_address.size()) continue;
            if (!symbol.shared() || symbol.name.empty()) continue;
            u8* address = module.section_address[symbol.section] + symbol.value;
            std::vector<Definition>& chain = definitions_[symbol.name];
            if (chain.empty() || symbol.binding == ObjectSymbolBinding::Weak) {
                // A weak definition never displaces anything: it is the copy an inline function, a
                // template instance or a vtable is emitted as, and one program has one of each.
                chain.push_back(Definition{index, address, symbol.binding});
                continue;
            }
            const Definition previous = chain.front();
            chain.insert(chain.begin(), Definition{index, address, symbol.binding});
            if (previous.binding == ObjectSymbolBinding::Global) {
                report_.overrides.push_back(CodeOverride{symbol.name, module.table.id,
                                                         modules_[previous.module].table.id});
                override_sources_.push_back(index);
            }
        }
    }

    // Second pass: what every symbol resolves to *now*. A module's own definition is looked up by
    // name like everybody else's, which is the whole point: a definition that lost the merge must not
    // still be reachable through the module that made it.
    for (usize index = 0; index < modules_.size(); ++index) {
        Module& module = modules_[index];
        if (module.failed) continue;
        for (usize s = 0; s < module.table.symbols.size(); ++s) {
            const CodeTableSymbol& symbol = module.table.symbols[s];
            if (symbol.defined() && symbol.section < module.section_address.size()) {
                if (symbol.shared() && !symbol.name.empty()) {
                    const auto found = definitions_.find(symbol.name);
                    if (found != definitions_.end() && !found->second.empty()) {
                        module.symbol_address[s] = static_cast<u8*>(found->second.front().address);
                        continue;
                    }
                }
                module.symbol_address[s] = module.section_address[symbol.section] + symbol.value;
                continue;
            }
            // Undefined here: somebody else's, or the running program's.
            module.symbol_address[s] = nullptr;
            // A file symbol is the name of the source the object came from, not something to resolve,
            // and the empty symbol at index zero is the table's own "no symbol".
            if (symbol.kind == ObjectSymbolKind::File || symbol.name.empty()) continue;
            // Two symbols every module answers for itself, the way a shared library does: the table
            // its own addressing is relative to, and the handle its static destructors are registered
            // against. Looking for either in the running program would find the program's, which is
            // the wrong answer for code that was placed somewhere else.
            if (symbol.name == "_GLOBAL_OFFSET_TABLE_") {
                module.symbol_address[s] = module.got != nullptr ? module.got : module.data;
                continue;
            }
            if (symbol.name == "__dso_handle") {
                module.symbol_address[s] = module.base;
                continue;
            }
            const auto found = definitions_.find(symbol.name);
            if (found != definitions_.end() && !found->second.empty()) {
                module.symbol_address[s] = static_cast<u8*>(found->second.front().address);
                continue;
            }
#if !defined(_WIN32)
            module.symbol_address[s] = static_cast<u8*>(dlsym(RTLD_DEFAULT, symbol.name.c_str()));
#endif
            if (module.symbol_address[s] == nullptr) {
                if (std::find(report_.unresolved.begin(), report_.unresolved.end(), symbol.name) ==
                    report_.unresolved.end()) {
                    report_.unresolved.push_back(symbol.name);
                }
            }
        }
    }
}

void CodeImage::relocate(Module& module) {
    const auto fail = [&](std::string message) {
        module.failed = true;
        report_.errors.push_back(std::format("module '{}': {}", module.table.id, message));
    };
    for (const CodeTableRelocation& relocation : module.table.relocations) {
        if (module.failed) return;
        if (relocation.section >= module.section_address.size() || module.section_address[relocation.section] == nullptr) {
            fail(std::format("a relocation names section {}, which was not placed", relocation.section));
            return;
        }
        const CodeTableSection& section = module.table.sections[relocation.section];
        if (relocation.symbol >= module.symbol_address.size()) {
            fail(std::format("a relocation in '{}' names symbol {}", section.name, relocation.symbol));
            return;
        }
        const std::string& name =
            module.table.symbols[relocation.symbol].name.empty()
                ? std::format("symbol {}", relocation.symbol)
                : module.table.symbols[relocation.symbol].name;
        u8* at = module.section_address[relocation.section] + relocation.offset;
        const u8* symbol = module.symbol_address[relocation.symbol];
        if (symbol == nullptr) {
            fail(std::format("'{}' is not defined by any module or by the running program", name));
            return;
        }
        const auto room = [&](u64 width) { return relocation.offset + width <= section.data.size(); };
        const u64 target = reinterpret_cast<u64>(symbol);
        const u64 where = reinterpret_cast<u64>(at);
        switch (relocation.type) {
            case kRelocationAbsolute64: {
                if (!room(8)) return fail(std::format("a relocation in '{}' runs past the section", section.name));
                write_u64(at, target + static_cast<u64>(relocation.addend));
                break;
            }
            case kRelocationAbsolute32:
            case kRelocationAbsolute32Signed: {
                if (!room(4)) return fail(std::format("a relocation in '{}' runs past the section", section.name));
                const i64 value = static_cast<i64>(target) + relocation.addend;
                if (relocation.type == kRelocationAbsolute32Signed &&
                    (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max())) {
                    return fail(std::format("'{}' does not fit the 32 bit field it is written into", name));
                }
                write_u32(at, static_cast<u32>(value));
                break;
            }
            case kRelocationPc32:
            case kRelocationPlt32: {
                if (!room(4)) return fail(std::format("a relocation in '{}' runs past the section", section.name));
                i64 value = static_cast<i64>(target) + relocation.addend - static_cast<i64>(where);
                if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) {
                    // Out of reach: a stub beside the call jumps the rest of the way. What a table calls
                    // into is wherever the program that loaded it put it, and that can be more than the
                    // 2 GiB a call can name.
                    if (module.trampolines == nullptr || module.trampoline_used >= module.trampoline_count) {
                        return fail(std::format("'{}' is more than 2 GiB away and the module has no room left "
                                                "for a stub to reach it",
                                                name));
                    }
                    u8* stub = module.trampolines + module.trampoline_used * kTrampolineSize;
                    ++module.trampoline_used;
                    write_trampoline(stub, target);
                    value = static_cast<i64>(reinterpret_cast<u64>(stub)) + relocation.addend -
                            static_cast<i64>(where);
                    if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) {
                        return fail(std::format("a stub for '{}' is out of reach of the call that wants it", name));
                    }
                }
                write_u32(at, static_cast<u32>(static_cast<i32>(value)));
                break;
            }
            case kRelocationGotPcRel:
            case kRelocationGotPcRelX:
            case kRelocationRexGotPcRelX: {
                if (!room(4)) return fail(std::format("a relocation in '{}' runs past the section", section.name));
                if (module.got_used >= module.got_count) {
                    return fail(std::format("'{}' needs a slot in the global offset table that was not "
                                            "counted when the module was placed",
                                            section.name));
                }
                u8* slot = module.got + module.got_used * 8;
                ++module.got_used;
                write_u64(slot, target);
                const i64 value = static_cast<i64>(reinterpret_cast<u64>(slot)) + relocation.addend -
                                  static_cast<i64>(where);
                if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) {
                    return fail(std::format("the global offset table entry for '{}' is out of reach", name));
                }
                write_u32(at, static_cast<u32>(static_cast<i32>(value)));
                break;
            }
            default:
                return fail(std::format("relocation type {} in '{}' is not one the runtime applies", relocation.type,
                                        section.name));
        }
    }
}

void CodeImage::run_initialisers() {
    for (Module& module : modules_) {
        if (module.failed) continue;
        for (usize index = 0; index < module.table.sections.size(); ++index) {
            const CodeTableSection& section = module.table.sections[index];
            if (section.type != kSectionInitArray || module.section_address[index] == nullptr) continue;
            for (usize entry = 0; entry + 8 <= section.data.size(); entry += 8) {
                u64 address = 0;
                std::memcpy(&address, module.section_address[index] + entry, sizeof(address));
                if (address == 0) continue;
                reinterpret_cast<void (*)()>(static_cast<std::uintptr_t>(address))();
                ++report_.init_calls;
            }
        }
    }
}

void CodeImage::prune_failed() {
    for (auto it = definitions_.begin(); it != definitions_.end();) {
        std::vector<Definition>& chain = it->second;
        std::erase_if(chain, [&](const Definition& definition) { return modules_[definition.module].failed; });
        if (chain.empty()) it = definitions_.erase(it);
        else ++it;
    }
    std::vector<CodeOverride> kept;
    for (usize index = 0; index < report_.overrides.size() && index < override_sources_.size(); ++index) {
        if (!modules_[override_sources_[index]].failed) kept.push_back(report_.overrides[index]);
    }
    report_.overrides = std::move(kept);
    override_sources_.clear();
}

const CodeImageReport& CodeImage::load() {
    if (loaded_) return report_;
    loaded_ = true;
    for (Module& module : modules_) {
        CodeModuleInfo info;
        info.id = module.table.id;
        info.name = module.table.name;
        info.version = module.table.version;
        info.sections = module.table.sections.size();
        info.symbols = module.table.symbols.size();
        info.relocations = module.table.relocations.size();
        report_.modules.push_back(std::move(info));
        report_.symbols += module.table.symbols.size();
        report_.relocations += module.table.relocations.size();
        // What it needs comes first: a module built for another engine is refused before anything of
        // it is placed, and never half merged.
        std::string unmet;
        if (!requirements_met(module.table, &unmet)) {
            module.failed = true;
            report_.errors.push_back(std::format("module '{}': {}", module.table.id, unmet));
            continue;
        }
        if (!place(module)) module.failed = true;
    }
    resolve_symbols();
    for (Module& module : modules_) {
        if (!module.failed) relocate(module);
    }
#if !defined(_WIN32)
    // Code that can still be written after it was filled in is a hole nobody needs. The data half of
    // the region starts on a page of its own, so it stays writable while the code does not.
    for (Module& module : modules_) {
        if (module.base != nullptr && module.text_size != 0) {
            mprotect(module.base, module.text_size, PROT_READ | PROT_EXEC);
        }
    }
#endif
    prune_failed();
    for (usize index = 0; index < report_.modules.size() && index < modules_.size(); ++index) {
        report_.modules[index].ok = !modules_[index].failed;
    }
    run_initialisers();
    return report_;
}

void* CodeImage::find(std::string_view name) const {
    const auto found = definitions_.find(std::string(name));
    if (found == definitions_.end() || found->second.empty()) return nullptr;
    return found->second.front().address;
}

void* CodeImage::find_previous(std::string_view name) const {
    const auto found = definitions_.find(std::string(name));
    if (found == definitions_.end() || found->second.size() < 2) return nullptr;
    return found->second[1].address;
}

} // namespace t2d
