// Building unlocks: which buildings a save may build - one state per building content, taken from the
// entry's default when a save starts, kept across a reload, and written/read as a save blob whose ids
// are translated through the save's content table by name (docs/GAME_DESIGN.md section 1.44).
#include <mine/building_unlocks.h>

#include <mine/types/tile_definition.h>

#include <support/test_support.h>

#include <t2d/core/bitstream.h>

#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

/// A registry and the definitions that go with it: what a test's content files would have produced.
struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    /// Registers one entry. A building gets a category and a build time, so its `building::` sub-object is
    /// not empty; `building` false is content that is not buildable at all.
    ContentId add(ContentKind kind, std::string_view name, bool default_unlock = true, bool building = true) {
        const ContentId id = registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        if (building) {
            definition.building.category = BuildingCategory::Storage;
            definition.building.seconds = 1.0f;
            // Left unwritten when it is true, so the "the data did not say" case is the default field's.
            if (!default_unlock) definition.building.default_unlock = false;
        }
        definitions.add(std::move(definition));
        return id;
    }
};

/// A blob in the layout serialize() writes, with whatever rows a test needs - including the malformed ones
/// the encoder itself would never produce.
[[nodiscard]] std::vector<u8> blob_with(const std::vector<std::tuple<u8, u32, u8>>& rows,
                                        u32 magic = BuildingUnlocks::kMagic,
                                        u8 version = BuildingUnlocks::kVersion,
                                        const std::vector<u8>& tail = {}) {
    std::vector<u8> bytes;
    t2d::ByteWriter writer(bytes);
    writer.write_u32(magic);
    writer.write_u8(version);
    writer.write_varint(static_cast<u32>(rows.size()));
    for (const auto& [kind, id, unlocked] : rows) {
        writer.write_u8(kind);
        writer.write_varint(id);
        writer.write_u8(unlocked);
    }
    writer.write_bytes(t2d::ConstSpan<const u8>(tail.data(), tail.size()));
    return bytes;
}

} // namespace

T2D_TEST(a_new_save_starts_from_the_defaults_the_data_gave) {
    Fixture content;
    const ContentId shed = content.add(ContentKind::Warehouse, "shed");   // the data does not lock it
    const ContentId rig = content.add(ContentKind::Drill, "rig", /*default_unlock=*/false);
    const ContentId dirt = content.add(ContentKind::Floor, "dirt", false, /*building=*/false);

    BuildingUnlocks unlocks;
    T2D_CHECK_EQ(unlocks.size(), 0u);
    unlocks.reset(content.registry, content.definitions);
    // One row per **building**, in the sidebar's order: kind by kind, registration order inside a kind.
    T2D_REQUIRE(unlocks.size() == 2u);
    T2D_CHECK(unlocks.entries()[0].kind == ContentKind::Drill);
    T2D_CHECK_EQ(unlocks.entries()[0].id, rig);
    T2D_CHECK_FALSE(unlocks.entries()[0].unlocked);
    T2D_CHECK(unlocks.entries()[1].kind == ContentKind::Warehouse);
    T2D_CHECK_EQ(unlocks.entries()[1].id, shed);
    T2D_CHECK(unlocks.entries()[1].unlocked);

    // What the table says and what a caller asks are the same answer, and content that is not a building -
    // or was never registered - is locked rather than guessed at.
    T2D_CHECK(unlocks.unlocked(ContentKind::Warehouse, shed));
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Drill, rig));
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Floor, dirt));
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, 99));
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Item, 1));
}

T2D_TEST(one_state_is_shared_by_every_question_and_a_stranger_cannot_change_it) {
    Fixture content;
    const ContentId shed = content.add(ContentKind::Warehouse, "shed");
    BuildingUnlocks unlocks;
    unlocks.reset(content.registry, content.definitions);

    // One row, one answer: what set_unlocked() writes is what unlocked() and entries() read. The state
    // belongs to the content entry, so every plot of it - and every question about it - sees the same one.
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, shed, false));
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, shed));
    T2D_CHECK_FALSE(unlocks.entries()[0].unlocked);
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, shed, true));
    T2D_CHECK(unlocks.unlocked(ContentKind::Warehouse, shed));

    // Content that is not in the table is refused, and nothing changes.
    T2D_CHECK_FALSE(unlocks.set_unlocked(ContentKind::Warehouse, 99, true));
    T2D_CHECK_FALSE(unlocks.set_unlocked(ContentKind::Item, 1, true));
    T2D_CHECK_FALSE(unlocks.set_unlocked(ContentKind::Warehouse, kNoContent, true));
    T2D_CHECK_EQ(unlocks.size(), 1u);
    T2D_CHECK(unlocks.unlocked(ContentKind::Warehouse, shed));
}

