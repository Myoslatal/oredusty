// Code tables: what a compiler emitted is packed into one file, two of those files are merged in
// memory, and a symbol the second one defines replaces the first one's - everywhere, including inside
// the code that was compiled first. That last part is the whole point, and it is what a shared library
// loaded at run time cannot do.
#include <t2d/core/code_table.h>
#include <t2d/core/object_file.h>

#include <support/test_support.h>

#include <algorithm>
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

// One definition, two bodies. A compiler is allowed to emit a vague linkage function differently in
// each translation unit that uses it - it inlines a different amount of it into its own body - and both
// bodies stand for the one symbol, which is why a linker keeps one of them and every caller uses it.
// Release GCC does this with libstdc++'s std::__format sinks, and a table that assumed the copies were
// the same bytes kept the small one while applying the big one's relocations to it: a relocation past
// the end of a section, reported at load time, and a package that would not start.
T2D_TEST(two_bodies_of_one_definition_are_two_sections_and_no_relocation_lands_past_one) {
    std::string error;
    std::optional<CodeTable> table =
        pack({object_of("twin_small"), object_of("twin_big"), object_of("twin_caller")}, "twins", &error);
    T2D_REQUIRE(table.has_value());

    // The copies are not the same bytes, so both are carried: dropping one would mean dropping the code
    // that is here for the other one's offsets. The first is what the symbol means, as in a link.
    usize copies = 0;
    usize small = 0;
    usize big = 0;
    for (const CodeTableSection& section : table->sections) {
        if (section.name != ".text._Z10twin_widthi") continue;
        if (copies == 0) small = section.data.size();
        if (copies == 1) big = section.data.size();
        ++copies;
    }
    T2D_CHECK_EQ(copies, 2u);
    T2D_CHECK_MSG(small < big, "the fixture's two bodies are {} and {} bytes", small, big);

    // The invariant the merge has to keep whatever it does with copies: every relocation patches a field
    // that is inside the section it names.
    for (const CodeTableRelocation& relocation : table->relocations) {
        T2D_REQUIRE(relocation.section < table->sections.size());
        const CodeTableSection& section = table->sections[relocation.section];
        const u64 width = relocation.type == kRelocationAbsolute64 ? 8 : 4;
        T2D_CHECK_MSG(relocation.offset + width <= section.data.size(), "'{}' + {:#x} is past its {} bytes",
                      section.name, relocation.offset, section.data.size());
    }

    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());

    // The call lands in the copy that came first, and that copy answers the way it was written.
    const auto answer = image.function<int()>("twin_answer");
    T2D_REQUIRE(answer != nullptr);
    T2D_CHECK_EQ(answer(), 42);
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


// --- what a build was compiled as (docs/ABI.md) ----------------------------------------------------
//
// Two tables are one program when the things that decide what a type looks like agree: the C++ library's
// own ABI macros, the sizes and alignments of the types the two sides share, and the headers each was
// compiled against. What does *not* decide that - the optimisation level, NDEBUG, RTTI, exceptions - is
// allowed to differ, and a table that differs only there is merged fully rather than loaded "with fewer
// rights". There is no third answer: a module whose types are not the program's is refused, with the
// fact that differs named.

/// The fingerprint a record's facts make, written the way the toolchain writes it: so that what the
/// tool produces and what the loader compares are the same thing and not two opinions of it.
[[nodiscard]] std::string fingerprint_of(const std::vector<std::pair<std::string, std::string>>& facts) {
    unsigned long long hash = 1469598103934665603ull;
    for (const auto& [name, value] : facts) {
        for (const char letter : name + "=" + value + "\n") {
            hash ^= static_cast<unsigned char>(letter);
            hash *= 1099511628211ull;
        }
    }
    return std::format("{:016x}", hash);
}

/// A record with those facts, fingerprinted the same way the toolchain fingerprints them.
[[nodiscard]] CodeAbi record_of(std::vector<std::pair<std::string, std::string>> required,
                                std::vector<std::pair<std::string, std::string>> allowed = {},
                                std::vector<std::pair<std::string, std::string>> headers = {}) {
    CodeAbi abi;
    abi.id = fingerprint_of(required);
    abi.required = std::move(required);
    abi.allowed = std::move(allowed);
    abi.headers = std::move(headers);
    return abi;
}

