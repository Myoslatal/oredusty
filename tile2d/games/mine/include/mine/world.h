// Mine - the world: the layers of a mine, and the plots they are made of.
//
// A **layer** is one floor of the mine: its own map, its own content, its own dice. It is not built
// until it is entered, and only the layer being played is held, so a mine of a hundred layers costs
// one layer (docs/GAME_DESIGN.md section 4).
//
// What a layer holds is **plots** (types/tile.h): fixed things that occupy cells. Both halves are
// needed and they answer different questions. The **grid** (content_grid.h) says which content sits on
// every cell, four bytes a cell: that is what a save, a routing query and a renderer's cull walk
// cheaply. The **plots** are the objects that content turns into, and they are what carries a
// footprint (a 3x3 is one plot, not nine), a cadence and a way of drawing itself.
//
// A layer is built from a **layer description** (LayerSpec): the shape of its map and the content that
// goes in it. That is what a generator produces - the designer's data in story mode, a derivation from
// (seed, index) in endless mode - and both produce the same description, so nothing below this comment
// has to know which mode it is.
//
// The two passes are the reason there are two kinds of plot (section 1.13): refresh() walks every plot
// and pays only for the dirty ones, tick() walks the plots that run and nothing else. Which plots those
// are is decided here, once, when they are created - never by asking a plot at runtime.
//
// Nothing in this file is content: no resource, structure, machine or recipe is named here.
#pragma once

#include <mine/content_grid.h>
#include <mine/content_loader.h>
#include <mine/registry.h>
#include <mine/types/types.h>

#include <t2d/core/math2d.h>
#include <t2d/core/rng.h>
#include <t2d/core/types.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mine {

using t2d::f32;
using t2d::i32;
using t2d::u32;
using t2d::u64;
using t2d::usize;

/// How big a layer is and how many tile layers it has. Engineering, not content: the numbers come from
/// the designer's layer rules (docs/GAME_DESIGN.md section 7.3, 7.10) or from a command line, and
/// nothing here invents one.
struct LayerShape {
    u32 width = 1;
    u32 height = 1;
    i32 tile_layers = 1;
};

/// One piece of content a layer description puts somewhere: what it is (its kind and the name the
/// registry knows it by), which tile layer it sits on, where its top left corner is, and how many
/// cells it covers. A 2x2, 3x3 or 2x3 structure is this struct with width and height set - not a
/// special case anywhere below.
struct PlacedContent {
    ContentKind kind = ContentKind::Structure;
    std::string name;
    i32 layer = 0;
    GridPos anchor{};
    i32 width = 1;
    i32 height = 1;
};

/// What a layer is, before it exists: the shape of its map, and the content that goes in it.
struct LayerSpec {
    i32 index = 0;
    LayerShape shape{};
    std::vector<PlacedContent> content;
};

/// The generator interface, fixed by docs/GAME_DESIGN.md section 4: a seed and a layer index in, a
/// layer description out. Story mode returns the designer's data, endless mode derives it from the
/// pair; the same pair always gives the same layer back, which is what makes a mine reproducible.
using LayerGenerator = std::function<LayerSpec(u64 seed, i32 index)>;

/// The dice one layer of a mine gets: the world's seed mixed with the layer index, so two layers of
/// one mine are not the same map and one (seed, index) is always the same map. It is what a layer
/// seeds its own generator with, and what a generator that has to derive something per layer uses.
[[nodiscard]] u64 layer_seed(u64 world_seed, i32 index);

/// The generator in force until the layer rules exist: an empty layer of \p shape. It places no
/// content, because which content a layer holds is the designer's (docs/GAME_DESIGN.md sections 7.3,
/// 7.4, 7.8, 7.9) - and an empty layer is a layer: it can be entered, walked, queried and drawn.
[[nodiscard]] LayerGenerator empty_layer_generator(LayerShape shape);

