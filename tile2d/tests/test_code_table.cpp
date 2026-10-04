// Code tables: what a compiler emitted is packed into one file, two of those files are merged in
// memory, and a symbol the second one defines replaces the first one's - everywhere, including inside
// the code that was compiled first. That last part is the whole point, and it is what a shared library
// loaded at run time cannot do.
#include <t2d/core/code_table.h>
#include <t2d/core/object_file.h>

#include <support/test_support.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

using namespace t2d;

namespace {

/// The object the compiler produced for one fixture. The fixtures are compiled by the build, not
/// linked into this test: a table holds what a compiler emitted, so the test reads a real object.
[[nodiscard]] std::string object_of(const char* name) {
    return std::format("{}/{}.o", T2D_TEST_TABLE_DIR, name);
}

[[nodiscard]] std::string source_of(const char* name) {
    return std::format("{}/{}.cpp", T2D_TEST_TABLE_SOURCES, name);
}

[[nodiscard]] std::optional<CodeTable> pack(std::vector<std::string> objects, const char* id, std::string* error) {
    std::vector<ObjectFile> files;
    for (const std::string& path : objects) {
        std::optional<ObjectFile> object = ObjectFile::load(path, error);
        if (!object.has_value()) return std::nullopt;
        files.push_back(std::move(*object));
    }
    std::optional<CodeTable> table = CodeTable::from_objects(files, error);
    if (table.has_value()) table->id = id;
    return table;
}

/// What a report's name lists say, so a failure names the symbol instead of only counting it.
[[nodiscard]] std::string names_of(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) text += ", ";
        text += name.empty() ? "(empty)" : name;
    }
    return text;
}

[[nodiscard]] const ObjectSymbol* find_symbol(const ObjectFile& object, std::string_view name) {
    for (const ObjectSymbol& symbol : object.symbols) {
        if (symbol.name == name) return &symbol;
    }
    return nullptr;
}

} // namespace