/// Whether some message says this.
[[nodiscard]] bool says(const std::vector<std::string>& messages, std::string_view text) {
    for (const std::string& message : messages) {
        if (message.find(text) != std::string::npos) return true;
    }
    return false;
}

T2D_TEST(two_tables_that_differ_only_in_what_may_differ_are_one_program) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    // The same build twice: one at -O3 with NDEBUG, one at -O0 without. Every fact that decides what a
    // type looks like is the same, so this is one program and the mod's override still lands.
    game->abi = record_of({{"_GLIBCXX_USE_CXX11_ABI", "1"}, {"sizeof(std::string)", "32"}},
                          {{"__OPTIMIZE__", "1"}, {"NDEBUG", "1"}});
    mod->abi = record_of({{"_GLIBCXX_USE_CXX11_ABI", "1"}, {"sizeof(std::string)", "32"}},
                         {{"__OPTIMIZE__", "0"}, {"NDEBUG", "0"}});

    CodeImage image;
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].abi_match);
    T2D_CHECK(report.modules[1].abi_match);
    // Both differences are said out loud - "why is this mod different" should have an answer - and
    // neither of them stops anything.
    T2D_CHECK_EQ(report.warnings.size(), 2u);
    T2D_CHECK(says(report.warnings, "__OPTIMIZE__: 0 here, 1 there"));
    T2D_CHECK(says(report.warnings, "NDEBUG: 0 here, 1 there"));
    T2D_REQUIRE(report.overrides.size() == 1u);
    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 101);
}

T2D_TEST(a_table_built_for_another_abi_is_refused_and_the_fact_is_named) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    // Another std::string: the same names, two layouts. No loader can make that work, so it is not
    // attempted - and the message says which fact, because a build that has to be fixed should say what.
    game->abi = record_of({{"_GLIBCXX_USE_CXX11_ABI", "1"}, {"sizeof(std::string)", "32"}});
    mod->abi = record_of({{"_GLIBCXX_USE_CXX11_ABI", "0"}, {"sizeof(std::string)", "8"}});

    CodeImage image;
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK_FALSE(report.modules[1].ok);
    T2D_CHECK(says(report.errors, "_GLIBCXX_USE_CXX11_ABI: 0 here, 1 there"));
    T2D_CHECK(says(report.errors, "sizeof(std::string): 8 here, 32 there"));
    // A refused module is not merged at all: the game's own call still goes where it went.
    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 11);
    T2D_CHECK_EQ(image.find_previous("base_value"), nullptr);
}

T2D_TEST(a_header_that_differs_is_refused_even_when_the_fingerprint_agrees) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    T2D_REQUIRE(game.has_value());
    // The same compiler, the same macros, the same sizes - and a different declaration of a type the
    // two share. The fingerprint cannot see that; the header hashes can, and do (docs/ABI.md H4).
    game->abi = record_of({{"sizeof(std::string)", "32"}}, {}, {{"t2d/core/log.h", "aaaaaaaaaaaaaaaa"}});

    std::optional<CodeTable> other = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(other.has_value());
    other->abi = record_of({{"sizeof(std::string)", "32"}}, {}, {{"t2d/core/log.h", "bbbbbbbbbbbbbbbb"}});

    CodeImage image;
    image.add(std::move(*game));
    image.add(std::move(*other));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(says(report.errors, "header t2d/core/log.h differs"));

    // A header only one side included says nothing about the other: a mod does not include what the
    // engine includes, and refusing it for that would refuse every mod there is.
    std::optional<CodeTable> game2 = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod2 = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game2.has_value());
    T2D_REQUIRE(mod2.has_value());
    game2->abi = record_of({{"sizeof(std::string)", "32"}}, {}, {{"t2d/core/log.h", "aaaaaaaaaaaaaaaa"}});
    mod2->abi = record_of({{"sizeof(std::string)", "32"}}, {},
                          {{"t2d/core/log.h", "aaaaaaaaaaaaaaaa"}, {"mine/app.h", "cccccccccccccccc"}});
    CodeImage image2;
    image2.add(std::move(*game2));
    image2.add(std::move(*mod2));
    const CodeImageReport& report2 = image2.load();
    T2D_CHECK_MSG(report2.clean(), "{}", report2.first_error());
    T2D_REQUIRE(report2.modules.size() == 2u);
    T2D_CHECK(report2.modules[1].abi_match);
}

