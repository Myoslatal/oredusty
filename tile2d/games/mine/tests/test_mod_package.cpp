// Mod packages: the manifest, the load order, everything that can be wrong with a package, and a
// native module that really runs. The example packages live in tests/mods and are assembled in the
// build tree by CMake; the broken ones are written on the fly, so a test hands the loader exactly the
// defect it means to.
#include <mine/content_loader.h>
#include <mine/mod_package.h>
#include <mine/registry.h>

#include <support/test_support.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

using namespace mine;
namespace fs = std::filesystem;

namespace {

/// A scratch directory of packages. Removed again when the test ends, whatever happens.
class Scratch {
public:
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / std::format("t2d_mods_{}", stamp);
        std::error_code code;
        fs::remove_all(root_, code);
        fs::create_directories(root_, code);
    }
    ~Scratch() {
        std::error_code code;
        fs::remove_all(root_, code);
    }
    T2D_NON_COPYABLE(Scratch);

    void write(const std::string& relative, const std::string& text) const {
        const fs::path path = root_ / relative;
        std::error_code code;
        fs::create_directories(path.parent_path(), code);
        std::ofstream file(path);
        file << text;
    }
    void copy_in(const std::string& from, const std::string& relative) const {
        const fs::path path = root_ / relative;
        std::error_code code;
        fs::create_directories(path.parent_path(), code);
        fs::copy_file(from, path, fs::copy_options::overwrite_existing, code);
    }
    [[nodiscard]] std::string root() const { return root_.string(); }

private:
    fs::path root_;
};

[[nodiscard]] bool mentions(const std::vector<std::string>& lines, std::string_view needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// The names a mod registered, in registration order.
[[nodiscard]] std::vector<std::string> names_of(const LoadedMod& mod) {
    std::vector<std::string> names;
    for (const ModContentEntry& entry : mod.registered) names.push_back(entry.name);
    return names;
}

} // namespace

T2D_TEST(the_example_packages_load_in_dependency_order) {
    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& report = host.load({T2D_TEST_MODS_DIR}, registry);

    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? "" : report.errors.front());
    T2D_CHECK_EQ(report.mods.size(), 2u);
    T2D_CHECK_EQ(host.count(), 2u);
    T2D_CHECK_EQ(report.native_modules, 1u);
    T2D_CHECK_EQ(host.native_count(), 1u);
    T2D_REQUIRE(report.mods.size() == 2u);
    // example_native requires example_data, so the data mod has to be first even though "example_data"
    // sorts after "example_native"... it does not, but the order comes from the requirement, not from
    // the directory listing.
    T2D_CHECK_EQ(report.mods[0].manifest.id, std::string("example_data"));
    T2D_CHECK_EQ(report.mods[1].manifest.id, std::string("example_native"));
    T2D_CHECK(report.mods[0].ok);
    T2D_CHECK(report.mods[1].ok);
    T2D_CHECK_FALSE(report.mods[0].native_loaded);
    T2D_CHECK(report.mods[1].native_loaded);

    // The data mod registered its three names, the native one generated three more from "tiers:3".
    T2D_CHECK_EQ(names_of(report.mods[0]).size(), 3u);
    T2D_CHECK_EQ(names_of(report.mods[1]).size(), 3u);
    T2D_CHECK_EQ(report.content_registered, 6u);
    T2D_CHECK_EQ(registry.count(ContentKind::Structure), 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Machine), 4u);   // mod_pump plus three generated drills
    T2D_CHECK(registry.find(ContentKind::Structure, "mod_wall") != kNoContent);
    T2D_CHECK(registry.find(ContentKind::Machine, "mod_tier_2_drill") != kNoContent);
    T2D_CHECK_EQ(registry.find(ContentKind::Machine, "mod_tier_9_drill"), kNoContent);
    T2D_CHECK_EQ(report.mods[1].registered[0].name, std::string("mod_tier_1_drill"));

    // A mod's content is indistinguishable from the game's own: same registry, same ids.
    const ContentId wall = registry.find(ContentKind::Structure, "mod_wall");
    T2D_CHECK_EQ(wall, 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Machine, "mod_tier_1_drill"), 2u);

    host.unload();
    T2D_CHECK_EQ(host.count(), 0u);
    T2D_CHECK(host.empty());
}

T2D_TEST(a_directory_that_is_one_package_is_a_package) {
    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& report =
        host.load({std::string(T2D_TEST_MODS_DIR) + "/example_data"}, registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? "" : report.errors.front());
    T2D_CHECK_EQ(report.mods.size(), 1u);
    T2D_REQUIRE(report.mods.size() == 1u);
    T2D_CHECK_EQ(report.mods[0].manifest.id, std::string("example_data"));
    T2D_CHECK_EQ(report.mods[0].manifest.name, std::string("Example (data only)"));
    T2D_CHECK_EQ(report.mods[0].manifest.version, std::string("1.0"));
    T2D_CHECK_EQ(report.native_modules, 0u);
    T2D_CHECK_EQ(registry.total_count(), 3u);

    // A directory that is not there is reported, not ignored.
    ModHost other;
    const ModLoadReport& missing = other.load({"/nonexistent/mods"}, registry);
    T2D_CHECK_FALSE(missing.clean());
    T2D_CHECK(mentions(missing.errors, "does not exist"));
}

