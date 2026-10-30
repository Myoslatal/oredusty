#include <mine/game_save.h>

#include <mine/types/drill.h>
#include <mine/types/entity_tile.h>

#include <format>
#include <utility>

namespace mine {

namespace {

using t2d::save::Reader;
using t2d::save::Writer;

/// Every chunk this game writes is at kGameSaveChunkVersion; a reader refuses anything else (a chunk
/// version is the game's, and this build does not know another one).
constexpr t2d::u32 kChunkVersion = kGameSaveChunkVersion;

/// Sanity limits on what one chunk may claim, so a corrupt save cannot ask for a huge table.
constexpr t2d::u32 kMaxPlots = 1u << 20;
constexpr t2d::u32 kMaxEntities = 1u << 20;
constexpr t2d::u32 kMaxBuilds = 1u << 16;
constexpr t2d::u32 kMaxSlots = 1u << 16;

/// One container slot as the PLOT chunk wrote it: content as the save's ids, plus the count.
struct SavedSlot {
    t2d::u8 kind = 0;
    t2d::u32 id = 0;
    t2d::u32 count = 0;
};

/// One plot as the PLOT chunk wrote it, before the layer is rebuilt out of it.
struct SavedPlot {
    t2d::u8 kind = 0;
    t2d::u32 id = 0;
    t2d::i32 layer = 0;
    t2d::i32 x = 0;
    t2d::i32 y = 0;
    t2d::i32 width = 1;
    t2d::i32 height = 1;
    bool mirrored = false;
    bool under_construction = false;
    bool ticking = false;
    t2d::f32 period = 0.0f;
    t2d::f32 since_last_update = 0.0f;
    bool drill = false;
    t2d::u32 produced = 0;
    bool has_container = false;
    std::vector<SavedSlot> slots;
};

/// One drop as the ENTT chunk wrote it.
struct SavedDrop {
    t2d::u8 kind = 0;
    t2d::u32 id = 0;
    t2d::f32 x = 0.0f;
    t2d::f32 y = 0.0f;
    t2d::u32 count = 1;
};

/// One sweep as the BUILD chunk wrote it: which plot (by its place in the save's order), and how far on.
struct SavedBuild {
    t2d::u32 plot = 0;
    t2d::f32 elapsed = 0.0f;
    t2d::f32 seconds = 0.0f;
};

/// The chunk a loader must have, or null with the reason in \p report. A chunk at another version is
/// refused too: a version is the game's statement that the bytes mean something else.
[[nodiscard]] const t2d::save::Chunk* required_chunk(const t2d::save::File& file, t2d::u32 id,
                                                     std::string_view name, GameLoadReport* report) {
    const t2d::save::Chunk* chunk = file.find(id);
    const auto fail = [report](std::string message) {
        if (report != nullptr) {
            report->ok = false;
            report->error = std::move(message);
        }
    };
    if (chunk == nullptr) {
        fail(std::format("the save has no {} chunk", name));
        return nullptr;
    }
    if (chunk->version != kChunkVersion) {
        fail(std::format("the {} chunk is at version {}, and this build reads version {}", name,
                         chunk->version, kChunkVersion));
        return nullptr;
    }
    return chunk;
}

} // namespace

t2d::save::File make_game_save(const MineWorld& world, const SessionConfig& session,
                               const BuildingUnlocks& unlocks, const ResearchState& research,
                               const BuildAnimation& builds, f32 world_seconds, t2d::Vec2 camera_center,
                               f32 camera_zoom, const ContentRegistry& registry) {
    t2d::save::File file;
    file.game = std::string(kGameSaveName);
    file.game_version = kGameSaveVersion;
    const MineLayer* layer = world.has_layer() ? &world.layer() : nullptr;

    // META: the session, both shapes and seeds, the layer's identity, the clock and the view.
    Writer meta;
    meta.write_u8(static_cast<t2d::u8>(session.mode));
    meta.write_u8(static_cast<t2d::u8>(session.role));
    meta.write_u32(session.seed);
    meta.write_u16(session.port);
    meta.write_string(session.connect_address);
    meta.write_u64(world.seed());
    meta.write_u32(world.shape().width);
    meta.write_u32(world.shape().height);
    meta.write_i32(world.shape().tile_layers);
    meta.write_i32(layer != nullptr ? layer->index() : -1);
    meta.write_u32(layer != nullptr ? layer->shape().width : 0u);
    meta.write_u32(layer != nullptr ? layer->shape().height : 0u);
    meta.write_i32(layer != nullptr ? layer->shape().tile_layers : 0);
    meta.write_u64(layer != nullptr ? layer->seed() : 0u);
    meta.write_f32(world_seconds);
    meta.write_f32(camera_center.x);
    meta.write_f32(camera_center.y);
    meta.write_f32(camera_zoom);
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kMeta, kChunkVersion, meta.finish()});

    // CONT: the table the ids in this save belong to, so a load can translate them by name.
    const ContentTable table = ContentTable::from_registry(registry);
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kContent, kChunkVersion, table.serialize()});

    // UNLK: which buildings this save has unlocked.
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kUnlocks, kChunkVersion, unlocks.serialize()});

    // RSCH: which research nodes this mine has studied (docs/GAME_DESIGN.md section 1.50).
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kResearch, kChunkVersion, research.serialize()});

    // PLOT: every plot, in the order it was placed - which is the order a load has to place it in again.
    Writer plots;
    plots.write_u32(layer != nullptr ? static_cast<t2d::u32>(layer->plots().size()) : 0u);
    if (layer != nullptr) {
        for (const std::unique_ptr<types::SceneTile>& plot : layer->plots()) {
            plots.write_u8(static_cast<t2d::u8>(plot->kind()));
            plots.write_u32(plot->id());
            plots.write_i32(plot->layer());
            plots.write_i32(plot->anchor().x);
            plots.write_i32(plot->anchor().y);
            plots.write_i32(plot->width());
            plots.write_i32(plot->height());
            plots.write_bool(plot->mirrored());
            plots.write_bool(plot->under_construction());
            const auto* ticking = dynamic_cast<const types::EntityTile*>(plot.get());
            plots.write_bool(ticking != nullptr);
            if (ticking != nullptr) {
                plots.write_f32(ticking->period_seconds());
                plots.write_f32(ticking->since_last_update());
            }
            const auto* drill = dynamic_cast<const types::Drill*>(plot.get());
            plots.write_bool(drill != nullptr);
            if (drill != nullptr) plots.write_u32(static_cast<t2d::u32>(drill->produced()));
            const types::Container* container = plot->container();
            plots.write_bool(container != nullptr);
            if (container != nullptr) {
                plots.write_u32(static_cast<t2d::u32>(container->slots().size()));
                for (const types::ContainerSlot& slot : container->slots()) {
                    plots.write_u8(static_cast<t2d::u8>(slot.kind));
                    plots.write_u32(slot.id);
                    plots.write_u32(static_cast<t2d::u32>(slot.count));
                }
            }
        }
    }
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kPlots, kChunkVersion, plots.finish()});

    // ENTT: the drops lying on the layer. Two passes, because a payload says how many records follow it.
    std::vector<const types::Drop*> drops;
    if (layer != nullptr) {
        for (const std::unique_ptr<types::Entity>& entity : layer->entities()) {
            // A drop that is gone but not yet swept up is not on the layer any more: a save is what is.
            if (!entity->alive()) continue;
            if (const auto* drop = dynamic_cast<const types::Drop*>(entity.get()); drop != nullptr) {
                drops.push_back(drop);
            }
        }
    }
    Writer entities;
    entities.write_u32(static_cast<t2d::u32>(drops.size()));
    for (const types::Drop* drop : drops) {
        entities.write_u8(static_cast<t2d::u8>(drop->kind()));
        entities.write_u32(drop->id());
        entities.write_f32(drop->position().x);
        entities.write_f32(drop->position().y);
        entities.write_u32(static_cast<t2d::u32>(drop->count()));
    }
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kEntities, kChunkVersion, entities.finish()});

    // BUILD: the sweeps in progress, by the plot's place in the PLOT list.
    std::vector<SavedBuild> records;
    if (layer != nullptr) {
        for (const BuildAnimation::Build& build : builds.builds()) {
            if (build.plot == nullptr) continue;
            const std::vector<std::unique_ptr<types::SceneTile>>& placed = layer->plots();
            for (t2d::usize index = 0; index < placed.size(); ++index) {
                if (placed[index].get() == build.plot) {
                    records.push_back(SavedBuild{static_cast<t2d::u32>(index), build.elapsed, build.seconds});
                    break;
                }
            }
        }
    }
    Writer build_payload;
    build_payload.write_u32(static_cast<t2d::u32>(records.size()));
    for (const SavedBuild& record : records) {
        build_payload.write_u32(record.plot);
        build_payload.write_f32(record.elapsed);
        build_payload.write_f32(record.seconds);
    }
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kBuilds, kChunkVersion, build_payload.finish()});

    return file;
}