/// Stand-ins for those layer rules, for looking at content before the rules exist: one horizontal band
/// per registered plot content, and a deterministic scatter of them. They name no content - they lay
/// out whatever the registry holds, exactly the way the sandbox's debug fills do - so they are a **view
/// of the registry, not a rule about the mine**, and the real rules replace them.
///
/// Both put one content entry per tile layer, in registration order, wrapping at the layer count: an
/// arbitrary but deterministic choice, made so that a stack of kinds can be told apart. The registry
/// must outlive the generator and every world it is installed in.
[[nodiscard]] LayerGenerator debug_band_generator(const ContentRegistry& registry, LayerShape shape);
[[nodiscard]] LayerGenerator debug_scatter_generator(const ContentRegistry& registry, LayerShape shape,
                                                     u32 percent = 25);

/// What building a layer did. A description that names content the registry does not have, puts a
/// footprint outside the map, or puts it on top of something already on that tile layer is **refused
/// with a reason**: a layer that quietly loses a structure is a bug report nobody can act on.
struct LayerBuildReport {
    /// How many different refusals are kept. A rule that names content the registry does not have
    /// refuses once per cell it wanted to place, and a layer of a million cells is not a bug report of a
    /// million lines: every refusal is counted, the list keeps the first of each kind and stops here.
    static constexpr usize kMaxErrors = 16;

    usize placed = 0;      ///< plots created
    usize cells = 0;       ///< cells they cover between them
    usize ticking = 0;     ///< plots that run on a cadence
    usize mirrored = 0;    ///< plots the dice turned around
    usize refused = 0;     ///< placements the layer refused, whether or not their reason is kept
    std::vector<std::string> errors;

    /// Records a refusal: counted always, kept once per distinct reason, up to kMaxErrors of them.
    void refuse(std::string message);

    [[nodiscard]] bool clean() const { return refused == 0; }
    /// True when some refusals are not in \c errors because the list was full or the reason repeated.
    [[nodiscard]] bool truncated() const { return refused > errors.size(); }
};

/// One layer of the mine: a map grid, the plots its content turned into, and the dice that decided how
/// those plots draw themselves.
class MineLayer {
public:
    /// The block of cells a plot is bucketed by. The same 32 the framework's TileMap uses: a plot is
    /// registered in every chunk its footprint touches, so a query walks what a viewport covers
    /// instead of what the layer holds (docs/GAME_DESIGN.md section 2).
    static constexpr i32 kChunkSize = 32;

    MineLayer(i32 index, LayerShape shape, u64 seed);

    // --- what it is -------------------------------------------------------------------------------
    [[nodiscard]] i32 index() const { return index_; }
    [[nodiscard]] const LayerShape& shape() const { return shape_; }
    [[nodiscard]] u32 width() const { return cells_.width(); }
    [[nodiscard]] u32 height() const { return cells_.height(); }
    [[nodiscard]] i32 layer_count() const { return cells_.layer_count(); }
    [[nodiscard]] bool inside(GridPos cell) const { return cells_.inside(cell); }
    /// The seed this layer's own dice were seeded with: the world's seed mixed with the index.
    [[nodiscard]] u64 seed() const { return seed_; }

    // --- building ---------------------------------------------------------------------------------
    /// Places the content \p where names: builds the plot its kind calls for (types/tile_definition.h),
    /// rolls its mirroring with this layer's dice, writes it into every cell of its footprint, and
    /// indexes it. Returns the plot, or nullptr and a reason in \p report - nothing is half placed, and
    /// a refusal leaves the layer exactly as it was.
    [[nodiscard]] types::SceneTile* place(const ContentRegistry& registry, const ContentDefinitions& definitions,
                                          const PlacedContent& where, LayerBuildReport* report = nullptr);

    /// Places everything \p spec describes. This is what entering a layer does.
    void build(const LayerSpec& spec, const ContentRegistry& registry, const ContentDefinitions& definitions,
               LayerBuildReport* report);