T2D_TEST(a_reload_keeps_what_is_still_there_and_starts_the_rest_fresh) {
    Fixture before;
    const ContentId a = before.add(ContentKind::Warehouse, "a");
    const ContentId b = before.add(ContentKind::Warehouse, "b", false);
    BuildingUnlocks unlocks;
    unlocks.reset(before.registry, before.definitions);
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, a, false));
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, b, true));

    // The same names in the same order, plus one building this load added: the ids of what was there did
    // not move, so the kept states stay, and the new entry starts where its definition says.
    Fixture after;
    T2D_CHECK_EQ(after.add(ContentKind::Warehouse, "a"), a);
    T2D_CHECK_EQ(after.add(ContentKind::Warehouse, "b", false), b);
    const ContentId fresh = after.add(ContentKind::Warehouse, "c", false);
    unlocks.refresh(after.registry, after.definitions);
    T2D_REQUIRE(unlocks.size() == 3u);
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, a));       // kept: locked by the player
    T2D_CHECK(unlocks.unlocked(ContentKind::Warehouse, b));             // kept: unlocked by the player
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, fresh));   // new: the data's default

    // A building the reload dropped has no row any more. refresh() matches by id, so this is the only
    // reload it is for: one that kept the ids of everything that stayed - a renumbering reload goes
    // through the save blob and a ContentRemap instead (app.cpp, and the test below).
    Fixture smaller;
    T2D_CHECK_EQ(smaller.add(ContentKind::Warehouse, "a"), a);
    unlocks.refresh(smaller.registry, smaller.definitions);
    T2D_CHECK_EQ(unlocks.size(), 1u);
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, a));
    T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, b));   // gone from this content
}

T2D_TEST(a_save_blob_round_trips_through_the_same_registry) {
    Fixture content;
    const ContentId shed = content.add(ContentKind::Warehouse, "shed");
    const ContentId rig = content.add(ContentKind::Drill, "rig", false);
    BuildingUnlocks unlocks;
    unlocks.reset(content.registry, content.definitions);
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, shed, false));
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Drill, rig, true));
    const std::vector<u8> blob = unlocks.serialize();
    T2D_CHECK(!blob.empty());

    // A new save's defaults first, then the blob over them: both values come back, because a locked row
    // is in the blob rather than implied by its absence.
    BuildingUnlocks loaded;
    loaded.reset(content.registry, content.definitions);
    T2D_CHECK(loaded.unlocked(ContentKind::Warehouse, shed));   // the default, before the blob
    T2D_CHECK_FALSE(loaded.unlocked(ContentKind::Drill, rig));
    const ContentRemap remap =
        ContentRemap::build(ContentTable::from_registry(content.registry), content.registry);
    T2D_CHECK(loaded.apply_saved(blob, remap));
    T2D_CHECK_EQ(loaded.size(), 2u);
    T2D_CHECK_FALSE(loaded.unlocked(ContentKind::Warehouse, shed));
    T2D_CHECK(loaded.unlocked(ContentKind::Drill, rig));
}

T2D_TEST(a_save_blob_follows_its_names_into_another_registry) {
    // The save-time content: "bin" unlocked, "shed" locked, "old" unlocked and gone from the current build.
    Fixture saved;
    const ContentId saved_bin = saved.add(ContentKind::Warehouse, "bin");    // saved id 1
    const ContentId saved_shed = saved.add(ContentKind::Warehouse, "shed");  // saved id 2
    const ContentId saved_old = saved.add(ContentKind::Warehouse, "old");    // saved id 3
    BuildingUnlocks unlocks;
    unlocks.reset(saved.registry, saved.definitions);
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, saved_bin, true));
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, saved_shed, false));
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, saved_old, true));
    const std::vector<u8> blob = unlocks.serialize();
    const ContentTable table = ContentTable::from_registry(saved.registry);

    // The current build registers the same names in **another order**, drops "old" and adds "crate" - so
    // every id moved. Only names can be trusted: that is what the ContentRemap is for.
    Fixture current;
    const ContentId current_shed = current.add(ContentKind::Warehouse, "shed");   // id 1, saved id 2
    const ContentId current_bin = current.add(ContentKind::Warehouse, "bin");     // id 2, saved id 1
    const ContentId current_crate = current.add(ContentKind::Warehouse, "crate", false);   // id 3, new
    T2D_CHECK_NE(current_bin, saved_bin);

    BuildingUnlocks loaded;
    loaded.reset(current.registry, current.definitions);
    T2D_REQUIRE(loaded.apply_saved(blob, ContentRemap::build(table, current.registry)));
    // By name: "bin" is unlocked and "shed" is locked, whichever number each has now. "crate" took the id
    // "old" had and is not unlocked by the save's row for a building that no longer exists.
    T2D_CHECK(loaded.unlocked(ContentKind::Warehouse, current_bin));
    T2D_CHECK_FALSE(loaded.unlocked(ContentKind::Warehouse, current_shed));
    T2D_CHECK_FALSE(loaded.unlocked(ContentKind::Warehouse, current_crate));
    T2D_CHECK_EQ(loaded.size(), 3u);
}