std::optional<GameSaveData> load_game_save(const t2d::save::File& file, const ContentRegistry& registry,
                                           const ContentDefinitions& definitions, GameLoadReport* report) {
    if (report != nullptr) {
        report->ok = false;
        report->error.clear();
        report->warnings.clear();
        report->plots = 0;
        report->entities = 0;
    }
    const auto refuse = [report](std::string message) -> std::optional<GameSaveData> {
        if (report != nullptr) {
            report->ok = false;
            report->error = std::move(message);
        }
        return std::nullopt;
    };
    // --- the container and its chunks ---------------------------------------------------------------
    if (file.game != kGameSaveName) {
        return refuse(std::format("this is not a mine save: it says game '{}'", file.game));
    }
    if (file.game_version != kGameSaveVersion) {
        return refuse(std::format("the save is mine version {}, and this build reads version {}",
                                  file.game_version, kGameSaveVersion));
    }
    const t2d::save::Chunk* meta_chunk = required_chunk(file, game_save_chunk::kMeta, "META", report);
    const t2d::save::Chunk* content_chunk = required_chunk(file, game_save_chunk::kContent, "CONT", report);
    const t2d::save::Chunk* unlocks_chunk = required_chunk(file, game_save_chunk::kUnlocks, "UNLK", report);
    const t2d::save::Chunk* plots_chunk = required_chunk(file, game_save_chunk::kPlots, "PLOT", report);
    const t2d::save::Chunk* entities_chunk = required_chunk(file, game_save_chunk::kEntities, "ENTT", report);
    const t2d::save::Chunk* builds_chunk = required_chunk(file, game_save_chunk::kBuilds, "BUIL", report);
    if (meta_chunk == nullptr || content_chunk == nullptr || unlocks_chunk == nullptr ||
        plots_chunk == nullptr || entities_chunk == nullptr || builds_chunk == nullptr) {
        return std::nullopt;
    }

    // --- META: the session, the shapes and seeds, the clock and the view ----------------------------
    SessionConfig session;
    t2d::u64 world_seed = 0;
    LayerShape world_shape{};
    t2d::i32 layer_index = 0;
    LayerShape layer_shape{};
    t2d::u64 layer_seed = 0;
    t2d::f32 world_seconds = 0.0f;
    t2d::Vec2 camera_center{};
    t2d::f32 camera_zoom = 1.0f;
    {
        Reader reader(meta_chunk->payload);
        t2d::u8 mode = 0;
        t2d::u8 role = 0;
        if (!reader.read_u8(mode) || !reader.read_u8(role) || !reader.read_u32(session.seed) ||
            !reader.read_u16(session.port) || !reader.read_string(session.connect_address) ||
            !reader.read_u64(world_seed) || !reader.read_u32(world_shape.width) ||
            !reader.read_u32(world_shape.height) || !reader.read_i32(world_shape.tile_layers) ||
            !reader.read_i32(layer_index) || !reader.read_u32(layer_shape.width) ||
            !reader.read_u32(layer_shape.height) || !reader.read_i32(layer_shape.tile_layers) ||
            !reader.read_u64(layer_seed) || !reader.read_f32(world_seconds) ||
            !reader.read_f32(camera_center.x) || !reader.read_f32(camera_center.y) ||
            !reader.read_f32(camera_zoom)) {
            return refuse(std::format("the META chunk cannot be read: {}", reader.error()));
        }
        if (!reader.empty()) return refuse("the META chunk has bytes after its fields");
        if (mode > static_cast<t2d::u8>(Mode::Sandbox)) {
            return refuse("the META chunk names a mode this build does not know");
        }
        if (role > static_cast<t2d::u8>(Role::Join)) {
            return refuse("the META chunk names a role this build does not know");
        }
        session.mode = static_cast<Mode>(mode);
        session.role = static_cast<Role>(role);
    }
    if (layer_index < 0) return refuse("the save has no layer in it");
    if (layer_shape.width == 0 || layer_shape.height == 0 || layer_shape.tile_layers <= 0) {
        return refuse("the layer shape in the save is empty");
    }

    // --- CONT: the content table, and the translation by name --------------------------------------
    ContentTable table;
    if (!ContentTable::deserialize(ConstSpan<const t2d::u8>(content_chunk->payload), table)) {
        return refuse("the content table in the save cannot be read");
    }
    const ContentRemap remap = ContentRemap::build(table, registry);
    if (!remap.complete()) {
        std::string names;
        for (const std::string& name : remap.missing()) {
            if (!names.empty()) names += ", ";
            names += name;
            if (names.size() > 160u) {
                names += ", ...";
                break;
            }
        }
        return refuse(std::format("the save names {} content entries this build does not have: {}",
                                  remap.missing().size(), names));
    }

    // --- PLOT: every plot, read first and placed later in the saved order --------------------------
    std::vector<SavedPlot> saved_plots;
    {
        Reader reader(plots_chunk->payload);
        t2d::u32 count = 0;
        if (!reader.read_u32(count)) {
            return refuse(std::format("the PLOT chunk cannot be read: {}", reader.error()));
        }
        if (count > kMaxPlots) return refuse("the PLOT chunk claims more plots than a save may have");
        saved_plots.reserve(count);
        for (t2d::u32 index = 0; index < count; ++index) {
            SavedPlot plot;
            if (!reader.read_u8(plot.kind) || !reader.read_u32(plot.id) || !reader.read_i32(plot.layer) ||
                !reader.read_i32(plot.x) || !reader.read_i32(plot.y) || !reader.read_i32(plot.width) ||
                !reader.read_i32(plot.height) || !reader.read_bool(plot.mirrored) ||
                !reader.read_bool(plot.under_construction) || !reader.read_bool(plot.ticking)) {
                return refuse(std::format("a plot record cannot be read: {}", reader.error()));
            }
            if (static_cast<t2d::usize>(plot.kind) >= kContentKindCount) {
                return refuse("a plot record names a content kind this build does not know");
            }
            if (plot.width < 1 || plot.height < 1) return refuse("a plot record has an empty footprint");
            if (plot.ticking) {
                if (!reader.read_f32(plot.period) || !reader.read_f32(plot.since_last_update)) {
                    return refuse(std::format("a plot record's cadence cannot be read: {}", reader.error()));
                }
            }
            if (!reader.read_bool(plot.drill)) {
                return refuse(std::format("a plot record cannot be read: {}", reader.error()));
            }
            if (plot.drill && !reader.read_u32(plot.produced)) {
                return refuse(std::format("a drill record cannot be read: {}", reader.error()));
            }
            if (!reader.read_bool(plot.has_container)) {
                return refuse(std::format("a plot record cannot be read: {}", reader.error()));
            }
            if (plot.has_container) {
                t2d::u32 slots = 0;
                if (!reader.read_u32(slots)) {
                    return refuse(std::format("a container record cannot be read: {}", reader.error()));
                }
                if (slots > kMaxSlots) {
                    return refuse("a container record claims more slots than a save may have");
                }
                plot.slots.reserve(slots);
                for (t2d::u32 slot_index = 0; slot_index < slots; ++slot_index) {
                    SavedSlot slot;
                    if (!reader.read_u8(slot.kind) || !reader.read_u32(slot.id) ||
                        !reader.read_u32(slot.count)) {
                        return refuse(std::format("a container slot cannot be read: {}", reader.error()));
                    }
                    if (static_cast<t2d::usize>(slot.kind) >= kContentKindCount) {
                        return refuse("a container slot names a content kind this build does not know");
                    }
                    if (slot.count == 0) return refuse("a container slot records nothing");
                    plot.slots.push_back(slot);
                }
            }
            saved_plots.push_back(std::move(plot));
        }
        if (!reader.empty()) return refuse("the PLOT chunk has bytes after its last record");
    }

    // --- ENTT: the drops lying on the layer --------------------------------------------------------
    std::vector<SavedDrop> saved_drops;
    {
        Reader reader(entities_chunk->payload);
        t2d::u32 count = 0;
        if (!reader.read_u32(count)) {
            return refuse(std::format("the ENTT chunk cannot be read: {}", reader.error()));
        }
        if (count > kMaxEntities) return refuse("the ENTT chunk claims more entities than a save may have");
        saved_drops.reserve(count);
        for (t2d::u32 index = 0; index < count; ++index) {
            SavedDrop drop;
            if (!reader.read_u8(drop.kind) || !reader.read_u32(drop.id) || !reader.read_f32(drop.x) ||
                !reader.read_f32(drop.y) || !reader.read_u32(drop.count)) {
                return refuse(std::format("an entity record cannot be read: {}", reader.error()));
            }
            if (static_cast<t2d::usize>(drop.kind) >= kContentKindCount) {
                return refuse("an entity record names a content kind this build does not know");
            }
            if (drop.count == 0) return refuse("an entity record carries nothing");
            saved_drops.push_back(drop);
        }
        if (!reader.empty()) return refuse("the ENTT chunk has bytes after its last record");
    }

    // --- BUIL: the sweeps in progress --------------------------------------------------------------
    std::vector<SavedBuild> saved_builds;
    {
        Reader reader(builds_chunk->payload);
        t2d::u32 count = 0;
        if (!reader.read_u32(count)) {
            return refuse(std::format("the BUIL chunk cannot be read: {}", reader.error()));
        }
        if (count > kMaxBuilds) return refuse("the BUIL chunk claims more builds than a save may have");
        saved_builds.reserve(count);
        for (t2d::u32 index = 0; index < count; ++index) {
            SavedBuild build;
            if (!reader.read_u32(build.plot) || !reader.read_f32(build.elapsed) ||
                !reader.read_f32(build.seconds)) {
                return refuse(std::format("a build record cannot be read: {}", reader.error()));
            }
            saved_builds.push_back(build);
        }
        if (!reader.empty()) return refuse("the BUIL chunk has bytes after its last record");
    }

    // --- the layer, rebuilt in the saved order -----------------------------------------------------
    auto layer = std::make_unique<MineLayer>(layer_index, layer_shape, layer_seed);
    LayerBuildReport build_report;
    for (const SavedPlot& saved : saved_plots) {
        const auto kind = static_cast<ContentKind>(saved.kind);
        const ContentId current = remap.to_current(kind, saved.id);
        if (current == kNoContent) {
            return refuse(std::format("a plot names {} #{} that the save's content table cannot translate",
                                      content_kind_name(kind), saved.id));
        }
        const ContentEntry* entry = registry.find(kind, current);
        if (entry == nullptr) {
            return refuse(std::format("a plot names {} #{} that this build does not have",
                                      content_kind_name(kind), current));
        }
        PlacedContent where;
        where.kind = kind;
        where.name = entry->name;
        where.layer = saved.layer;
        where.anchor = GridPos{saved.x, saved.y};
        where.width = saved.width;
        where.height = saved.height;
        // Placed through the layer, not built by hand: the placement rules (footprint on the map, the
        // tile layer, a drill that has ore under it) are the same on load as they were the first time.
        types::SceneTile* plot = layer->place(registry, definitions, where, &build_report);
        if (plot == nullptr) {
            const std::string why = build_report.errors.empty() ? std::string("the layer refused it")
                                                                : build_report.errors.back();
            return refuse(std::format("plot '{}' at {},{} could not be placed again: {}", where.name, saved.x,
                                      saved.y, why));
        }
        plot->set_mirrored(saved.mirrored);
        plot->set_under_construction(saved.under_construction);
        if (saved.has_container) {
            types::Container* container = plot->container();
            if (container == nullptr) {
                return refuse(std::format("plot '{}' held things, and this build gives it no container",
                                          where.name));
            }
            for (const SavedSlot& slot : saved.slots) {
                const auto slot_kind = static_cast<ContentKind>(slot.kind);
                const ContentId slot_id = remap.to_current(slot_kind, slot.id);
                if (slot_id == kNoContent) {
                    return refuse(std::format("a slot of plot '{}' names content the save cannot translate",
                                              where.name));
                }
                if (container->add(slot_kind, slot_id, slot.count) != slot.count) {
                    return refuse(std::format("plot '{}' cannot hold the items it was saved with",
                                              where.name));
                }
            }
        }
        auto* ticking = dynamic_cast<types::EntityTile*>(plot);
        if (saved.ticking) {
            if (ticking == nullptr) {
                return refuse(std::format("plot '{}' was ticking, and this build made it scenery",
                                          where.name));
            }
            ticking->set_period_seconds(saved.period);
            ticking->set_since_last_update(saved.since_last_update);
        } else if (ticking != nullptr) {
            return refuse(std::format("plot '{}' was not ticking, and this build made it an entity plot",
                                      where.name));
        }
        auto* drill = dynamic_cast<types::Drill*>(plot);
        if (saved.drill) {
            if (drill == nullptr) {
                return refuse(std::format("plot '{}' was mining, and this build made it something else",
                                          where.name));
            }
            drill->set_produced(saved.produced);
        } else if (drill != nullptr) {
            return refuse(std::format("plot '{}' was not mining, and this build made it a drill",
                                      where.name));
        }
    }

    // The mirrored flag is a running count: the plots were placed one by one above, so it is recounted
    // here rather than trusted to have been kept.
    layer->recount_mirrored();

    // --- UNLK: which buildings the save had unlocked -----------------------------------------------
    BuildingUnlocks unlocks;
    unlocks.reset(registry, definitions);
    if (!unlocks.apply_saved(ConstSpan<const t2d::u8>(unlocks_chunk->payload), remap)) {
        return refuse("the UNLK chunk cannot be read");
    }

    // --- RSCH: which research nodes the save had studied -------------------------------------------
    // The chunk is optional: a save written before the technology tree existed has none, and comes back
    // with nothing researched (docs/GAME_DESIGN.md section 1.50). A chunk that is there but cannot be
    // read refuses the load like every other one.
    ResearchState research;
    research.reset(registry, definitions);
    if (const t2d::save::Chunk* research_chunk = file.find(game_save_chunk::kResearch);
        research_chunk != nullptr) {
        if (research_chunk->version != kChunkVersion) {
            return refuse(std::format("the RSCH chunk is at version {}, and this build reads version {}",
                                      research_chunk->version, kChunkVersion));
        }
        if (!research.apply_saved(ConstSpan<const t2d::u8>(research_chunk->payload), remap)) {
            return refuse("the RSCH chunk cannot be read");
        }
    }

    // --- ENTT again: the drops, adopted rather than dropped in, so a pile keeps its order and place ---
    for (const SavedDrop& saved : saved_drops) {
        const auto kind = static_cast<ContentKind>(saved.kind);
        const ContentId current = remap.to_current(kind, saved.id);
        if (current == kNoContent) {
            return refuse(std::format("a drop names {} #{} that the save's content table cannot translate",
                                      content_kind_name(kind), saved.id));
        }
        layer->adopt(std::make_unique<types::Drop>(kind, current, t2d::Vec2{saved.x, saved.y},
                                                   static_cast<t2d::usize>(saved.count)));
    }

    // --- BUIL again: the sweeps, each with the time it had already run -----------------------------
    // The chunk and the plots must agree: every record names a plot that is under construction, and every
    // plot that is under construction has a record. A save that disagrees is refused, not guessed at: the
    // two halves cannot both be right, and a plot that came back finished would hide a lost sweep.
    BuildAnimation builds;
    std::vector<bool> has_build(layer->plots().size(), false);
    for (const SavedBuild& saved : saved_builds) {
        if (static_cast<t2d::usize>(saved.plot) >= layer->plots().size()) {
            return refuse(std::format("the BUIL chunk names plot {}, and the save has {} plots", saved.plot,
                                      layer->plots().size()));
        }
        if (has_build[saved.plot]) {
            return refuse(std::format("the BUIL chunk names plot {} twice", saved.plot));
        }
        has_build[saved.plot] = true;
        types::SceneTile* plot = layer->plots()[saved.plot].get();
        if (!plot->under_construction()) {
            return refuse(std::format("the BUIL chunk names plot {}, and that plot is not under construction",
                                      saved.plot));
        }
        if (!builds.restore(plot, saved.seconds, saved.elapsed)) {
            return refuse(std::format("plot {} is under construction, and its build cannot be restored ({}s "
                                      "of {}s is not a sweep in progress)",
                                      saved.plot, saved.elapsed, saved.seconds));
        }
    }
    for (t2d::usize index = 0; index < layer->plots().size(); ++index) {
        if (layer->plots()[index]->under_construction() && !has_build[index]) {
            return refuse(std::format("plot {} is under construction, and the save has no build for it",
                                      index));
        }
    }

    auto world = std::make_unique<MineWorld>(world_seed, world_shape);
    world->install_layer(std::move(layer), std::move(build_report));

    GameSaveData data;
    data.session = session;
    data.world = std::move(world);
    data.unlocks = std::move(unlocks);
    data.research = std::move(research);
    data.builds = std::move(builds);
    data.world_seconds = world_seconds;
    data.camera_center = camera_center;
    data.camera_zoom = camera_zoom;
    data.plots = saved_plots.size();
    data.entities = saved_drops.size();
    if (report != nullptr) {
        report->ok = true;
        report->plots = data.plots;
        report->entities = data.entities;
    }
    return std::optional<GameSaveData>(std::move(data));
}

} // namespace mine
