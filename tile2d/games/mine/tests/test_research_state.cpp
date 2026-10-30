// Which research nodes a save has studied: one row per node, the RST1 blob, the name remap a load
// needs, and the refusals that leave the table alone (mine/research_state.h, docs/GAME_DESIGN.md
// section 1.50).
#include <mine/research_state.h>

#include <mine/types/tile_definition.h>

#include <support/test_support.h>

#include <t2d/core/bitstream.h>

#include <string>
#include <utility>
#include <vector>

using namespace mine;
using namespace mine::types;

namespace {

struct Fixture {
    ContentRegistry registry;
    ContentDefinitions definitions;

    ContentId add_node(ContentKind kind, std::string_view name) {
        const ContentId id = registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        Research research;
        research.is_root = true;
        research.tree = "intro";
        definition.research = std::move(research);
        definitions.add(std::move(definition));
        return id;
    }

    void add_plain(ContentKind kind, std::string_view name) {
        (void)registry.register_content(kind, name);
        TileDefinition definition;
        definition.kind = kind;
        definition.name = std::string(name);
        definitions.add(std::move(definition));
    }

    /// The identity translation for applying a blob written from this same registry.
    [[nodiscard]] ContentRemap identity_remap() const {
        return ContentRemap::build(ContentTable::from_registry(registry), registry);
    }
};

[[nodiscard]] std::vector<u8> one_row(u32 magic, u8 version, u8 kind, ContentId id, u8 researched,
                                      bool trailing) {
    std::vector<u8> bytes;
    t2d::ByteWriter writer(bytes);
    writer.write_u32(magic);
    writer.write_u8(version);
    writer.write_varint(1u);
    writer.write_u8(kind);
    writer.write_varint(id);
    writer.write_u8(researched);
    (void)writer.valid();
    if (trailing) bytes.push_back(0u);
    return bytes;
}

} // namespace

T2D_TEST(a_new_save_has_one_row_per_research_node) {
    Fixture fixture;
    (void)fixture.add_node(ContentKind::Warehouse, "stone");
    (void)fixture.add_node(ContentKind::Warehouse, "workshop");
    fixture.add_plain(ContentKind::Warehouse, "plain");
    (void)fixture.registry.register_content(ContentKind::Drill, "no_definition");

    ResearchState state;
    state.reset(fixture.registry, fixture.definitions);
    T2D_REQUIRE(state.size() == 2u);
    T2D_CHECK_EQ(state.entries()[0].kind, ContentKind::Warehouse);
    T2D_CHECK_FALSE(state.researched(ContentKind::Warehouse,
                                     fixture.registry.find(ContentKind::Warehouse, "stone")));
    // Nothing a node is not can be studied.
    T2D_CHECK_FALSE(state.researched(ContentKind::Warehouse, fixture.registry.find(ContentKind::Warehouse,
                                                                                   "plain")));
    T2D_CHECK_FALSE(state.set_researched(ContentKind::Warehouse,
                                         fixture.registry.find(ContentKind::Warehouse, "plain"), true));
}

T2D_TEST(researching_a_node_survives_a_reload_that_adds_another) {
    Fixture fixture;
    const ContentId stone = fixture.add_node(ContentKind::Warehouse, "stone");
    (void)fixture.add_node(ContentKind::Warehouse, "workshop");
    ResearchState state;
    state.reset(fixture.registry, fixture.definitions);
    T2D_CHECK(state.set_researched(ContentKind::Warehouse, stone, true));
    T2D_CHECK(state.researched(ContentKind::Warehouse, stone));

    (void)fixture.add_node(ContentKind::Warehouse, "forge");
    state.refresh(fixture.registry, fixture.definitions);
    T2D_REQUIRE(state.size() == 3u);
    T2D_CHECK(state.researched(ContentKind::Warehouse, stone));
    T2D_CHECK_FALSE(state.researched(ContentKind::Warehouse,
                                     fixture.registry.find(ContentKind::Warehouse, "forge")));
}

