#include <mine/game_save.h>

#include <mine/runtime/content/builtin.h>
#include <mine/runtime/component.h>
#include <mine/runtime/object_data.h>

#include <t2d/core/log.h>

#include <cmath>
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
constexpr t2d::u32 kMaxComponentRecords = 1u << 20;
constexpr t2d::u32 kMaxComponentBytes = 1u << 20;

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

/// One custom-component state record, as the CMPT chunk writes it.
struct SavedComponent {
    t2d::u32 plot = 0;
    t2d::u64 type_id = 0;
    t2d::u32 version = 0;
    t2d::u32 size = 0;
    std::vector<t2d::u8> bytes;
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
        for (const std::unique_ptr<types::Tile>& plot : layer->plots()) {
            plots.write_u8(static_cast<t2d::u8>(plot->kind()));
            plots.write_u32(plot->id());
            plots.write_i32(plot->layer());
            plots.write_i32(plot->anchor().x);
            plots.write_i32(plot->anchor().y);
            plots.write_i32(plot->width());
            plots.write_i32(plot->height());
            plots.write_bool(plot->mirrored());
            plots.write_bool(plot->under_construction());
            const bool ticking = plot->is_ticking();
            plots.write_bool(ticking);
            if (ticking) {
                plots.write_f32(plot->period_seconds());
                plots.write_f32(plot->since_last_update());
            }
            const bool drill = plot->is_drill();
            plots.write_bool(drill);
            if (drill) plots.write_u32(static_cast<t2d::u32>(plot->produced()));
            const types::Container container = plot->container();
            const std::vector<types::ContainerSlot> slots = container.slots();
            plots.write_bool(plot->has_container());
            if (plot->has_container()) {
                plots.write_u32(static_cast<t2d::u32>(slots.size()));
                for (const types::ContainerSlot& slot : slots) {
                    plots.write_u8(static_cast<t2d::u8>(slot.kind));
                    plots.write_u32(slot.id);
                    plots.write_u32(static_cast<t2d::u32>(slot.count));
                }
            }
        }
    }
    file.chunks.push_back(t2d::save::Chunk{game_save_chunk::kPlots, kChunkVersion, plots.finish()});

    // TASK: how far each task block's chain has got, and the live state its rate steps measure - the held
    // seconds and the current window's deliveries (docs/GAME_DESIGN.md sections 1.52-1.55). Optional: a
    // save with no task block writes no chunk, and a save written before tasks existed has none.
    if (layer != nullptr) {
        Writer task_states;
        t2d::u32 task_plots = 0;
        for (const std::unique_ptr<types::Tile>& plot : layer->plots()) {
            if (plot->has_task()) ++task_plots;
        }
        if (task_plots > 0) {
            // The window is written as **remaining** seconds, not the time each delivery happened: a
            // load's layer clock starts at 0, so a remaining life stays exactly as long as it was
            // without the save having to carry a clock at all (docs/SAVE.md).
            const t2d::f32 now = layer->clock_seconds();
            task_states.write_u32(task_plots);
            for (t2d::usize index = 0; index < layer->plots().size(); ++index) {
                const types::Tile& plot = *layer->plots()[index];
                if (!plot.has_task()) continue;
                task_states.write_u32(static_cast<t2d::u32>(index));
                const types::Task task = plot.task();
                const t2d::u32 current = static_cast<t2d::u32>(task.current());
                task_states.write_u32(current);
                // The built-in task data is read here the same way PLOT reads the other built-in
                // component fields: TASK is the task component's other half (docs/SAVE.md).
                const auto* task_data = static_cast<const runtime::content::TaskData*>(
                    plot.object()->instance(runtime::content::kTaskComponent));
                t2d::f32 window = 0.0f;
                if (const std::optional<types::TaskStepView> step = task.step(current);
                    step.has_value() && step->type == types::TaskType::Rate) {
                    window = step->window;
                }
                const t2d::f32 held = task_data != nullptr ? task_data->held : 0.0f;
                // Only a rate step's window is worth keeping; a deliver step's progress is the
                // container, which PLOT already holds.
                t2d::u32 entries = 0;
                if (task_data != nullptr && window > 0.0f) {
                    for (t2d::u32 slot = 0; slot < task_data->entries; ++slot) {
                        if (window - (now - task_data->recent[slot].at) > 0.0f) ++entries;
                    }
                }
                task_states.write_f32(held);
                task_states.write_u32(entries);
                for (t2d::u32 slot = 0; task_data != nullptr && slot < task_data->entries; ++slot) {
                    const t2d::f32 remaining = window - (now - task_data->recent[slot].at);
                    if (remaining <= 0.0f) continue; // already slid out of the window
                    task_states.write_f32(remaining);
                    task_states.write_u32(task_data->recent[slot].amount);
                }
            }
            file.chunks.push_back(
                t2d::save::Chunk{game_save_chunk::kTasks, kTasksChunkVersion, task_states.finish()});
        }
    }

    // CMPT: custom component state, keyed by the plot's place in PLOT. The built-in fields already in
    // PLOT are never written here; a component without a save callback, or with nothing to say, is
    // skipped. The chunk is omitted entirely when there is no state at all, so old saves and saves of
    // pure built-in content stay byte-for-byte what they were.
    std::vector<SavedComponent> component_records;
    if (layer != nullptr) {
        const std::vector<std::unique_ptr<types::Tile>>& plots_in_layer = layer->plots();
        for (t2d::usize plot_index = 0; plot_index < plots_in_layer.size(); ++plot_index) {
            const types::Tile& plot = *plots_in_layer[plot_index];
            const runtime::ObjectData* object = plot.object();
            if (object == nullptr) continue;
            for (const runtime::Archetype::ComponentLayout& layout : object->archetype().components()) {
                const runtime::ComponentType* type = layout.type;
                if (type == nullptr || runtime::content::is_builtin_component(type->type_id())) continue;
                if (!type->has_save()) continue;
                const void* data = plot.instance(type->type_id());
                if (data == nullptr) continue;
                const void* shared = plot.shared(type->type_id());
                const t2d::u32 size = type->save_instance(shared, data, nullptr, 0);
                if (size == 0) continue;
                if (size > kMaxComponentBytes) {
                    T2D_WARN("save: plot {} component {} needs {} bytes, over the save limit; skipped",
                             plot_index, type->type_id(), size);
                    continue;
                }
                SavedComponent record;
                record.plot = static_cast<t2d::u32>(plot_index);
                record.type_id = type->type_id();
                record.version = type->version();
                record.size = size;
                record.bytes.resize(size);
                if (type->save_instance(shared, data, record.bytes.data(), size) != size) {
                    T2D_WARN("save: plot {} component {} refused to write its state; skipped", plot_index,
                             type->type_id());
                    continue;
                }
                component_records.push_back(std::move(record));
            }
        }
    }
    if (!component_records.empty()) {
        Writer components;
        components.write_u32(static_cast<t2d::u32>(component_records.size()));
        for (const SavedComponent& record : component_records) {
            components.write_u32(record.plot);
            components.write_u64(record.type_id);
            components.write_u32(record.version);
            components.write_u32(record.size);
            components.write_bytes(ConstSpan<const t2d::u8>(record.bytes.data(), record.bytes.size()));
        }
        file.chunks.push_back(
            t2d::save::Chunk{game_save_chunk::kComponents, kChunkVersion, components.finish()});
    }

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
            const std::vector<std::unique_ptr<types::Tile>>& placed = layer->plots();
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
        types::Tile* plot = layer->place(registry, definitions, where, &build_report);
        if (plot == nullptr) {
            const std::string why = build_report.errors.empty() ? std::string("the layer refused it")
                                                                : build_report.errors.back();
            return refuse(std::format("plot '{}' at {},{} could not be placed again: {}", where.name, saved.x,
                                      saved.y, why));
        }
        plot->set_mirrored(saved.mirrored);
        plot->set_under_construction(saved.under_construction);
        if (saved.has_container) {
            if (!plot->has_container()) {
                return refuse(std::format("plot '{}' held things, and this build gives it no container",
                                          where.name));
            }
            types::Container container = plot->container();
            for (const SavedSlot& slot : saved.slots) {
                const auto slot_kind = static_cast<ContentKind>(slot.kind);
                const ContentId slot_id = remap.to_current(slot_kind, slot.id);
                if (slot_id == kNoContent) {
                    return refuse(std::format("a slot of plot '{}' names content the save cannot translate",
                                              where.name));
                }
                if (container.add(slot_kind, slot_id, slot.count) != slot.count) {
                    return refuse(std::format("plot '{}' cannot hold the items it was saved with",
                                              where.name));
                }
            }
        }
        if (saved.ticking) {
            if (!plot->is_ticking()) {
                return refuse(std::format("plot '{}' was ticking, and this build made it scenery",
                                          where.name));
            }
            plot->set_period_seconds(saved.period);
            plot->set_since_last_update(saved.since_last_update);
        } else if (plot->is_ticking()) {
            return refuse(std::format("plot '{}' was not ticking, and this build made it an entity plot",
                                      where.name));
        }
        if (saved.drill) {
            if (!plot->is_drill()) {
                return refuse(std::format("plot '{}' was mining, and this build made it something else",
                                          where.name));
            }
            plot->set_produced(saved.produced);
        } else if (plot->is_drill()) {
            return refuse(std::format("plot '{}' was not mining, and this build made it a drill",
                                      where.name));
        }
    }

    // The mirrored flag is a running count: the plots were placed one by one above, so it is recounted
    // here rather than trusted to have been kept.
    layer->recount_mirrored();

    // --- TASK: how far each task block's chain had got ---------------------------------------------
    // Optional: an old save (or one with no task block) has none, and every plot starts at 0
    // (docs/GAME_DESIGN.md section 1.52). A chunk that is there must name task plots, one record each; a
    // count past the chain this build gives the content is clamped, with a warning - the designer changed
    // the tasks, which is not a reason to lose the whole mine.
    if (const t2d::save::Chunk* task_chunk = file.find(game_save_chunk::kTasks); task_chunk != nullptr) {
        // Version 1 is the old shape (plot index + completed count); version 2 adds the live rate state
        // - held seconds and the window's deliveries as remaining seconds plus amount. Both are read;
        // a save written against version 1 comes back with no hold and an empty window, which is what
        // it meant then (docs/SAVE.md, docs/GAME_DESIGN.md section 1.55).
        if (task_chunk->version != 1 && task_chunk->version != kTasksChunkVersion) {
            return refuse(std::format("the TASK chunk is at version {}, and this build reads versions 1 and {}",
                                      task_chunk->version, kTasksChunkVersion));
        }
        const bool live_state = task_chunk->version >= 2;
        Reader reader(task_chunk->payload);
        t2d::u32 count = 0;
        if (!reader.read_u32(count)) {
            return refuse(std::format("the TASK chunk cannot be read: {}", reader.error()));
        }
        if (count > kMaxPlots) return refuse("the TASK chunk claims more records than a save may have");
        std::vector<bool> seen(layer->plots().size(), false);
        for (t2d::u32 index = 0; index < count; ++index) {
            t2d::u32 plot_index = 0;
            t2d::u32 completed = 0;
            if (!reader.read_u32(plot_index) || !reader.read_u32(completed)) {
                return refuse(std::format("a TASK record cannot be read: {}", reader.error()));
            }
            t2d::f32 held = 0.0f;
            t2d::u32 entries = 0;
            if (live_state && (!reader.read_f32(held) || !reader.read_u32(entries))) {
                return refuse(std::format("a TASK record cannot be read: {}", reader.error()));
            }
            if (static_cast<t2d::usize>(plot_index) >= layer->plots().size()) {
                return refuse(std::format("the TASK chunk names plot {}, and the save has {} plots", plot_index,
                                          layer->plots().size()));
            }
            if (seen[plot_index]) {
                return refuse(std::format("the TASK chunk names plot {} twice", plot_index));
            }
            seen[plot_index] = true;
            types::Tile* plot = layer->plots()[plot_index].get();
            if (!plot->has_task()) {
                return refuse(
                    std::format("the TASK chunk names plot {}, and that plot has no tasks", plot_index));
            }
            if (live_state) {
                if (!std::isfinite(held) || held < 0.0f) {
                    return refuse(std::format("the TASK record for plot {} says {} second(s) of hold, and a "
                                              "hold is a finite time that cannot be negative",
                                              plot_index, held));
                }
                if (entries > runtime::content::kMaxTaskWindowEntries) {
                    return refuse(std::format("the TASK record for plot {} holds {} deliveries, and a window "
                                              "holds at most {}",
                                              plot_index, entries,
                                              runtime::content::kMaxTaskWindowEntries));
                }
            }
            // The window's records are read now and placed on the clock below, once the step they
            // belong to is known: a saved entry says how much life it had left, and that life does not
            // depend on the layer clock the save was written at.
            std::vector<std::pair<t2d::f32, t2d::u32>> window_entries;
            window_entries.reserve(entries);
            for (t2d::u32 slot = 0; slot < entries; ++slot) {
                t2d::f32 remaining = 0.0f;
                t2d::u32 amount = 0;
                if (!reader.read_f32(remaining) || !reader.read_u32(amount)) {
                    return refuse(std::format("a TASK window record cannot be read: {}", reader.error()));
                }
                if (!std::isfinite(remaining) || remaining <= 0.0f) {
                    return refuse(std::format("the TASK window of plot {} says {} second(s) remain, and a "
                                              "delivery has to have a finite time left",
                                              plot_index, remaining));
                }
                if (amount == 0) {
                    return refuse(
                        std::format("the TASK window of plot {} records a delivery of 0 items", plot_index));
                }
                window_entries.emplace_back(remaining, amount);
            }
            const t2d::usize total = plot->task().count();
            if (static_cast<t2d::usize>(completed) > total) {
                if (report != nullptr) {
                    report->warnings.push_back(std::format(
                        "plot '{}' at {},{} says {} task(s) are done, and this build gives it {}; the count "
                        "is clamped",
                        plot->name(registry), plot->anchor().x, plot->anchor().y, completed, total));
                }
                completed = static_cast<t2d::u32>(total);
            }
            if (!plot->set_task_completed(completed)) {
                return refuse(std::format("plot {} has no task component to restore", plot_index));
            }
            // The live state after the clamped count: a save that had just reached the hold's end must
            // settle into the completed step below, consuming its items as it would have in play.
            auto* task_data = static_cast<runtime::content::TaskData*>(
                plot->object()->instance(runtime::content::kTaskComponent));
            if (task_data == nullptr) {
                return refuse(std::format("plot {} has no task component to restore", plot_index));
            }
            // The window drops an entry once it is a whole window old (at <= now - window). On a clock
            // that starts at 0, a delivery that had "remaining" seconds left therefore sits at
            // remaining - window: from now 0 it survives exactly that long. (Putting it at -remaining
            // would place it a whole window too early and lose it on the first ticks.)
            t2d::f32 window_seconds = 0.0f;
            if (const std::optional<types::TaskStepView> step = plot->task().step(plot->task().current());
                step.has_value()) {
                window_seconds = step->window;
            }
            if (entries > 0 && window_seconds <= 0.0f) {
                return refuse(std::format("the TASK record for plot {} has a delivery window, and its "
                                          "current step is not a rate step",
                                          plot_index));
            }
            task_data->held = held;
            task_data->entries = entries;
            for (t2d::u32 slot = 0; slot < entries; ++slot) {
                task_data->recent[slot] =
                    runtime::content::TaskDelivery{window_entries[slot].first - window_seconds,
                                                   window_entries[slot].second};
            }
        }
        if (!reader.empty()) return refuse("the TASK chunk has bytes after its last record");
    }

    // With the containers and the counts back, settle each task plot once: whatever the container holds
    // toward the current step is delivered now, so a load and a live play agree about the box.
    for (const std::unique_ptr<types::Tile>& plot : layer->plots()) {
        if (plot->has_task()) (void)plot->run_tasks();
    }

    // --- CMPT: custom component state, if this save has any -----------------------------------------
    // Applied only after every plot exists. A missing chunk is the old format (or pure built-in content)
    // and is fine. A record naming a built-in component is refused: PLOT owns those fields.
    if (const t2d::save::Chunk* component_chunk = file.find(game_save_chunk::kComponents);
        component_chunk != nullptr) {
        if (component_chunk->version != kChunkVersion) {
            return refuse(std::format("the CMPT chunk is at version {}, and this build reads version {}",
                                      component_chunk->version, kChunkVersion));
        }
        Reader reader(component_chunk->payload);
        t2d::u32 count = 0;
        if (!reader.read_u32(count)) {
            return refuse(std::format("the CMPT chunk cannot be read: {}", reader.error()));
        }
        if (count > kMaxComponentRecords) {
            return refuse("the CMPT chunk claims more component records than a save may have");
        }
        std::vector<std::pair<t2d::u32, t2d::u64>> seen;
        seen.reserve(count);
        for (t2d::u32 index = 0; index < count; ++index) {
            t2d::u32 plot_index = 0;
            t2d::u64 type_id = 0;
            t2d::u32 version = 0;
            t2d::u32 size = 0;
            if (!reader.read_u32(plot_index) || !reader.read_u64(type_id) || !reader.read_u32(version) ||
                !reader.read_u32(size)) {
                return refuse(std::format("a CMPT record cannot be read: {}", reader.error()));
            }
            if (plot_index >= layer->plots().size()) {
                return refuse(std::format("the CMPT chunk names plot {}, and the save has {} plots",
                                          plot_index, layer->plots().size()));
            }
            if (size == 0 || size > kMaxComponentBytes) {
                return refuse("a CMPT record has an unusable state size");
            }
            types::Tile* plot = layer->plots()[plot_index].get();
            const runtime::ObjectData* object = plot->object();
            const runtime::ComponentType* type =
                object != nullptr ? object->archetype().type(type_id) : nullptr;
            if (type == nullptr) {
                return refuse(std::format("plot {} carries component type {}, which this build does not have",
                                          plot_index, type_id));
            }
            if (runtime::content::is_builtin_component(type_id)) {
                return refuse(std::format("the CMPT chunk names built-in component {}; PLOT owns that state",
                                          type_id));
            }
            if (type->version() != version) {
                return refuse(std::format("component type {} is version {}, and the save says {}", type_id,
                                          type->version(), version));
            }
            if (!type->has_load()) {
                return refuse(std::format("component type {} has no load callback in this build", type_id));
            }
            for (const auto& [saved_plot, saved_type] : seen) {
                if (saved_plot == plot_index && saved_type == type_id) {
                    return refuse(std::format("the CMPT chunk names plot {} component {} twice", plot_index,
                                              type_id));
                }
            }
            seen.emplace_back(plot_index, type_id);
            std::vector<t2d::u8> bytes;
            if (!reader.read_bytes(size, bytes)) {
                return refuse(std::format("a CMPT state record cannot be read: {}", reader.error()));
            }
            void* data = const_cast<void*>(plot->instance(type_id));
            if (data == nullptr) {
                return refuse(std::format("plot {} has no data for component {}", plot_index, type_id));
            }
            if (type->load_instance(plot->shared(type_id), data, bytes.data(), size) != size) {
                return refuse(
                    std::format("plot {} component {} refused its saved state", plot_index, type_id));
            }
        }
        if (!reader.empty()) return refuse("the CMPT chunk has bytes after its last record");
    }

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
        types::Tile* plot = layer->plots()[saved.plot].get();
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