T2D_TEST(an_object_file_is_read_into_sections_symbols_and_relocations) {
    std::string error;
    std::optional<ObjectFile> caller = ObjectFile::load(object_of("caller"), &error);
    T2D_REQUIRE(caller.has_value());

    // The name of the source file is a symbol too, and it is not something to resolve: a runtime that
    // looked it up in the running program would report it as missing on every single load.
    const ObjectSymbol* source_name = find_symbol(*caller, "caller.cpp");
    T2D_REQUIRE(source_name != nullptr);
    T2D_CHECK_EQ(source_name->kind, ObjectSymbolKind::File);

    const ObjectSymbol* use_base = find_symbol(*caller, "use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK(use_base->defined());
    T2D_CHECK_EQ(use_base->kind, ObjectSymbolKind::Function);
    T2D_CHECK_EQ(use_base->binding, ObjectSymbolBinding::Global);
    // Every function is in a section of its own, which is what lets a merge keep one and drop another.
    const ObjectSection* section = caller->section_of(*use_base);
    T2D_REQUIRE(section != nullptr);
    T2D_CHECK_EQ(section->name, std::string(".text.use_base"));
    T2D_CHECK(section->is_exec());
    T2D_CHECK(section->is_alloc());

    // The call it makes is a relocation against a name rather than a jump to a number: that is the
    // thing a merge needs to be able to change, and it is why the toolchain compiles with semantic
    // interposition on.
    bool calls_base = false;
    for (const ObjectRelocation& relocation : caller->relocations) {
        if (relocation.section != use_base->section) continue;
        const ObjectSymbol& target = caller->symbols[relocation.symbol];
        if (target.name != "base_value") continue;
        calls_base = true;
        T2D_CHECK(relocation.type == kRelocationPlt32 || relocation.type == kRelocationPc32);
    }
    T2D_CHECK(calls_base);

    // A global is reached through the global offset table, which is the other thing the runtime has to
    // build for a module.
    std::optional<ObjectFile> base = ObjectFile::load(object_of("base"), &error);
    T2D_REQUIRE(base.has_value());
    const ObjectSymbol* read_counter = find_symbol(*base, "read_counter");
    T2D_REQUIRE(read_counter != nullptr);
    bool through_got = false;
    for (const ObjectRelocation& relocation : base->relocations) {
        if (relocation.section != read_counter->section) continue;
        const ObjectSymbol& target = base->symbols[relocation.symbol];
        if (target.name != "counter") continue;
        through_got = true;
        T2D_CHECK(relocation.type == kRelocationGotPcRelX || relocation.type == kRelocationRexGotPcRelX ||
                  relocation.type == kRelocationGotPcRel);
    }
    T2D_CHECK(through_got);
}

T2D_TEST(a_table_is_the_objects_packed_and_reads_back_the_same) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("base"), object_of("caller")}, "vanilla", &error);
    T2D_REQUIRE(table.has_value());
    T2D_CHECK_GT(table->sections.size(), 0u);
    T2D_CHECK_GT(table->symbols.size(), 0u);
    T2D_CHECK_GT(table->relocations.size(), 0u);
    // A table carries what has to be there when the program runs: a comment or a debug section has
    // nothing to place and is not in it.
    for (const CodeTableSection& section : table->sections) {
        T2D_CHECK((section.flags & kSectionAlloc) != 0);
    }
    table->version = "1.0";
    table->requirements = {CodeRequirement{"engine", "1.0"}, CodeRequirement{"art", ""}};

    const std::vector<u8> bytes = table->serialize();
    std::optional<CodeTable> again = CodeTable::parse(ConstSpan<const u8>(bytes.data(), bytes.size()), &error);
    T2D_REQUIRE(again.has_value());
    T2D_CHECK_EQ(again->id, std::string("vanilla"));
    T2D_CHECK_EQ(again->version, std::string("1.0"));
    T2D_CHECK_EQ(again->requirements.size(), 2u);
    T2D_CHECK_EQ(again->requirements[0].id, std::string("engine"));
    T2D_CHECK_EQ(again->requirements[0].version, std::string("1.0"));
    T2D_CHECK_EQ(again->requirements[1].id, std::string("art"));
    T2D_CHECK_EQ(again->requirements[1].version, std::string(""));
    T2D_CHECK_EQ(again->sections.size(), table->sections.size());
    T2D_CHECK_EQ(again->symbols.size(), table->symbols.size());
    T2D_CHECK_EQ(again->relocations.size(), table->relocations.size());
    for (usize index = 0; index < again->sections.size(); ++index) {
        T2D_CHECK_EQ(again->sections[index].name, table->sections[index].name);
        T2D_CHECK_EQ(again->sections[index].data.size(), table->sections[index].data.size());
    }
    for (usize index = 0; index < again->symbols.size(); ++index) {
        T2D_CHECK_EQ(again->symbols[index].name, table->symbols[index].name);
        T2D_CHECK_EQ(again->symbols[index].section, table->symbols[index].section);
        T2D_CHECK_EQ(again->symbols[index].binding, table->symbols[index].binding);
        T2D_CHECK_EQ(again->symbols[index].kind, table->symbols[index].kind);
    }
    T2D_CHECK(again->overridable_symbols().size() >= 4u);   // use_base, length_of, base_value, counter

    // Something that is not a table is refused with a reason, never read as far as it goes.
    std::optional<CodeTable> truncated = CodeTable::parse(ConstSpan<const u8>(bytes.data(), 40), &error);
    T2D_CHECK_FALSE(truncated.has_value());
    T2D_CHECK_FALSE(error.empty());
    std::vector<u8> wrong_magic(bytes);
    // Guarded rather than assumed: a table that serialised to nothing would make the next line the
    // bug it is meant to be testing for.
    if (!wrong_magic.empty()) wrong_magic[0] = 'X';
    T2D_CHECK_FALSE(CodeTable::parse(ConstSpan<const u8>(wrong_magic.data(), wrong_magic.size()), &error).has_value());
}