T2D_TEST(the_blob_round_trips_through_a_remap_by_name) {
    Fixture first;
    const ContentId stone = first.add_node(ContentKind::Warehouse, "stone");
    (void)first.add_node(ContentKind::Warehouse, "workshop");
    ResearchState saved;
    saved.reset(first.registry, first.definitions);
    T2D_CHECK(saved.set_researched(ContentKind::Warehouse, stone, true));
    const std::vector<u8> bytes = saved.serialize();
    T2D_CHECK(bytes.size() >= 6u);

    // A build where the ids moved: another warehouse is registered first, so "stone" is no longer id 1.
    Fixture second;
    second.add_plain(ContentKind::Warehouse, "aaa");
    (void)second.add_node(ContentKind::Warehouse, "stone");
    (void)second.add_node(ContentKind::Warehouse, "workshop");
    ResearchState loaded;
    loaded.reset(second.registry, second.definitions);
    T2D_CHECK_FALSE(loaded.researched(ContentKind::Warehouse,
                                      second.registry.find(ContentKind::Warehouse, "stone")));
    const ContentRemap remap =
        ContentRemap::build(ContentTable::from_registry(first.registry), second.registry);
    T2D_CHECK(loaded.apply_saved(t2d::ConstSpan<const u8>(bytes.data(), bytes.size()), remap));
    T2D_CHECK(loaded.researched(ContentKind::Warehouse,
                                second.registry.find(ContentKind::Warehouse, "stone")));
    T2D_CHECK_FALSE(loaded.researched(ContentKind::Warehouse,
                                      second.registry.find(ContentKind::Warehouse, "workshop")));
}

T2D_TEST(a_node_this_build_no_longer_has_is_skipped) {
    Fixture first;
    const ContentId stone = first.add_node(ContentKind::Warehouse, "stone");
    (void)first.add_node(ContentKind::Warehouse, "workshop");
    ResearchState saved;
    saved.reset(first.registry, first.definitions);
    T2D_CHECK(saved.set_researched(ContentKind::Warehouse, stone, true));
    const std::vector<u8> bytes = saved.serialize();

    Fixture second;
    (void)second.add_node(ContentKind::Warehouse, "stone");
    ResearchState loaded;
    loaded.reset(second.registry, second.definitions);
    const ContentRemap remap =
        ContentRemap::build(ContentTable::from_registry(first.registry), second.registry);
    T2D_CHECK_FALSE(remap.complete()); // "workshop" is gone from this build
    T2D_CHECK(loaded.apply_saved(t2d::ConstSpan<const u8>(bytes.data(), bytes.size()), remap));
    T2D_CHECK(loaded.researched(ContentKind::Warehouse,
                                second.registry.find(ContentKind::Warehouse, "stone")));
}

T2D_TEST(a_blob_that_cannot_be_read_changes_nothing) {
    Fixture fixture;
    const ContentId stone = fixture.add_node(ContentKind::Warehouse, "stone");
    ResearchState state;
    state.reset(fixture.registry, fixture.definitions);
    T2D_CHECK(state.set_researched(ContentKind::Warehouse, stone, true));
    const ContentRemap remap = fixture.identity_remap();
    const auto refuses = [&](const std::vector<u8>& bytes) {
        const bool applied = state.apply_saved(t2d::ConstSpan<const u8>(bytes.data(), bytes.size()), remap);
        T2D_CHECK_FALSE(applied);
        T2D_CHECK(state.researched(ContentKind::Warehouse, stone)); // the table is untouched
    };

    const auto valid = [&] {
        return one_row(ResearchState::kMagic, ResearchState::kVersion,
                       static_cast<u8>(ContentKind::Warehouse), stone, 1u, false);
    };

    // Wrong magic and wrong version.
    refuses(one_row(0x12345678u, ResearchState::kVersion, static_cast<u8>(ContentKind::Warehouse), stone,
                    1u, false));
    refuses(one_row(ResearchState::kMagic, static_cast<u8>(ResearchState::kVersion + 1),
                    static_cast<u8>(ContentKind::Warehouse), stone, 1u, false));
    // A kind out of range, id zero, a bool that is neither 0 nor 1.
    refuses(one_row(ResearchState::kMagic, ResearchState::kVersion, 200u, stone, 1u, false));
    refuses(one_row(ResearchState::kMagic, ResearchState::kVersion, static_cast<u8>(ContentKind::Warehouse),
                    kNoContent, 1u, false));
    refuses(one_row(ResearchState::kMagic, ResearchState::kVersion, static_cast<u8>(ContentKind::Warehouse),
                    stone, 2u, false));
    // Truncated and with trailing bytes.
    std::vector<u8> cut = valid();
    cut.resize(cut.size() - 1);
    refuses(cut);
    refuses(one_row(ResearchState::kMagic, ResearchState::kVersion, static_cast<u8>(ContentKind::Warehouse),
                    stone, 1u, true));

    // The same node twice.
    std::vector<u8> doubled;
    t2d::ByteWriter writer(doubled);
    writer.write_u32(ResearchState::kMagic);
    writer.write_u8(ResearchState::kVersion);
    writer.write_varint(2u);
    for (int row = 0; row < 2; ++row) {
        writer.write_u8(static_cast<u8>(ContentKind::Warehouse));
        writer.write_varint(stone);
        writer.write_bool(row == 0);
    }
    refuses(doubled);
}

T2D_TEST_MAIN