T2D_TEST(a_table_that_does_not_say_what_it_was_built_as_is_loaded_and_said_out_loud) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    game->abi = record_of({{"sizeof(std::string)", "32"}});
    // No record: the load cannot tell, so it says so and trusts the author rather than refusing a table
    // that was built before records existed - or by hand.
    CodeImage image;
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK(says(report.warnings, "module 'mod' does not say what ABI it was built as"));
    const auto use_base = image.function<int()>("use_base");
    T2D_REQUIRE(use_base != nullptr);
    T2D_CHECK_EQ(use_base(), 101);
}

T2D_TEST(the_engines_own_record_is_what_a_module_is_measured_against) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    // The launcher's own record: the game's table agrees with it, the mod's does not. What a module is
    // measured against is the code it will call, not the table that happens to be first.
    game->abi = record_of({{"_GLIBCXX_USE_CXX11_ABI", "1"}});
    mod->abi = record_of({{"_GLIBCXX_USE_CXX11_ABI", "0"}});

    CodeImage image;
    image.declare_host("engine", "1.0");
    image.declare_host_abi(record_of({{"_GLIBCXX_USE_CXX11_ABI", "1"}}));
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK(report.modules[0].abi_match);
    T2D_CHECK_FALSE(report.modules[1].ok);
    T2D_CHECK(says(report.errors, "_GLIBCXX_USE_CXX11_ABI: 0 here, 1 there"));
}

T2D_TEST(a_module_is_measured_against_the_games_headers_too) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    // The engine's own record was written without compiling anything, so it carries facts but no
    // headers. The game's table carries the headers, and a module is measured against both: a mod
    // compiled against a different copy of an engine header is caught by the table, not by the record.
    game->abi = record_of({{"sizeof(std::string)", "32"}}, {}, {{"t2d/core/log.h", "aaaaaaaaaaaaaaaa"}});
    mod->abi = record_of({{"sizeof(std::string)", "32"}}, {}, {{"t2d/core/log.h", "bbbbbbbbbbbbbbbb"}});

    CodeImage image;
    image.declare_host("engine", "1.0");
    image.declare_host_abi(record_of({{"sizeof(std::string)", "32"}}));
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].ok);
    T2D_CHECK_FALSE(report.modules[1].ok);
    T2D_CHECK(says(report.errors, "header t2d/core/log.h differs"));
    // And a header only the mod includes still says nothing: this is not a check that every module
    // included the same files.
    std::optional<CodeTable> game2 = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod2 = pack({object_of("mod")}, "mod", &error);
    T2D_REQUIRE(game2.has_value());
    T2D_REQUIRE(mod2.has_value());
    game2->abi = record_of({{"sizeof(std::string)", "32"}}, {}, {{"t2d/core/log.h", "aaaaaaaaaaaaaaaa"}});
    mod2->abi = record_of({{"sizeof(std::string)", "32"}}, {},
                          {{"t2d/core/log.h", "aaaaaaaaaaaaaaaa"}, {"mine/app.h", "dddddddddddddddd"}});
    CodeImage image2;
    image2.declare_host("engine", "1.0");
    image2.declare_host_abi(record_of({{"sizeof(std::string)", "32"}}));
    image2.add(std::move(*game2));
    image2.add(std::move(*mod2));
    const CodeImageReport& report2 = image2.load();
    T2D_CHECK_MSG(report2.clean(), "{}", report2.first_error());
    T2D_REQUIRE(report2.modules.size() == 2u);
    T2D_CHECK(report2.modules[1].abi_match);
    // Two references can find the same difference (the engine's record and the game's table were built
    // the same way); it is reported once.
    std::vector<std::string> warnings = report2.warnings;
    std::sort(warnings.begin(), warnings.end());
    T2D_CHECK_EQ(std::unique(warnings.begin(), warnings.end()) - warnings.begin(),
                 static_cast<std::ptrdiff_t>(report2.warnings.size()));
}