T2D_TEST(a_merged_image_runs_the_code_it_was_given) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("base"), object_of("caller")}, "vanilla", &error);
    T2D_REQUIRE(table.has_value());

    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.modules.size(), 1u);
    T2D_CHECK_EQ(report.modules[0].id, std::string("vanilla"));
    T2D_CHECK_EQ(report.overrides.size(), 0u);
    T2D_CHECK_MSG(report.unresolved.empty(), "unresolved: {}", names_of(report.unresolved));
    T2D_CHECK_GT(report.relocations, 0u);

    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 11);
    const auto read_counter = image.function<int()>("read_counter");
    T2D_REQUIRE(read_counter != nullptr);
    T2D_CHECK_EQ(read_counter(), 3);
    // The image owns the memory the code was copied into; a name it does not have answers nothing.
    T2D_CHECK(image.find("no_such_symbol") == nullptr);
}

T2D_TEST(a_later_table_replaces_a_symbol_and_every_call_to_it) {
    std::string error;
    std::optional<CodeTable> vanilla = pack({object_of("base"), object_of("caller")}, "vanilla", &error);
    std::optional<CodeTable> mod = pack({object_of("mod")}, "my_mod", &error);
    T2D_REQUIRE(vanilla.has_value());
    T2D_REQUIRE(mod.has_value());

    CodeImage image;
    image.add(std::move(*vanilla));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.modules.size(), 2u);
    T2D_REQUIRE(report.overrides.size() == 1u);
    T2D_CHECK_EQ(report.overrides[0].symbol, std::string("base_value"));
    T2D_CHECK_EQ(report.overrides[0].from, std::string("my_mod"));
    T2D_CHECK_EQ(report.overrides[0].replaced, std::string("vanilla"));

    // The game's own call goes to the mod's definition: this is the sentence the whole design is for.
    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 101);

    const auto winner = image.function<int()>("base_value");
    T2D_REQUIRE(winner != nullptr);
    T2D_CHECK_EQ(winner(), 100);

    // And what it replaced is still reachable, which is how a mod wraps a function instead of
    // replacing it outright.
    const auto replaced = reinterpret_cast<int (*)()>(image.find_previous("base_value"));
    T2D_REQUIRE(replaced != nullptr);
    T2D_CHECK_EQ(replaced(), 10);
    // Nothing else was touched.
    T2D_CHECK_EQ(image.find_previous("use_base"), nullptr);
    const auto read_counter = image.function<int()>("read_counter");
    T2D_REQUIRE(read_counter != nullptr);
    T2D_CHECK_EQ(read_counter(), 3);
}

T2D_TEST(a_symbol_the_running_program_provides_is_reached) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("base"), object_of("caller")}, "vanilla", &error);
    T2D_REQUIRE(table.has_value());

    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_MSG(report.unresolved.empty(), "unresolved: {}", names_of(report.unresolved));

    // No table defines the C library's strlen, so it is the running program that answers - which is
    // how a table calls back into the engine it was loaded by.
    const auto length_of = image.function<unsigned long(const char*)>("length_of");
    T2D_REQUIRE(length_of != nullptr);
    T2D_CHECK_EQ(length_of("hello"), 5ul);
}

T2D_TEST(a_symbol_nobody_defines_is_reported_and_the_module_is_refused) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("missing")}, "missing", &error);
    T2D_REQUIRE(table.has_value());

    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_REQUIRE(report.unresolved.size() == 1u);
    T2D_CHECK_EQ(report.unresolved[0], std::string("nowhere_to_be_found"));
    T2D_CHECK_MSG(report.unresolved.size() == 1u, "unresolved: {}", names_of(report.unresolved));
    // A module that could not be relocated is not loaded, and nothing it defined is reachable: a
    // symbol table that still offered it would be offering code that cannot run.
    T2D_CHECK(image.find("call_missing") == nullptr);
    T2D_CHECK(image.find_previous("call_missing") == nullptr);
}