T2D_TEST(a_manifest_that_is_wrong_is_refused) {
    const struct {
        const char* name;
        const char* manifest;
        const char* expected;
    } cases[] = {
        {"noid", "name:\"no id\"\n", "needs a non empty id"},
        {"badid", "id:\"has space\"\n", "may only use letters"},
        {"unknownkey", "id:\"a\"\ntiers:3\n", "'tiers' is not a manifest key"},
        {"nativeapi", "id:\"a\"\nnative:\"liba.so\"\n", "needs api:1"},
        {"apionly", "id:\"a\"\napi:1\ncontent:[\"c.ecfg\"]\n", nullptr},   // a warning, not an error
        {"badcontent", "id:\"a\"\ncontent:3\n", "content must be a file name"},
    };
    for (const auto& test_case : cases) {
        Scratch scratch;
        scratch.write(std::string(test_case.name) + "/mod.ecfg", test_case.manifest);
        if (std::string(test_case.name) == "apionly") scratch.write("apionly/c.ecfg", "item::\n    x::\n");

        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        if (test_case.expected == nullptr) {
            T2D_CHECK_MSG(report.clean(), "{}: unexpected error '{}'", test_case.name,
                          report.errors.empty() ? "" : report.errors.front());
            T2D_CHECK_EQ(report.mods.size(), 1u);
            continue;
        }
        T2D_CHECK_MSG(!report.clean(), "{}: '{}' was accepted", test_case.name, test_case.manifest);
        T2D_CHECK_MSG(mentions(report.errors, test_case.expected), "{}: errors were {}", test_case.name,
                      report.errors.empty() ? "(none)" : report.errors.front());
        T2D_CHECK_EQ(host.count(), 0u);
        T2D_CHECK_EQ(registry.total_count(), 0u);
    }
}

T2D_TEST(a_missing_or_cyclic_requirement_is_refused) {
    {
        Scratch scratch;
        scratch.write("needy/mod.ecfg", "id:\"needy\"\nrequires:[\"nope\"]\n");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK_FALSE(report.clean());
        T2D_CHECK(mentions(report.errors, "requires 'nope', which is not installed"));
        // It is not loaded: a mod whose requirements are missing must not half run.
        T2D_CHECK_EQ(host.count(), 0u);
    }
    {
        Scratch scratch;
        scratch.write("alpha/mod.ecfg", "id:\"alpha\"\nrequires:[\"beta\"]\n");
        scratch.write("beta/mod.ecfg", "id:\"beta\"\nrequires:[\"alpha\"]\n");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK_FALSE(report.clean());
        T2D_CHECK(mentions(report.errors, "form a cycle"));
        T2D_CHECK_EQ(report.mods.size(), 2u);
        for (const LoadedMod& mod : report.mods) T2D_CHECK_FALSE(mod.ok);
        T2D_CHECK_EQ(host.native_count(), 0u);
    }
    {
        // Two packages claiming the same id: the second is refused, the first still loads.
        Scratch scratch;
        scratch.write("one/mod.ecfg", "id:\"same\"\n");
        scratch.write("two/mod.ecfg", "id:\"same\"\n");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK_FALSE(report.clean());
        T2D_CHECK(mentions(report.errors, "is defined twice"));
        T2D_CHECK_EQ(report.mods.size(), 1u);
    }
}

T2D_TEST(two_mods_that_register_the_same_name_are_reported) {
    Scratch scratch;
    scratch.write("first/mod.ecfg", "id:\"first\"\ncontent:[\"c.ecfg\"]\n");
    scratch.write("first/c.ecfg", "item::\n    shared_thing::\n");
    scratch.write("second/mod.ecfg", "id:\"second\"\ncontent:[\"c.ecfg\"]\n");
    scratch.write("second/c.ecfg", "item::\n    shared_thing::\n    its_own::\n");

    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& report = host.load({scratch.root()}, registry);
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(mentions(report.errors, "item 'shared_thing' is already registered"));
    // The name keeps its first owner, and the rest of the second mod still loads.
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "shared_thing"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "its_own"), 2u);
    T2D_REQUIRE(report.mods.size() == 2u);
    T2D_CHECK_EQ(names_of(report.mods[1]).size(), 1u);
    T2D_CHECK_EQ(report.content_registered, 2u);
}