T2D_TEST(the_toolchain_records_what_a_build_is_and_two_of_them_agree) {
    // The whole path a mod author walks, twice: the compiler is asked what the build's ABI is, the
    // answer travels in the table, and two builds that differ only in what may differ are one program.
    const std::filesystem::path debug_build = std::filesystem::path(T2D_TEST_TABLE_DIR) / "abi_debug.codetab";
    const std::filesystem::path release_build = std::filesystem::path(T2D_TEST_TABLE_DIR) / "abi_release.codetab";
    // Both halves of the class: the machine and the factory that makes one.
    const std::string common = std::format("\"{}\" build \"{}\" \"{}\" --compiler \"{}\" --include \"{}\"",
                                           T2D_TEST_TOOLCHAIN, source_of("machine"), source_of("factory"),
                                           T2D_TEST_COMPILER, T2D_TEST_TABLE_SOURCES);
    T2D_CHECK_EQ(std::system(std::format("{} -o \"{}\" --id debug_build --opt -O0", common, debug_build.string()).c_str()), 0);
    T2D_CHECK_EQ(std::system(std::format("{} -o \"{}\" --id release_build --opt -O3 --define NDEBUG", common,
                                         release_build.string()).c_str()),
                 0);

    std::string error;
    std::optional<CodeTable> debug = CodeTable::load(debug_build.string(), &error);
    std::optional<CodeTable> release = CodeTable::load(release_build.string(), &error);
    T2D_REQUIRE(debug.has_value());
    T2D_REQUIRE(release.has_value());
    T2D_REQUIRE(debug->abi.recorded());
    T2D_REQUIRE(release->abi.recorded());
    // The optimisation level and NDEBUG are not part of the ABI: the compiler was asked, and its answer
    // is the same. What the compiler *did* answer with is in the record, and the fingerprint is over
    // exactly the facts that must agree - the same formula the loader compares by.
    T2D_CHECK_EQ(debug->abi.id, release->abi.id);
    T2D_CHECK_EQ(debug->abi.id, fingerprint_of(debug->abi.required));
    T2D_CHECK_GT(debug->abi.required.size(), 10u);
    bool saw_abi_macro = false;
    for (const auto& [name, value] : debug->abi.required) {
        if (name == "_GLIBCXX_USE_CXX11_ABI") saw_abi_macro = true;
    }
    T2D_CHECK(saw_abi_macro);
    // The header it included is pinned by content, under a path that does not depend on where the build
    // tree is: that is what lets a mod built against a package be compared with the engine.
    bool saw_header = false;
    for (const auto& [name, hash] : debug->abi.headers) {
        if (name == "machine.h") saw_header = true;
    }
    T2D_CHECK(saw_header);

    // And the load accepts both, in either order: the same record, so the same program.
    CodeImage image;
    image.add(std::move(*debug));
    image.add(std::move(*release));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(report.modules.size() == 2u);
    T2D_CHECK(report.modules[0].abi_match);
    T2D_CHECK(report.modules[1].abi_match);
}


/// A module's static destructors are registered against the address the runtime answered __dso_handle
/// with, so destroying the image has to run them first - the way dlclose does. The flag this destructor
/// writes to lives in the test program, not in the table: reading it after the image is gone is only
/// safe because what it points at was never the image's memory.
extern "C" {
int dtor_host_flag = 0;
}

T2D_TEST(a_destroyed_image_runs_the_destructors_it_registered) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("dtor")}, "dtor", &error);
    T2D_REQUIRE(table.has_value());
    dtor_host_flag = 0;
    {
        CodeImage image;
        image.add(std::move(*table));
        const CodeImageReport& report = image.load();
        T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
        // The static constructor has run, and the destructor it registered has not.
        T2D_CHECK_EQ(dtor_host_flag, 0);
    }
    // The image is gone: its memory is unmapped, and the destructor ran before that happened rather
    // than at exit, when it would have been a jump into an unmapped page.
    T2D_CHECK_EQ(dtor_host_flag, 1);
}


