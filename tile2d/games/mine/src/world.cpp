#include <mine/world.h>

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

// ----------------------------------------------------------------------- the layer ---

void LayerBuildReport::refuse(std::string message) {
    ++refused;
    if (errors.size() >= kMaxErrors) return;
    if (std::find(errors.begin(), errors.end(), message) != errors.end()) return;
    errors.push_back(std::move(message));
}

MineLayer::MineLayer(i32 index, LayerShape shape, u64 seed)
    : index_(index), shape_(shape), seed_(seed), rng_(seed),
      cells_(shape.width, shape.height, shape.tile_layers) {
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
    const types::TileDefinition* definition = definitions.find(where.kind, where.name);
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
    plots_.push_back(std::move(plot));
    index_plot(serial);

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
    return ran;
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
    for (usize serial = 0; serial < plots_.size(); ++serial) {
        const types::SceneTile& plot = *plots_[serial];
        text += std::format("  [{}] {} #{} '{}' at {},{} {}x{} on tile layer {}{}{}{}\n", serial,
                            content_kind_name(plot.kind()), plot.id(), plot.name(registry), plot.anchor().x,
                            plot.anchor().y, plot.width(), plot.height(), plot.layer(),
                            dynamic_cast<const types::EntityTile*>(&plot) != nullptr ? " ticking" : "",
                            plot.mirrored() ? " mirrored" : "", plot.dirty() ? " dirty" : "");
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
    layer_.emplace(index, shape, layer_seed(seed_, index));
    layer_->build(spec, registry, definitions, &build_report_);
    return *layer_;
}

usize MineWorld::refresh() { return layer_.has_value() ? layer_->refresh() : 0; }

usize MineWorld::tick(f32 delta_seconds) { return layer_.has_value() ? layer_->tick(delta_seconds) : 0; }

bool MineWorld::blocks(i32 layer_mask, GridPos cell) const {
    return !layer_.has_value() || layer_->blocks(layer_mask, cell);
}

} // namespace mine
