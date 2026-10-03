// The path a designer's data takes: .ecfg file -> registry -> the table a save stores -> loading it
// back with a registry that changed. This is the end to end check of the two pieces together.
#include <mine/content_loader.h>
#include <mine/registry.h>

#include <support/test_support.h>

#include <string>

using namespace mine;
using t2d::EcfgDocument;
using t2d::EcfgError;

namespace {

/// A content file in the shape the loader documents: one table per content kind, keys are names.
constexpr const char* kContent = R"(
# The designer's data: fields under a name are theirs to define, the engine only needs the names.
item::
    coal::
        hardness:2
    iron_ore::
        hardness:5
structure::
    cave::
    hard_rock::
machine::
    drill::
        power:10
)";

} // namespace

T2D_TEST(content_names_are_registered_from_a_config_file) {
    ContentRegistry registry;
    EcfgError error;
    const ContentLoadReport report = register_content_from_ecfg_text(registry, kContent, &error);
    if (report.registered == 0 && registry.total_count() == 0) {
        T2D_CHECK_MSG(false, "the content file did not parse: {}", error.describe("content"));
        return;
    }

    T2D_CHECK_EQ(report.registered, 5u);
    T2D_CHECK_EQ(report.already_present, 0u);
    T2D_CHECK_EQ(report.unknown_tables.size(), 0u);
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Structure), 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Machine), 1u);
    T2D_CHECK_EQ(registry.count(ContentKind::Recipe), 0u);

    // Ids follow the order the file lists them in, per kind.
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "coal"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "iron_ore"), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Structure, "cave"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Machine, "drill"), 1u);

    // Loading the same file twice must not move any id.
    const ContentLoadReport second = register_content_from_ecfg_text(registry, kContent);
    T2D_CHECK_EQ(second.registered, 0u);
    T2D_CHECK_EQ(second.already_present, 5u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "iron_ore"), 2u);
}

T2D_TEST(tables_that_are_not_content_kinds_are_reported) {
    ContentRegistry registry;
    const ContentLoadReport report = register_content_from_ecfg_text(registry, R"(
item::
    coal::
itmes::
    typo::
settings::
    volume:3
)");
    T2D_CHECK_EQ(report.registered, 1u);
    T2D_REQUIRE(report.unknown_tables.size() == 2u);
    T2D_CHECK_EQ(report.unknown_tables[0], std::string("itmes"));
    T2D_CHECK_EQ(report.unknown_tables[1], std::string("settings"));
    T2D_CHECK_EQ(registry.total_count(), 1u);
}

T2D_TEST(a_content_file_survives_the_whole_save_path) {
    // 1. The build the save was written with.
    ContentRegistry old_registry;
    (void)register_content_from_ecfg_text(old_registry, kContent);
    const ContentTable saved = old_registry.table();
    const std::vector<t2d::u8> bytes = saved.serialize();

    // 2. A later build: the same file with an entry added at the top of "item", so every item id moves.
    ContentRegistry current;
    (void)register_content_from_ecfg_text(current, R"(
item::
    copper_ore::
    coal::
    iron_ore::
structure::
    cave::
    hard_rock::
machine::
    drill::
)");
    T2D_CHECK_EQ(current.find(ContentKind::Item, "coal"), 2u); // was 1

    // 3. Loading the save: the table travels with it, so the ids resolve by name.
    ContentTable decoded;
    T2D_REQUIRE(ContentTable::deserialize(t2d::ConstSpan<const t2d::u8>(bytes.data(), bytes.size()), decoded));
    const ContentRemap remap = ContentRemap::build(decoded, current);
    T2D_CHECK(remap.complete());

    const t2d::u32 saved_coal = decoded.find_id(ContentKind::Item, "coal");
    T2D_CHECK_EQ(saved_coal, 1u);
    const t2d::u32 resolved = remap.to_current(ContentKind::Item, saved_coal);
    T2D_CHECK_EQ(resolved, current.find(ContentKind::Item, "coal"));
    const ContentEntry* entry = current.find(ContentKind::Item, resolved);
    T2D_REQUIRE(entry != nullptr);
    T2D_CHECK_EQ(entry->name, std::string("coal"));

    // Reading the number as-is would have meant a different resource.
    T2D_REQUIRE(current.find(ContentKind::Item, saved_coal) != nullptr);
    T2D_CHECK_EQ(current.find(ContentKind::Item, saved_coal)->name, std::string("copper_ore"));
}

T2D_TEST(a_broken_content_file_registers_nothing) {
    ContentRegistry registry;
    EcfgError error;
    const ContentLoadReport report = register_content_from_ecfg_text(registry, "item::\n    coal\n", &error);
    T2D_CHECK_EQ(report.registered, 0u);
    T2D_CHECK_EQ(registry.total_count(), 0u);
    T2D_CHECK_EQ(error.line, 2u);
    T2D_CHECK_GT(error.message.size(), 0u);
}

T2D_TEST(a_name_with_a_space_is_a_format_error) {
    // A key cannot contain a space, so this never reaches the registry: the format refuses the file,
    // which is the earliest and clearest place to catch it.
    ContentRegistry registry;
    EcfgError error;
    (void)register_content_from_ecfg_text(registry, "item::\n    bad name::\n", &error);
    T2D_CHECK_EQ(registry.total_count(), 0u);
    T2D_CHECK_EQ(error.line, 2u);
    T2D_CHECK_GT(error.message.size(), 0u);
}

T2D_TEST(a_name_the_registry_cannot_use_is_refused_without_stopping_the_load) {
    // The format allows any key length, the registry does not: one unusable name must not cost the
    // rest of the file, and it must not consume an id either.
    ContentRegistry registry;
    const std::string long_name(200, 'x');
    const ContentLoadReport report =
        register_content_from_ecfg_text(registry, "item::\n    good::\n    " + long_name + "::\n    other::\n");
    T2D_CHECK_EQ(report.registered, 2u);
    T2D_CHECK_EQ(registry.count(ContentKind::Item), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "good"), 1u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, "other"), 2u);
    T2D_CHECK_EQ(registry.find(ContentKind::Item, long_name), kNoContent);
}

T2D_TEST_MAIN
