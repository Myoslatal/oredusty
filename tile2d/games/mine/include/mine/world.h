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
// A layer also holds **entities** (types/entity.h): things that are not on a cell, that move, and that
// can interact. They are found the other way round - a plot through the chunk index, because a plot is
// cells, and an entity through a quadtree over the layer's rectangle, because an entity is a point
// (quadtree.h, docs/GAME_DESIGN.md section 1.24).
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
#include <mine/quadtree.h>
#include <mine/registry.h>
#include <mine/types/content_stack.h>
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
    /// What the plot is created **holding**: the starting slots of a container sub-object, in file order.
    /// Empty for a plot that starts empty, which is every placement that does not say otherwise.
    std::vector<ContentStack> holds{};
};

/// What a layer is, before it exists: the shape of its map, and the content that goes in it.
struct LayerSpec {
    i32 index = 0;
    LayerShape shape{};
    std::vector<PlacedContent> content;
    /// What the description could not do, in the generator's own words: a rule that names a tile layer
    /// this map does not have, a scatter whose floor restriction leaves fewer cells than it asked for.
    /// The build report carries them, so a layer that is not the layer the data asked for says so
    /// instead of just being small - the rule everything else in this file follows.
    std::vector<std::string> problems;
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

/// Lays out one of every registered **drop** in the layer, spread across it, and returns how many. This
/// is a **debug fill for entities**, the same idea as the plot fills above: it names no content, it lays
/// out whatever the registry holds under the drop table, so a designer can see and drag them before the
/// data that places drops exists (docs/GAME_DESIGN.md sections 1.25, 7.7). The positions are deliberately
/// not on whole cells: a drop's position is a point, and a layout that landed on cell corners would hide
/// that. The registry must outlive the layer's use of what it names, the way every other fill's must.
[[nodiscard]] usize lay_out_drops(MineLayer& layer, const ContentRegistry& registry,
                                 const ContentDefinitions& definitions);

/// Lays out one of every registered **drill** in \p layer, each on a cell that has ore under it, on the
/// layer's **top** tile layer - a drill stands above what it mines (docs/GAME_DESIGN.md section 1.26).
/// This is the debug fill for the mining rule, the way lay_out_drops is for drops: it names no content,
/// it places what the registry holds under the drill table, and what it cannot place is logged rather
/// than guessed at. Returns how many it placed.
[[nodiscard]] usize lay_out_drills(MineLayer& layer, const ContentRegistry& registry,
                                   const ContentDefinitions& definitions);

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
    /// Whether \p where may stand where it says, and why not when it may not: the query a caller makes
    /// **before** placing anything (docs/GAME_DESIGN.md section 1.34). The conditions are the map's, not
    /// the plot's: the cell has to be on the map, the tile layer has to exist, the cells have to be free
    /// on that tile layer, and content that has to stand on something has to find it there - a drill has
    /// to sit on ore (types/drill.h).
    [[nodiscard]] bool can_place(const PlacedContent& where, std::string* why = nullptr) const;

    void build(const LayerSpec& spec, const ContentRegistry& registry, const ContentDefinitions& definitions,
               LayerBuildReport* report);

    // --- the two passes ---------------------------------------------------------------------------
    /// Walks every plot and brings the dirty ones up to date; returns how many actually ran. A plot
    /// nobody touched costs one branch (types/scene_tile.h).
    usize refresh();
    /// Hands the frame's seconds to everything that runs - the plots on a cadence, and the entities -
    /// and returns how many took their turn. Plots that do not run are not in the list at all, which is
    /// the saving for the many; entities are walked and each one decides, because a cadence belongs to
    /// the thing that owns it and there are few entities next to the plots (types/entity.h).
    usize tick(f32 delta_seconds);

    // --- what is where ----------------------------------------------------------------------------
    /// What the grid says is on \p cell of \p layer: four bytes, no walk.
    [[nodiscard]] ContentRef ref_at(i32 layer, GridPos cell) const { return cells_.at(layer, cell); }
    /// The plot covering \p cell on \p layer, or nullptr when there is none. A plot is found through
    /// the chunk index, so a 3x3 answers for all nine of its cells and the caller never has to know
    /// where its anchor is.
    [[nodiscard]] const types::SceneTile* plot_at(i32 layer, GridPos cell) const;
    /// The same lookup, for the things that **act** on a plot: a drop let go on a warehouse goes into it
    /// (pointer_drag.h), a click opens its panel (app.cpp). Everything that only reads keeps the const one.
    [[nodiscard]] types::SceneTile* plot_at(i32 layer, GridPos cell);
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