extern "C" {
/// A name this test program exports and one of the fixtures defines as well: what "the program already
/// provides this" looks like (docs/ABI.md H6).
int host_owned() { return 1; }
}

T2D_TEST(a_32_bit_absolute_address_that_does_not_fit_is_refused) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("abs32")}, "abs32", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    // A table is placed by mmap, so the address never fits 32 bits: writing it anyway is a silently
    // wrong pointer, and the module is refused instead (docs/ABI.md H5).
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(says(report.errors, "does not fit the 32 bit field"));
    T2D_CHECK(image.find("abs32_probe") == nullptr);
}

T2D_TEST(constructors_run_in_priority_order_not_section_order) {
    std::string error;
    // The object with the *plain* .init_array comes first, the one with .init_array.00101 second - which
    // is the order that used to run the priority-101 constructor last (docs/ABI.md H7).
    std::optional<CodeTable> table = pack({object_of("ctor_default"), object_of("ctor_priority")}, "ctors", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.init_calls, 2u);
    const auto probe = image.function<int()>("ctor_probe");
    T2D_REQUIRE(probe != nullptr);
    // 14 = "A,D": the priority the section name asks for is what decides, as in a link.
    T2D_CHECK_EQ(probe(), 14);
}

T2D_TEST(two_bodies_of_one_definition_are_reported) {
    std::string error;
    std::optional<CodeTable> table =
        pack({object_of("twin_small"), object_of("twin_big"), object_of("twin_caller")}, "twins", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    // The two copies are not the same machine code, which a compiler is allowed to do with a vague
    // linkage function: one of them wins, so the other module's calls do not do what it was compiled to
    // do. Said out loud rather than left as a mystery (docs/ABI.md H3).
    // One line for the pair, with the names in it: the same sentence per symbol would be a page of it.
    T2D_CHECK(says(report.warnings, "both define these and the two bodies differ"));
    T2D_CHECK(says(report.warnings, "_Z10twin_widthi"));
    T2D_CHECK(says(report.warnings, "definition(s) with a different body"));
}

T2D_TEST(a_module_that_defines_what_the_program_provides_is_reported) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("host_name")}, "host_name", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK(says(report.warnings, "which the running program provides too"));
    // The table's own call goes to the module, and that is the part that works.
    const auto owned = image.function<int()>("host_owned");
    T2D_REQUIRE(owned != nullptr);
    T2D_CHECK_EQ(owned(), 7);
    // The program's own call was bound before the merge and still answers with its own definition.
    T2D_CHECK_EQ(host_owned(), 1);
}

T2D_TEST(a_vtable_that_disagrees_about_its_slots_is_refused) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("machine"), object_of("factory")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("machine"), object_of("factory")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());

    const std::string vtable = "_ZTVN4shop7MachineE";
    const auto index_of = [](const CodeTable& table, std::string_view name) -> std::optional<usize> {
        for (usize index = 0; index < table.symbols.size(); ++index) {
            if (table.symbols[index].defined() && table.symbols[index].name == name) return index;
        }
        return std::nullopt;
    };
    // One slot of the mod's vtable names another function than the game's does. A virtual call is an
    // index into that table, so the mod would call something else than it thinks it calls (H2).
    const std::optional<usize> slot_symbol = index_of(*mod, vtable);
    const std::optional<usize> other_symbol = index_of(*mod, "_ZN4shop11speed_bonusEv");
    T2D_REQUIRE(slot_symbol.has_value());
    T2D_REQUIRE(other_symbol.has_value());
    const u32 vtable_section = mod->symbols[*slot_symbol].section;
    const u64 vtable_value = mod->symbols[*slot_symbol].value;
    usize changed = 0;
    for (CodeTableRelocation& relocation : mod->relocations) {
        if (relocation.section != vtable_section || relocation.offset < vtable_value) continue;
        if (relocation.symbol == *other_symbol) continue;
        relocation.symbol = static_cast<u32>(*other_symbol);
        ++changed;
        break;
    }
    T2D_CHECK_EQ(changed, 1u);

    CodeImage image;
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(says(report.errors, "the vtable for '_ZTVN4shop7MachineE'"));
    T2D_CHECK_FALSE(report.modules[1].ok);

    // And a vtable with a different number of entries: the class is not the same class in the two
    // builds, which no amount of slot comparison can repair.
    std::optional<CodeTable> game2 = pack({object_of("machine"), object_of("factory")}, "game", &error);
    std::optional<CodeTable> mod2 = pack({object_of("machine"), object_of("factory")}, "mod", &error);
    T2D_REQUIRE(game2.has_value());
    T2D_REQUIRE(mod2.has_value());
    const std::optional<usize> grown = index_of(*mod2, vtable);
    T2D_REQUIRE(grown.has_value());
    mod2->symbols[*grown].size += 8;
    CodeImage image2;
    image2.add(std::move(*game2));
    image2.add(std::move(*mod2));
    const CodeImageReport& report2 = image2.load();
    T2D_CHECK_FALSE(report2.clean());
    T2D_CHECK(says(report2.errors, "the vtable for '_ZTVN4shop7MachineE' has"));
    T2D_CHECK(says(report2.errors, "the class is not the same class in the two builds"));
}

