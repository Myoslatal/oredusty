#include <mine/world.h>

#include <t2d/core/log.h>

#include <format>

namespace mine {
namespace {

/// Every registered content entry that occupies cells, in registration order: what the debug
/// generators lay out. Nothing is named here - the registry is what says which content exists.
[[nodiscard]] std::vector<PlacedContent> registered_plots(const ContentRegistry& registry, LayerShape shape) {
    std::vector<PlacedContent> entries;
    for (usize kind_index = 0; kind_index < kContentKindCount; ++kind_index) {
        const auto kind = static_cast<ContentKind>(kind_index);
        if (!types::kind_is_a_tile(kind)) continue;
        for (const ContentEntry& entry : registry.entries(kind)) {
            PlacedContent placed;
            placed.kind = entry.kind;
            placed.name = entry.name;
            // One entry per tile layer, in registration order, wrapping at the layer count: an
            // arbitrary but deterministic choice, made so a stack of kinds can be told apart on screen.
            placed.layer = shape.tile_layers > 0
                               ? static_cast<i32>(entries.size() % static_cast<usize>(shape.tile_layers))
                               : 0;
            entries.push_back(std::move(placed));
        }
    }
    return entries;
}

/// The ore a drill standing on \p rig_layer at \p cell would mine: the topmost plot that is ore on a
/// tile layer below it, or nullptr. Ore is on a layer under the drill because a plot occupies cells and
/// one cell holds one plot per tile layer - which is what tile layers are for (docs/GAME_DESIGN.md
/// sections 1.12, 1.26).
[[nodiscard]] const types::SceneTile* ore_under(const MineLayer& layer, i32 rig_layer, GridPos cell) {
    for (i32 tile_layer = rig_layer - 1; tile_layer >= 0; --tile_layer) {
        const types::SceneTile* plot = layer.plot_at(tile_layer, cell);
        if (plot != nullptr && plot->is_ore()) return plot;
    }
    return nullptr;
}

/// How a placement is named in a message: kind, name, the cells it covers and the tile layer.
[[nodiscard]] std::string describe(const PlacedContent& where, const t2d::TileRect& cells) {
    return std::format("{} '{}' at {},{} {}x{} on tile layer {}", content_kind_name(where.kind), where.name,
                       cells.x, cells.y, cells.width, cells.height, where.layer);
}

} // namespace

u64 layer_seed(u64 world_seed, i32 index) {
    // The world's seed mixed with the layer index, then finalised the way splitmix64 does: a mine whose
    // layers differed by one has to give streams that are not related to each other.
    u64 mixed = world_seed ^ (0x9E3779B97F4A7C15ull * (static_cast<u64>(static_cast<u32>(index)) + 1u));
    mixed += 0x9E3779B97F4A7C15ull;
    mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
    mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
    return mixed ^ (mixed >> 31);
}

LayerGenerator empty_layer_generator(LayerShape shape) {
    return [shape](u64 seed, i32 index) {
        (void)seed;
        LayerSpec spec;
        spec.index = index;
        spec.shape = shape;
        return spec;
    };
}

LayerGenerator debug_band_generator(const ContentRegistry& registry, LayerShape shape) {
    return [&registry, shape](u64 seed, i32 index) {
        (void)seed;
        LayerSpec spec;
        spec.index = index;
        spec.shape = shape;
        const std::vector<PlacedContent> entries = registered_plots(registry, shape);
        if (entries.empty() || shape.height == 0) return spec;
        for (u32 y = 0; y < shape.height; ++y) {
            const usize band = std::min(entries.size() - 1, static_cast<usize>(y) * entries.size() / shape.height);
            for (u32 x = 0; x < shape.width; ++x) {
                PlacedContent where = entries[band];
                where.anchor = GridPos{static_cast<i32>(x), static_cast<i32>(y)};
                spec.content.push_back(std::move(where));
            }
        }
        return spec;
    };
}

LayerGenerator debug_scatter_generator(const ContentRegistry& registry, LayerShape shape, u32 percent) {
    return [&registry, shape, percent](u64 seed, i32 index) {
        LayerSpec spec;
        spec.index = index;
        spec.shape = shape;
        const std::vector<PlacedContent> entries = registered_plots(registry, shape);
        if (entries.empty()) return spec;
        // The layer's own dice, from the pair the generator is handed: the same (seed, index) gives the
        // same layer back, and two layers of one mine are not the same map.
        t2d::Rng rng(layer_seed(seed, index));
        const u32 chance = std::min(percent, 100u);
        for (u32 y = 0; y < shape.height; ++y) {
            for (u32 x = 0; x < shape.width; ++x) {
                if (rng.next_bounded(100) >= chance) continue;
                PlacedContent where = entries[rng.next_bounded(static_cast<u32>(entries.size()))];
                where.anchor = GridPos{static_cast<i32>(x), static_cast<i32>(y)};
                spec.content.push_back(std::move(where));
            }
        }
        return spec;
    };
}

usize lay_out_drops(MineLayer& layer, const ContentRegistry& registry,
                     const ContentDefinitions& definitions) {
    const ConstSpan<ContentEntry> drops = registry.entries(ContentKind::Drop);
    if (drops.empty()) return 0;
    // Evenly spaced across the middle of the layer, alternating slightly above and below the middle row -
    // and deliberately not on whole cells, because a drop's position is a point rather than a cell.
    const f32 middle = static_cast<f32>(layer.height()) * 0.5f;
    usize placed = 0;
    for (usize index = 0; index < drops.size(); ++index) {
        const f32 across = (static_cast<f32>(index) + 1.0f) / (static_cast<f32>(drops.size()) + 1.0f);
        const t2d::Vec2 where{static_cast<f32>(layer.width()) * across,
                              middle + (index % 2 == 0 ? 0.25f : -0.25f)};
        // Built through the definer when the load read one - the road a plot takes through make_tile -
        // and as a plain drop when the content was registered without a file behind it.
        if (const types::DropDefinition* definition = definitions.find_drop(drops[index].name);
            definition != nullptr) {
            layer.adopt(types::make_drop(*definition, drops[index].id, where));
        } else {
            layer.adopt(std::make_unique<types::Drop>(drops[index].kind, drops[index].id, where));
        }
        ++placed;
    }
    return placed;
}

void MineLayer::configure_drill(types::Drill& drill, const ContentRegistry& registry,
                                const ContentDefinitions& definitions, LayerBuildReport* report) {
    const auto refuse = [&](std::string message) {
        if (report != nullptr) report->refuse(std::move(message));
    };
    const std::string at = std::format("{},{}", drill.anchor().x, drill.anchor().y);
    const std::string what = std::format("the drill '{}' at {}", drill.name(registry), at);
    if (drill.dig_power() <= 0.0f) {
        refuse(std::format("layer {}: {} says nothing about its dig_power, so there is no rate to mine at",
                           index_, what));
        return;
    }
    const types::SceneTile* ore = ore_under(*this, drill.layer(), drill.anchor());
    if (ore == nullptr) {
        refuse(std::format("layer {}: {} has no ore under it (a drill mines what is on a tile layer below "
                           "the one it stands on)",
                           index_, what));
        return;
    }
    const std::string ore_name = ore->name(registry);
    const types::TileDefinition* definition = definitions.find_tile(ore->kind(), ore_name);
    if (definition == nullptr) {
        refuse(std::format("layer {}: the ore '{}' under {} was registered without a file, so nothing says "
                           "how hard it is or what it drops",
                           index_, ore_name, what));
        return;
    }
    if (definition->hardness <= 0.0f) {
        refuse(std::format("layer {}: the ore '{}' under {} says nothing about its hardness", index_,
                           ore_name, what));
        return;
    }
    if (definition->drop.empty()) {
        refuse(std::format("layer {}: the ore '{}' under {} says nothing about what it drops", index_,
                           ore_name, what));
        return;
    }
    const ContentId drop = registry.find(ContentKind::Drop, definition->drop);
    if (drop == kNoContent) {
        refuse(std::format("layer {}: the ore '{}' under {} drops '{}', and no drop named '{}' is registered",
                           index_, ore_name, what, definition->drop, definition->drop));
        return;
    }
    types::DrillJob job;
    job.ore_kind = ore->kind();
    job.ore_id = ore->id();
    job.ore_name = ore_name;
    job.drop_id = drop;
    job.drop_name = definition->drop;
    // The rule the designer asked for: the ore's hardness over the drill's power, in seconds per drop
    // (docs/GAME_DESIGN.md section 1.26).
    job.period_seconds = definition->hardness / drill.dig_power();
    drill.set_job(std::move(job));
    T2D_INFO("world: the drill '{}' at {} mines ore '{}' (hardness {:.2f} / power {:.2f} = {:.2f}s per drop "
             "of '{}')",
             drill.name(registry), at, ore_name, definition->hardness, drill.dig_power(),
             drill.job().period_seconds, definition->drop);
}

usize lay_out_drills(MineLayer& layer, const ContentRegistry& registry,
                     const ContentDefinitions& definitions) {
    const ConstSpan<ContentEntry> drills = registry.entries(ContentKind::Drill);
    // A drill stands above what it mines, so the top tile layer is where this fill puts it - and a layer
    // with only one tile layer has no room for both (docs/GAME_DESIGN.md section 1.26).
    const i32 rig_layer = layer.layer_count() - 1;
    if (drills.empty() || rig_layer < 1) return 0;
    usize placed = 0;
    usize next_drill = 0;
    for (i32 y = 0; y < static_cast<i32>(layer.height()) && next_drill < drills.size(); ++y) {
        for (i32 x = 0; x < static_cast<i32>(layer.width()) && next_drill < drills.size(); ++x) {
            const GridPos cell{x, y};
            if (layer.plot_at(rig_layer, cell) != nullptr) continue;   // something is already on top
            if (ore_under(layer, rig_layer, cell) == nullptr) continue;
            PlacedContent where;
            where.kind = ContentKind::Drill;
            where.name = drills[next_drill].name;
            where.layer = rig_layer;
            where.anchor = cell;
            // The fill's own report: a drill it cannot place is logged with the reason, never silent.
            LayerBuildReport report;
            if (layer.place(registry, definitions, where, &report) != nullptr) ++placed;
            for (const std::string& error : report.errors) T2D_WARN("world: {}", error);
            ++next_drill;
        }
    }
    return placed;
}

// ----------------------------------------------------------------------- the layer ---

void LayerBuildReport::refuse(std::string message) {
    ++refused;
    if (errors.size() >= kMaxErrors) return;
    if (std::find(errors.begin(), errors.end(), message) != errors.end()) return;
    errors.push_back(std::move(message));
}

MineLayer::MineLayer(i32 index, LayerShape shape, u64 seed)
    : index_(index), shape_(shape), seed_(seed), rng_(seed),
      cells_(shape.width, shape.height, shape.tile_layers),
      // The index is over the map: what a radius query is bounded by, and what its root rectangle is.
      // A position outside it is still kept - it is just not found by a query that does not cover it
      // (quadtree.h).
      entity_index_(t2d::Aabb2::from_top_left(t2d::Vec2{0.0f, 0.0f},
                                              t2d::Vec2{static_cast<f32>(cells_.width()),
                                                        static_cast<f32>(cells_.height())})) {
    shape_.width = cells_.width();
    shape_.height = cells_.height();
    shape_.tile_layers = cells_.layer_count();
}

types::SceneTile* MineLayer::place(const ContentRegistry& registry, const ContentDefinitions& definitions,
                                   const PlacedContent& where, LayerBuildReport* report) {
    const auto refuse = [&](std::string message) -> types::SceneTile* {
        if (report != nullptr) report->refuse(std::move(message));
        return nullptr;
    };
    if (!types::kind_is_a_tile(where.kind)) {
        return refuse(std::format("layer {}: a {} is not something that occupies cells", index_,
                                  content_kind_name(where.kind)));
    }
    const t2d::TileRect cells = types::footprint(where.anchor, where.width, where.height);
    if (where.layer < 0 || where.layer >= layer_count()) {
        return refuse(std::format("layer {}: {} names tile layer {}, and this layer has {}", index_,
                                  describe(where, cells), where.layer, layer_count()));
    }
    if (cells.x < 0 || cells.y < 0 || cells.right() > static_cast<i32>(width()) ||
        cells.bottom() > static_cast<i32>(height())) {
        return refuse(std::format("layer {}: {} does not fit a {}x{} map", index_, describe(where, cells), width(),
                                  height()));
    }
    const ContentId id = registry.find(where.kind, where.name);
    if (id == kNoContent) {
        return refuse(std::format("layer {}: no {} named '{}' is registered", index_,
                                  content_kind_name(where.kind), where.name));
    }
    // A plot is built out of its definition. Content with none - a name a mod registered through the C
    // ABI, with no file behind it - is a plot with no picture and nothing to turn around, not a plot
    // that cannot be placed.
    types::TileDefinition fallback;
    fallback.kind = where.kind;
    fallback.name = where.name;
    const types::TileDefinition* definition = definitions.find_tile(where.kind, where.name);
    if (definition == nullptr) definition = &fallback;
    std::unique_ptr<types::SceneTile> plot =
        types::make_tile(*definition, id, where.layer, where.anchor, cells.width, cells.height);
    if (plot == nullptr) {
        return refuse(std::format("layer {}: {} cannot be built into a plot", index_, describe(where, cells)));
    }

    // Overlap is a question about one tile layer: a machine and the floor under it are on two layers,
    // and both are wanted. The check goes through the chunk index, so it costs what is near the plot
    // rather than what the layer holds.
    const types::SceneTile* clash = nullptr;
    for_each_plot_in(cells, [&](const types::SceneTile& other) {
        if (clash != nullptr || other.layer() != plot->layer()) return;
        if (other.overlaps(*plot)) clash = &other;
    });
    if (clash != nullptr) {
        return refuse(std::format("layer {}: {} overlaps {} '{}' at {},{}", index_, describe(where, cells),
                                  content_kind_name(clash->kind()), clash->name(registry), clash->anchor().x,
                                  clash->anchor().y));
    }

    // The dice, rolled once, here, while the map is being built (docs/GAME_DESIGN.md section 1.14). A
    // plot whose content may not be turned around never touches the generator.
    plot->randomise_mirror(rng_);
    const bool mirrored = plot->mirrored();
    const bool ticking = dynamic_cast<types::EntityTile*>(plot.get()) != nullptr;
    if (mirrored) ++mirrored_;

    // The cells: every cell of the footprint points at the content, so the grid alone answers "what is
    // here" for a save, a routing query and a renderer's cull without walking the plots.
    for (i32 y = cells.y; y < cells.bottom(); ++y) {
        for (i32 x = cells.x; x < cells.right(); ++x) {
            cells_.set(where.layer, GridPos{x, y}, ContentRef{where.kind, id, false});
        }
    }
    const usize serial = plots_.size();
    if (ticking) ticking_.push_back(static_cast<types::EntityTile*>(plot.get()));
    // A plot is placed *into* a layer, and what it does while it runs happens in that layer: a drill
    // making a drop is the first thing that needs to know which one (types/tile.h, types/drill.h).
    plot->owner_ = this;
    plots_.push_back(std::move(plot));
    index_plot(serial);

    // A drill is told what it stands on here, while the layer is built: this is the one moment the
    // registry (names to ids) and the definitions (the designer's numbers) are both in hand, and a drill
    // with nothing to mine is then a **reported** refusal instead of a machine that does nothing forever
    // (docs/GAME_DESIGN.md section 1.26).
    if (auto* drill = dynamic_cast<types::Drill*>(plots_[serial].get()); drill != nullptr) {
        configure_drill(*drill, registry, definitions, report);
    }

    if (report != nullptr) {
        ++report->placed;
        report->cells += static_cast<usize>(cells.width) * static_cast<usize>(cells.height);
        if (ticking) ++report->ticking;
        if (mirrored) ++report->mirrored;
    }
    return plots_[serial].get();
}

void MineLayer::build(const LayerSpec& spec, const ContentRegistry& registry,
                      const ContentDefinitions& definitions, LayerBuildReport* report) {
    for (const PlacedContent& where : spec.content) (void)place(registry, definitions, where, report);
}

void MineLayer::index_plot(usize serial) {
    const t2d::TileRect& cells = plots_[serial]->cells();
    const auto key = static_cast<u32>(serial);
    for (i32 cy = chunk_of(cells.y); cy <= chunk_of(cells.bottom() - 1); ++cy) {
        for (i32 cx = chunk_of(cells.x); cx <= chunk_of(cells.right() - 1); ++cx) {
            chunks_[chunk_key(cx, cy)].push_back(key);
        }
    }
}

usize MineLayer::refresh() {
    usize ran = 0;
    for (const std::unique_ptr<types::SceneTile>& plot : plots_) {
        if (plot->refresh()) ++ran;
    }
    return ran;
}

usize MineLayer::tick(f32 delta_seconds) {
    usize ran = 0;
    for (types::EntityTile* plot : ticking_) {
        if (plot->advance(delta_seconds)) ++ran;
    }
    // The entities are walked and each one decides for itself: a cadence belongs to the thing that owns
    // it, and an entity that says "not me" costs one branch rather than a list the layer would have to
    // keep in step with what it said (types/entity.h). The count is taken before the walk, so an entity
    // that spawns another during its turn does not have that one run in the same tick.
    const usize count = entities_.size();
    for (usize index = 0; index < count; ++index) {
        types::Entity* entity = entities_[index].get();
        if (entity->alive() && entity->advance(delta_seconds)) ++ran;
    }
    sweep_destroyed();
    return ran;
}

// ----------------------------------------------------------------------- entities ---

types::Entity& MineLayer::adopt(std::unique_ptr<types::Entity> entity) {
    entity->layer_ = this;
    entity->serial_ = next_serial_++;
    entity->alive_ = true;
    types::Entity* raw = entity.get();
    entities_.push_back(std::move(entity));
    entity_index_.insert(raw->serial_, raw->position_, raw);
    return *raw;
}

types::Entity* MineLayer::spawn(ContentKind kind, ContentId id, t2d::Vec2 position) {
    // An entity no save can name is not something a layer should hold: the id is what a save stores,
    // and kNoContent is what "there is no content here" is spelled as (registry.h).
    if (id == kNoContent) return nullptr;
    return &adopt(std::make_unique<types::Entity>(kind, id, position));
}

bool MineLayer::destroy(types::Entity& entity) {
    if (!entity.alive() || entity.layer_ != this) return false;
    // Out of the index at once, so a query in the same tick cannot find it; out of the storage at the
    // end of the pass, because whoever is walking the entities right now is holding this one.
    entity.alive_ = false;
    entity_index_.remove(entity.serial_, entity.position_);
    destroyed_.push_back(&entity);
    return true;
}

bool MineLayer::interact(types::Entity& a, types::Entity& b) {
    if (!a.alive() || !b.alive()) return false;
    // Both sides are asked, and neither is privileged: an interaction is between two things, and the
    // default (do nothing, say so) costs one call each (types/entity.h).
    const bool first = a.on_interact(b);
    const bool second = b.on_interact(a);
    return first || second;
}

std::vector<types::Entity*> MineLayer::entities_within(t2d::Vec2 center, f32 radius) const {
    std::vector<types::Entity*> found;
    for_each_entity_within(center, radius, [&](types::Entity& entity) { found.push_back(&entity); });
    // The tree visits in its own order; a caller that acts on the answer (a pointer action, a save, a
    // test) gets spawn order instead, so the same question gives the same answer twice.
    std::sort(found.begin(), found.end(), [](const types::Entity* left, const types::Entity* right) {
        return left->serial() < right->serial();
    });
    return found;
}

usize MineLayer::auto_updating_count() const {
    usize count = 0;
    for (const std::unique_ptr<types::Entity>& entity : entities_) {
        if (entity->auto_update()) ++count;
    }
    return count;
}

void MineLayer::reindex(types::Entity& entity, t2d::Vec2 before) {
    entity_index_.move(entity.serial_, before, entity.position_);
}

void MineLayer::sweep_destroyed() {
    if (destroyed_.empty()) return;
    destroyed_.clear();
    std::erase_if(entities_, [](const std::unique_ptr<types::Entity>& entity) { return !entity->alive(); });
}

const types::SceneTile* MineLayer::plot_at(i32 layer, GridPos cell) const {
    if (!inside(cell)) return nullptr;
    const types::SceneTile* found = nullptr;
    for_each_plot_in(t2d::TileRect{cell.x, cell.y, 1, 1}, [&](const types::SceneTile& plot) {
        if (found != nullptr || plot.layer() != layer) return;
        found = &plot;
    });
    return found;
}

const types::SceneTile* MineLayer::top_plot_at(GridPos cell) const {
    for (i32 layer = layer_count() - 1; layer >= 0; --layer) {
        if (const types::SceneTile* plot = plot_at(layer, cell); plot != nullptr) return plot;
    }
    return nullptr;
}

bool MineLayer::blocks(i32 layer_mask, GridPos cell) const {
    if (!inside(cell)) return true;
    for (i32 layer = 0; layer < layer_count(); ++layer) {
        if ((layer_mask & (1 << layer)) == 0) continue;
        if (!cells_.at(layer, cell).empty()) return true;
    }
    return false;
}

std::string MineLayer::dump_text(const ContentRegistry& registry) const {
    std::string text = std::format("layer {}: {}x{}, {} tile layer(s), seed {:016x}\n", index_, width(), height(),
                                   layer_count(), seed_);
    text += std::format("  {} plot(s), {} of them ticking, {} mirrored, {} cell(s) filled of {} ({} KiB)\n",
                        plots_.size(), ticking_.size(), mirrored_, cells_.filled(), cells_.cell_count(),
                        cell_bytes() / 1024);
    text += std::format("  {} entity(ies), {} of them updating themselves\n", entities_.size(),
                        auto_updating_count());
    for (const std::unique_ptr<types::Entity>& entity : entities_) {
        text += std::format("  [e{}] {} #{} '{}' at {:.2f},{:.2f}{}\n", entity->serial(),
                            content_kind_name(entity->kind()), entity->id(), entity->name(registry),
                            entity->position().x, entity->position().y,
                            entity->auto_update()
                                ? (entity->period_seconds() > 0.0f
                                       ? std::format(" every {:.3f}s", entity->period_seconds())
                                       : std::string(" every tick"))
                                : std::string(" not updated by the layer"));
    }
    for (usize serial = 0; serial < plots_.size(); ++serial) {
        const types::SceneTile& plot = *plots_[serial];
        text += std::format("  [{}] {} #{} '{}' at {},{} {}x{} on tile layer {}{}{}{}\n", serial,
                            content_kind_name(plot.kind()), plot.id(), plot.name(registry), plot.anchor().x,
                            plot.anchor().y, plot.width(), plot.height(), plot.layer(),
                            dynamic_cast<const types::EntityTile*>(&plot) != nullptr ? " ticking" : "",
                            plot.mirrored() ? " mirrored" : "", plot.dirty() ? " dirty" : "");
        // A drill is the one plot whose *job* is part of what it is: what it mines, how long a drop
        // takes, and how many it has thrown - which is what a bug report about production needs.
        if (const auto* drill = dynamic_cast<const types::Drill*>(&plot); drill != nullptr) {
            if (drill->ready()) {
                text += std::format("        mines ore '{}' every {:.3f}s -> drop '{}', {} thrown\n",
                                    drill->job().ore_name, drill->job().period_seconds, drill->job().drop_name,
                                    drill->produced());
            } else {
                text += std::format("        idle: no ore under it, or nothing that says how hard it is\n");
            }
        }
    }
    return text;
}

// ----------------------------------------------------------------------- the world ---

MineWorld::MineWorld(u64 seed, LayerShape shape) : seed_(seed), shape_(shape) {}

MineWorld::MineWorld() : shape_(LayerShape{1, 1, 1}) {}

const MineLayer& MineWorld::enter(i32 index, const ContentRegistry& registry,
                                  const ContentDefinitions& definitions) {
    build_report_ = LayerBuildReport{};
    LayerSpec spec;
    if (generator_) spec = generator_(seed_, index);
    if (spec.index != index) {
        build_report_.refuse(
            std::format("the generator answered for layer {} when layer {} was asked for", spec.index, index));
    }
    // The description's shape is what the layer is; a description that does not say (a generator that
    // only knows the content) falls back to the world's shape.
    LayerShape shape = spec.shape;
    if (shape.width == 0) shape.width = shape_.width;
    if (shape.height == 0) shape.height = shape_.height;
    if (shape.tile_layers <= 0) shape.tile_layers = shape_.tile_layers;
    layer_ = std::make_unique<MineLayer>(index, shape, layer_seed(seed_, index));
    // What the description itself could not do comes first: it is about the data, and everything the
    // build says after it is about what that cost.
    for (const std::string& problem : spec.problems) build_report_.refuse(problem);
    layer_->build(spec, registry, definitions, &build_report_);
    return *layer_;
}

usize MineWorld::refresh() { return layer_ != nullptr ? layer_->refresh() : 0; }

usize MineWorld::tick(f32 delta_seconds) { return layer_ != nullptr ? layer_->tick(delta_seconds) : 0; }

bool MineWorld::blocks(i32 layer_mask, GridPos cell) const {
    return layer_ == nullptr || layer_->blocks(layer_mask, cell);
}

} // namespace mine