    // --- entities: what is in the layer without being on a cell (types/entity.h) -------------------
    /// Takes ownership of \p entity, hands it the next serial and puts it in the quadtree. Nothing is
    /// refused: which content exists is the registry's business, and an entity is a thing a caller
    /// built (a machine's own code, a mod, a test).
    types::Entity& adopt(std::unique_ptr<types::Entity> entity);
    /// Makes an entity of content \p kind and \p id at \p position, or returns nullptr for a
    /// kNoContent id - an entity no save can name is not something a layer should hold.
    [[nodiscard]] types::Entity* spawn(ContentKind kind, ContentId id, t2d::Vec2 position);
    /// Marks \p entity dead and takes it out of the index **now**, so the next query cannot find it;
    /// the storage is compacted at the end of the tick pass, because a callback may be walking it.
    /// Returns whether it was alive and this layer's.
    bool destroy(types::Entity& entity);
    /// Asks two entities to interact: each is handed the other, and the layer reports whether either
    /// did anything. Both are asked, because an interaction has two sides - and neither side has to
    /// know what the other one is (types/entity.h).
    bool interact(types::Entity& a, types::Entity& b);
    /// Visits every entity within \p radius of \p center, in the tree's own order. A query does not
    /// change the layer, but what it finds is fair game: a caller may move it, ask it to interact, or
    /// destroy it.
    template <typename Visitor>
    void for_each_entity_within(t2d::Vec2 center, f32 radius, Visitor&& visit) const {
        entity_index_.for_each_within(center, radius, [&](const QuadTree<types::Entity*>::Entry& entry) {
            visit(*entry.payload);
        });
    }
    /// The same walk, collected and ordered by serial - spawn order. A caller that acts on the answer
    /// gets the same order twice, which is what a save and a test both want from it.
    [[nodiscard]] std::vector<types::Entity*> entities_within(t2d::Vec2 center, f32 radius) const;

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

    /// Every entity, in spawn order. Dead ones are gone from here at the end of the tick that killed
    /// them, so this is the list of what is alive.
    [[nodiscard]] const std::vector<std::unique_ptr<types::Entity>>& entities() const { return entities_; }
    [[nodiscard]] usize entity_count() const { return entities_.size(); }
    /// How many of them the layer updates by itself (the rest are driven by something else).
    [[nodiscard]] usize auto_updating_count() const;
    /// What the last entity query walked (quadtree.h): the number that makes "the index is why a
    /// radius query is cheap" a measurement rather than a claim.
    [[nodiscard]] const QuadTree<types::Entity*>::Cost& last_entity_query_cost() const {
        return entity_index_.last_query_cost();
    }

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

    /// An entity that is in this layer moved: the quadtree follows it. Called by
    /// types::Entity::set_position(), which is the only way a position changes - an index nobody has to
    /// remember to update is an index that cannot go stale.
    void reindex(types::Entity& entity, t2d::Vec2 before);

    /// Tells a drill what it stands on, while the layer is being built: the ore below it, how hard that
    /// ore is and what it drops, and the period that comes out of it (hardness / power). A drill with
    /// nothing to mine is refused into \p report rather than left to do nothing forever
    /// (docs/GAME_DESIGN.md section 1.26).
    void configure_drill(types::Drill& drill, const ContentRegistry& registry,
                         const ContentDefinitions& definitions, LayerBuildReport* report);
    /// Frees the storage of what was destroyed during a pass, once nothing can be walking it.
    void sweep_destroyed();

    /// The bookkeeping an entity cannot do for itself: which layer it is in, which serial it has, and
    /// whether it is still alive (types/entity.h).
    friend class types::Entity;

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

    /// Everything that is in the layer without being on a cell, owned here, in spawn order. What
    /// destroy() marked leaves the index at once and this vector at the end of the tick.
    std::vector<std::unique_ptr<types::Entity>> entities_;
    /// The same things, indexed by position: a query answers "what is near this point" instead of
    /// walking the layer (quadtree.h). Its root is the map, so the layer's rectangle is what a radius
    /// query is bounded by.
    QuadTree<types::Entity*> entity_index_;
    /// What destroy() marked, waiting for the pass that is running to finish.
    std::vector<types::Entity*> destroyed_;
    /// Handed out in spawn order, never reused: it is what a query is ordered by, and what a save can
    /// point at without depending on where an entity sits in the vector.
    u32 next_serial_ = 1;
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
    [[nodiscard]] bool has_layer() const { return layer_ != nullptr; }
    [[nodiscard]] const MineLayer& layer() const { return *layer_; }
    /// The same layer, for the one screen that **acts** on it: the world view moves a drop the pointer
    /// dragged (pointer_drag.h). Everything that only reads the mine keeps taking the const one.
    [[nodiscard]] MineLayer& layer() { return *layer_; }
    [[nodiscard]] i32 layer_index() const { return layer_ != nullptr ? layer_->index() : 0; }
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
    /// The layer being played, held by pointer: **what is in a layer points back at it** (an entity
    /// knows the layer it is in, so that moving tells the index, types/entity.h), and a layer that moved
    /// would leave those pointers naming a layer that is no longer there. Entering a layer replaces this
    /// one, which is what makes "a mine of a hundred layers costs one layer" true.
    std::unique_ptr<MineLayer> layer_;
    LayerBuildReport build_report_{};
};

} // namespace mine