T2D_TEST(the_toolchain_packs_sources_into_a_table) {
    const std::filesystem::path output = std::filesystem::path(T2D_TEST_TABLE_DIR) / "toolchain.codetab";
    // The toolchain drives the real compiler, so this is the whole path a mod author walks:
    // source -> compiler -> object -> table -> merged and called.
    const std::string command = std::format("\"{}\" build \"{}\" \"{}\" -o \"{}\" --id toolchain "
                                            "--version 1.0 --requires engine@0.1 --compiler \"{}\"",
                                            T2D_TEST_TOOLCHAIN, source_of("base"), source_of("caller"),
                                            output.string(), T2D_TEST_COMPILER);
    T2D_CHECK_EQ(std::system(command.c_str()), 0);

    std::string error;
    std::optional<CodeTable> table = CodeTable::load(output.string(), &error);
    T2D_REQUIRE(table.has_value());
    T2D_CHECK_EQ(table->id, std::string("toolchain"));
    T2D_CHECK_EQ(table->version, std::string("1.0"));
    // "id@version" went through the command line, the file and the reader: what the load checks is
    // what the author asked for.
    T2D_REQUIRE(table->requirements.size() == 1u);
    T2D_CHECK_EQ(table->requirements[0].id, std::string("engine"));
    T2D_CHECK_EQ(table->requirements[0].version, std::string("0.1"));
    T2D_CHECK_GT(table->overridable_symbols().size(), 0u);

    CodeImage image;
    // The table asked for "engine@0.1", so the program loading it says it is that.
    image.declare_host("engine", "0.1");
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 11);
}

T2D_TEST(a_mod_replaces_a_virtual_method_and_the_vtable_follows_it) {
    std::string error;
    // The game's own half: the class, the object and the call that goes through the vtable.
    std::optional<CodeTable> vanilla = pack({object_of("machine"), object_of("factory")}, "vanilla", &error);
    T2D_REQUIRE(vanilla.has_value());

    // What a C++ compiler emits for a class is in the table, and the parts more than one translation
    // unit emits are weak: one program has one vtable, one typeinfo, one inline destructor.
    bool saw_vtable = false;
    bool saw_typeinfo = false;
    for (const CodeTableSymbol& symbol : vanilla->symbols) {
        // The same name appears twice - defined here, and undefined in the translation unit that only
        // uses the class - so it is the definition the binding is asked about.
        if (!symbol.defined()) continue;
        if (symbol.name == "_ZTVN4shop7MachineE") {
            saw_vtable = true;
            T2D_CHECK_EQ(symbol.binding, ObjectSymbolBinding::Weak);
        }
        if (symbol.name == "_ZTIN4shop7MachineE") {
            saw_typeinfo = true;
            T2D_CHECK_EQ(symbol.binding, ObjectSymbolBinding::Weak);
        }
    }
    T2D_CHECK(saw_vtable);
    T2D_CHECK(saw_typeinfo);

    CodeImage plain;
    plain.add(*vanilla);
    const CodeImageReport& plain_report = plain.load();
    T2D_CHECK_MSG(plain_report.clean(), "{}", plain_report.first_error());
    // The static constructor ran, which is what a global that cannot be computed at compile time
    // needs: it is run after every relocation is in, never before.
    T2D_CHECK_GT(plain_report.init_calls, 0u);
    const auto started = plain.function<int()>("_ZN4shop22machines_started_valueEv");
    T2D_REQUIRE(started != nullptr);
    T2D_CHECK_EQ(started(), 41);
    const auto plain_output = plain.function<int()>("machine_output");
    T2D_REQUIRE(plain_output != nullptr);
    T2D_CHECK_EQ(plain_output(), 25);   // 2 * 10 + 5

    std::optional<CodeTable> mod = pack({object_of("mod_machine")}, "shop_mod", &error);
    T2D_REQUIRE(mod.has_value());
    CodeImage image;
    image.add(std::move(*vanilla));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());

    // Two strong definitions were replaced - the virtual method and the plain function - and the weak
    // ones (the vtable, the typeinfo, the inline destructor) were coalesced instead of reported: a
    // merge that complained about those would complain about every mod that includes a header.
    T2D_CHECK_EQ(report.overrides.size(), 2u);
    for (const CodeOverride& replaced : report.overrides) {
        T2D_CHECK(replaced.symbol == "_ZNK4shop7Machine4rateEv" || replaced.symbol == "_ZN4shop11speed_bonusEv");
        T2D_CHECK_EQ(replaced.from, std::string("shop_mod"));
        T2D_CHECK_EQ(replaced.replaced, std::string("vanilla"));
    }
    const auto output = image.function<int()>("machine_output");
    T2D_REQUIRE(output != nullptr);
    // 9 * 10 + 7: the call the game makes through the vtable went to the mod, and so did the plain one.
    T2D_CHECK_EQ(output(), 97);
}

