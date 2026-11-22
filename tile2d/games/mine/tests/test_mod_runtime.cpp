// The runtime-type mod surface: a real shared library registers a component and a tile type, overrides
// an original interface, and the game builds an object out of it. The fixture is loaded by ModHost; no
// part of it is linked into this test.
#include <mine/content_loader.h>
#include <mine/content_pack.h>
#include <mine/game_save.h>
#include <mine/mod_package.h>
#include <mine/runtime/content/content_ids.h>
#include <mine/runtime/runtime_context.h>
#include <mine/types/tile.h>
#include <mine/types/tile_definition.h>
#include <mine/world.h>

#include <support/test_support.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace mine;
using mine::types::TileDefinition;

namespace {

namespace fs = std::filesystem;

static_assert(!std::is_polymorphic_v<runtime::RuntimeContext>);
static_assert(!std::is_polymorphic_v<runtime::ComponentType>);
static_assert(!std::is_polymorphic_v<runtime::TileType>);

constexpr runtime::ComponentTypeId kMarkerComponent = runtime::content::hash_name("example_runtime.marker");
constexpr runtime::TileTypeId kMarkerTileType = runtime::content::hash_name("example_runtime.marker_tile");
constexpr u32 kMarkerMagic = 0x4d41524bu;

class Scratch {
public:
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / std::format("t2d_runtime_mods_{}", stamp);
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

[[nodiscard]] const TileDefinition* find_definition(const ContentDefinitions& definitions,
                                                    std::string_view name) {
    return definitions.find_tile(ContentKind::Structure, name);
}

[[nodiscard]] bool mentions(const std::vector<std::string>& lines, std::string_view needle) {
    for (const std::string& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// One manifest pointing at one variant library, with no content: the API refusal is the only thing the
/// module is being tested for.
[[nodiscard]] bool load_variant(const char* library, const std::vector<std::string>& expected) {
    Scratch scratch;
    scratch.write("variant/mod.ecfg",
                  std::format("id:\"example_runtime\"\napi:2\nnative:\"libvariant.so\"\n"));
    scratch.copy_in(library, "variant/libvariant.so");

    auto context = std::make_shared<runtime::RuntimeContext>();
    ContentRegistry registry;
    ModHost host;
    const ModLoadReport& report = host.load({scratch.root()}, registry, *context);
    if (host.native_count() != 1) return false;
    for (const std::string& needle : expected) {
        if (!mentions(report.errors, needle)) return false;
    }
    return true;
}

} // namespace

T2D_TEST(a_module_registers_a_component_and_a_tile_type_that_the_game_builds) {
    auto context = std::make_shared<runtime::RuntimeContext>();

    // An object built *before* the override exists: it must see the new active table later, because
    // every interface query goes through the ComponentType rather than a cached pointer.
    ContentDefinitions before_definitions;
    before_definitions.set_runtime_context(context);
    TileDefinition floor;
    floor.kind = ContentKind::Floor;
    floor.name = "dirt";
    floor.image = "art/floor.png";
    ContentRegistry registry;
    const std::unique_ptr<types::Tile> before =
        types::make_tile(before_definitions, floor, registry, 1, 0, GridPos{0, 0});
    T2D_REQUIRE(before != nullptr);
    T2D_CHECK_EQ(before->get_preview().images.size(), 1u);
    T2D_REQUIRE(before->tile_type() != nullptr);
    T2D_CHECK_EQ(before->tile_type()->name(), std::string_view("floor"));

    ModHost host;
    const ModLoadReport& report = host.load({T2D_TEST_RUNTIME_MODS_DIR}, registry, *context);
    T2D_REQUIRE(report.mods.size() == 1u);
    T2D_CHECK_MSG(report.clean(), "{}", report.errors.empty() ? std::string{} : report.errors.front());
    T2D_CHECK(report.mods.front().native_loaded);

    // Lookups answer through the shared registries, not through a per-content copy.
    T2D_CHECK(context->components().find(kMarkerComponent) != nullptr);
    T2D_CHECK(context->components().find(kMarkerComponent) ==
              context->components().find_by_name("example:marker"));
    T2D_CHECK(context->tile_types().find(kMarkerTileType) != nullptr);
    T2D_CHECK(context->tile_types().find(kMarkerTileType) ==
              context->tile_types().find_by_name("example:marker"));
    T2D_CHECK(host.api()->find_component != nullptr);
    T2D_CHECK(host.api()->find_tile_type != nullptr);

    // The override is visible to the object that already existed, and it calls the previous table: the
    // base picture is still first, the module's marker part is second.
    const types::TilePreview after_override = before->get_preview();
    T2D_REQUIRE(after_override.images.size() == 2u);
    T2D_CHECK_NEAR(after_override.images[1].radians, 1.25f, 1e-5f);

    // Build the mod's own content: one custom data component plus the built-in preview.
    ContentDefinitions definitions;
    definitions.set_runtime_context(context);
    for (const TileDefinition& definition : report.mods.front().definitions)
        definitions.add(definition);
    const TileDefinition* marker = find_definition(definitions, "mod_marker");
    T2D_REQUIRE(marker != nullptr);
    T2D_CHECK_EQ(marker->tile_type, std::string("example:marker"));
    const ContentId marker_id = registry.find(ContentKind::Structure, "mod_marker");
    T2D_REQUIRE(marker_id != kNoContent);
    const std::unique_ptr<types::Tile> tile =
        types::make_tile(definitions, *marker, registry, marker_id, 0, GridPos{3, 4});
    T2D_REQUIRE(tile != nullptr);
    T2D_REQUIRE(tile->tile_type() != nullptr);
    T2D_CHECK_EQ(tile->tile_type()->name(), std::string_view("example:marker"));
    T2D_CHECK(tile->has_component(kMarkerComponent));
    const auto* marker_shared = static_cast<const u32*>(tile->shared(kMarkerComponent));
    T2D_REQUIRE(marker_shared != nullptr);
    T2D_CHECK_EQ(marker_shared[0], 31u);
    const auto* marker_data = static_cast<const u32*>(tile->instance(kMarkerComponent));
    T2D_REQUIRE(marker_data != nullptr);
    T2D_CHECK_EQ(marker_data[0], kMarkerMagic);
    T2D_CHECK_EQ(marker_data[1], 0u); // zero initialised, then construct() set the magic
    T2D_CHECK(tile->get_preview().images.size() == 1u);
    T2D_CHECK_NEAR(tile->get_preview().images[0].radians, 1.25f, 1e-5f);

    // Every instance of one tile type gets the same shared record.
    const std::unique_ptr<types::Tile> second =
        types::make_tile(definitions, *marker, registry, marker_id, 0, GridPos{4, 4});
    T2D_REQUIRE(second != nullptr);
    T2D_CHECK(second->shared(kMarkerComponent) == tile->shared(kMarkerComponent));

    // The same component with another tile type gets another shared record.
    const TileDefinition* marker_alt = find_definition(definitions, "mod_marker_alt");
    T2D_REQUIRE(marker_alt != nullptr);
    const ContentId marker_alt_id = registry.find(ContentKind::Structure, "mod_marker_alt");
    T2D_REQUIRE(marker_alt_id != kNoContent);
    const std::unique_ptr<types::Tile> alt =
        types::make_tile(definitions, *marker_alt, registry, marker_alt_id, 0, GridPos{5, 5});
    T2D_REQUIRE(alt != nullptr);
    const auto* alt_shared = static_cast<const u32*>(alt->shared(kMarkerComponent));
    T2D_REQUIRE(alt_shared != nullptr);
    T2D_CHECK_EQ(alt_shared[0], 47u);

    // The layer places and constructs it through the same path a map build takes.
    auto layer = std::make_unique<MineLayer>(0, LayerShape{8, 8, 1}, 1);
    LayerSpec spec;
    spec.shape = LayerShape{8, 8, 1};
    PlacedContent where;
    where.kind = ContentKind::Structure;
    where.name = "mod_marker";
    where.layer = 0;
    where.anchor = GridPos{2, 2};
    spec.content.push_back(where);
    LayerBuildReport build_report;
    layer->build(spec, registry, definitions, &build_report);
    T2D_CHECK_MSG(build_report.clean(), "{}",
                  build_report.errors.empty() ? std::string{} : build_report.errors.front());
    T2D_REQUIRE(layer->plot_count() == 1u);
    const types::Tile* placed = layer->plot_at(0, GridPos{2, 2});
    T2D_REQUIRE(placed != nullptr);
    T2D_CHECK(placed->has_component(kMarkerComponent));
    T2D_CHECK_EQ(placed->get_preview().images.size(), 1u);

    // Custom component state goes through the optional CMPT chunk; the PLOT chunk is untouched.
    auto* placed_data = static_cast<u32*>(const_cast<void*>(placed->instance(kMarkerComponent)));
    T2D_REQUIRE(placed_data != nullptr);
    placed_data[1] = 7u;
    MineWorld world(1, LayerShape{8, 8, 1});
    world.install_layer(std::move(layer), build_report);
    const SessionConfig session;
    const t2d::save::File saved =
        make_game_save(world, session, BuildingUnlocks{}, ResearchState{}, BuildAnimation{}, 0.0f,
                       t2d::Vec2{0.0f, 0.0f}, 1.0f, registry);
    const t2d::save::Chunk* components = saved.find(game_save_chunk::kComponents);
    T2D_REQUIRE(components != nullptr);
    T2D_CHECK_EQ(components->version, kGameSaveChunkVersion);

    const auto loaded = load_game_save(saved, registry, definitions);
    T2D_REQUIRE(loaded.has_value());
    const types::Tile* loaded_plot = loaded->world->layer().plot_at(0, GridPos{2, 2});
    T2D_REQUIRE(loaded_plot != nullptr);
    T2D_REQUIRE(loaded_plot->has_component(kMarkerComponent));
    const auto* loaded_data = static_cast<const u32*>(loaded_plot->instance(kMarkerComponent));
    T2D_REQUIRE(loaded_data != nullptr);
    T2D_CHECK_EQ(loaded_data[1], 7u);
    const auto* loaded_shared = static_cast<const u32*>(loaded_plot->shared(kMarkerComponent));
    T2D_REQUIRE(loaded_shared != nullptr);
    T2D_CHECK_EQ(loaded_shared[0], 31u);

    // A save written before the chunk existed is still loadable; its component starts at its own default.
    t2d::save::File legacy = saved;
    legacy.chunks.erase(
        std::remove_if(legacy.chunks.begin(), legacy.chunks.end(),
                       [](const t2d::save::Chunk& chunk) {
                           return chunk.id == game_save_chunk::kComponents;
                       }),
        legacy.chunks.end());
    T2D_REQUIRE(legacy.find(game_save_chunk::kComponents) == nullptr);
    const auto legacy_loaded = load_game_save(legacy, registry, definitions);
    T2D_REQUIRE(legacy_loaded.has_value());
    const types::Tile* legacy_plot = legacy_loaded->world->layer().plot_at(0, GridPos{2, 2});
    T2D_REQUIRE(legacy_plot != nullptr);
    const auto* legacy_data = static_cast<const u32*>(legacy_plot->instance(kMarkerComponent));
    T2D_REQUIRE(legacy_data != nullptr);
    T2D_CHECK_EQ(legacy_data[1], 0u);

    // A CMPT chunk at another version is refused as a whole, like every other chunk version.
    t2d::save::File bad_version = saved;
    for (t2d::save::Chunk& chunk : bad_version.chunks) {
        if (chunk.id == game_save_chunk::kComponents) chunk.version = 2;
    }
    GameLoadReport bad_report;
    T2D_CHECK_FALSE(load_game_save(bad_version, registry, definitions, &bad_report).has_value());
    T2D_CHECK(bad_report.error.find("CMPT") != std::string::npos);

    // A record written against another schema version is refused, and so is one that tries to take
    // over a built-in PLOT field.
    t2d::save::File bad_component_version = saved;
    for (t2d::save::Chunk& chunk : bad_component_version.chunks) {
        if (chunk.id != game_save_chunk::kComponents) continue;
        const t2d::u32 version = 99;
        std::memcpy(chunk.payload.data() + 16u, &version, sizeof(version));
    }
    GameLoadReport version_report;
    T2D_CHECK_FALSE(
        load_game_save(bad_component_version, registry, definitions, &version_report).has_value());
    T2D_CHECK(version_report.error.find("version") != std::string::npos);

    t2d::save::File missing_component = saved;
    for (t2d::save::Chunk& chunk : missing_component.chunks) {
        if (chunk.id != game_save_chunk::kComponents) continue;
        const t2d::u64 type_id = 0xDEADBEEFDEADBEEFull;
        std::memcpy(chunk.payload.data() + 8u, &type_id, sizeof(type_id));
    }
    GameLoadReport missing_report;
    T2D_CHECK_FALSE(load_game_save(missing_component, registry, definitions, &missing_report).has_value());
    T2D_CHECK(missing_report.error.find("does not have") != std::string::npos);

    t2d::save::File builtin_component = saved;
    for (t2d::save::Chunk& chunk : builtin_component.chunks) {
        if (chunk.id != game_save_chunk::kComponents) continue;
        const t2d::u64 type_id = runtime::content::kPreviewComponent;
        std::memcpy(chunk.payload.data() + 8u, &type_id, sizeof(type_id));
    }
    GameLoadReport builtin_report;
    T2D_CHECK_FALSE(load_game_save(builtin_component, registry, definitions, &builtin_report).has_value());
    T2D_CHECK(builtin_report.error.find("built-in") != std::string::npos);

    // A reload refreshes the tables in place. The old objects keep the same ComponentType and TileType
    // pointers; the new module's functions are what those pointers now answer with.
    const runtime::ComponentType* component_type = context->components().find(kMarkerComponent);
    const runtime::TileType* tile_type = context->tile_types().find(kMarkerTileType);
    T2D_REQUIRE(component_type != nullptr && tile_type != nullptr);

    // Unloading restores the active table first: an object built before the override is back on the
    // game's preview function, and no pointer into the module is left to be called.
    host.unload();
    T2D_CHECK_EQ(before->get_preview().images.size(), 1u);
    T2D_CHECK(tile->get_preview().images.empty());

    registry.clear();
    const ModLoadReport& reloaded = host.load({T2D_TEST_RUNTIME_MODS_DIR}, registry, *context);
    T2D_CHECK_MSG(reloaded.clean(), "{}", reloaded.errors.empty() ? std::string{} : reloaded.errors.front());
    T2D_CHECK_EQ(context->components().find(kMarkerComponent), component_type);
    T2D_CHECK_EQ(context->tile_types().find(kMarkerTileType), tile_type);
    T2D_CHECK_EQ(before->get_preview().images.size(), 2u);
    T2D_CHECK_NEAR(before->get_preview().images[1].radians, 1.25f, 1e-5f);
    T2D_CHECK_EQ(tile->get_preview().images.size(), 1u);
    T2D_CHECK_EQ(tile->get_preview().images[0].radians, 1.25f);
    const auto* reloaded_data = static_cast<const u32*>(tile->instance(kMarkerComponent));
    T2D_REQUIRE(reloaded_data != nullptr);
    T2D_CHECK_EQ(reloaded_data[0], kMarkerMagic);
    const auto* reloaded_shared = static_cast<const u32*>(tile->shared(kMarkerComponent));
    T2D_REQUIRE(reloaded_shared != nullptr);
    T2D_CHECK_EQ(reloaded_shared[0], 31u);

    // A library built after the reload takes the refreshed tile type's shared pointer. This is the
    // path the game takes: a new content load builds new runtime definitions, not the old objects'.
    ContentDefinitions after_reload;
    after_reload.set_runtime_context(context);
    for (const TileDefinition& definition : reloaded.mods.front().definitions) after_reload.add(definition);
    const TileDefinition* refreshed =
        after_reload.find_tile(ContentKind::Structure, "mod_marker");
    T2D_REQUIRE(refreshed != nullptr);
    const ContentId refreshed_id = registry.find(ContentKind::Structure, "mod_marker");
    const std::unique_ptr<types::Tile> refreshed_tile =
        types::make_tile(after_reload, *refreshed, registry, refreshed_id, 0, GridPos{6, 6});
    T2D_REQUIRE(refreshed_tile != nullptr);
    const auto* refreshed_shared = static_cast<const u32*>(refreshed_tile->shared(kMarkerComponent));
    T2D_REQUIRE(refreshed_shared != nullptr);
    T2D_CHECK_EQ(refreshed_shared[0], 31u);
}

T2D_TEST(the_content_pipeline_gives_mods_the_same_registries_and_builds_their_content) {
    ContentPipeline pipeline;
    pipeline.set_mod_directories({T2D_TEST_RUNTIME_MODS_DIR});
    ContentRegistry registry;
    const ContentPipelineReport& report = pipeline.load(registry);
    T2D_CHECK_MSG(report.clean(), "{}", report.first_error());
    T2D_CHECK_EQ(report.mods, 1u);
    T2D_CHECK_EQ(report.native_mods, 1u);

    // The pipeline's content definitions were built against the same context the module registered
    // into, so the custom tile type name resolves and the object can be constructed.
    const TileDefinition* marker = report.definitions.find_tile(ContentKind::Structure, "mod_marker");
    T2D_REQUIRE(marker != nullptr);
    T2D_CHECK_EQ(marker->tile_type, std::string("example:marker"));
    const ContentId id = registry.find(ContentKind::Structure, "mod_marker");
    T2D_REQUIRE(id != kNoContent);
    const std::unique_ptr<types::Tile> tile =
        types::make_tile(report.definitions, *marker, registry, id, 0, GridPos{1, 1});
    T2D_REQUIRE(tile != nullptr);
    T2D_CHECK(tile->has_component(kMarkerComponent));
    T2D_CHECK(tile->get_preview().images.size() == 1u);

    // The pipeline exposes the same registries for a caller that wants to inspect them.
    T2D_CHECK(pipeline.runtime() != nullptr);
    T2D_CHECK(pipeline.runtime()->components().find(kMarkerComponent) != nullptr);
    T2D_CHECK(pipeline.runtime()->tile_types().find(kMarkerTileType) != nullptr);
}

T2D_TEST(the_mod_api_refuses_bad_abis_duplicates_and_unknown_interfaces) {
    T2D_CHECK(load_variant(T2D_TEST_RUNTIME_BAD_ABI, {"built for ABI"}));
    T2D_CHECK(load_variant(T2D_TEST_RUNTIME_DUPLICATE, {"registered twice in one load"}));
    T2D_CHECK(load_variant(T2D_TEST_RUNTIME_MISSING_PREVIOUS, {"has no interface"}));
    T2D_CHECK(load_variant(T2D_TEST_RUNTIME_BAD_VERSION, {"is version 1, not 99"}));
}

T2D_TEST_MAIN