T2D_TEST(a_broken_content_file_is_reported_with_its_position) {
    Scratch scratch;
    scratch.write("broken/mod.ecfg", "id:\"broken\"\ncontent:[\"content/a.ecfg\"]\n");
    scratch.write("broken/content/a.ecfg", "item::\n    good_one::\n    bad_one:unquoted\n");

    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& report = host.load({scratch.root()}, registry);
    T2D_CHECK_FALSE(report.clean());
    T2D_CHECK(mentions(report.errors, "a.ecfg"));
    T2D_CHECK(mentions(report.errors, ":3:"));   // the line the reader refused
    T2D_REQUIRE(report.mods.size() == 1u);
    T2D_CHECK_FALSE(report.mods[0].ok);
    // Nothing of a broken file is registered: the whole package is parsed before anything is taken.
    T2D_CHECK_EQ(registry.total_count(), 0u);

    // A file that is not there at all is reported the same way.
    Scratch other;
    other.write("gone/mod.ecfg", "id:\"gone\"\ncontent:[\"content/missing.ecfg\"]\n");
    ContentRegistry other_registry;
    ModHost other_host;
    const ModLoadReport& missing = other_host.load({other.root()}, other_registry);
    T2D_CHECK(mentions(missing.errors, "is missing"));
}

T2D_TEST(an_unknown_table_in_a_content_file_is_a_warning) {
    Scratch scratch;
    scratch.write("typo/mod.ecfg", "id:\"typo\"\ncontent:[\"c.ecfg\"]\n");
    scratch.write("typo/c.ecfg", "structurs::\n    wall::\nitem::\n    ore::\n");

    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& report = host.load({scratch.root()}, registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? "" : report.errors.front());
    T2D_CHECK(mentions(report.warnings, "'structurs' is not a content kind"));
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 1u);
    T2D_CHECK_EQ(registry.total_count(), 1u);
}

T2D_TEST(a_native_module_is_checked_before_it_is_called) {
    // Wrong ABI: refused before on_load runs.
    {
        Scratch scratch;
        scratch.write("badapi/mod.ecfg",
                      "id:\"badapi\"\napi:1\nnative:\"libbad.so\"\n");
        scratch.copy_in(T2D_TEST_MOD_BAD_API, "badapi/libbad.so");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK(mentions(report.errors, "built against mod API 99"));
        T2D_CHECK_EQ(host.native_count(), 0u);
    }
    // A module that refuses to run: the host closes it again and keeps nothing.
    {
        Scratch scratch;
        scratch.write("fails/mod.ecfg", "id:\"example_native\"\napi:1\nnative:\"libfails.so\"\n");
        scratch.copy_in(T2D_TEST_MOD_FAILS, "fails/libfails.so");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK(mentions(report.errors, "on_load() returned 7"));
        T2D_CHECK_EQ(host.native_count(), 0u);
    }
    // A library that is not there, and one that is not a library.
    {
        Scratch scratch;
        scratch.write("gone/mod.ecfg", "id:\"gone\"\napi:1\nnative:\"libgone.so\"\n");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK(mentions(report.errors, "native library"));
        T2D_CHECK(mentions(report.errors, "is missing"));
    }
    {
        Scratch scratch;
        scratch.write("text/mod.ecfg", "id:\"text\"\napi:1\nnative:\"libtext.so\"\n");
        scratch.write("text/libtext.so", "this is not a shared library\n");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK(mentions(report.errors, "could not be loaded"));
        T2D_CHECK_EQ(host.native_count(), 0u);
    }
    // A library whose own id does not match the manifest is refused: the manifest is what the host
    // ordered and validated.
    {
        Scratch scratch;
        scratch.write("liar/mod.ecfg", "id:\"liar\"\napi:1\nnative:\"libliar.so\"\n");
        scratch.copy_in(T2D_TEST_MOD_GOOD, "liar/libliar.so");
        ContentRegistry registry;
        ModHost host;
        const ModLoadReport& report = host.load({scratch.root()}, registry);
        T2D_CHECK(mentions(report.errors, "calls itself 'example_native'"));
    }
}

T2D_TEST(reloading_gives_the_same_ids) {
    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& first = host.load({T2D_TEST_MODS_DIR}, registry);
    T2D_REQUIRE(first.clean());
    const ContentId wall = registry.find(ContentKind::Structure, "mod_wall");
    const ContentId drill = registry.find(ContentKind::Machine, "mod_tier_3_drill");
    T2D_CHECK(wall != kNoContent);
    T2D_CHECK(drill != kNoContent);
    const usize entries = registry.total_count();

    // A reload starts from an empty registry, the way the game loads its own content again: the mods
    // are unloaded (on_unload runs, the libraries close) and loaded again, and the ids come out the
    // same because registration order is deterministic.
    registry.clear();
    const ModLoadReport& second = host.load({T2D_TEST_MODS_DIR}, registry);
    T2D_CHECK_MSG(second.clean(), "{}", second.errors.empty() ? "" : second.errors.front());
    T2D_CHECK_EQ(registry.total_count(), entries);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "mod_wall"), wall);
    T2D_CHECK_EQ(registry.find(ContentKind::Machine, "mod_tier_3_drill"), drill);

    // Loading twice *without* clearing is a different thing: every name is taken, and that is
    // reported rather than merged.
    const ModLoadReport& again = host.load({T2D_TEST_MODS_DIR}, registry);
    T2D_CHECK_FALSE(again.clean());
    T2D_CHECK_EQ(again.content_registered, 0u);
    T2D_CHECK_EQ(registry.total_count(), entries);
}

T2D_TEST_MAIN