T2D_TEST(a_throw_inside_a_table_finds_its_handler) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("exc")}, "exc", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    // Caught inside the table: the frame descriptions the module brought were registered, so the
    // unwinder can walk the frame that raised it (docs/ABI.md H1 - this used to be terminate).
    const auto catcher = image.function<int()>("exc_catcher");
    T2D_REQUIRE(catcher != nullptr);
    T2D_CHECK_EQ(catcher(), 7);

    // And caught by the program that loaded it: the unwind walks out through the table's frames.
    int caught = 0;
    const auto thrower = image.function<void()>("exc_thrower");
    T2D_REQUIRE(thrower != nullptr);
    try {
        thrower();
    } catch (int value) {
        caught = value;
    }
    T2D_CHECK_EQ(caught, 7);
}

T2D_TEST(an_absolute_symbol_is_its_value) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("abs_sym")}, "abs_sym", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    // The object carried the number; a linker would fold it in, and the runtime does the same
    // instead of reporting a symbol nobody defines (docs/ABI.md H11).
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    const auto probe = image.function<unsigned long long()>("abs_probe");
    T2D_REQUIRE(probe != nullptr);
    T2D_CHECK_EQ(probe(), 0x1234ull);
}

T2D_TEST(an_absolute_symbol_where_an_address_is_wanted_is_refused) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("abs_sym")}, "abs_sym", &error);
    T2D_REQUIRE(table.has_value());
    // The fixture's own relocation writes the value, which is what an absolute symbol is for. Asking it
    // for an *address* is the case a linker would refuse, and so does the runtime: a number written
    // where an address belongs is a call into nowhere.
    std::optional<usize> constant;
    for (usize index = 0; index < table->symbols.size(); ++index) {
        if (table->symbols[index].absolute && table->symbols[index].name == "abs_constant") constant = index;
    }
    T2D_REQUIRE(constant.has_value());
    usize changed = 0;
    for (CodeTableRelocation& relocation : table->relocations) {
        relocation.symbol = static_cast<u32>(*constant);
        relocation.type = kRelocationPc32;
        ++changed;
        break;
    }
    T2D_CHECK_EQ(changed, 1u);
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(says(report.errors, "is an absolute symbol"));
    T2D_CHECK(image.find("abs_probe") == nullptr);
}

T2D_TEST(a_protected_definition_says_its_override_may_not_reach_every_caller) {
    std::string error;
    std::optional<CodeTable> game = pack({object_of("base"), object_of("caller")}, "game", &error);
    std::optional<CodeTable> mod = pack({object_of("protected_mod")}, "mod", &error);
    T2D_REQUIRE(game.has_value());
    T2D_REQUIRE(mod.has_value());
    CodeImage image;
    image.add(std::move(*game));
    image.add(std::move(*mod));
    const CodeImageReport& report = image.load();
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_REQUIRE(report.overrides.size() == 1u);
    T2D_CHECK(says(report.warnings, "has protected visibility"));
}

