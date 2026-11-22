// A mine layer saved to a .t2dsave and loaded back (docs/SAVE.md, docs/GAME_DESIGN.md section 1.46):
// the session, the world and layer identity, every plot with its container and cadence, the drops, the
// sweeps, the unlocked buildings - and the rule that a save never comes back as half a mine.
#include <mine/game_save.h>

#include <mine/types/drop.h>
#include <mine/types/tile_definition.h>

#include <support/test_support.h>

#include <t2d/save/archive.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
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

    ContentId add_floor(std::string_view name) {
        const ContentId id = registry.register_content(ContentKind::Floor, name);
        TileDefinition definition;
        definition.kind = ContentKind::Floor;
        definition.name = std::string(name);
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_ore(std::string_view name, f32 hardness, std::string drop) {
        const ContentId id = registry.register_content(ContentKind::Ore, name);
        TileDefinition definition;
        definition.kind = ContentKind::Ore;
        definition.name = std::string(name);
        definition.hardness = hardness;
        definition.drop = std::move(drop);
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_item(std::string_view name) {
        const ContentId id = registry.register_content(ContentKind::Item, name);
        TileDefinition definition;
        definition.kind = ContentKind::Item;
        definition.name = std::string(name);
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_warehouse(std::string_view name, usize capacity, bool default_unlock = true,
                            bool research_node = false) {
        const ContentId id = registry.register_content(ContentKind::Warehouse, name);
        TileDefinition definition;
        definition.kind = ContentKind::Warehouse;
        definition.name = std::string(name);
        definition.capacity = capacity;
        definition.building.category = BuildingCategory::Storage;
        definition.building.seconds = 10.0f;
        if (!default_unlock) definition.building.default_unlock = false;
        if (research_node) {
            Research research;
            research.is_root = true;
            research.tree = "intro";
            research.min_layer = 1;
            research.cost.push_back(ContentStack{"stone", 20});
            definition.research = std::move(research);
        }
        definitions.add(std::move(definition));
        return id;
    }

    ContentId add_drill(std::string_view name, f32 power) {
        const ContentId id = registry.register_content(ContentKind::Drill, name);
        TileDefinition definition;
        definition.kind = ContentKind::Drill;
        definition.name = std::string(name);
        definition.dig_power = power;
        definition.building.category = BuildingCategory::Extraction;
        definition.building.seconds = 5.0f;
        definitions.add(std::move(definition));
        return id;
    }

    /// A 2x1 task block: structure:: in the registry, the built-in "task_block" tile type for its
    /// components, and the footprint and capacity its entry declares (docs/GAME_DESIGN.md section 1.51).
    ContentId add_task_block(std::string_view name, usize capacity) {
        const ContentId id = registry.register_content(ContentKind::Structure, name);
        TileDefinition definition;
        definition.kind = ContentKind::Structure;
        definition.name = std::string(name);
        definition.tile_type = "task_block";
        definition.capacity = capacity;
        definition.width = 2;
        definition.height = 1;
        TaskSpec first;
        first.name = "first_delivery";
        first.type = TaskType::Deliver;
        first.item = "stone";
        first.count = 10;
        TaskSpec second;
        second.name = "second_delivery";
        second.type = TaskType::Deliver;
        second.item = "stone";
        second.count = 25;
        definition.tasks = {first, second};
        definitions.add(std::move(definition));
        return id;
    }

    /// A 2x1 task block whose one step is a **rate** step: the item has to arrive fast enough within
    /// p window seconds and keep doing so for p hold seconds (docs/GAME_DESIGN.md sections 1.53-1.55).
    ContentId add_rate_block(std::string_view name, usize capacity, f32 rate, f32 window, f32 hold) {
        const ContentId id = registry.register_content(ContentKind::Structure, name);
        TileDefinition definition;
        definition.kind = ContentKind::Structure;
        definition.name = std::string(name);
        definition.tile_type = "task_block";
        definition.capacity = capacity;
        definition.width = 2;
        definition.height = 1;
        TaskSpec feed;
        feed.name = "feed";
        feed.type = TaskType::Rate;
        feed.item = "stone";
        feed.rate = rate;
        feed.window = window;
        feed.hold = hold;
        definition.tasks = {feed};
        definitions.add(std::move(definition));
        return id;
    }
};

/// One layer with everything a save has to bring back: a floor, ore in the ground and a drill on it, a
/// warehouse holding stone and turned around, a 2x1 task block holding stone, two drops (one of them a
/// pile), a locked building, and two sweeps half way through with **different** elapsed times.
struct BuiltScene {
    MineWorld world{7, LayerShape{8, 6, 3}};
    BuildAnimation builds;
    BuildingUnlocks unlocks;
    ResearchState research;
    ContentId stone = kNoContent;
    ContentId bin = kNoContent;
    ContentId rig = kNoContent;
    ContentId task_block = kNoContent;
    usize rig_index = 0;
    usize bin_index = 0;
    usize task_index = 0;
};

[[nodiscard]] std::optional<BuiltScene> build_scene(Fixture& content) {
    BuiltScene scene;
    scene.stone = content.add_item("stone");
    scene.bin = content.add_warehouse("bin", 100, true, /*research_node=*/true);
    scene.rig = content.add_drill("rig", 1.0f);
    scene.task_block = content.add_task_block("task_block", 100);
    (void)content.add_floor("dirt");
    (void)content.add_ore("stone", 1.0f, "stone");

    auto layer = std::make_unique<MineLayer>(3, LayerShape{8, 6, 3}, 11);
    LayerBuildReport report;
    const auto place = [&](ContentKind kind, std::string_view name, i32 tile_layer, i32 x, i32 y) -> Tile* {
        PlacedContent where;
        where.kind = kind;
        where.name = std::string(name);
        where.layer = tile_layer;
        where.anchor = GridPos{x, y};
        return layer->place(content.registry, content.definitions, where, &report);
    };
    Tile* dirt = place(ContentKind::Floor, "dirt", 0, 1, 1);
    Tile* ore = place(ContentKind::Ore, "stone", 1, 2, 2);
    Tile* rig = place(ContentKind::Drill, "rig", 2, 2, 2);
    Tile* bin = place(ContentKind::Warehouse, "bin", 2, 4, 4);
    // The task block's 2x1 footprint comes from its definition; the placement names no size.
    Tile* task_block = place(ContentKind::Structure, "task_block", 2, 0, 4);
    if (dirt == nullptr || ore == nullptr || rig == nullptr || bin == nullptr || task_block == nullptr)
        return std::nullopt;

    const std::vector<std::unique_ptr<Tile>>& plots = layer->plots();
    for (usize index = 0; index < plots.size(); ++index) {
        if (plots[index].get() == rig) scene.rig_index = index;
        if (plots[index].get() == bin) scene.bin_index = index;
        if (plots[index].get() == task_block) scene.task_index = index;
    }
    bin->set_mirrored(true);
    layer->recount_mirrored(); // the count place() keeps is not told about a flag set afterwards
    bin->container().add(ContentKind::Item, scene.stone, 10);
    task_block->container().add(ContentKind::Item, scene.stone, 5);
    Tile* drill = rig;
    if (drill == nullptr) return std::nullopt;
    drill->set_produced(7);
    drill->set_since_last_update(1.25f);
    layer->adopt(std::make_unique<Drop>(ContentKind::Item, scene.stone, t2d::Vec2{5.5f, 1.5f}, 3));
    layer->adopt(std::make_unique<Drop>(ContentKind::Item, scene.stone, t2d::Vec2{6.25f, 2.75f}, 1));

    // Two sweeps at once, started at different moments: the save has to bring each one back where it was
    // (the warehouse is 3 s into 10, the drill 1 s into 5).
    scene.builds.start(bin, 10.0f);
    scene.builds.advance(2.0f);
    scene.builds.start(rig, 5.0f);
    scene.builds.advance(1.0f);

    scene.unlocks.reset(content.registry, content.definitions);
    scene.unlocks.set_unlocked(ContentKind::Warehouse, scene.bin, false);
    // The warehouse is a research node as well, and this mine has studied it.
    scene.research.reset(content.registry, content.definitions);
    scene.research.set_researched(ContentKind::Warehouse, scene.bin, true);
    scene.world.install_layer(std::move(layer), report);
    return scene;
}

/// The session and view a save test writes down.
[[nodiscard]] SessionConfig test_session() {
    SessionConfig session;
    session.mode = Mode::Endless;
    session.role = Role::Host;
    session.seed = 42;
    session.port = 7001;
    session.connect_address = "10.0.0.1:7001";
    return session;
}

[[nodiscard]] t2d::save::File make_test_save(const BuiltScene& scene, const Fixture& content,
                                             const SessionConfig& session, f32 world_seconds = 12.5f,
                                             t2d::Vec2 center = t2d::Vec2{3.25f, 4.5f}, f32 zoom = 0.75f) {
    return make_game_save(scene.world, session, scene.unlocks, scene.research, scene.builds, world_seconds,
                          center, zoom, content.registry);
}

/// One layer whose only content is a dirt floor and a 2x1 **rate** task block holding \p stock stone:
/// the window is satisfied as soon as the layer exists, so the hold starts counting on the first tick
/// (docs/GAME_DESIGN.md section 1.54). The block is plot 1, so a TASK record for plot 0 names the floor.
struct RateScene {
    Fixture content;
    MineWorld world{7, LayerShape{8, 6, 3}};
    BuildAnimation builds;
    BuildingUnlocks unlocks;
    ResearchState research;
    ContentId stone = kNoContent;
    usize block_index = 1;
};

[[nodiscard]] std::optional<RateScene> build_rate_scene(usize stock = 20, f32 rate = 3.0f, f32 window = 5.0f,
                                                        f32 hold = 2.0f) {
    RateScene scene;
    scene.stone = scene.content.add_item("stone");
    (void)scene.content.add_rate_block("pump", 100, rate, window, hold);
    (void)scene.content.add_floor("dirt");

    auto layer = std::make_unique<MineLayer>(0, LayerShape{8, 6, 3}, 31);
    LayerBuildReport build;
    PlacedContent floor;
    floor.kind = ContentKind::Floor;
    floor.name = "dirt";
    floor.layer = 0;
    floor.anchor = GridPos{0, 0};
    if (layer->place(scene.content.registry, scene.content.definitions, floor, &build) == nullptr) {
        return std::nullopt;
    }
    PlacedContent block;
    block.kind = ContentKind::Structure;
    block.name = "pump";
    block.layer = 2;
    block.anchor = GridPos{1, 1};
    Tile* pump = layer->place(scene.content.registry, scene.content.definitions, block, &build);
    if (pump == nullptr) return std::nullopt;
    // The stock a layer rule's place_block would start it with, and the same note that rule's fill makes:
    // items already in the box count toward the rate window from the moment the layer exists
    // (docs/GAME_DESIGN.md sections 1.53 and 1.55).
    pump->container().add(ContentKind::Item, scene.stone, stock);
    pump->note_delivery(ContentKind::Item, scene.stone, stock);
    const std::vector<std::unique_ptr<Tile>>& plots = layer->plots();
    for (usize index = 0; index < plots.size(); ++index) {
        if (plots[index].get() == pump) scene.block_index = index;
    }
    scene.unlocks.reset(scene.content.registry, scene.content.definitions);
    scene.research.reset(scene.content.registry, scene.content.definitions);
    scene.world.install_layer(std::move(layer), build);
    return scene;
}

[[nodiscard]] t2d::save::File save_rate_scene(const RateScene& scene) {
    return make_game_save(scene.world, test_session(), scene.unlocks, scene.research, scene.builds, 3.0f,
                          t2d::Vec2{3.0f, 3.0f}, 1.0f, scene.content.registry);
}

} // namespace

T2D_TEST(a_layer_round_trips_through_a_save) {
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    T2D_CHECK_EQ(scene->rig_index, 2u); // the order the layer placed them in: floor, ore, drill, warehouse
    T2D_CHECK_EQ(scene->bin_index, 3u);

    const SessionConfig session = test_session();
    const t2d::save::File file = make_test_save(*scene, content, session);
    T2D_CHECK_EQ(file.game, std::string("mine"));
    T2D_CHECK_EQ(file.game_version, kGameSaveVersion);
    // All eight chunks are there, each at the version this build reads. TASK is optional in general, but
    // this layer has a task block, so it is written here - at its own version, which carries the rate
    // state as well (docs/SAVE.md, docs/GAME_DESIGN.md sections 1.52 and 1.55).
    for (const t2d::u32 id : {game_save_chunk::kMeta, game_save_chunk::kContent, game_save_chunk::kUnlocks,
                              game_save_chunk::kResearch, game_save_chunk::kPlots, game_save_chunk::kEntities,
                              game_save_chunk::kBuilds, game_save_chunk::kTasks}) {
        const t2d::save::Chunk* chunk = file.find(id);
        T2D_REQUIRE(chunk != nullptr);
        T2D_CHECK_EQ(chunk->version, id == game_save_chunk::kTasks ? kTasksChunkVersion : kGameSaveChunkVersion);
    }
    // No custom component state means no CMPT chunk: a pure built-in save stays at the old shape.
    T2D_CHECK(file.find(game_save_chunk::kComponents) == nullptr);

    // Through the framework's container first: a save is bytes before it is a game's state.
    const std::vector<u8> bytes = t2d::save::encode(file);
    T2D_REQUIRE(!bytes.empty());
    const std::optional<t2d::save::File> decoded = t2d::save::decode(bytes);
    T2D_REQUIRE(decoded.has_value());
    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(*decoded, content.registry, content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    T2D_CHECK(report.error.empty());
    T2D_CHECK(report.warnings.empty());
    T2D_CHECK_EQ(report.plots, 5u);
    T2D_CHECK_EQ(report.entities, 2u);

    // --- the session, the world, the view -----------------------------------------------------------
    T2D_CHECK(loaded->session.mode == Mode::Endless);
    T2D_CHECK(loaded->session.role == Role::Host);
    T2D_CHECK_EQ(loaded->session.seed, 42u);
    T2D_CHECK_EQ(loaded->session.port, static_cast<u16>(7001));
    T2D_CHECK_EQ(loaded->session.connect_address, std::string("10.0.0.1:7001"));
    T2D_REQUIRE(loaded->world != nullptr);
    T2D_CHECK_EQ(loaded->world->seed(), 7u);
    T2D_CHECK_EQ(loaded->world->shape().width, 8u);
    T2D_CHECK_EQ(loaded->world->shape().height, 6u);
    T2D_CHECK_EQ(loaded->world->shape().tile_layers, 3);
    T2D_CHECK(loaded->world->has_layer());
    T2D_CHECK_EQ(loaded->world->layer_index(), 3);
    T2D_CHECK_EQ(loaded->world->layer().seed(), 11u);
    T2D_CHECK_EQ(loaded->world->layer().width(), 8u);
    T2D_CHECK_EQ(loaded->world->layer().height(), 6u);
    T2D_CHECK_EQ(loaded->world->layer().layer_count(), 3);
    T2D_CHECK(loaded->world->build_report().clean());
    T2D_CHECK_NEAR(loaded->world_seconds, 12.5f, 1e-6f);
    T2D_CHECK_NEAR(loaded->camera_center.x, 3.25f, 1e-6f);
    T2D_CHECK_NEAR(loaded->camera_center.y, 4.5f, 1e-6f);
    T2D_CHECK_NEAR(loaded->camera_zoom, 0.75f, 1e-6f);

    // --- the plots, in the saved order --------------------------------------------------------------
    const std::vector<std::unique_ptr<Tile>>& before = scene->world.layer().plots();
    const std::vector<std::unique_ptr<Tile>>& after = loaded->world->layer().plots();
    T2D_REQUIRE(after.size() == before.size());
    for (usize index = 0; index < before.size(); ++index) {
        T2D_CHECK_EQ(after[index]->kind(), before[index]->kind());
        T2D_CHECK_EQ(after[index]->id(), before[index]->id());
        T2D_CHECK_EQ(after[index]->layer(), before[index]->layer());
        T2D_CHECK_EQ(after[index]->anchor(), before[index]->anchor());
        T2D_CHECK_EQ(after[index]->width(), before[index]->width());
        T2D_CHECK_EQ(after[index]->height(), before[index]->height());
        T2D_CHECK_EQ(after[index]->mirrored(), before[index]->mirrored());
        T2D_CHECK_EQ(after[index]->under_construction(), before[index]->under_construction());
    }
    T2D_CHECK(after[0]->is_floor());
    T2D_CHECK(after[1]->is_ore());
    T2D_CHECK_FALSE(after[0]->under_construction());
    // The count place() keeps is part of the layer's identity, not just the plots' flags: it has to
    // survive the round trip too, or the layer reports nothing turned around (MineLayer::recount_mirrored).
    T2D_CHECK_EQ(before.size(), 5u);
    T2D_CHECK_EQ(scene->world.layer().mirrored_count(), 1u);
    T2D_CHECK_EQ(loaded->world->layer().mirrored_count(), 1u);

    // The warehouse: turned around, and holding what it held.
    const Tile* before_bin = before[scene->bin_index].get();
    const Tile* after_bin = after[scene->bin_index].get();
    T2D_REQUIRE(before_bin != nullptr);
    T2D_REQUIRE(after_bin != nullptr);
    T2D_CHECK(after_bin->mirrored());
    T2D_REQUIRE(after_bin->has_container());
    T2D_CHECK_EQ(after_bin->container().total(), before_bin->container().total());
    T2D_CHECK_EQ(after_bin->container().total(), 10u);
    T2D_CHECK_EQ(after_bin->container().count_of(ContentKind::Item, scene->stone), 10u);
    T2D_CHECK(after_bin->under_construction());

    // The 2x1 task block: the footprint the save recorded comes back, the second cell answers for the
    // same plot, and what its container held is in it again (docs/GAME_DESIGN.md section 1.51).
    T2D_CHECK_EQ(scene->task_index, 4u);
    const Tile* before_task = before[scene->task_index].get();
    const Tile* after_task = after[scene->task_index].get();
    T2D_REQUIRE(before_task != nullptr);
    T2D_REQUIRE(after_task != nullptr);
    T2D_CHECK_EQ(after_task->width(), 2);
    T2D_CHECK_EQ(after_task->height(), 1);
    T2D_CHECK(loaded->world->layer().plot_at(2, GridPos{1, 4}) == after_task);
    T2D_CHECK(loaded->world->layer().plot_at(2, GridPos{2, 4}) == nullptr);
    T2D_REQUIRE(after_task->has_container());
    T2D_CHECK_EQ(after_task->container().total(), before_task->container().total());
    T2D_CHECK_EQ(after_task->container().total(), 5u);
    T2D_CHECK_EQ(after_task->container().count_of(ContentKind::Item, scene->stone), 5u);
    // Its task chain came back too: two deliver steps, nothing completed, and the five stone in the box
    // are progress toward the first step (5 of 10), not a step the load already finished.
    T2D_REQUIRE(after_task->has_task());
    T2D_CHECK_EQ(after_task->task().count(), 2u);
    T2D_CHECK_EQ(after_task->task().current(), 0u);
    T2D_CHECK_FALSE(after_task->task().complete());
    T2D_REQUIRE(after_task->task().step(0).has_value());
    T2D_CHECK_EQ(after_task->task().step(0)->id, scene->stone);
    T2D_CHECK_EQ(after_task->task().step(0)->count, 10u);
    T2D_CHECK_EQ(after_task->task().step(1)->count, 25u);

    // The drill: what it produced and the time its cadence had carried.
    const Tile* before_rig = before[scene->rig_index].get();
    const Tile* after_rig = after[scene->rig_index].get();
    T2D_REQUIRE(before_rig != nullptr);
    T2D_REQUIRE(after_rig != nullptr);
    T2D_CHECK_EQ(after_rig->produced(), 7u);
    T2D_CHECK_NEAR(after_rig->since_last_update(), 1.25f, 1e-6f);
    T2D_CHECK_NEAR(after_rig->period_seconds(), before_rig->period_seconds(), 1e-6f);
    T2D_CHECK_EQ(after_rig->ready(), before_rig->ready());
    T2D_CHECK(after_rig->under_construction());

    // --- the drops: both, with the pile's count and both positions ----------------------------------
    const std::vector<std::unique_ptr<Entity>>& entities = loaded->world->layer().entities();
    T2D_REQUIRE(entities.size() == 2u);
    const auto* pile = dynamic_cast<const Drop*>(entities[0].get());
    const auto* single = dynamic_cast<const Drop*>(entities[1].get());
    T2D_REQUIRE(pile != nullptr);
    T2D_REQUIRE(single != nullptr);
    T2D_CHECK_EQ(pile->id(), scene->stone);
    T2D_CHECK_EQ(pile->count(), 3u);
    T2D_CHECK_NEAR(pile->position().x, 5.5f, 1e-6f);
    T2D_CHECK_NEAR(pile->position().y, 1.5f, 1e-6f);
    T2D_CHECK_EQ(single->count(), 1u);
    T2D_CHECK_NEAR(single->position().x, 6.25f, 1e-6f);
    T2D_CHECK_NEAR(single->position().y, 2.75f, 1e-6f);

    // --- the sweeps: two of them, each with the progress it had ------------------------------------
    T2D_REQUIRE(loaded->builds.size() == 2u);
    T2D_CHECK_NEAR(scene->builds.progress_of(before[scene->bin_index].get()), 0.3f, 1e-6f);
    T2D_CHECK_NEAR(scene->builds.progress_of(before[scene->rig_index].get()), 0.2f, 1e-6f);
    T2D_CHECK_NEAR(loaded->builds.progress_of(after[scene->bin_index].get()), 0.3f, 1e-6f);
    T2D_CHECK_NEAR(loaded->builds.progress_of(after[scene->rig_index].get()), 0.2f, 1e-6f);

    // --- the unlocks: the warehouse was locked, and is locked again --------------------------------
    T2D_CHECK_EQ(loaded->unlocks.size(), 2u);
    T2D_CHECK_FALSE(loaded->unlocks.unlocked(ContentKind::Warehouse, scene->bin));
    T2D_CHECK(loaded->unlocks.unlocked(ContentKind::Drill, scene->rig));

    // --- the research: the warehouse's node was studied, and is again --------------------------------
    T2D_CHECK_EQ(loaded->research.size(), 1u);
    T2D_CHECK(loaded->research.researched(ContentKind::Warehouse, scene->bin));

    // --- and the save is a value: the same state encodes to the same bytes --------------------------
    T2D_CHECK_EQ(t2d::save::encode(make_test_save(*scene, content, session)), bytes);
    // What came back writes the same file: the round trip loses nothing.
    const t2d::save::File resaved =
        make_game_save(*loaded->world, loaded->session, loaded->unlocks, loaded->research, loaded->builds,
                       loaded->world_seconds, loaded->camera_center, loaded->camera_zoom, content.registry);
    T2D_CHECK_EQ(t2d::save::encode(resaved), bytes);
}

T2D_TEST(a_save_follows_its_names_into_another_registry) {
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const std::vector<u8> bytes = t2d::save::encode(make_test_save(*scene, content, test_session()));
    T2D_REQUIRE(!bytes.empty());
    const std::optional<t2d::save::File> decoded = t2d::save::decode(bytes);
    T2D_REQUIRE(decoded.has_value());

    // The same content in another build, with **other ids**: extra entries registered first in every kind,
    // so every number the save wrote means something else now. Only names can be trusted.
    Fixture other;
    (void)other.add_item("bolt");
    (void)other.add_floor("sand");
    (void)other.add_ore("copper", 2.0f, "bolt");
    (void)other.add_warehouse("shed", 5);
    (void)other.add_drill("press", 2.0f);
    const ContentId other_stone = other.add_item("stone");
    (void)other.add_floor("dirt");
    (void)other.add_ore("stone", 1.0f, "stone");
    const ContentId other_bin = other.add_warehouse("bin", 100);
    const ContentId other_rig = other.add_drill("rig", 1.0f);
    (void)other.add_task_block("task_block", 100);
    T2D_CHECK_NE(other_stone, scene->stone);
    T2D_CHECK_NE(other_bin, scene->bin);
    T2D_CHECK_NE(other_rig, scene->rig);

    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(*decoded, other.registry, other.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    const std::vector<std::unique_ptr<Tile>>& plots = loaded->world->layer().plots();
    T2D_REQUIRE(plots.size() == 5u);
    // The names came across; the ids are the ones this build hands out.
    T2D_CHECK_EQ(plots[scene->rig_index]->id(), other_rig);
    T2D_CHECK_EQ(plots[scene->bin_index]->id(), other_bin);
    T2D_CHECK_EQ(plots[scene->bin_index]->kind(), ContentKind::Warehouse);
    const Tile* bin = plots[scene->bin_index].get();
    T2D_REQUIRE(bin != nullptr);
    T2D_CHECK_EQ(bin->container().count_of(ContentKind::Item, other_stone), 10u);
    T2D_CHECK_FALSE(loaded->unlocks.unlocked(ContentKind::Warehouse, other_bin));
    T2D_CHECK_EQ(loaded->world->layer().entity_count(), 2u);
    const Tile* drill = plots[scene->rig_index].get();
    T2D_REQUIRE(drill != nullptr);
    T2D_CHECK_EQ(drill->produced(), 7u);
    T2D_CHECK_NEAR(loaded->builds.progress_of(plots[scene->rig_index].get()), 0.2f, 1e-6f);
    // The task block's footprint and container came across by name too.
    const Tile* task_block = plots[scene->task_index].get();
    T2D_REQUIRE(task_block != nullptr);
    T2D_CHECK_EQ(task_block->width(), 2);
    T2D_CHECK_EQ(task_block->container().count_of(ContentKind::Item, other_stone), 5u);
}

T2D_TEST(a_save_that_names_content_this_build_lacks_is_refused) {
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const std::vector<u8> bytes = t2d::save::encode(make_test_save(*scene, content, test_session()));
    const std::optional<t2d::save::File> decoded = t2d::save::decode(bytes);
    T2D_REQUIRE(decoded.has_value());

    // The same floor, ore and item - but no drill and no warehouse, so the save names content this build
    // does not have. That is refused, not quietly dropped.
    Fixture missing;
    (void)missing.add_item("stone");
    (void)missing.add_floor("dirt");
    (void)missing.add_ore("stone", 1.0f, "stone");
    GameLoadReport report;
    T2D_CHECK_FALSE(load_game_save(*decoded, missing.registry, missing.definitions, &report).has_value());
    T2D_CHECK_FALSE(report.ok);
    T2D_CHECK(report.error.find("does not have") != std::string::npos);
    T2D_CHECK_EQ(report.plots, 0u);
    T2D_CHECK_EQ(report.entities, 0u);
}

T2D_TEST(a_save_that_cannot_come_back_exactly_is_refused) {
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const t2d::save::File good = make_test_save(*scene, content, test_session());

    const auto refuses = [&](const t2d::save::File& file) {
        GameLoadReport report;
        return !load_game_save(file, content.registry, content.definitions, &report).has_value() &&
               !report.ok && !report.error.empty();
    };

    // A chunk this build needs is missing.
    {
        t2d::save::File broken = good;
        broken.chunks.erase(
            std::remove_if(broken.chunks.begin(), broken.chunks.end(),
                           [](const t2d::save::Chunk& chunk) { return chunk.id == game_save_chunk::kMeta; }),
            broken.chunks.end());
        GameLoadReport report;
        T2D_CHECK_FALSE(load_game_save(broken, content.registry, content.definitions, &report).has_value());
        T2D_CHECK(report.error.find("META") != std::string::npos);
    }
    // A chunk at a version this build does not know, and a chunk whose bytes are cut short.
    {
        t2d::save::File broken = good;
        for (t2d::save::Chunk& chunk : broken.chunks) {
            if (chunk.id == game_save_chunk::kPlots) chunk.version = 2;
        }
        T2D_CHECK(refuses(broken));
    }
    {
        t2d::save::File broken = good;
        for (t2d::save::Chunk& chunk : broken.chunks) {
            if (chunk.id == game_save_chunk::kPlots) chunk.payload.resize(chunk.payload.size() / 2);
        }
        T2D_CHECK(refuses(broken));
    }
    // A file another game wrote, and one this game wrote at another version.
    {
        t2d::save::File broken = good;
        broken.game = "sandbox";
        T2D_CHECK(refuses(broken));
    }
    {
        t2d::save::File broken = good;
        broken.game_version = kGameSaveVersion + 1;
        T2D_CHECK(refuses(broken));
    }
    // A chunk this build does not know is ignored, not refused: a newer save still loads.
    {
        t2d::save::File with_extra = good;
        with_extra.chunks.push_back(t2d::save::Chunk{t2d::save::fourcc('X', 'T', 'R', 'A'), 7, {1u, 2u, 3u}});
        GameLoadReport report;
        T2D_CHECK(load_game_save(with_extra, content.registry, content.definitions, &report).has_value());
        T2D_CHECK(report.ok);
    }
    // A file whose bytes were touched is refused by the container, before this game ever sees it: the
    // checksum is the framework's, and it catches a single flipped bit.
    {
        std::vector<u8> flipped = t2d::save::encode(good);
        T2D_REQUIRE(!flipped.empty());
        const usize last = flipped.size() - 1;
        flipped.at(last) = static_cast<u8>(flipped.at(last) ^ 0x01u);
        T2D_CHECK_FALSE(t2d::save::decode(flipped).has_value());
    }
}

T2D_TEST(a_world_with_no_layer_saves_a_file_that_cannot_load) {
    Fixture content;
    (void)content.add_item("stone");
    const MineWorld world(5, LayerShape{4, 4, 1});
    const SessionConfig session;
    const t2d::save::File file = make_game_save(world, session, BuildingUnlocks{}, ResearchState{},
                                                BuildAnimation{}, 0.0f, t2d::Vec2{}, 1.0f, content.registry);
    T2D_CHECK_EQ(file.game, std::string("mine"));
    GameLoadReport report;
    T2D_CHECK_FALSE(load_game_save(file, content.registry, content.definitions, &report).has_value());
    T2D_CHECK(report.error.find("no layer") != std::string::npos);
}

T2D_TEST(a_save_whose_plots_no_longer_fit_is_refused) {
    // Content that can be translated by name and still not come back: a container with less room than the
    // save says, and a drill whose ore under it no longer says how hard it is (so the layer refuses to
    // place it). Both refuse the whole load - a save never comes back as half a mine.
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const std::vector<u8> bytes = t2d::save::encode(make_test_save(*scene, content, test_session()));
    const std::optional<t2d::save::File> decoded = t2d::save::decode(bytes);
    T2D_REQUIRE(decoded.has_value());

    Fixture small;
    (void)small.add_item("stone");
    (void)small.add_floor("dirt");
    (void)small.add_ore("stone", 1.0f, "stone");
    (void)small.add_warehouse("bin", 5); // the save says it held 10
    (void)small.add_drill("rig", 1.0f);
    // The task block is content this build has, with the same capacity: the save has to reach the
    // warehouse it cannot refill, not be refused for a name the build never had.
    (void)small.add_task_block("task_block", 100);
    GameLoadReport report;
    T2D_CHECK_FALSE(load_game_save(*decoded, small.registry, small.definitions, &report).has_value());
    T2D_CHECK_MSG(report.error.find("cannot hold") != std::string::npos, "error: {}", report.error);

    // A PLOT chunk edited so two plots claim one cell - the layout is the one make_game_save() writes,
    // which is how a test makes a save the game itself would never produce. The layer refuses the second
    // plot, and the load refuses instead of returning a world that is missing it.
    t2d::save::File edited = *decoded;
    for (t2d::save::Chunk& chunk : edited.chunks) {
        if (chunk.id != game_save_chunk::kPlots) continue;
        const ContentId dirt = content.registry.find(ContentKind::Floor, "dirt");
        t2d::save::Writer writer;
        writer.write_u32(2u);
        for (int plot = 0; plot < 2; ++plot) {
            writer.write_u8(static_cast<u8>(ContentKind::Floor));
            writer.write_u32(dirt);
            writer.write_i32(0);      // tile layer
            writer.write_i32(1);      // anchor x
            writer.write_i32(1);      // anchor y
            writer.write_i32(1);      // width
            writer.write_i32(1);      // height
            writer.write_bool(false); // mirrored
            writer.write_bool(false); // under construction
            writer.write_bool(false); // ticking
            writer.write_bool(false); // drill
            writer.write_bool(false); // container
        }
        chunk.payload = writer.finish();
    }
    GameLoadReport second;
    T2D_CHECK_FALSE(load_game_save(edited, content.registry, content.definitions, &second).has_value());
    T2D_CHECK(second.error.find("could not be placed") != std::string::npos);
}

T2D_TEST(a_drop_that_is_gone_is_not_saved) {
    // destroy() takes a drop out of the index at once but leaves it in the layer's list until the tick
    // that is running ends. A save written in between is what is on the layer: the two drops, not three.
    Fixture content;
    std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    Entity& adopted = scene->world.layer().adopt(
        std::make_unique<Drop>(ContentKind::Item, scene->stone, t2d::Vec2{1.5f, 1.5f}, 1));
    auto* doomed = dynamic_cast<Drop*>(&adopted);
    T2D_REQUIRE(doomed != nullptr);
    T2D_CHECK(scene->world.layer().destroy(*doomed));
    T2D_CHECK_EQ(scene->world.layer().entity_count(), 3u); // still listed until the tick ends

    const std::optional<t2d::save::File> decoded =
        t2d::save::decode(t2d::save::encode(make_test_save(*scene, content, test_session())));
    T2D_REQUIRE(decoded.has_value());
    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(*decoded, content.registry, content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK_EQ(report.entities, 2u);
    T2D_REQUIRE(loaded->world->layer().entity_count() == 2u);
    T2D_CHECK(loaded->world->layer().entities_within(t2d::Vec2{1.5f, 1.5f}, 0.25f).empty());
}

T2D_TEST(a_save_whose_build_records_disagree_with_its_plots_is_refused) {
    // The BUIL chunk and the plots' under-construction flags are two halves of one fact. A save where the
    // two disagree cannot come back exactly, so it is refused rather than guessed at - a plot quietly
    // brought back finished would hide a sweep the save says was still running.
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const std::optional<t2d::save::File> decoded =
        t2d::save::decode(t2d::save::encode(make_test_save(*scene, content, test_session())));
    T2D_REQUIRE(decoded.has_value());

    // The layout make_game_save() writes: a count, then (plot index, elapsed, seconds) for each record.
    const auto with_builds = [](t2d::save::File file, const std::vector<std::tuple<u32, f32, f32>>& records) {
        for (t2d::save::Chunk& chunk : file.chunks) {
            if (chunk.id != game_save_chunk::kBuilds) continue;
            t2d::save::Writer writer;
            writer.write_u32(static_cast<u32>(records.size()));
            for (const auto& [plot, elapsed, seconds] : records) {
                writer.write_u32(plot);
                writer.write_f32(elapsed);
                writer.write_f32(seconds);
            }
            chunk.payload = writer.finish();
        }
        return file;
    };
    const auto refuses = [&](const t2d::save::File& file, const std::string& expected) {
        GameLoadReport report;
        T2D_CHECK_FALSE(load_game_save(file, content.registry, content.definitions, &report).has_value());
        T2D_CHECK_MSG(report.error.find(expected) != std::string::npos, "error: {}", report.error);
    };

    const auto bin = static_cast<u32>(scene->bin_index);
    const auto rig = static_cast<u32>(scene->rig_index);
    // The rig's sweep only: the warehouse is under construction, and nothing brings its sweep back.
    refuses(with_builds(*decoded, {{rig, 1.0f, 5.0f}}), "no build for it");
    // A record for a plot that is not under construction.
    refuses(with_builds(*decoded, {{0u, 1.0f, 5.0f}}), "not under construction");
    // The same plot named twice.
    refuses(with_builds(*decoded, {{bin, 3.0f, 10.0f}, {rig, 1.0f, 5.0f}, {rig, 1.0f, 5.0f}}), "twice");
    // A sweep that had already run out is not a sweep in progress.
    refuses(with_builds(*decoded, {{bin, 10.0f, 10.0f}, {rig, 1.0f, 5.0f}}), "cannot be restored");
    // A record for a plot the save does not have.
    refuses(with_builds(*decoded, {{9u, 1.0f, 5.0f}}), "has 5 plots");
}

T2D_TEST(a_mirrored_plots_are_counted_again_after_a_load) {
    // place() keeps the mirrored count as it goes, and a load sets each flag *after* placing the plot. The
    // layer has to recount, or it would report no plot turned around while some are.
    Fixture content;
    (void)content.add_floor("dirt");
    auto layer = std::make_unique<MineLayer>(4, LayerShape{6, 4, 1}, 3);
    LayerBuildReport report;
    const auto place = [&](i32 x, i32 y) {
        PlacedContent where;
        where.kind = ContentKind::Floor;
        where.name = "dirt";
        where.layer = 0;
        where.anchor = GridPos{x, y};
        return layer->place(content.registry, content.definitions, where, &report);
    };
    Tile* first = place(1, 1);
    Tile* second = place(3, 1);
    Tile* third = place(5, 1);
    T2D_REQUIRE(first != nullptr && second != nullptr && third != nullptr);
    first->set_mirrored(true);
    second->set_mirrored(true);
    layer->recount_mirrored();
    T2D_CHECK_EQ(layer->mirrored_count(), 2u);

    MineWorld world{9, LayerShape{6, 4, 1}};
    world.install_layer(std::move(layer), report);
    const std::optional<t2d::save::File> decoded = t2d::save::decode(t2d::save::encode(
        make_game_save(world, test_session(), BuildingUnlocks{}, ResearchState{}, BuildAnimation{}, 2.0f,
                       t2d::Vec2{1.0f, 2.0f}, 1.5f, content.registry)));
    T2D_REQUIRE(decoded.has_value());
    GameLoadReport load_report;
    const std::optional<GameSaveData> loaded =
        load_game_save(*decoded, content.registry, content.definitions, &load_report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(load_report.ok);
    T2D_REQUIRE(loaded->world->layer().plots().size() == 3u);
    T2D_CHECK_EQ(loaded->world->layer().mirrored_count(), 2u);
    T2D_CHECK(loaded->world->layer().plots()[0]->mirrored());
    T2D_CHECK(loaded->world->layer().plots()[1]->mirrored());
    T2D_CHECK_FALSE(loaded->world->layer().plots()[2]->mirrored());
}

T2D_TEST(a_save_written_before_the_technology_tree_loads_with_nothing_researched) {
    // The RSCH chunk is optional: a save from before the tree existed has none, and must still load -
    // with the node table rebuilt from this build's data and nothing studied.
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    t2d::save::File old = make_test_save(*scene, content, test_session());
    old.chunks.erase(
        std::remove_if(old.chunks.begin(), old.chunks.end(),
                       [](const t2d::save::Chunk& chunk) { return chunk.id == game_save_chunk::kResearch; }),
        old.chunks.end());
    T2D_CHECK(old.find(game_save_chunk::kResearch) == nullptr);
    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(old, content.registry, content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    T2D_CHECK_EQ(loaded->research.size(), 1u);
    T2D_CHECK_FALSE(loaded->research.researched(ContentKind::Warehouse, scene->bin));
}

T2D_TEST(a_save_written_before_tasks_existed_comes_back_at_zero) {
    // The TASK chunk is optional: a save from before tasks existed has none, and every task block starts
    // at the first step with nothing done (docs/SAVE.md, docs/GAME_DESIGN.md section 1.52).
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    t2d::save::File old = make_test_save(*scene, content, test_session());
    old.chunks.erase(
        std::remove_if(old.chunks.begin(), old.chunks.end(),
                       [](const t2d::save::Chunk& chunk) { return chunk.id == game_save_chunk::kTasks; }),
        old.chunks.end());
    T2D_CHECK(old.find(game_save_chunk::kTasks) == nullptr);

    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(old, content.registry, content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    const std::vector<std::unique_ptr<Tile>>& plots = loaded->world->layer().plots();
    T2D_REQUIRE(plots.size() == 5u);
    const Tile* task_block = plots[scene->task_index].get();
    T2D_REQUIRE(task_block != nullptr);
    T2D_REQUIRE(task_block->has_task());
    T2D_CHECK_EQ(task_block->task().count(), 2u);
    T2D_CHECK_EQ(task_block->task().current(), 0u);
    T2D_CHECK_EQ(task_block->container().count_of(ContentKind::Item, scene->stone), 5u);
}

T2D_TEST(a_task_chunk_that_does_not_fit_the_plots_is_refused) {
    // The TASK chunk names a plot and how many steps it had done. A record that cannot describe a task
    // plot refuses the whole load; a count the designer has shortened since is clamped with a warning.
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const std::optional<t2d::save::File> decoded =
        t2d::save::decode(t2d::save::encode(make_test_save(*scene, content, test_session())));
    T2D_REQUIRE(decoded.has_value());

    const auto with_tasks = [](t2d::save::File file, const std::vector<std::pair<t2d::u32, t2d::u32>>& records) {
        for (t2d::save::Chunk& chunk : file.chunks) {
            if (chunk.id != game_save_chunk::kTasks) continue;
            t2d::save::Writer writer;
            writer.write_u32(static_cast<t2d::u32>(records.size()));
            for (const auto& [plot, completed] : records) {
                writer.write_u32(plot);
                writer.write_u32(completed);
                writer.write_f32(0.0f); // held
                writer.write_u32(0u);   // no window
            }
            chunk.payload = writer.finish();
            chunk.version = kTasksChunkVersion;
        }
        return file;
    };
    const auto refuses = [&](const t2d::save::File& file, std::string_view expected) {
        GameLoadReport report;
        T2D_CHECK_FALSE(load_game_save(file, content.registry, content.definitions, &report).has_value());
        T2D_CHECK_MSG(report.error.find(expected) != std::string::npos, "error: {}", report.error);
    };
    const auto task_plot = static_cast<t2d::u32>(scene->task_index);

    // Plot 0 is the dirt floor: a TASK record for it names a plot that has no tasks.
    refuses(with_tasks(*decoded, {{0u, 0u}}), "has no tasks");
    // The same task block named twice: which count would win is not something a load may guess.
    refuses(with_tasks(*decoded, {{task_plot, 0u}, {task_plot, 0u}}), "twice");
    // A plot the save does not have.
    refuses(with_tasks(*decoded, {{9u, 0u}}), "has 5 plots");
    // The chunk at another version.
    {
        t2d::save::File broken = *decoded;
        for (t2d::save::Chunk& chunk : broken.chunks) {
            if (chunk.id == game_save_chunk::kTasks) chunk.version = 3;
        }
        refuses(broken, "TASK chunk is at version 3");
    }
    // A count past the chain is not a refusal: the content changed, so it is clamped and said so.
    {
        GameLoadReport report;
        const std::optional<GameSaveData> loaded =
            load_game_save(with_tasks(*decoded, {{task_plot, 99u}}), content.registry, content.definitions,
                           &report);
        T2D_REQUIRE(loaded.has_value());
        T2D_CHECK(report.ok);
        T2D_REQUIRE(report.warnings.size() == 1u);
        T2D_CHECK(report.warnings.front().find("clamped") != std::string::npos);
        const Tile* plot = loaded->world->layer().plots()[task_plot].get();
        T2D_REQUIRE(plot != nullptr);
        T2D_CHECK_EQ(plot->task().current(), 2u);
        T2D_CHECK(plot->task().complete());
    }
}

T2D_TEST(a_half_held_rate_task_continues_after_a_load) {
    // TASK v2 carries the live rate state (docs/SAVE.md, docs/GAME_DESIGN.md section 1.55): a hold that
    // was 0.8 s into its 2 s at save time is 0.8 s into it after the load, and the window's deliveries
    // still count for as long as they had left.
    std::optional<RateScene> scene = build_rate_scene();
    T2D_REQUIRE(scene.has_value());
    const ContentId stone = scene->stone;
    const usize block = scene->block_index;
    Tile* live = scene->world.layer().plots()[block].get();
    T2D_REQUIRE(live != nullptr);
    T2D_CHECK_EQ(live->task().window_progress(0.0f), 20u);
    T2D_CHECK_EQ(scene->world.layer().tick(0.8f), 0u);
    T2D_CHECK_EQ(live->task().held(), 0.8f);
    T2D_CHECK_EQ(live->task().current(), 0u);
    T2D_CHECK_EQ(live->container().count_of(ContentKind::Item, stone), 20u);

    const t2d::save::File file = save_rate_scene(*scene);
    const t2d::save::Chunk* task_chunk = file.find(game_save_chunk::kTasks);
    T2D_REQUIRE(task_chunk != nullptr);
    T2D_CHECK_EQ(task_chunk->version, kTasksChunkVersion);

    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(file, scene->content.registry, scene->content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    const std::vector<std::unique_ptr<Tile>>& plots = loaded->world->layer().plots();
    T2D_REQUIRE(plots.size() == 2u);
    Tile* restored = plots[block].get();
    T2D_REQUIRE(restored != nullptr);
    T2D_CHECK_EQ(restored->task().current(), 0u);
    T2D_CHECK_MSG(std::abs(restored->task().held() - 0.8f) < 1e-4f, "held: {}", restored->task().held());
    // The window came back with its remaining life: 20 items still satisfy the 15 the step asks for.
    T2D_CHECK_EQ(restored->task().window_progress(0.0f), 20u);
    T2D_CHECK_EQ(restored->container().count_of(ContentKind::Item, stone), 20u);
    // The hold continues from where it stopped: 1.3 more seconds reach 2.1 >= 2 and the step completes,
    // consuming exactly the 15 the window asked for.
    T2D_CHECK_EQ(loaded->world->layer().tick(1.3f), 0u);
    T2D_CHECK_EQ(restored->task().current(), 1u);
    T2D_CHECK_EQ(restored->task().held(), 0.0f);
    T2D_CHECK_EQ(restored->task().window_progress(1.3f), 0u);
    T2D_CHECK_EQ(restored->container().count_of(ContentKind::Item, stone), 5u);
}

T2D_TEST(a_finished_rate_task_comes_back_finished) {
    // A rate step that had already completed is just its count: held and window were cleared when it
    // advanced, so the save writes zeros for them and the load settles into the finished chain.
    std::optional<RateScene> scene = build_rate_scene();
    T2D_REQUIRE(scene.has_value());
    const ContentId stone = scene->stone;
    const usize block = scene->block_index;
    Tile* live = scene->world.layer().plots()[block].get();
    T2D_REQUIRE(live != nullptr);
    T2D_CHECK_EQ(scene->world.layer().tick(2.5f), 0u);
    T2D_CHECK_EQ(live->task().current(), 1u);
    T2D_CHECK_EQ(live->task().held(), 0.0f);
    T2D_CHECK_EQ(live->container().count_of(ContentKind::Item, stone), 5u);

    GameLoadReport report;
    const std::optional<GameSaveData> loaded = load_game_save(
        save_rate_scene(*scene), scene->content.registry, scene->content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    const std::vector<std::unique_ptr<Tile>>& plots = loaded->world->layer().plots();
    T2D_REQUIRE(plots.size() == 2u);
    Tile* restored = plots[block].get();
    T2D_REQUIRE(restored != nullptr);
    T2D_CHECK(restored->task().complete());
    T2D_CHECK_EQ(restored->task().held(), 0.0f);
    T2D_CHECK_EQ(restored->task().window_progress(0.0f), 0u);
    T2D_CHECK_EQ(restored->container().count_of(ContentKind::Item, stone), 5u);
}

T2D_TEST(a_version_1_task_chunk_still_loads_without_live_state) {
    // Version 1 knew only plot + completed. A load reads it as it was meant then: the count is kept, and
    // the hold and window start empty - live state it never wrote (docs/SAVE.md).
    std::optional<RateScene> scene = build_rate_scene();
    T2D_REQUIRE(scene.has_value());
    const ContentId stone = scene->stone;
    const usize block = scene->block_index;
    T2D_CHECK_EQ(scene->world.layer().tick(0.8f), 0u);

    t2d::save::File old = save_rate_scene(*scene);
    for (t2d::save::Chunk& chunk : old.chunks) {
        if (chunk.id != game_save_chunk::kTasks) continue;
        t2d::save::Writer writer;
        writer.write_u32(1);
        writer.write_u32(static_cast<t2d::u32>(block));
        writer.write_u32(0);
        chunk.payload = writer.finish();
        chunk.version = 1;
    }

    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(old, scene->content.registry, scene->content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    const std::vector<std::unique_ptr<Tile>>& plots = loaded->world->layer().plots();
    T2D_REQUIRE(plots.size() == 2u);
    Tile* restored = plots[block].get();
    T2D_REQUIRE(restored != nullptr);
    T2D_CHECK_EQ(restored->task().current(), 0u);
    T2D_CHECK_EQ(restored->task().held(), 0.0f);
    T2D_CHECK_EQ(restored->task().window_progress(0.0f), 0u); // no window in v1
    // The items were in the container at save time and stay there; with no window they are no progress
    // yet, so the step does not complete just because the box is full.
    T2D_CHECK_EQ(restored->container().count_of(ContentKind::Item, stone), 20u);
}

T2D_TEST(a_task_chunk_with_bad_live_state_is_refused) {
    // TASK v2's live state is validated like the rest of the chunk: a hold or a window record that could
    // not have been written by this game refuses the whole load (docs/SAVE.md, section 1.55).
    std::optional<RateScene> scene = build_rate_scene();
    T2D_REQUIRE(scene.has_value());
    T2D_CHECK_EQ(scene->world.layer().tick(0.8f), 0u);
    const t2d::save::File good = save_rate_scene(*scene);
    const t2d::u32 block = static_cast<t2d::u32>(scene->block_index);

    const auto with_payload = [](t2d::save::File file, const std::vector<t2d::u8>& payload,
                                 t2d::u32 version = kTasksChunkVersion) {
        for (t2d::save::Chunk& chunk : file.chunks) {
            if (chunk.id != game_save_chunk::kTasks) continue;
            chunk.payload = payload;
            chunk.version = version;
        }
        return file;
    };
    // One record: plot, completed, held, entries, then the window's (remaining, amount) pairs.
    const auto record = [&](t2d::save::Writer& writer, t2d::f32 held, t2d::u32 entries,
                            const std::vector<std::pair<t2d::f32, t2d::u32>>& window) {
        writer.write_u32(block);
        writer.write_u32(0);
        writer.write_f32(held);
        writer.write_u32(entries);
        for (const auto& [remaining, amount] : window) {
            writer.write_f32(remaining);
            writer.write_u32(amount);
        }
    };
    const auto refuses = [&](const t2d::save::File& file, std::string_view expected) {
        GameLoadReport report;
        T2D_CHECK_FALSE(
            load_game_save(file, scene->content.registry, scene->content.definitions, &report).has_value());
        T2D_CHECK_MSG(!report.error.empty() && report.error.find(expected) != std::string::npos,
                      "error: {}", report.error);
    };

    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.5f, runtime::content::kMaxTaskWindowEntries + 1u, {});
        refuses(with_payload(good, writer.finish()), "holds at most");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.5f, 1u, {{0.0f, 5u}});
        refuses(with_payload(good, writer.finish()), "finite time left");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.5f, 1u, {{-3.0f, 5u}});
        refuses(with_payload(good, writer.finish()), "finite time left");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.5f, 1u, {{std::numeric_limits<t2d::f32>::quiet_NaN(), 5u}});
        refuses(with_payload(good, writer.finish()), "finite time left");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.5f, 1u, {{4.0f, 0u}});
        refuses(with_payload(good, writer.finish()), "delivery of 0 items");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, -1.0f, 0u, {});
        refuses(with_payload(good, writer.finish()), "cannot be negative");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, std::numeric_limits<t2d::f32>::quiet_NaN(), 0u, {});
        refuses(with_payload(good, writer.finish()), "finite time");
    }
    {
        // The floor at plot 0 has no tasks.
        t2d::save::Writer writer;
        writer.write_u32(1);
        writer.write_u32(0);
        writer.write_u32(0);
        writer.write_f32(0.0f);
        writer.write_u32(0);
        refuses(with_payload(good, writer.finish()), "has no tasks");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(2);
        record(writer, 0.0f, 0u, {});
        record(writer, 0.0f, 0u, {});
        refuses(with_payload(good, writer.finish()), "twice");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.0f, 0u, {});
        writer.write_u32(0); // one trailing word
        refuses(with_payload(good, writer.finish()), "bytes after");
    }
    {
        t2d::save::Writer writer;
        writer.write_u32(1);
        record(writer, 0.0f, 0u, {});
        refuses(with_payload(good, writer.finish(), 3), "version 3");
    }
}

T2D_TEST(a_deliver_task_block_round_trips_without_a_window) {
    // A pure deliver chain writes TASK v2 with held 0 and no entries; the round trip is what it was.
    Fixture content;
    const std::optional<BuiltScene> scene = build_scene(content);
    T2D_REQUIRE(scene.has_value());
    const t2d::save::File file = make_test_save(*scene, content, test_session());
    const t2d::save::Chunk* task_chunk = file.find(game_save_chunk::kTasks);
    T2D_REQUIRE(task_chunk != nullptr);
    T2D_CHECK_EQ(task_chunk->version, kTasksChunkVersion);
    // count + plot + completed + held + entries: one record, no window.
    T2D_CHECK_EQ(task_chunk->payload.size(), sizeof(t2d::u32) * 5u);

    GameLoadReport report;
    const std::optional<GameSaveData> loaded =
        load_game_save(file, content.registry, content.definitions, &report);
    T2D_REQUIRE(loaded.has_value());
    T2D_CHECK(report.ok);
    const Tile* task_block = loaded->world->layer().plots()[scene->task_index].get();
    T2D_REQUIRE(task_block != nullptr);
    T2D_CHECK_EQ(task_block->task().current(), 0u);
    T2D_CHECK_EQ(task_block->task().held(), 0.0f);
    T2D_CHECK_EQ(task_block->task().window_progress(0.0f), 0u);
    T2D_CHECK_EQ(task_block->container().count_of(ContentKind::Item, scene->stone), 5u);
}

T2D_TEST_MAIN