T2D_TEST(a_blob_does_not_reach_content_that_is_not_a_building_now) {
    Fixture saved;
    const ContentId shed = saved.add(ContentKind::Warehouse, "shed");
    BuildingUnlocks unlocks;
    unlocks.reset(saved.registry, saved.definitions);
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, shed, true));
    const ContentTable table = ContentTable::from_registry(saved.registry);

    // The current build has the same name, but it is not a building any more: the row is skipped, because
    // a thing that cannot be built cannot be unlocked either.
    Fixture current;
    (void)current.add(ContentKind::Warehouse, "shed", true, /*building=*/false);
    BuildingUnlocks loaded;
    loaded.reset(current.registry, current.definitions);
    T2D_CHECK_EQ(loaded.size(), 0u);
    T2D_CHECK(loaded.apply_saved(unlocks.serialize(), ContentRemap::build(table, current.registry)));
    T2D_CHECK_EQ(loaded.size(), 0u);
    T2D_CHECK_FALSE(loaded.unlocked(ContentKind::Warehouse, shed));
}

T2D_TEST(a_save_blob_for_an_empty_table_round_trips) {
    const ContentRegistry empty_registry;
    const ContentDefinitions empty_definitions;
    BuildingUnlocks none;
    none.reset(empty_registry, empty_definitions);
    T2D_CHECK_EQ(none.size(), 0u);
    const std::vector<u8> blob = none.serialize();
    T2D_CHECK(!empty_registry.total_count() && !blob.empty());
    const ContentRemap empty_remap =
        ContentRemap::build(ContentTable::from_registry(empty_registry), empty_registry);
    BuildingUnlocks loaded;
    T2D_CHECK(loaded.apply_saved(blob, empty_remap));
    T2D_CHECK_EQ(loaded.size(), 0u);

    // An empty blob over content that has defaults leaves every building at its default.
    Fixture content;
    const ContentId shed = content.add(ContentKind::Warehouse, "shed", false);
    BuildingUnlocks defaults;
    defaults.reset(content.registry, content.definitions);
    T2D_CHECK(defaults.apply_saved(blob, ContentRemap::build(ContentTable::from_registry(empty_registry),
                                                              content.registry)));
    T2D_CHECK_FALSE(defaults.unlocked(ContentKind::Warehouse, shed));
}

T2D_TEST(a_blob_that_cannot_be_read_changes_nothing) {
    Fixture content;
    const ContentId shed = content.add(ContentKind::Warehouse, "shed");
    const ContentId rig = content.add(ContentKind::Drill, "rig");
    const ContentRemap remap =
        ContentRemap::build(ContentTable::from_registry(content.registry), content.registry);
    BuildingUnlocks unlocks;
    unlocks.reset(content.registry, content.definitions);
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Warehouse, shed, false));
    T2D_CHECK(unlocks.set_unlocked(ContentKind::Drill, rig, true));

    const auto warehouse = static_cast<u8>(ContentKind::Warehouse);
    const std::vector<std::vector<u8>> malformed{
        blob_with({{warehouse, shed, 1u}}, /*magic=*/0x12345678u),
        blob_with({{warehouse, shed, 1u}}, BuildingUnlocks::kMagic, /*version=*/2u),
        blob_with({{warehouse, kNoContent, 1u}}),
        blob_with({{warehouse, kMaxContentId, 1u}}),
        blob_with({{warehouse, shed, 2u}}),                              // not a bool
        blob_with({{static_cast<u8>(kContentKindCount), shed, 1u}}),     // no such kind
        blob_with({{warehouse, shed, 1u}, {warehouse, shed, 0u}}),       // the same building twice
        blob_with({{warehouse, shed, 1u}}, BuildingUnlocks::kMagic, BuildingUnlocks::kVersion, {7u}),
    };
    for (const std::vector<u8>& bad : malformed) {
        T2D_CHECK_FALSE(unlocks.apply_saved(bad, remap));
        // Nothing changed: the two states set above are still what the table says.
        T2D_CHECK_EQ(unlocks.size(), 2u);
        T2D_CHECK_FALSE(unlocks.unlocked(ContentKind::Warehouse, shed));
        T2D_CHECK(unlocks.unlocked(ContentKind::Drill, rig));
    }

    // A truncated stream is refused, and so is a count that claims rows the bytes do not hold.
    const std::vector<u8> good = unlocks.serialize();
    const std::vector<u8> truncated(good.begin(), good.end() - 1);
    T2D_CHECK_FALSE(unlocks.apply_saved(truncated, remap));
    const std::vector<u8> none;
    T2D_CHECK_FALSE(unlocks.apply_saved(none, remap));
    std::vector<u8> lying;
    t2d::ByteWriter liar(lying);
    liar.write_u32(BuildingUnlocks::kMagic);
    liar.write_u8(BuildingUnlocks::kVersion);
    liar.write_varint(3u);
    T2D_CHECK_FALSE(unlocks.apply_saved(lying, remap));
    // The good blob is accepted, which is what makes the refusals above about the bytes and not the table.
    T2D_CHECK(unlocks.apply_saved(good, remap));
    T2D_CHECK_EQ(unlocks.size(), 2u);
}

T2D_TEST_MAIN