    // --- the two passes ---------------------------------------------------------------------------
    /// Walks every plot and brings the dirty ones up to date; returns how many actually ran. A plot
    /// nobody touched costs one branch (types/scene_tile.h).
    usize refresh();
    /// Hands the frame's seconds to the plots that run, and returns how many took their turn. Plots
    /// that do not run are not in this list at all - that is the saving (types/entity_tile.h).
    usize tick(f32 delta_seconds);

    // --- what is where ----------------------------------------------------------------------------
    /// What the grid says is on \p cell of \p layer: four bytes, no walk.
    [[nodiscard]] ContentRef ref_at(i32 layer, GridPos cell) const { return cells_.at(layer, cell); }
    /// The plot covering \p cell on \p layer, or nullptr when there is none. A plot is found through
    /// the chunk index, so a 3x3 answers for all nine of its cells and the caller never has to know
    /// where its anchor is.
    [[nodiscard]] const types::SceneTile* plot_at(i32 layer, GridPos cell) const;
    /// The topmost plot at \p cell, whatever tile layer it is on: what the pointer is over.
    [[nodiscard]] const types::SceneTile* top_plot_at(GridPos cell) const;
    /// Whether \p cell is occupied on any tile layer in \p layer_mask. This is the collision question
    /// the game asks - routing, placing, demands - and which layers block is the designer's
    /// (docs/GAME_DESIGN.md section 2), so the mask is a parameter rather than a rule in here. A cell
    /// outside the map counts as blocked: there is nothing to route through out there.
    [[nodiscard]] bool blocks(i32 layer_mask, GridPos cell) const;

    /// Visits every plot whose footprint touches \p cells, each one exactly once: a plot that straddles
    /// two chunks is not visited twice. The visitor takes a \c const \c types::SceneTile&.
    template <typename Visitor>
    void for_each_plot_in(const t2d::TileRect& cells, Visitor&& visit) const {
        if (cells.empty() || plots_.empty()) return;
        if (visited_.size() < plots_.size()) visited_.resize(plots_.size(), 0);
        ++visit_stamp_;
        if (visit_stamp_ == 0) {
            // The stamp counter wrapped: every stamp in the buffer is stale again.
            std::fill(visited_.begin(), visited_.end(), 0u);
            visit_stamp_ = 1;
        }
        for (i32 cy = chunk_of(cells.y); cy <= chunk_of(cells.bottom() - 1); ++cy) {
            for (i32 cx = chunk_of(cells.x); cx <= chunk_of(cells.right() - 1); ++cx) {
                const auto found = chunks_.find(chunk_key(cx, cy));
                if (found == chunks_.end()) continue;
                for (const u32 serial : found->second) {
                    if (visited_[serial] == visit_stamp_) continue;
                    const types::SceneTile& plot = *plots_[serial];
                    const t2d::TileRect& covered = plot.cells();
                    if (!(covered.x < cells.right() && cells.x < covered.right() && covered.y < cells.bottom() &&
                          cells.y < covered.bottom())) {
                        continue;
                    }
                    visited_[serial] = visit_stamp_;
                    visit(plot);
                }
            }
        }
    }

    // --- what it holds ----------------------------------------------------------------------------
    [[nodiscard]] usize plot_count() const { return plots_.size(); }
    [[nodiscard]] usize ticking_count() const { return ticking_.size(); }
    [[nodiscard]] usize mirrored_count() const { return mirrored_; }
    [[nodiscard]] usize filled_cells() const { return cells_.filled(); }
    [[nodiscard]] usize filled_cells(i32 layer) const { return cells_.filled(layer); }
    [[nodiscard]] usize cell_count() const { return cells_.cell_count(); }
    [[nodiscard]] usize cell_bytes() const { return cells_.bytes(); }
    /// Every plot, in the order it was placed.
    [[nodiscard]] const std::vector<std::unique_ptr<types::SceneTile>>& plots() const { return plots_; }
    [[nodiscard]] const ContentGrid& cells() const { return cells_; }