T2D_TEST(a_module_built_for_another_engine_version_is_refused) {
    std::string error;
    std::optional<CodeTable> vanilla = pack({object_of("base"), object_of("caller")}, "mine", &error);
    T2D_REQUIRE(vanilla.has_value());
    vanilla->version = "1.0";

    // A mod that was built for this engine, and one that was built for the next one.
    std::optional<CodeTable> matching = pack({object_of("mod")}, "matching_mod", &error);
    std::optional<CodeTable> stale = pack({object_of("mod")}, "stale_mod", &error);
    T2D_REQUIRE(matching.has_value());
    T2D_REQUIRE(stale.has_value());
    matching->version = "1.0";
    matching->requirements = {CodeRequirement{"mine", "1.0"}};
    stale->requirements = {CodeRequirement{"mine", "2.0"}};

    CodeImage image;
    image.declare_host("mine", "1.0");
    image.add(*vanilla);
    image.add(std::move(*matching));
    image.add(std::move(*stale));
    const CodeImageReport& report = image.load();

    // Three modules were looked at, and the one that asked for a version this engine is not is refused
    // with a message naming both versions - it is not merged and its symbols are not in the table.
    T2D_REQUIRE(report.modules.size() == 3u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK(report.modules[1].ok);
    T2D_CHECK_FALSE(report.modules[2].ok);
    T2D_CHECK_FALSE(report.clean());
    bool named_both = false;
    for (const std::string& message : report.errors) {
        if (message.find("stale_mod") != std::string::npos && message.find("2.0") != std::string::npos &&
            message.find("1.0") != std::string::npos) {
            named_both = true;
        }
    }
    T2D_CHECK(named_both);
    // The matching mod was merged, so the engine's own call goes to it; the stale one changed nothing.
    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 101);
    T2D_CHECK_EQ(report.overrides.size(), 1u);
    T2D_CHECK_EQ(report.overrides[0].from, std::string("matching_mod"));
}

T2D_TEST(a_requirement_nothing_provides_is_refused) {
    std::string error;
    std::optional<CodeTable> mod = pack({object_of("mod")}, "lonely_mod", &error);
    T2D_REQUIRE(mod.has_value());
    // An id on its own only asks that somebody provides it; nobody does here.
    mod->requirements = {CodeRequirement{"some_engine", ""}};

    CodeImage image;
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_REQUIRE(report.modules.size() == 1u);
    T2D_CHECK_FALSE(report.modules[0].ok);
    T2D_CHECK(image.find("base_value") == nullptr);
    bool said_so = false;
    for (const std::string& message : report.errors) {
        if (message.find("some_engine") != std::string::npos) said_so = true;
    }
    T2D_CHECK(said_so);
}

T2D_TEST_MAIN