T2D_TEST(a_section_that_asks_for_more_alignment_than_a_page_is_refused) {
    std::string error;
    std::optional<CodeTable> table = pack({object_of("aligned")}, "aligned", &error);
    T2D_REQUIRE(table.has_value());
    CodeImage image;
    image.add(std::move(*table));
    const CodeImageReport& report = image.load();
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(says(report.errors, "byte alignment"));
    T2D_CHECK(image.find("aligned_probe") == nullptr);
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

T2D_TEST(the_version_rule_follows_the_surface_a_module_uses) {
    const ApiVersion one{1, 0};
    const ApiVersion next{1, 1};
    const ApiVersion far{1, 3};
    const ApiVersion other{2, 0};
    // Inside the published surface: the whole major version, and nothing said about it.
    T2D_CHECK(api_verdict(one, next, true) == ApiVerdict::Accept);
    T2D_CHECK(api_verdict(far, one, true) == ApiVerdict::Accept);
    // Outside it: the same version quietly, one minor either way with a warning, further is refused.
    T2D_CHECK(api_verdict(one, one, false) == ApiVerdict::Accept);
    T2D_CHECK(api_verdict(one, next, false) == ApiVerdict::Warn);
    T2D_CHECK(api_verdict(next, one, false) == ApiVerdict::Warn);
    T2D_CHECK(api_verdict(far, one, false) == ApiVerdict::Refuse);
    // A different major version is refused whichever surface the module used.
    T2D_CHECK(api_verdict(other, one, true) == ApiVerdict::Refuse);
    T2D_CHECK(api_verdict(other, one, false) == ApiVerdict::Refuse);

    T2D_REQUIRE(ApiVersion::parse("1.0").has_value());
    T2D_CHECK_EQ(ApiVersion::parse("1.0")->major, static_cast<i64>(1));
    T2D_CHECK_EQ(ApiVersion::parse("2").value().minor, static_cast<i64>(0));
    T2D_CHECK_EQ(ApiVersion::parse("3.14").value().minor, static_cast<i64>(14));
    T2D_CHECK_FALSE(ApiVersion::parse("x.y").has_value());
    T2D_CHECK_FALSE(ApiVersion::parse("1.").has_value());
    T2D_CHECK_FALSE(ApiVersion::parse("").has_value());
}

T2D_TEST(a_surface_says_which_symbols_are_the_engines) {
    std::string error;
    const char* text = "engine 1.2\nA _ZN3t2d3logEv t2d/core   # t2d::log\nB _ZN3ore5WindowD1Ev ore/platform\n";
    std::optional<ApiSurface> surface = ApiSurface::parse(text, &error);
    T2D_REQUIRE(surface.has_value());
    T2D_CHECK_EQ(surface->version.major, static_cast<i64>(1));
    T2D_CHECK_EQ(surface->version.minor, static_cast<i64>(2));
    T2D_CHECK_EQ(surface->symbols.size(), 2u);
    T2D_CHECK(surface->contains("_ZN3t2d3logEv"));
    T2D_CHECK_FALSE(surface->contains("_ZN3t2d4nopeEv"));

    // The platform's symbols are not the engine's to publish, and are never asked about - while a const
    // member function and a vtable are, whatever their mangling starts with.
    T2D_CHECK_FALSE(ApiSurface::engine_symbol("memcpy"));
    T2D_CHECK_FALSE(ApiSurface::engine_symbol("_ZSt20__throw_length_errorPKc"));
    T2D_CHECK(ApiSurface::engine_symbol("_ZNK3t2d8Camera2D8world_ofENS_4Vec2E"));
    T2D_CHECK(ApiSurface::engine_symbol("_ZTVN3ore8RendererE"));
    T2D_CHECK(ApiSurface::engine_symbol("_ZN4mine13ContentPack4loadERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPS6_"));

    // A surface that does not say which engine it belongs to is refused, not assumed.
    T2D_CHECK_FALSE(ApiSurface::parse("A _ZN3t2d3logEv t2d/core", &error).has_value());
}

T2D_TEST_MAIN