    /// The layer as text: one line per plot, then what the layer costs. What goes into a bug report -
    /// and what a test compares to prove that the same (seed, index) builds the same layer twice.
    [[nodiscard]] std::string dump_text(const ContentRegistry& registry) const;

private:
    /// Which chunk a cell falls in. Floor division, so a cell left of the map lands in the chunk
    /// before it rather than in chunk zero.
    [[nodiscard]] static constexpr i32 chunk_of(i32 cell) {
        return cell >= 0 ? cell / kChunkSize : -((kChunkSize - 1 - cell) / kChunkSize);
    }
    [[nodiscard]] static constexpr u64 chunk_key(i32 cx, i32 cy) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cy);
    }
    /// Registers a plot in every chunk its footprint touches.
    void index_plot(usize serial);

    i32 index_ = 0;
    LayerShape shape_{};
    u64 seed_ = 0;
    /// This layer's own dice: the world's seed mixed with the index, used while the map is built to
    /// decide which plots draw themselves mirrored, and for nothing else (section 1.14).
    t2d::Rng rng_;
    ContentGrid cells_{};
    /// Every plot, owned here. A plot never moves once it is placed, so the pointers the chunk index
    /// and the callers hold stay valid.
    std::vector<std::unique_ptr<types::SceneTile>> plots_;
    /// The plots that run, decided when they were created: the tick pass walks this and nothing else.
    std::vector<types::EntityTile*> ticking_;
    usize mirrored_ = 0;
    /// Chunk -> the plots whose footprint touches it. The spatial index that keeps a query, a draw and
    /// a placement check proportional to what is on screen rather than to the size of the map.
    std::unordered_map<u64, std::vector<u32>> chunks_;
    /// Per query scratch: the stamp of the last query that visited a plot, so a plot registered in two
    /// chunks is still visited once. Mutable because a query does not change what the layer is.
    mutable std::vector<u32> visited_;
    mutable u32 visit_stamp_ = 0;
};

/// The mine: a seed, the shape of its layers, the generator that describes them, and the one layer
/// that is being played.
class MineWorld {
public:
    /// A world nobody has described yet: no seed, a one cell shape, no generator. A caller gives it
    /// both before it enters a layer - which is what makes it a member a screen can hold.
    MineWorld();
    MineWorld(u64 seed, LayerShape shape);

    [[nodiscard]] u64 seed() const { return seed_; }
    [[nodiscard]] const LayerShape& shape() const { return shape_; }
    /// The shape a generator that does not describe one itself falls back to.
    void set_shape(LayerShape shape) { shape_ = shape; }
    void set_generator(LayerGenerator generator) { generator_ = std::move(generator); }
    [[nodiscard]] const LayerGenerator& generator() const { return generator_; }

    /// Builds \p index and makes it the layer being played, replacing whatever layer was held: a mine
    /// of a hundred layers costs one layer, and entering a layer again builds the same mine back,
    /// because the generator only ever sees (seed, index).
    const MineLayer& enter(i32 index, const ContentRegistry& registry, const ContentDefinitions& definitions);
    [[nodiscard]] bool has_layer() const { return layer_.has_value(); }
    [[nodiscard]] const MineLayer& layer() const { return *layer_; }
    [[nodiscard]] i32 layer_index() const { return layer_.has_value() ? layer_->index() : 0; }
    /// What building the layer in force did, including everything the generator asked for that was
    /// refused.
    [[nodiscard]] const LayerBuildReport& build_report() const { return build_report_; }

    /// The layer's passes, and nothing without a layer.
    usize refresh();
    usize tick(f32 delta_seconds);
    [[nodiscard]] bool blocks(i32 layer_mask, GridPos cell) const;

private:
    u64 seed_ = 0;
    LayerShape shape_{};
    LayerGenerator generator_{};
    std::optional<MineLayer> layer_{};
    LayerBuildReport build_report_{};
};

} // namespace mine
